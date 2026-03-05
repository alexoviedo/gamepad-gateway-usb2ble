cmake_minimum_required(VERSION 3.16)

# Prevent Ninja/CMake regeneration loops when something keeps touching inputs
set(CMAKE_SUPPRESS_REGENERATION ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hotas_ble)