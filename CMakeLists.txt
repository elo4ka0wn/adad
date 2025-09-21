cmake_minimum_required(VERSION 3.16)
project(mouse_direction_binder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 COMPONENTS Widgets REQUIRED)
find_package(X11 REQUIRED)

if (NOT X11_XTest_FOUND)
    message(FATAL_ERROR "X11 XTest extension is required")
endif()
if (NOT X11_Xi_FOUND)
    message(FATAL_ERROR "X11 XInput2 extension is required")
endif()

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

target_include_directories(mouse_direction_binder PRIVATE
    ${X11_INCLUDE_DIR}
)

target_link_libraries(mouse_direction_binder PRIVATE
    Qt6::Widgets
    ${X11_LIBRARIES}
    ${X11_Xi_LIB}
    ${X11_XTest_LIB}
)

if (WIN32)
    message(FATAL_ERROR "This project is intended for Linux/X11 environments only.")
endif()
