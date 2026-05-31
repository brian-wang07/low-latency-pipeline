cmake_minimum_required(VERSION 3.20)
project(LowLatencyPipeline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LL_RELEASE_FLAGS
    -O3
    -g1
    -march=native
    -mtune=native
    -fno-exceptions
    -fno-rtti
    -fno-plt
    -fno-semantic-interposition
    -falign-functions=64
    -falign-loops=32
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-math-errno
)

string(REPLACE ";" " " LL_RELEASE_FLAGS_STR "${LL_RELEASE_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${LL_RELEASE_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${LL_RELEASE_FLAGS_STR} -g3" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3" CACHE STRING "" FORCE)

include(CheckIPOSupported)
check_ipo_supported(RESULT LL_IPO_OK OUTPUT LL_IPO_MSG)
if(LL_IPO_OK)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
  message(STATUS "IPO/LTO not supported: ${LL_IPO_MSG}")
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

add_executable(test_itch_parser
    tests/test_itch_parser.cpp
    src/exchange/itch/itch_parser.cpp
)
target_include_directories(test_itch_parser PRIVATE src)
target_link_libraries(test_itch_parser PRIVATE Threads::Threads)

add_executable(test_orderbook
    tests/test_orderbook.cpp
    src/core/core.cpp
)
target_include_directories(test_orderbook PRIVATE src)

add_executable(parse_to_orderbook
    tests/parse_to_orderbook.cpp
    src/exchange/itch/itch_parser.cpp
    src/core/core.cpp
)
target_include_directories(parse_to_orderbook PRIVATE src)
target_link_libraries(parse_to_orderbook PRIVATE Threads::Threads)

add_executable(manager
    src/manager/manager.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(manager PRIVATE src)
target_link_libraries(manager PRIVATE Threads::Threads)

add_executable(exchange_main
    src/exchange/exchange_main.cpp
    src/exchange/itch/itch_parser.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(exchange_main PRIVATE src)
target_link_libraries(exchange_main PRIVATE Threads::Threads)

add_executable(core_main
    src/core/core_main.cpp
    src/core/core.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(core_main PRIVATE src)
target_link_libraries(core_main PRIVATE Threads::Threads)
