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
# Header-only unit tests: tests/<name>.cpp, nothing else to link.
set(LL_UNIT_TESTS
    test_spsc_ring
    test_market_view
    test_wire_layout
    test_kv_config
    test_account
    test_order_table
    test_rate_model
    test_risk_gate
)
foreach(t ${LL_UNIT_TESTS})
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE src)
    target_link_libraries(${t} PRIVATE Threads::Threads)
    target_compile_definitions(${t} PRIVATE LL_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
endforeach()

add_executable(test_exec_replay
    tests/test_exec_replay.cpp
    src/exchange/itch/itch_parser.cpp
    src/core/core.cpp
)
target_include_directories(test_exec_replay PRIVATE src)
target_link_libraries(test_exec_replay PRIVATE Threads::Threads)

enable_testing()
foreach(t test_orderbook test_level_bitmap ${LL_UNIT_TESTS})
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

# Identifies the build a strategy module came from (logged when it loads).
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LL_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT LL_BUILD_ID)
    set(LL_BUILD_ID "unknown")
endif()

# Strategies. Format per entry: <name>:<type>:<header>. Each produces:
#   exec_main_<name>     exec with <name> built in (builtin:<name>), and
#   strategy_<name>.so   the same run_strategy<Type, Router> as a loadable module.
# Both go through the module table (src/exec/module_abi.hpp), so the hot loop code
# is identical; the generic exec_main loads modules only.
set(EXEC_STRATEGIES
    "logging:LoggingStrategy:exec/strategies/logging_strategy.hpp"
    "dummy:DummyQuoter:exec/strategies/dummy_quoter.hpp"
    "imbalance_taker:ImbalanceTaker:exec/strategies/imbalance_taker.hpp"
)

function(ll_exec_target target)
    add_executable(${target}
        src/exec/exec_main.cpp
        src/exec/loader.cpp
        src/common/ipc/shm_segment.cpp
    )
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(${target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(${target} PRIVATE LL_BUILD_ID="${LL_BUILD_ID}")
endfunction()

# A strategy module: hidden visibility and -Bsymbolic keep every call inside it
# direct; the version script exports only ll_strategy_module.
function(ll_strategy_module target source)
    add_library(${target} MODULE ${source})
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE LL_BUILD_ID="${LL_BUILD_ID}")
    target_link_options(${target} PRIVATE
        -Wl,-Bsymbolic -Wl,-z,now
        -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/exec/strategies/export.map)
endfunction()

ll_exec_target(exec_main)
foreach(spec ${EXEC_STRATEGIES})
    string(REPLACE ":" ";" parts "${spec}")
    list(GET parts 0 strat_name)
    list(GET parts 1 strat_type)
    list(GET parts 2 strat_header)

    ll_exec_target(exec_main_${strat_name})
    target_compile_definitions(exec_main_${strat_name} PRIVATE
        EXEC_STRATEGY_TYPE=${strat_type}
        EXEC_STRATEGY_HEADER="${strat_header}"
        EXEC_STRATEGY_NAME="${strat_name}"
    )

    set(gen ${CMAKE_BINARY_DIR}/gen/strategy_${strat_name}.cpp)
    file(GENERATE OUTPUT ${gen} CONTENT
"#include \"exec/module_abi.hpp\"
#include \"${strat_header}\"
LL_STRATEGY_MODULE(exec::${strat_type}, \"${strat_name}\")
")
    ll_strategy_module(strategy_${strat_name} ${gen})
    add_custom_command(TARGET strategy_${strat_name} POST_BUILD
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_strategy_so.sh
                $<TARGET_FILE:strategy_${strat_name}>
        VERBATIM)
endforeach()

# Deliberately bad module (thread_local) that the loader must reject; not run
# through check_strategy_so.sh, which would fail the build on it.
ll_strategy_module(strategy_tls_fixture tests/fixtures/tls_module.cpp)

add_executable(test_module_abi tests/test_module_abi.cpp src/exec/loader.cpp)
target_include_directories(test_module_abi PRIVATE src)
target_link_libraries(test_module_abi PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(test_module_abi PRIVATE LL_BUILD_ID="${LL_BUILD_ID}")
add_dependencies(test_module_abi strategy_dummy strategy_logging strategy_tls_fixture)
add_test(NAME test_module_abi COMMAND test_module_abi WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(dashboard
    src/dashboard/dashboard_main.cpp
    src/dashboard/trading_panels.cpp
    src/common/ipc/shm_segment.cpp
)
target_include_directories(dashboard PRIVATE src)
target_link_libraries(dashboard PRIVATE Threads::Threads imgui_lib implot_lib)
target_compile_definitions(dashboard PRIVATE
    JETBRAINS_MONO_FONT="${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf"
)
