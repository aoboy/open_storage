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

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# W6.4: the CFFI extension is a shared object and links the static
# archives; every in-tree object must be position independent (the
# vendored nanorq archive builds with -fPIC for the same reason).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
target_link_libraries(sdf_common PUBLIC Threads::Threads)

# --- Origin drivers (plan W1, ADR-0008) -----------------------------------
add_library(sdf_origin STATIC src/c/origin/origin_common.c)
target_include_directories(sdf_origin PUBLIC include)

add_library(sdf_origin_loopback STATIC src/c/origin/loopback/loopback_origin.c)
target_link_libraries(sdf_origin_loopback PUBLIC sdf_common sdf_origin)
target_include_directories(sdf_origin_loopback PUBLIC include)

add_library(sdf_origin_http STATIC src/c/origin/http/http_origin.c)
target_link_libraries(sdf_origin_http PUBLIC sdf_common sdf_origin)
target_include_directories(sdf_origin_http PUBLIC include)

# W6.15: explicit class selection and strict-TOML origin materialization.
add_library(sdf_origin_registry STATIC
    src/c/origin/origin_registry.c
    src/c/origin/origin_config.c)
target_link_libraries(sdf_origin_registry PUBLIC sdf_common sdf_origin
                                                sdf_origin_loopback
                                                sdf_origin_http)
target_include_directories(sdf_origin_registry PUBLIC include)

# --- Whack-a-Mole dispatch ---------------------------------------------------
add_library(sdf_wm STATIC
    src/c/whack_a_mole/wm_profile.c
    src/c/whack_a_mole/wm_select.c
    src/c/whack_a_mole/wm_shed.c
    src/c/whack_a_mole/wm_shm.c
    src/c/whack_a_mole/wm_proxy.c
)
target_link_libraries(sdf_wm PUBLIC sdf_common sdf_origin_registry)
target_include_directories(sdf_wm PUBLIC include)

# --- Fountain codec (plan W3/W6.8; ADR-0003/0004 arms) ---------------------
# The C frontends and vendored nanorq adapter are common to both variants.
# SDF_USE_RUST_FOUNTAIN replaces ONLY fe_codec_lt.c + fe_prng.c. The Rust
# staticlib is folded into libsdf_fountain.a, preserving every installed
# consumer surface (pkg-config, find_package, CFFI and the Debian payload).
option(SDF_USE_RUST_FOUNTAIN "Use the Rust LT fountain arm (nanorq stays C)" OFF)
set(_fountain_sources
    src/c/fountain/fe_enc.c
    src/c/fountain/fe_dec.c
    src/c/fountain/fe_codec_nanorq.c
)
set(SDF_FOUNTAIN_NATIVE_LIBS m)
set(SDF_FOUNTAIN_PC_NATIVE_LIBS "-lm")
if(SDF_USE_RUST_FOUNTAIN)
    find_program(SDF_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    set(_rust_crate "${CMAKE_SOURCE_DIR}/src/rust/sdf-fountain")
    set(_rust_target "${CMAKE_CURRENT_BINARY_DIR}/rust-fountain-target")
    set(_rust_archive "${_rust_target}/release/libsdf_fountain.a")
    set(_rust_stamp "${CMAKE_CURRENT_BINARY_DIR}/sdf-fountain-rust.stamp")
    set(_rust_link_source "${CMAKE_CURRENT_BINARY_DIR}/rust_fountain_link.c")
    file(GLOB _rust_sources CONFIGURE_DEPENDS "${_rust_crate}/src/*.rs")
    add_custom_command(
        OUTPUT "${_rust_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${_rust_target}"
            "${SDF_CARGO_EXECUTABLE}" build --manifest-path "${_rust_crate}/Cargo.toml"
            --locked --release
        COMMAND "${CMAKE_COMMAND}" -E touch "${_rust_stamp}"
        BYPRODUCTS "${_rust_archive}"
        DEPENDS ${_rust_sources} "${_rust_crate}/Cargo.toml"
            "${_rust_crate}/Cargo.lock" "${_rust_crate}/build.rs"
            "${CMAKE_SOURCE_DIR}/bindings/cbindgen.toml"
        COMMENT "Building the Rust LT fountain staticlib"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_rust_link_source}"
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/cmake/rust_fountain_link_anchor.c" "${_rust_link_source}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_rust_link_source}"
        DEPENDS "${_rust_stamp}" "${CMAKE_SOURCE_DIR}/cmake/rust_fountain_link_anchor.c"
        VERBATIM)
    list(APPEND _fountain_sources "${_rust_link_source}")
    # rustc --print native-static-libs for the pinned Linux toolchain. libc is
    # named explicitly for non-compiler drivers; the C driver already adds it.
    set(SDF_FOUNTAIN_NATIVE_LIBS gcc_s util rt pthread m ${CMAKE_DL_LIBS} c)
    set(SDF_FOUNTAIN_PC_NATIVE_LIBS "-lgcc_s -lutil -lrt -lpthread -lm -l${CMAKE_DL_LIBS} -lc")
    message(STATUS "Fountain LT arm: Rust (C frontends + nanorq retained)")
else()
    list(APPEND _fountain_sources
        src/c/fountain/fe_codec_lt.c
        src/c/fountain/fe_prng.c)
    message(STATUS "Fountain LT arm: C (comparison-window default)")
endif()

add_custom_target(nanorq_build
    COMMAND make -C ${CMAKE_SOURCE_DIR}/third_party/nanorq libnanorq.a)
add_library(sdf_fountain STATIC ${_fountain_sources})
add_dependencies(sdf_fountain nanorq_build)
target_link_libraries(sdf_fountain PUBLIC sdf_common
    ${CMAKE_SOURCE_DIR}/third_party/nanorq/libnanorq.a ${SDF_FOUNTAIN_NATIVE_LIBS})
target_include_directories(sdf_fountain PUBLIC include
    PRIVATE src/c/fountain ${CMAKE_SOURCE_DIR}/third_party/nanorq/include)
if(SDF_USE_RUST_FOUNTAIN)
    set(_rust_combined "${CMAKE_CURRENT_BINARY_DIR}/libsdf_fountain.combined.a")
    add_custom_command(TARGET sdf_fountain POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSDF_C_ARCHIVE=$<TARGET_FILE:sdf_fountain>"
            "-DSDF_RUST_ARCHIVE=${_rust_archive}"
            "-DSDF_COMBINED_ARCHIVE=${_rust_combined}"
            "-DSDF_AR=${CMAKE_AR}"
            "-DSDF_RANLIB=${CMAKE_RANLIB}"
            -P "${CMAKE_SOURCE_DIR}/cmake/merge_rust_fountain.cmake"
        COMMENT "Merging the Rust LT arm into libsdf_fountain.a"
        VERBATIM)
endif()
# find_package(sdf) consumes the same variant-specific native dependencies.
set(SDF_FOUNTAIN_CMAKE_NATIVE_LIBS "${SDF_FOUNTAIN_NATIVE_LIBS}")

# --- FLUID repair gate + coordinator (plan W3.4/W3.5) ----------------------
add_library(sdf_fluid STATIC
    src/c/fluid/fluid_gate.c
    src/c/fluid/fluid_repair_main.c
)
target_link_libraries(sdf_fluid PUBLIC sdf_common sdf_fountain sdf_wm
                                        sdf_origin_registry)
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

# --- Benches (plan W0.5: recorded, never ctest-gated) --------------------
add_executable(bench_wm bench/bench_wm.c)
target_link_libraries(bench_wm PRIVATE sdf_wm sdf_common)

# W6.12: record the revision in ARC benchmark lines. A dirty first-party
# tree is called out rather than looking like a clean tagged build.
set(SDF_BENCH_REVISION "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE _sdf_git_rev_rc
        OUTPUT_VARIABLE _sdf_git_rev
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_sdf_git_rev_rc EQUAL 0 AND _sdf_git_rev)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE _sdf_git_status_rc
            OUTPUT_VARIABLE _sdf_git_status
            ERROR_QUIET)
        if(_sdf_git_status_rc EQUAL 0 AND _sdf_git_status)
            string(APPEND _sdf_git_rev "-dirty")
        endif()
        set(SDF_BENCH_REVISION "${_sdf_git_rev}")
    endif()
endif()

add_executable(bench_arc bench/bench_arc.c)
target_link_libraries(bench_arc PRIVATE sdf_arc sdf_common)
target_compile_definitions(bench_arc PRIVATE
    SDF_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    SDF_BENCH_REVISION="${SDF_BENCH_REVISION}")

# W6.15 measured HTTP transport leg (recorded, not ctest-gated).
add_executable(bench_origin bench/bench_origin.c)
target_link_libraries(bench_origin PRIVATE sdf_origin_http sdf_common)
target_compile_definitions(bench_origin PRIVATE
    SDF_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

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
        sdf_origin_registry sdf_wm sdf_fountain sdf_fluid sdf_arc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# The vendored RaptorQ archive is sdf_fountain's private link dep; the
# installed .pc/config surface names it so consumers (CFFI included)
# resolve the fountain arm from the install tree (W6.4).
install(FILES ${CMAKE_SOURCE_DIR}/third_party/nanorq/libnanorq.a
    DESTINATION ${CMAKE_INSTALL_LIBDIR})

# pkg-config: one .pc per link granularity (core = the whole stack today;
# consumers pick what they call — the deps chain is flat, no --static
# surprises because everything vendored builds from source in-tree)
foreach(pc sdf_common sdf_origin sdf_wm sdf_fountain sdf_fluid sdf_arc
         sdf_origin_loopback sdf_origin_http sdf_origin_registry)
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
configure_file(packaging/sdf-config.cmake sdf-config.cmake @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdf-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdf)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdf-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sdf)
