cmake_minimum_required(VERSION 3.20)
project(LowLatencyPipeline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_executable(test_itch_parser
    tests/test_itch_parser.cpp
    src/exchange/itch/itch_parser.cpp
)
target_include_directories(test_itch_parser PRIVATE src)
target_compile_options(test_itch_parser PRIVATE -O2)
target_link_libraries(test_itch_parser PRIVATE Threads::Threads)

add_executable(test_orderbook
    tests/test_orderbook.cpp
    src/core/core.cpp
)
target_include_directories(test_orderbook PRIVATE src)
target_compile_options(test_orderbook PRIVATE -O2)

add_executable(parse_to_orderbook
    tests/parse_to_orderbook.cpp
    src/exchange/itch/itch_parser.cpp
    src/core/core.cpp
)
target_include_directories(parse_to_orderbook PRIVATE src)
target_compile_options(parse_to_orderbook PRIVATE -O2)
target_link_libraries(parse_to_orderbook PRIVATE Threads::Threads)

add_executable(manager
    src/manager/manager.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(manager PRIVATE src)
target_compile_options(manager PRIVATE -O2)
target_link_libraries(manager PRIVATE Threads::Threads)

add_executable(exchange_main
    src/exchange/exchange_main.cpp
    src/exchange/itch/itch_parser.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(exchange_main PRIVATE src)
target_compile_options(exchange_main PRIVATE -O2)
target_link_libraries(exchange_main PRIVATE Threads::Threads)

add_executable(core_main
    src/core/core_main.cpp
    src/core/core.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(core_main PRIVATE src)
target_compile_options(core_main PRIVATE -O2)
target_link_libraries(core_main PRIVATE Threads::Threads)
