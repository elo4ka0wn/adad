cmake_minimum_required(VERSION 3.16)
project(mouse_direction_binder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 COMPONENTS Widgets REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/inputcontroller.cpp
)

set(HEADERS
    src/mainwindow.h
    src/inputcontroller.h
)

qt_add_executable(mouse_direction_binder
    ${SOURCES}
    ${HEADERS}
)

target_link_libraries(mouse_direction_binder PRIVATE
    Qt6::Widgets
    PkgConfig::LIBINPUT
    PkgConfig::LIBUDEV
)

if (WIN32)
    message(FATAL_ERROR "This project is intended for Linux/Wayland environments only.")
endif()
