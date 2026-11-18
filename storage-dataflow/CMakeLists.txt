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
)
target_link_libraries(sdf_wm PUBLIC sdf_common)
target_include_directories(sdf_wm PUBLIC include)

# --- Fountain codec ----------------------------------------------------------
add_library(sdf_fountain STATIC
    src/c/fountain/fe_prng.c
    src/c/fountain/fe_codec_lt.c
    src/c/fountain/fe_codec_nanorq.c
    src/c/fountain/fe_enc.c
    src/c/fountain/fe_dec.c
)
target_link_libraries(sdf_fountain PUBLIC sdf_common)
target_include_directories(sdf_fountain PUBLIC include)

# --- FLUID repair gate -------------------------------------------------------
add_library(sdf_fluid STATIC
    src/c/fluid/fluid_gate.c
    src/c/fluid/fluid_round.c
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

# --- Benches (plan W0.5: recorded, never ctest-gated) --------------------
add_executable(bench_wm bench/bench_wm.c)
target_link_libraries(bench_wm PRIVATE sdf_wm sdf_common)

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
