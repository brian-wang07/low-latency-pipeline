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

# ─────────────────────────────────────────────────────────────────────────
# GUI toolchain for the dashboard process: Dear ImGui + ImPlot + GLFW/OpenGL.
# ImGui/ImPlot ship as plain sources (no CMake target), so we fetch and build
# them into static libs ourselves. JetBrainsMono supplies the UI font.
# ─────────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

FetchContent_Declare(
    jetbrains_mono
    GIT_REPOSITORY https://github.com/JetBrains/JetBrainsMono.git
    GIT_TAG        v2.304
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(jetbrains_mono)

find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)
target_compile_options(imgui_lib PRIVATE -w) # vendored: drop -Wall/-Wextra noise

add_library(implot_lib STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot_lib PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot_lib PUBLIC imgui_lib)
target_compile_options(implot_lib PRIVATE -w)

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

add_executable(test_level_bitmap
    tests/test_level_bitmap.cpp
)
target_include_directories(test_level_bitmap PRIVATE src)

# ctest runs from build/ so feed paths like ../itch_feed resolve. Tests labelled
# "slow" replay the ITCH feed; `ctest -LE slow` is the quick loop.
add_executable(test_spsc_ring tests/test_spsc_ring.cpp)
target_include_directories(test_spsc_ring PRIVATE src)
target_link_libraries(test_spsc_ring PRIVATE Threads::Threads)

add_executable(test_market_view tests/test_market_view.cpp)
target_include_directories(test_market_view PRIVATE src)

add_executable(test_wire_layout tests/test_wire_layout.cpp)
target_include_directories(test_wire_layout PRIVATE src)

add_executable(test_exec_replay
    tests/test_exec_replay.cpp
    src/exchange/itch/itch_parser.cpp
    src/core/core.cpp
)
target_include_directories(test_exec_replay PRIVATE src)
target_link_libraries(test_exec_replay PRIVATE Threads::Threads)

enable_testing()
foreach(t test_orderbook test_level_bitmap test_spsc_ring test_market_view
          test_wire_layout)
    add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
foreach(t test_itch_parser test_exec_replay)
    add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(${t} PROPERTIES LABELS slow)
endforeach()

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

# One exec binary per strategy, each with its strategy statically inlined (zero
# hot-path overhead). Select which to launch at runtime via EXEC_STRATEGY (see the
# manager). Format per entry: <name>:<type>:<header>.
set(EXEC_STRATEGIES
    "logging:LoggingStrategy:exec/strategies/logging_strategy.hpp"
    "dummy:DummyQuoter:exec/strategies/dummy_quoter.hpp"
)
foreach(spec ${EXEC_STRATEGIES})
    string(REPLACE ":" ";" parts "${spec}")
    list(GET parts 0 strat_name)
    list(GET parts 1 strat_type)
    list(GET parts 2 strat_header)
    add_executable(exec_main_${strat_name}
        src/exec/exec_main.cpp
        src/common/ipc/shm_segment.cpp
    )
    target_include_directories(exec_main_${strat_name} PRIVATE src)
    target_link_libraries(exec_main_${strat_name} PRIVATE Threads::Threads)
    target_compile_definitions(exec_main_${strat_name} PRIVATE
        EXEC_STRATEGY_TYPE=${strat_type}
        EXEC_STRATEGY_HEADER="${strat_header}"
    )
endforeach()

add_executable(dashboard
    src/dashboard/dashboard_main.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(dashboard PRIVATE src)
target_link_libraries(dashboard PRIVATE Threads::Threads imgui_lib implot_lib)
target_compile_definitions(dashboard PRIVATE
    JETBRAINS_MONO_FONT="${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf"
)
