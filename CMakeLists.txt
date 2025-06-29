cmake_minimum_required(VERSION 3.10)
project(viture_ar_desktop VERSION 0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# Include headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Find system libraries
find_package(OpenGL REQUIRED)

# Add executable sources
file(GLOB SOURCES
    src/*.cpp
    src/*.c
)

add_executable(${PROJECT_NAME} ${SOURCES})

# VITURE SDK shared object (assumes prebuilt .so is in libs/)
add_library(viture_sdk SHARED IMPORTED)
set_target_properties(viture_sdk PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/libviture_one_sdk.so
)

# Link system and SDK libraries
target_link_libraries(${PROJECT_NAME}
    viture_sdk
    ${OPENGL_gl_LIBRARY}
    GLU
    X11
    Xrandr
    Xext
    Xfixes
    m
    rt
)

# Optional: set C++17 (or C++20) if needed
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

