cmake_minimum_required(VERSION 3.20)
project(ForgeFS VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()

option(FORGEFS_BUILD_TESTS "Build ForgeFS unit/integration tests" ON)
option(FORGEFS_BUILD_BENCHMARKS "Build ForgeFS benchmarking tool" ON)

add_subdirectory(common)
add_subdirectory(network)
add_subdirectory(protocol)
add_subdirectory(crypto)
add_subdirectory(database)
add_subdirectory(storage)
add_subdirectory(server)
add_subdirectory(client)

if(FORGEFS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FORGEFS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
