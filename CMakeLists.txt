cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

##
## PROJECT
## name and version
##
project(kernel-manager
        VERSION 1.19.0
        LANGUAGES CXX)


##
## INCLUDE
##
include(GNUInstallDirs)
include(StandardProjectSettings)
include(CheckSymbolExists)
include(CompilerWarnings)
include(EnableCcache)
include(Linker)
include(StaticAnalyzers)
include(Sanitizers)
include(CPM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Qt6 COMPONENTS Widgets LinguistTools Concurrent REQUIRED)
pkg_check_modules(
  LIBALPM
  REQUIRED
  IMPORTED_TARGET
  libalpm>=13.0.0)
pkg_check_modules(
  LIBGLIB
  REQUIRED
  IMPORTED_TARGET
  glib-2.0>=2.72.1)

CPMAddPackage(
  NAME fmt
  GITHUB_REPOSITORY fmtlib/fmt
  GIT_TAG 12.2.0
  EXCLUDE_FROM_ALL YES
)
CPMAddPackage(
  NAME frozen
  GITHUB_REPOSITORY serge-sans-paille/frozen
  GIT_TAG b07a5c8187bc3e6f1461efab89fe07f467e300db
  EXCLUDE_FROM_ALL YES
)
CPMAddPackage(
  NAME Corrosion
  GITHUB_REPOSITORY corrosion-rs/corrosion
  GIT_TAG v0.6.1
  EXCLUDE_FROM_ALL YES
)

find_package(PolkitQt6-1 REQUIRED)

# sched-ext (scx-manager) support is OPTIONAL (WU-5): the default build for
# generic Arch must configure/build/link without it.
#
#   default (no -D):           auto — scxctl-ui is probed silently;
#                              present -> WITH_SCX_MANAGER=ON, absent -> OFF
#   -DWITH_SCX_MANAGER=ON/OFF: explicit user choice always wins
#                              (ON requires the scx-manager package to be
#                              installed; a missing package is a hard error)
find_package(scxctl-ui 1 QUIET)
if(NOT DEFINED WITH_SCX_MANAGER)
    set(WITH_SCX_MANAGER ${scxctl-ui_FOUND})
else()
    if(WITH_SCX_MANAGER AND NOT scxctl-ui_FOUND)
        message(FATAL_ERROR "WITH_SCX_MANAGER=ON requires scx-manager (scxctl-ui) but it was not found; install scx-manager or set WITH_SCX_MANAGER=OFF")
    endif()
endif()

##
## CONFIGURATION
##
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
   set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=thin -fwhole-program-vtables")
   set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -flto=thin -fwhole-program-vtables")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto -fwhole-program -fuse-linker-plugin")
endif()

list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "-lalpm")
check_symbol_exists(alpm_pkg_get_installed_db "alpm.h" HAVE_ALPM_INSTALLED_DB)
if(HAVE_ALPM_INSTALLED_DB)
    add_compile_options(-DHAVE_ALPM_INSTALLED_DB)
endif()

# Link this 'library' to set the c++ standard / compile-time options requested
add_library(project_options INTERFACE)
target_compile_features(project_options INTERFACE cxx_std_23)

# Files with translation sources. You should run lupdate manually when needed to regenerate them
file(GLOB TS_FILES "${PROJECT_SOURCE_DIR}/lang/*.ts")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lang")

set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/lang") # set qm files output directory
qt_add_translation(QM_FILES ${TS_FILES})   # generate qm files

add_custom_command(
    DEPENDS "${QM_FILES}"
    OUTPUT "${CMAKE_BINARY_DIR}/km_locale.qrc"
    COMMAND cp "${CMAKE_SOURCE_DIR}/km_locale.qrc" "${CMAKE_CURRENT_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

##
## Target
##
add_custom_command(
    DEPENDS "${CMAKE_SOURCE_DIR}/src/mkoptions.py" "${CMAKE_SOURCE_DIR}/src/compile_options.json"
    OUTPUT "${CMAKE_BINARY_DIR}/compile_options.hpp"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/src/mkoptions.py" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
qt_add_executable(${PROJECT_NAME}
    src/ini.hpp
    src/string_utils.hpp
    src/alpm_utils.hpp src/alpm_utils.cpp
    src/utils.hpp src/utils.cpp
    src/kernel.hpp src/kernel.cpp
    src/aur_kernel.hpp src/aur_kernel.cpp
    src/known_kernels.hpp src/known_kernels.cpp
    src/bootloader.hpp src/bootloader.cpp
    src/boot_instructions.hpp src/boot_instructions.cpp
    src/km-window.hpp src/km-window.cpp
    "${CMAKE_BINARY_DIR}/compile_options.hpp"
    src/config-options.hpp src/config-options.cpp
    src/conf-window.hpp src/conf-window.cpp
    src/conf-patches-page.hpp src/conf-patches-page.ui
    src/conf-options-page.hpp src/conf-options-page.ui
    src/km-window.ui src/conf-window.ui
    src/main.cpp "${CMAKE_BINARY_DIR}/km_locale.qrc" src/km_icons.qrc
    )

##
## Polkit privilege layer (WU-2)
##
# Single install prefix for the pkexec helpers (terminal-helper, rootshell.sh).
# Exposed to C++ as KM_HELPER_DIR so no hard-coded path literals remain in src/.
# Keep in sync with the exec.path annotation in org.archlinux.kernel-manager.pkexec.policy.
set(KM_HELPER_DIR "/usr/lib/kernel-manager")
target_compile_definitions(${PROJECT_NAME} PRIVATE KM_HELPER_DIR="${KM_HELPER_DIR}")

# Sync repo (pacman.conf section) excluded from kernel discovery.
# Build-configurable: -DKM_IGNORE_REPO=<repo>; default empty = no exclusion.
set(KM_IGNORE_REPO "" CACHE STRING "pacman.conf sync repo to ignore during kernel discovery")
target_compile_definitions(${PROJECT_NAME} PRIVATE KM_IGNORE_REPO="${KM_IGNORE_REPO}")

# sched-ext feature gate (WU-5): only defined when scx-manager is available,
# so the scxctl::SchedExtWindow member + handlers compile out cleanly otherwise.
if(WITH_SCX_MANAGER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WITH_SCX_MANAGER=1)
endif()

# Link this 'library' to use the warnings specified in CompilerWarnings.cmake
add_library(project_warnings INTERFACE)
set_project_warnings(project_warnings)

# Add linker configuration
configure_linker(project_options)

# sanitizer options if supported by compiler
enable_sanitizers(project_options)

include_directories(${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})

corrosion_import_crate(MANIFEST_PATH "config-option-lib/Cargo.toml" FLAGS "${CARGO_FLAGS}")
corrosion_add_cxxbridge(config-option-lib-cxxbridge CRATE config_option_lib FILES lib.rs)

target_link_libraries(${PROJECT_NAME} PRIVATE project_warnings project_options Qt6::Widgets Qt6::Concurrent Threads::Threads fmt::fmt frozen::frozen config-option-lib-cxxbridge PkgConfig::LIBALPM PkgConfig::LIBGLIB)

# Link scx-manager's UI library only when the optional feature is enabled (WU-5);
# a generic-Arch build (WITH_SCX_MANAGER=OFF) must link without it.
if(WITH_SCX_MANAGER)
    target_link_libraries(${PROJECT_NAME} PRIVATE scxctl::scxctl-ui)
endif()

option(ENABLE_UNITY "Enable Unity builds of projects" OFF)
if(ENABLE_UNITY)
   # Add for any project you want to apply unity builds for
   set_target_properties(${PROJECT_NAME} PROPERTIES UNITY_BUILD ON)
endif()

install(
   TARGETS ${PROJECT_NAME}
   RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    PROGRAMS ${CMAKE_SOURCE_DIR}/src/terminal-helper
    DESTINATION ${KM_HELPER_DIR}
)

install(
    PROGRAMS ${CMAKE_SOURCE_DIR}/src/rootshell.sh
    DESTINATION ${KM_HELPER_DIR}
)

install(
    FILES org.archlinux.kernel-manager.pkexec.policy
   DESTINATION "${POLKITQT-1_POLICY_FILES_INSTALL_DIR}"
)

install(
    FILES org.archlinux.KernelManager.desktop
   DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/16x16/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/22x22/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/22x22/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/32x32/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/44x44/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/44x44/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/48x48/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/64x64/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/128x128/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/150x150/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/150x150/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/256x256/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
)
install(
    FILES ${CMAKE_SOURCE_DIR}/icons/310x310/org.archlinux.KernelManager.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/310x310/apps
)
