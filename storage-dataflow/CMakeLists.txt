cmake_minimum_required(VERSION 3.24)
project(storage_dataflow VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror)

# --- Common primitives -------------------------------------------------------
add_library(sdf_common STATIC
    src/c/common/ring.c
    src/c/common/pool.c
    src/c/common/seqlock.c
    src/c/common/timer.c
    src/c/common/shm.c
    src/c/common/metrics.c
    src/c/common/log.c
    src/c/common/config.c
)
target_include_directories(sdf_common PUBLIC include)

# --- Whack-a-Mole dispatch ---------------------------------------------------
add_library(sdf_wm STATIC
    src/c/whack_a_mole/wm_profile.c
    src/c/whack_a_mole/wm_select.c
    src/c/whack_a_mole/wm_shed.c
    src/c/whack_a_mole/wm_shm.c
    src/c/whack_a_mole/wm_proxy.c
)
target_link_libraries(sdf_wm PUBLIC sdf_common)
target_include_directories(sdf_wm PUBLIC include)

# --- Fountain codec (plan W3; ADR-0004 arms) --------------------------------
add_custom_target(nanorq_build
    COMMAND make -C ${CMAKE_SOURCE_DIR}/third_party/nanorq libnanorq.a)
add_library(sdf_fountain STATIC
    src/c/fountain/fe_enc.c
    src/c/fountain/fe_dec.c
    src/c/fountain/fe_codec_nanorq.c
    src/c/fountain/fe_codec_lt.c
    src/c/fountain/fe_prng.c
)
add_dependencies(sdf_fountain nanorq_build)
target_link_libraries(sdf_fountain PUBLIC sdf_common
    ${CMAKE_SOURCE_DIR}/third_party/nanorq/libnanorq.a m)
target_include_directories(sdf_fountain PUBLIC include
    PRIVATE src/c/fountain ${CMAKE_SOURCE_DIR}/third_party/nanorq/include)

# --- FLUID repair gate (plan W3.4) ------------------------------------------
add_library(sdf_fluid STATIC
    src/c/fluid/fluid_gate.c
)
target_link_libraries(sdf_fluid PUBLIC sdf_fountain sdf_common)
target_include_directories(sdf_fluid PUBLIC include)

# --- ARC pacer ---------------------------------------------------------------
add_library(sdf_arc STATIC
    src/c/arc/arc_heap.c
    src/c/arc/arc_offset.c
    src/c/arc/arc_pacer.c
)
target_link_libraries(sdf_arc PUBLIC sdf_common)
target_include_directories(sdf_arc PUBLIC include)

# --- Apps (plan W2.5: the P1 proxy shell) ---------------------------------
add_executable(wm-proxy src/c/whack_a_mole/wm_proxy_main.c)
target_link_libraries(wm-proxy PRIVATE sdf_wm sdf_origin_loopback sdf_common)

# --- Origin drivers (plan W1, ADR-0008) -----------------------------------
add_library(sdf_origin STATIC src/c/origin/origin_common.c)
target_include_directories(sdf_origin PUBLIC include)

add_library(sdf_origin_loopback STATIC src/c/origin/loopback/loopback_origin.c)
target_link_libraries(sdf_origin_loopback PUBLIC sdf_common sdf_origin)
target_include_directories(sdf_origin_loopback PUBLIC include)

add_library(sdf_origin_http STATIC src/c/origin/http/http_origin.c)
target_link_libraries(sdf_origin_http PUBLIC sdf_common sdf_origin)
target_include_directories(sdf_origin_http PUBLIC include)

# --- Benches (plan W0.5: recorded, never ctest-gated) --------------------
add_executable(bench_wm bench/bench_wm.c)
target_link_libraries(bench_wm PRIVATE sdf_wm sdf_common)

# --- Tools (plan W1.4: origin loadgen, degradation harness) --------------
add_executable(origin_loadgen tools/loadgen/origin_loadgen.c)
target_link_libraries(origin_loadgen PRIVATE sdf_origin_loopback sdf_wm
                                             sdf_common)

# --- Tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests/c)
endif()

# --- Install -----------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS sdf_common sdf_wm sdf_fountain sdf_fluid sdf_arc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
