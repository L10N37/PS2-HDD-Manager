cmake_minimum_required(VERSION 3.21)

project(PS2HDDManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(ps2hddcore STATIC
    src/core/FhdbConfig.cpp
    src/core/FhdbConfig.h
    src/core/PhysicalDisk.cpp
    src/core/PhysicalDisk.h
    src/core/OplConfig.cpp
    src/core/OplConfig.h
    src/core/Ps2Apa.cpp
    src/core/Ps2Apa.h
    src/core/Ps2ApaBank.cpp
    src/core/Ps2ApaBank.h
    src/core/Ps2HddLayout.cpp
    src/core/Ps2HddLayout.h
    src/core/Ps2HddFormat.cpp
    src/core/Ps2HddFormat.h
)
target_include_directories(ps2hddcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(ps2hddcore PRIVATE /W4 /permissive-)
else()
    target_compile_options(ps2hddcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UNIX AND NOT APPLE)
    add_executable(ps2-hdd-writer src/formatter_main.cpp)
    target_link_libraries(ps2-hdd-writer PRIVATE ps2hddcore)
    set_target_properties(ps2-hdd-writer PROPERTIES OUTPUT_NAME "PS2-HDD-Writer")
    install(TARGETS ps2-hdd-writer RUNTIME DESTINATION .)
endif()

if(BUILD_TESTING)
    add_executable(ps2_core_tests tests/ps2_core_tests.cpp)
    target_link_libraries(ps2_core_tests PRIVATE ps2hddcore)
    add_test(NAME ps2_core_tests COMMAND ps2_core_tests)
endif()

option(PS2_HDD_BUILD_GUI "Build the Qt 6 desktop application" ON)
if(PS2_HDD_BUILD_GUI)
    find_package(Qt6 6.5 COMPONENTS Widgets Network QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        set(PS2_HDD_GUI_SOURCES
            src/main.cpp
            src/ui/FhdbConfigDialog.cpp
            src/ui/FhdbConfigDialog.h
            src/ui/MainWindow.cpp
            src/ui/MainWindow.h
            src/ui/Ps2HddSetupDialog.cpp
            src/ui/Ps2HddSetupDialog.h
            src/ui/PrivilegedSession.cpp
            src/ui/PrivilegedSession.h
            src/ui/ArtworkProvider.cpp
            src/ui/ArtworkProvider.h
        )
        if(WIN32)
            set(PS2_HDD_MANIFEST_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/ps2-hdd-manager.manifest")
            configure_file(resources/ps2-hdd-manager.rc.in
                ${CMAKE_CURRENT_BINARY_DIR}/ps2-hdd-manager.rc @ONLY)
            list(APPEND PS2_HDD_GUI_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/ps2-hdd-manager.rc)
        endif()
        qt_add_executable(ps2-hdd-manager WIN32 ${PS2_HDD_GUI_SOURCES})
        target_link_libraries(ps2-hdd-manager PRIVATE ps2hddcore Qt6::Widgets Qt6::Network)
        target_compile_definitions(ps2-hdd-manager PRIVATE
            PS2_HDD_APP_VERSION="${PROJECT_VERSION}-alpha"
            PS2_HDD_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
        set_target_properties(ps2-hdd-manager PROPERTIES
            OUTPUT_NAME "PS2-HDD-Manager"
        )
        install(TARGETS ps2-hdd-manager RUNTIME DESTINATION . BUNDLE DESTINATION .)
        if(UNIX AND NOT APPLE)
            include(GNUInstallDirs)
            install(FILES resources/ps2-hdd-manager.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        endif()
    else()
        message(STATUS "Qt 6.5+ was not found; building the PS2 core and tests only.")
    endif()
endif()
