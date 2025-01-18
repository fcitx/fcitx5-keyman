cmake_minimum_required(VERSION 3.6)

project(fcitx5-keyman VERSION 1.1.1)

set(REQUIRED_FCITX_VERSION 5.1.12)
find_package(ECM REQUIRED 1.0.0)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
include(FeatureSummary)
include(GNUInstallDirs)
include(ECMUninstallTarget)

find_package(Gettext REQUIRED)
find_package(Fcitx5Core ${REQUIRED_FCITX_VERSION} REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Keyman REQUIRED IMPORTED_TARGET "keyman_core")
pkg_check_modules(JsonC REQUIRED IMPORTED_TARGET "json-c")

add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-keyman\")
fcitx5_add_i18n_definition()

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_subdirectory(po)
add_subdirectory(src)

fcitx5_translate_desktop_file(org.fcitx.Fcitx5.Addon.Keyman.metainfo.xml.in
                              org.fcitx.Fcitx5.Addon.Keyman.metainfo.xml XML)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Keyman.metainfo.xml" DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

