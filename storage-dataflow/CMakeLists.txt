cmake_minimum_required(VERSION 3.24)

# Version single source of truth: include/sdf/version.h (W5.2).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/sdf/version.h" _ver_hdr)
string(REGEX MATCH "#define SDF_VERSION_MAJOR ([0-9]+)" _ ${_ver_hdr})
set(SDF_VER_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SDF_VERSION_MINOR ([0-9]+)" _ ${_ver_hdr})
set(SDF_VER_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SDF_VERSION_PATCH ([0-9]+)" _ ${_ver_hdr})
set(SDF_VER_PATCH ${CMAKE_MATCH_1})
project(storage_dataflow
    VERSION "${SDF_VER_MAJOR}.${SDF_VER_MINOR}.${SDF_VER_PATCH}"
    LANGUAGES C)

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
    src/c/common/metrics_endpoint.c
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

# --- FLUID repair gate + coordinator (plan W3.4/W3.5) ----------------------
add_library(sdf_fluid STATIC
    src/c/fluid/fluid_gate.c
    src/c/fluid/fluid_repair_main.c
)
target_link_libraries(sdf_fluid PUBLIC sdf_common sdf_fountain sdf_wm
                                        sdf_origin_loopback)
target_link_libraries(sdf_fluid PUBLIC sdf_fountain sdf_common)
target_include_directories(sdf_fluid PUBLIC include)

# --- ARC pacer (plan W4, ADR-0006) -----------------------------------------
add_library(sdf_arc STATIC
    src/c/arc/arc_heap.c
    src/c/arc/arc_offset.c
    src/c/arc/arc_pacer.c
    src/c/arc/arc_sidecar_main.c
)
target_link_libraries(sdf_arc PUBLIC sdf_common m)
target_link_libraries(sdf_arc PUBLIC sdf_common m)
target_include_directories(sdf_arc PUBLIC include)

# --- Apps (plan W2.5: the P1 proxy shell) ---------------------------------
add_executable(wm-proxy src/c/whack_a_mole/wm_proxy_main.c)
target_link_libraries(wm-proxy PRIVATE sdf_wm sdf_origin_loopback sdf_common)

# --- Apps (plan W3.6: the repair e2e driver) -------------------------------
add_executable(fluid-repair src/c/fluid/fluid_repair_shell.c)
target_link_libraries(fluid-repair PRIVATE sdf_fluid)

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

# --- Install (plan W5.2: the wam R2/M.2 pattern) ---------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS sdf_common sdf_origin sdf_origin_loopback sdf_origin_http
        sdf_wm sdf_fountain sdf_fluid sdf_arc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# pkg-config: one .pc per link granularity (core = the whole stack today;
# consumers pick what they call — the deps chain is flat, no --static
# surprises because everything vendored builds from source in-tree)
foreach(pc sdf_common sdf_wm sdf_fountain sdf_fluid sdf_arc
         sdf_origin_loopback sdf_origin_http)
    string(REPLACE sdf_ "" _short "${pc}")
    configure_file(packaging/${pc}.pc.in ${pc}.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${pc}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endforeach()

# find_package(sdf) config
write_basic_package_version_file(
    sdf-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES packaging/sdf-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdf
    RENAME sdf-config.cmake)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdf-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdf)
