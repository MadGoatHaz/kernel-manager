// Copyright (C) 2022-2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "transaction_worker.hpp"

#include "kernel.hpp"
#include "utils.hpp"

#include <algorithm>  // for any_of, find_if
#include <ranges>     // for ranges::*
#include <span>       // for span

#include <alpm.h>

#include <fmt/compile.h>
#include <fmt/core.h>

namespace {

// Build the install list for the given change list. For each kernel name in
// changeList, if the matching kernel is found and is either not installed or
// has an update available, add it (and its dependencies) to the global
// install list via kernel->install().
//
// Runs entirely in the worker thread with the worker's own alpm handle.
bool install_packages(alpm_handle_t* handle, const std::span<Kernel>& kernels, const std::span<std::string>& selected_list) {
    for (const auto& selected : selected_list) {
        const auto kernel = std::ranges::find_if(kernels, [selected](auto&& el) { return el.get_raw() == selected; });
        if ((kernel != kernels.end()) && (!kernel->is_installed() || kernel->is_update_available())) {
            if (!kernel->install()) {
                fmt::print(stderr, "failed to add package to be installed ({})\n", alpm_strerror(alpm_errno(handle)));
            }
        }
    }
    return true;
}

// Build the removal list for the given change list. For each kernel name in
// changeList, if the matching kernel is found and is currently installed,
// add it (and its dependencies) to the global removal list via
// kernel->remove().
//
// Runs entirely in the worker thread with the worker's own alpm handle.
bool remove_packages(alpm_handle_t* handle, const std::span<Kernel>& kernels, const std::span<std::string>& selected_list) {
    for (const auto& selected : selected_list) {
        const auto kernel = std::ranges::find_if(kernels, [selected](auto&& el) { return el.get_raw() == selected; });
        if ((kernel != kernels.end()) && (kernel->is_installed())) {
            if (!kernel->remove()) {
                fmt::print(stderr, "failed to add package to be removed ({})\n", alpm_strerror(alpm_errno(handle)));
            }
        }
    }
    return true;
}

// Check whether the kernel install state has actually changed as a result of
// the transaction: any package in the install list that is now present in the
// local DB, or any package in the removal list that is no longer present.
//
// Runs entirely in the worker thread with the worker's own alpm handle.
bool is_kernels_change_state(alpm_handle_t* handle, std::span<std::string> kernel_install_list, std::span<std::string> kernel_removal_list) {
    if (handle == nullptr) {
        return false;
    }
    auto* local_db = alpm_get_localdb(handle);
    if (local_db == nullptr) {
        return false;
    }

    if (std::ranges::any_of(kernel_install_list, [local_db](auto&& kernel_install) { return nullptr != alpm_db_get_pkg(local_db, kernel_install.data()); })) {
        return true;
    }
    if (std::ranges::any_of(kernel_removal_list, [local_db](auto&& kernel_removal) { return nullptr == alpm_db_get_pkg(local_db, kernel_removal.data()); })) {
        return true;
    }

    return false;
}

}  // namespace

TransactionWorker::TransactionWorker(QObject* parent)
  : QObject(parent) {
    // Intentionally empty: the worker is configured by the main thread
    // before being moved to its QThread. No state is initialized here
    // because everything is set up per-transaction.
}

TransactionWorker::~TransactionWorker() {
    // Ensure the worker stops before destruction. This is a safety net;
    // the normal path is for MainWindow::closeEvent to call requestStop()
    // and wait for the thread to finish before destroying the worker.
    requestStop();
}

void TransactionWorker::startTransaction(const QStringList& changeList) {
    // If we've been asked to stop, don't start new work.
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }

    // Prevent overlapping transactions. The main thread disables the
    // Execute button while a transaction is in flight, so this is a
    // defensive guard.
    if (m_in_flight.load(std::memory_order_acquire)) {
        fmt::print(stderr, "TransactionWorker: ignoring startTransaction — a transaction is already in flight\n");
        return;
    }

    // If the change list is empty, there's nothing to do.
    if (changeList.isEmpty()) {
        return;
    }

    m_in_flight.store(true, std::memory_order_release);
    m_pendingChangeList = changeList;

    // Notify the main thread that we're starting (so it can update UI).
    emit transactionStarted();

    // --- The actual transaction work (runs in the worker thread) ---

    // Create our OWN alpm handle. This is the key design decision: the
    // worker never touches the main thread's handle, so there's no
    // cross-thread sharing and no use-after-free on window close.
    alpm_errno_t err{};
    auto* handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &err);
    if (handle == nullptr) {
        m_in_flight.store(false, std::memory_order_release);
        emit error(QStringLiteral("Failed to initialize alpm handle (%1)").arg(QString::fromStdString(alpm_strerror(err))));
        return;
    }

    // Fetch the kernel list from our own handle. The Kernel objects hold
    // raw pointers into this handle's memory, so they're only valid while
    // we own the handle (i.e., within this function).
    auto kernels = Kernel::get_kernels(handle);

    // Convert the change list to std::string for the transaction functions.
    std::vector<std::string> change_list_vec(static_cast<std::size_t>(m_pendingChangeList.size()));
    for (int i = 0; i < m_pendingChangeList.size(); ++i) {
        change_list_vec.at(static_cast<std::size_t>(i)) = m_pendingChangeList.at(i).toStdString();
    }

    // Build the install and removal lists (populates the global
    // g_kernel_install_list and g_kernel_removal_list in kernel.cpp).
    install_packages(handle, kernels, std::span{change_list_vec});
    remove_packages(handle, kernels, std::span{change_list_vec});

    // Execute the transaction: runs `pacman -S --needed <pkgs>` and/or
    // `pacman -Rsn <pkgs>` in the escalated terminal. This is the
    // long-running part (can take minutes). The terminal blocks until
    // the user presses enter, so this call returns when the transaction
    // is complete.
    //
    // Check for stop request before the blocking call: if the user closed
    // the window while we were setting up, skip the pacman run entirely.
    if (m_stopping.load(std::memory_order_acquire)) {
        utils::release_alpm(handle, &err);
        Kernel::get_install_list().clear();
        Kernel::get_removal_list().clear();
        m_in_flight.store(false, std::memory_order_release);
        return;
    }

    Kernel::commit_transaction();

    // Check if the kernel state actually changed (to decide whether the
    // main thread needs to re-fetch the kernel list).
    auto& kernel_install_list = Kernel::get_install_list();
    auto& kernel_removal_list = Kernel::get_removal_list();
    const bool is_kernel_status_changed = is_kernels_change_state(handle, std::span{kernel_install_list}, std::span{kernel_removal_list});

    // Release our handle and clean up the global lists.
    utils::release_alpm(handle, &err);
    kernel_install_list.clear();
    kernel_removal_list.clear();

    m_in_flight.store(false, std::memory_order_release);
    m_pendingChangeList.clear();

    // Notify the main thread that we're done. The main thread will
    // re-fetch the kernel list if the state changed and update the UI.
    emit transactionFinished(true, is_kernel_status_changed);
}

void TransactionWorker::requestStop() {
    // Set the stop flag (release semantics: all prior writes are visible).
    m_stopping.store(true, std::memory_order_release);
    fmt::print(stderr, "TransactionWorker: stop requested\n");
}
