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

#ifndef TRANSACTION_WORKER_HPP
#define TRANSACTION_WORKER_HPP

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#elifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#pragma GCC diagnostic ignored "-Wsuggest-final-types"
#pragma GCC diagnostic ignored "-Wsuggest-final-methods"
#endif

#include <atomic>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#ifdef __clang__
#pragma clang diagnostic pop
#elifdef __GNUC__
#pragma GCC diagnostic pop
#endif

// TransactionWorker: a dedicated QObject that runs pacman install/remove
// transactions in its own QThread (via moveToThread). This is the proper
// Qt pattern for long-running background operations:
//
//   - The worker is created on the main thread, then moved to a QThread.
//   - All long-running work (alpm handle creation, kernel fetch, pacman
//     execution) happens in the worker thread.
//   - Communication with the main thread is ONLY via signals/slots.
//     The worker NEVER touches Qt widgets or the main thread's alpm handle.
//   - The worker creates and releases its OWN alpm handle for each
//     transaction, eliminating cross-thread handle sharing (the root
//     cause of the use-after-free crashes on window close).
//   - Clean shutdown: requestStop() sets a flag; the worker checks it
//     before starting any transaction. The main thread calls
//     QThread::quit() + wait() to join the worker before releasing
//     its own resources.
//
// Threading invariants:
//   - Only the main thread touches Qt widgets.
//   - Only the worker thread touches its own alpm handle.
//   - The main thread's alpm handle is never shared with the worker.
//   - Global kernel install/remove lists are only written by the worker.
//   - All cross-thread data transfer is via value types (QStringList, bool,
//     QString).
class TransactionWorker final : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TransactionWorker)

  public:
    explicit TransactionWorker(QObject* parent = nullptr);
    ~TransactionWorker() override;

    // Called from the main thread to start a kernel install/remove
    // transaction. The changeList contains the raw kernel names
    // ("repo/name") that the user has checked (to install) or unchecked
    // (if immutable, to remove) in the kernel tree.
    //
    // This is Q_INVOKABLE, so it can be called directly from the main
    // thread; since the worker lives in a different thread, Qt
    // automatically queues the call (auto-connection semantics).
    Q_INVOKABLE void startTransaction(const QStringList& changeList);

    // Called from the main thread to request a clean shutdown. Sets the
    // m_stopping flag; the worker checks it before starting any new
    // transaction and after finishing the current one. A transaction
    // already in flight (pacman running) is allowed to complete — pacman
    // transactions are atomic and should not be killed mid-run.
    Q_INVOKABLE void requestStop();

  signals:
    // Emitted when the worker is about to start processing a transaction.
    // The main thread uses this to disable the Execute button and show
    // a "working" state.
    void transactionStarted();

    // Emitted when the worker has finished processing a transaction.
    // `success` is true if the pacman command(s) ran without a fatal
    // error (the global install/remove lists were populated).
    // `kernelsChanged` is true if the kernel install state actually
    // changed (the main thread should then re-fetch the kernel list).
    void transactionFinished(bool success, bool kernelsChanged);

    // Emitted when the worker encounters an error (e.g., alpm handle
    // creation failed). The main thread shows this to the user via a
    // QMessageBox — the worker itself never creates dialogs.
    void error(const QString& message);

  private:
    // Whether the worker has been asked to stop. Checked before starting
    // any new transaction. Once set, the worker will not start new work
    // but will finish the current transaction if one is in flight.
    std::atomic<bool> m_stopping{false};

    // The pending change list (copy of the main thread's m_change_list).
    // Only accessed from the worker thread (set by startTransaction,
    // consumed by processTransaction).
    QStringList m_pendingChangeList;

    // Whether a transaction is currently in flight. Prevents overlapping
    // transactions if startTransaction is called while one is running
    // (defensive; the main thread disables the Execute button).
    std::atomic<bool> m_in_flight{false};
};

#endif  // TRANSACTION_WORKER_HPP
