# CMakeLists.txt -- top-level build for epicaDB.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
#   ctest --test-dir build
#
# Options:
#   EPICA_BUILD_TESTS  (ON)   build the GoogleTest suite (fetched automatically)
#   EPICA_SANITIZE     (OFF)  AddressSanitizer + UndefinedBehaviorSanitizer
#
# The Java query layer under server/ is built separately with Maven.
cmake_minimum_required(VERSION 3.20)
project(epicaDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(AppleLibcxxFallback)

option(EPICA_BUILD_TESTS "Build unit tests" ON)
option(EPICA_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

if(EPICA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(EPICA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(engine)
