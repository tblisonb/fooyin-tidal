cmake_minimum_required(VERSION 3.22)

project(
    fooyin-tidal
    VERSION 0.1.0
    DESCRIPTION "TIDAL for fooyin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fooyin's development files, built for the installed Flatpak by scripts/build-fooyin.sh.
find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network Widgets)

add_subdirectory(src)
add_subdirectory(tools)
