##
## PROJECT
## name and version
##
project(scx-manager
        VERSION 1.14.0
        LANGUAGES CXX)


##
## INCLUDE
##
include(CMakePackageConfigHelpers)

find_package(Qt6 COMPONENTS Widgets REQUIRED)

##
## CONFIGURATION
##
#set(CMAKE_AUTOUIC ON)
#set(CMAKE_AUTOMOC ON)
#set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Files with translation sources. You should run lupdate manually when needed to regenerate them
#file(GLOB SCXCTLUI_TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/lang/*.ts")
#file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lang")

#set_source_files_properties(${SCXCTLUI_TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/lang") # set qm files output directory
#qt_add_translation(SCXCTLUI_QM_FILES ${SCXCTLUI_TS_FILES})   # generate qm files

#add_custom_command(
#    DEPENDS "${SCXCTLUI_QM_FILES}"
#    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/scx_manager_locale.qrc"
#    COMMAND cp "${CMAKE_CURRENT_SOURCE_DIR}/scx_manager_locale.qrc" "${CMAKE_CURRENT_BINARY_DIR}"
#    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
#)

# Prevent shared library to export unintended symbols
if(NOT DEFINED CMAKE_CXX_VISIBILITY_PRESET AND NOT DEFINED CMAKE_VISIBILITY_INLINES_HIDDEN)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
endif()
set(SCXCTLUI_INCLUDE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/")

##
## Target
##
add_library(scxctl-ui SHARED
    src/scx_utils.hpp src/scx_utils.cpp
    src/schedext-window-internal.hpp src/schedext-window-internal.cpp
    include/schedext-window.hpp src/schedext-window.cpp
    src/schedext-window.ui
    #"${CMAKE_CURRENT_BINARY_DIR}/scx_manager_locale.qrc"
)
add_library(scxctl::scxctl-ui ALIAS scxctl-ui)
target_compile_definitions(scxctl-ui PRIVATE SCHEDEXT_LIB)

set_target_properties(${SCXCTLUI_TARGET_NAME} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(${SCXCTLUI_TARGET_NAME}
    PUBLIC
    $<BUILD_INTERFACE:${SCXCTLUI_INCLUDE_BUILD_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

#corrosion_import_crate(MANIFEST_PATH "${CMAKE_SOURCE_DIR}/config-option-lib/Cargo.toml" FLAGS "${CARGO_FLAGS}")
corrosion_add_cxxbridge(scx-lib-cxxbridge CRATE config_option_lib FILES scx_loader_config.rs)

target_link_libraries(scxctl-ui PRIVATE project_warnings project_options Qt6::Widgets fmt::fmt-header-only scx-lib-cxxbridge)

##
## INSTALL
## install header files, generate and install cmake config files for find_package()
##
# use a custom package version config file instead of
# write_basic_package_version_file to ensure that it's architecture-independent
# https://github.com/nlohmann/json/issues/1697
configure_file(
    "cmake/scxctl-ui-config.cmake.in"
    ${SCXCTLUI_CMAKE_VERSION_CONFIG_FILE}
    @ONLY
)
configure_file(
    ${SCXCTLUI_CMAKE_CONFIG_TEMPLATE}
    ${SCXCTLUI_CMAKE_PROJECT_CONFIG_FILE}
    @ONLY
)

#write_basic_package_version_file(
#    ${SCXCTLUI_CMAKE_PROJECT_CONFIG_FILE}
#    VERSION ${PROJECT_VERSION}
#    COMPATIBILITY SameMajorVersion
#)

install(
    DIRECTORY ${SCXCTLUI_INCLUDE_BUILD_DIR}
    DESTINATION ${SCXCTLUI_INCLUDE_INSTALL_DIR}
)
install(
    FILES ${SCXCTLUI_CMAKE_PROJECT_CONFIG_FILE} ${SCXCTLUI_CMAKE_VERSION_CONFIG_FILE}
    DESTINATION ${SCXCTLUI_CONFIG_INSTALL_DIR}
)

export(
    TARGETS ${SCXCTLUI_TARGET_NAME}
    NAMESPACE scxctl::
    FILE ${SCXCTLUI_CMAKE_PROJECT_TARGETS_FILE}
)
install(
    TARGETS ${SCXCTLUI_TARGET_NAME}
    EXPORT ${SCXCTLUI_TARGETS_EXPORT_NAME}
    INCLUDES DESTINATION ${SCXCTLUI_INCLUDE_INSTALL_DIR}
)
install(
    EXPORT ${SCXCTLUI_TARGETS_EXPORT_NAME}
    NAMESPACE scxctl::
    DESTINATION "${SCXCTLUI_CONFIG_INSTALL_DIR}"
)
