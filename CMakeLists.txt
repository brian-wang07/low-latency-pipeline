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
