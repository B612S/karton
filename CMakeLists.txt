# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
# SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

cmake_minimum_required(VERSION 3.20)
project(karton VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KARTON_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(KARTON_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

option(KARTON_CLANG_TIDY "Run clang-tidy as part of the build" OFF)
if(KARTON_CLANG_TIDY)
    find_program(KARTON_CLANG_TIDY_EXE NAMES clang-tidy-wrapper
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/scripts
        NO_DEFAULT_PATH)
    if(NOT KARTON_CLANG_TIDY_EXE)
        find_program(KARTON_CLANG_TIDY_EXE NAMES clang-tidy)
    endif()
    if(NOT KARTON_CLANG_TIDY_EXE)
        message(FATAL_ERROR "KARTON_CLANG_TIDY is enabled but clang-tidy was not found")
    endif()
    set(CMAKE_CXX_CLANG_TIDY
        "${KARTON_CLANG_TIDY_EXE};--header-filter=^(core|main\.cpp);--quiet"
    )
endif()

option(KARTON_CPPCHECK "Run cppcheck as part of the build" OFF)
if(KARTON_CPPCHECK)
    find_program(KARTON_CPPCHECK_EXE NAMES cppcheck)
    if(NOT KARTON_CPPCHECK_EXE)
        message(FATAL_ERROR "KARTON_CPPCHECK is enabled but cppcheck was not found")
    endif()
    set(CMAKE_CXX_CPPCHECK
        "${KARTON_CPPCHECK_EXE};--enable=warning,performance,portability;--inline-suppr;--suppress=missingIncludeSystem;--quiet"
    )
endif()

find_package(ECM 6.0.0 REQUIRED NO_MODULE)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMFindQmlModule)
include(ECMAddAppIcon)
include(ECMQmlModule)
include(ECMQtDeclareLoggingCategory)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMSetupVersion)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Quick
    Gui
    GuiPrivate
    QuickControls2
    Widgets
    Xml
    Multimedia
    Network
    DBus
    LinguistTools
)

find_package(Libvirt REQUIRED)
find_package(Libosinfo REQUIRED)
find_package(Glib REQUIRED)
find_package(Spiceglib REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PHODAV REQUIRED IMPORTED_TARGET libphodav-3.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
set(KEYBOARD_SHORTCUTS_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml")
set(KEYBOARD_SHORTCUTS_HEADER "${CMAKE_BINARY_DIR}/keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h")
set(KEYBOARD_SHORTCUTS_CODE "${CMAKE_BINARY_DIR}/keyboard-shortcuts-inhibit-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT ${KEYBOARD_SHORTCUTS_HEADER} ${KEYBOARD_SHORTCUTS_CODE}
    COMMAND ${WAYLAND_SCANNER} client-header ${KEYBOARD_SHORTCUTS_XML} ${KEYBOARD_SHORTCUTS_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${KEYBOARD_SHORTCUTS_XML} ${KEYBOARD_SHORTCUTS_CODE}
    DEPENDS ${KEYBOARD_SHORTCUTS_XML}
    VERBATIM
)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)

find_package(KF6 REQUIRED COMPONENTS
    Kirigami
    CoreAddons
    QQC2DesktopStyle
    IconThemes
)

find_package(KF6KirigamiAddons 1.11.0 REQUIRED)

install(FILES
    img/karton_logo.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    RENAME org.kde.karton.svg
)
install(FILES
    org.kde.karton.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
install(DIRECTORY img/os/
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    FILES_MATCHING PATTERN "*.svg"
)

ecm_find_qmlmodule(org.kde.kirigami REQUIRED)


ecm_setup_version(${CMAKE_PROJECT_VERSION}
    VARIABLE_PREFIX KARTON
    VERSION_HEADER core/karton_version.h)

add_executable(karton)

ecm_add_qml_module(karton
    GENERATE_PLUGIN_SOURCE
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/core/org/kde/karton
    URI org.kde.karton
)

target_include_directories(karton PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/app
    ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/core/libvirt
    ${CMAKE_CURRENT_SOURCE_DIR}/core/viewer
    ${CMAKE_CURRENT_SOURCE_DIR}/core/os
    ${CMAKE_CURRENT_SOURCE_DIR}/core/util
    ${CMAKE_CURRENT_BINARY_DIR}/core
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(karton
    PRIVATE
    main.cpp
    core/app/karton.cpp
    core/app/karton_autostart.cpp
    core/app/karton_private.h
    core/app/libvirtconnection.cpp
    core/app/libvirtconnection.h
    core/app/karton_devices.cpp
    core/app/karton_disk.cpp
    core/app/karton_domain.cpp
    core/app/karton_import.cpp
    core/app/karton_storage.cpp
    core/app/karton_ovf.cpp
    core/app/ovfextract.cpp
    core/app/karton_network.cpp
    core/app/karton_os.cpp
    core/app/karton_pci.cpp
    core/app/karton_portforward.cpp
    core/app/karton_settings.cpp
    core/app/karton_snapshots.cpp
    core/app/karton_usb.cpp
    core/app/shortcutinhibitor.cpp
    ${KEYBOARD_SHORTCUTS_HEADER}
    ${KEYBOARD_SHORTCUTS_CODE}
    core/app/vmlistmodel.cpp
    core/domain/domain.cpp
    core/domain/domainconfig.cpp
    core/libvirt/libvirtmonitor.cpp
    core/libvirt/libvirteventloop.cpp
    core/libvirt/libvirtreader.cpp
    core/viewer/domainviewer.cpp
    core/viewer/clipboardmanager.cpp
    core/viewer/pipewireaudio.cpp
    core/viewer/spicedisplayrenderer.h
    core/viewer/glscanoutrenderer.cpp
    core/viewer/primarysurfacerenderer.cpp
    core/viewer/terminalview.cpp
    core/viewer/terminalview.h
    core/viewer/previewitem.cpp
    core/viewer/previewitem.h
    core/domain/domainxmlbuilder.cpp
    core/os/osiconmapper.cpp
    core/os/osinfoconfig.cpp
    core/domain/domainxmlreader.cpp
    core/util/commandrunner.cpp
    core/util/portforwarder.cpp
    core/util/portforwarder.h
    core/util/xmlhighlighter.cpp
    core/util/xmlhighlighter.h
    core/util/evdev_to_xtkbd_map.c
    core/util/evdev_to_xtkbd_map.h
    core/util/devicepixelratiohelper.cpp
)

ecm_target_qml_sources(karton
    SOURCES
    qml/Main.qml
    qml/VMListPage.qml
    qml/EmptyDetailsPage.qml
    qml/NetworksPage.qml
    qml/StoragePoolsPage.qml
    qml/ConnectionSettingsDialog.qml
    qml/ImportDomainDialog.qml
    qml/VMViewerWindow.qml
    qml/QmlSmokeTest.qml
    qml/InstallationDialog.qml
    qml/VMPage.qml
    qml/UsageGraph.qml
    qml/KartonDialog.qml
    qml/ErrorLogDialog.qml
    qml/NetworkDhcpDialog.qml
    qml/OvfImportDialog.qml
    qml/OvfExportDialog.qml
    qml/AddHardwareDialog.qml
    qml/ApplicationSettingsDialog.qml
    qml/ScrollableSettingsView.qml
    qml/UsbDevicesDialog.qml
    qml/SettingsDialog.qml
    qml/SnapshotsDialog.qml
    qml/BusyOperationDialog.qml
    qml/CloneDialog.qml
    qml/DeleteDomainDialog.qml
    qml/StorageVolumePickerDialog.qml
)

qt_add_translations(karton
    TS_FILES
        locales/en.ts
        locales/ru.ts
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/en.qm
    ${CMAKE_CURRENT_BINARY_DIR}/ru.qm
    DESTINATION ${KDE_INSTALL_DATADIR}/karton/translations
)

ecm_qt_declare_logging_category(
    karton
    HEADER core/karton_debug.h
    IDENTIFIER KARTON_DEBUG
    CATEGORY_NAME org.kde.karton
    DESCRIPTION "Karton Virtual Machine Manager"
    EXPORT Karton
)

ecm_qt_install_logging_categories(
    EXPORT Karton
    FILE karton.categories
    DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)

target_link_libraries(karton
    PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Xml
    Qt6::Multimedia
    Qt6::Network
    Qt6::GuiPrivate
    Qt6::DBus
    PkgConfig::WAYLAND_CLIENT
    KF6::CoreAddons
    KF6::IconThemes
    KirigamiApp
    Libvirt::Libvirt
    Spiceglib::Spiceglib
    PkgConfig::PHODAV
    PkgConfig::PIPEWIRE
    Libosinfo::Libosinfo
    Glib::Glib
    OpenGL::EGL
    OpenGL::GL
)

install(TARGETS karton ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})


# Tests are opt-in and are never part of the application target or package.
option(KARTON_BUILD_TESTS "Build Karton autotests (Qt Test)" OFF)
if (KARTON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
