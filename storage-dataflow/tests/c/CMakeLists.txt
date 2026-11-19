# C tests

find_package(Threads REQUIRED)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

add_executable(test_abi test_abi.c)
target_link_libraries(test_abi PRIVATE sdf_common)
add_test(NAME abi_smoke COMMAND test_abi)

add_executable(test_wm_select unit/test_wm_select.c)
target_link_libraries(test_wm_select PRIVATE sdf_wm)
add_test(NAME wm_select COMMAND test_wm_select)

add_executable(test_wm_profile unit/test_wm_profile.c)
target_link_libraries(test_wm_profile PRIVATE sdf_wm)
add_test(NAME wm_profile COMMAND test_wm_profile)

# Plan W0.2 — common primitives (import map 3.4).
add_executable(test_ring unit/test_ring.c)
target_link_libraries(test_ring PRIVATE sdf_common Threads::Threads)
add_test(NAME ring COMMAND test_ring)

add_executable(test_seqlock unit/test_seqlock.c)
target_link_libraries(test_seqlock PRIVATE sdf_common Threads::Threads)
add_test(NAME seqlock COMMAND test_seqlock)

add_executable(test_pool unit/test_pool.c)
target_link_libraries(test_pool PRIVATE sdf_common)
add_test(NAME pool COMMAND test_pool)

add_executable(test_shm unit/test_shm.c)
target_link_libraries(test_shm PRIVATE sdf_common)
add_test(NAME shm COMMAND test_shm)

# Plan W0.5 — metric primitives (conventions imported per WAM K.1).
add_executable(test_metrics unit/test_metrics.c)
target_link_libraries(test_metrics PRIVATE sdf_common Threads::Threads)
add_test(NAME metrics COMMAND test_metrics)

# Plan W1.2 — loopback origin driver (ADR-0008 seam, harness knobs).
add_executable(test_origin_loopback unit/test_origin_loopback.c)
target_link_libraries(test_origin_loopback PRIVATE sdf_origin_loopback sdf_common)
add_test(NAME origin_loopback COMMAND test_origin_loopback)

# Plan W1.3 — HTTP origin driver (in-process server, no env deps).
add_executable(test_origin_http unit/test_origin_http.c)
target_link_libraries(test_origin_http PRIVATE sdf_origin_http sdf_common)
add_test(NAME origin_http COMMAND test_origin_http)

# Plan W2.3 — WM SHM publication (ADR-0002 protocol).
add_executable(test_wm_shm unit/test_wm_shm.c)
target_link_libraries(test_wm_shm PRIVATE sdf_wm sdf_common Threads::Threads)
add_test(NAME wm_shm COMMAND test_wm_shm)

# Plan W2.5 — cache-fill proxy over the origin seam.
add_executable(test_wm_proxy integration/test_wm_proxy.c)
target_link_libraries(test_wm_proxy PRIVATE sdf_wm sdf_origin_loopback
                                           sdf_origin sdf_common)
add_test(NAME wm_proxy COMMAND test_wm_proxy)

# Plan W0.3 — strict TOML loader (ADR-0005). The source tree path is
# baked in so the shipped examples resolve from any CWD.
add_executable(test_config unit/test_config.c)
target_link_libraries(test_config PRIVATE sdf_common)
target_compile_definitions(test_config PRIVATE "SDF_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME config COMMAND test_config)

# Plan W0.4 — golden-vector parity gate (ADR-0009).
add_executable(test_parity test_parity.c)
target_link_libraries(test_parity PRIVATE sdf_common sdf_wm sdf_fountain)
target_compile_definitions(test_parity PRIVATE "SDF_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME parity COMMAND test_parity)

# Future targets (enable once the corresponding C sources are implemented):
#   test_config   — plan W0.3 (strict TOML loader)
#   test_parity   — plan W0.4 (golden-vector harness)
#   test_metrics  — plan W0.5 (metric series pinning)
#   test_fluid_gate / test_arc_* — plan W3.4/W4
#
# add_executable(test_fluid_gate unit/test_fluid_gate.c)
# target_link_libraries(test_fluid_gate PRIVATE sdf_fluid)
# add_test(NAME fluid_gate COMMAND test_fluid_gate)

# Plan W3.2 — nanorq fountain arm (ADR-0004).
add_executable(test_fe_codec unit/test_fe_codec.c)
target_link_libraries(test_fe_codec PRIVATE sdf_fountain sdf_common)
add_test(NAME fe_codec COMMAND test_fe_codec)

# Plan W3.4 — FLUID budget + gate (O.1/O.5 identities).
add_executable(test_fluid_gate unit/test_fluid_gate.c)
target_link_libraries(test_fluid_gate PRIVATE sdf_fluid)
add_test(NAME fluid_gate COMMAND test_fluid_gate)

# Plan W3.5 — repair coordinator (design 02 loop + ARQ fallback).
add_executable(test_fluid_repair integration/test_fluid_repair.c)
target_link_libraries(test_fluid_repair PRIVATE sdf_fluid)
add_test(NAME fluid_repair COMMAND test_fluid_repair)

# Plan W4.2 — ARC core: paper 12.3 goldens + the QG guard (the
# missing-predecessor timeout is pinned at the internal layer — the
# public submit API cannot manufacture a block gap).
add_executable(test_arc_core unit/test_arc_core.c)
target_link_libraries(test_arc_core PRIVATE sdf_arc sdf_common)
target_include_directories(test_arc_core PRIVATE ${CMAKE_SOURCE_DIR}/src/c/arc)
add_test(NAME arc_core COMMAND test_arc_core)

# Plan W4.3/W4.5 — the sidecar over the SPSC ring.
add_executable(test_arc_sidecar integration/test_arc_sidecar.c)
target_link_libraries(test_arc_sidecar PRIVATE sdf_arc sdf_common)
add_test(NAME arc_sidecar COMMAND test_arc_sidecar)

# Plan W6.1 — the live scrape endpoint (the wam K.1b contract).
add_executable(test_metrics_endpoint unit/test_metrics_endpoint.c)
target_link_libraries(test_metrics_endpoint PRIVATE sdf_common)
add_test(NAME metrics_endpoint COMMAND test_metrics_endpoint)

# Plan W5.1 — fuzzers (shapes from wam M.1/X4; deterministic smoke
# budget in ctest, NOT the full libFuzzer leg which runs in CI nightly).
add_executable(fuzz_fountain fuzz/fuzz_fountain.c)
target_link_libraries(fuzz_fountain PRIVATE sdf_fountain sdf_common)
add_test(NAME fuzz_fountain COMMAND fuzz_fountain)
set_tests_properties(fuzz_fountain PROPERTIES ENVIRONMENT "SDF_FUZZ_ITERS=20000")

add_executable(fuzz_wm_profile fuzz/fuzz_wm_profile.c)
target_link_libraries(fuzz_wm_profile PRIVATE sdf_wm sdf_common)
add_test(NAME fuzz_wm_profile COMMAND fuzz_wm_profile)
set_tests_properties(fuzz_wm_profile PROPERTIES ENVIRONMENT "SDF_FUZZ_ITERS=20000")

add_executable(fuzz_arc_stream fuzz/fuzz_arc_stream.c)
target_link_libraries(fuzz_arc_stream PRIVATE sdf_arc sdf_common)
add_test(NAME fuzz_arc_stream COMMAND fuzz_arc_stream)
set_tests_properties(fuzz_arc_stream PROPERTIES ENVIRONMENT "SDF_FUZZ_ITERS=5000")
#
# add_executable(test_arc_pacer unit/test_arc_offset.c unit/test_arc_heap.c)
# target_link_libraries(test_arc_pacer PRIVATE sdf_arc)
# add_test(NAME arc_pacer COMMAND test_arc_pacer)
#
# add_executable(test_fe_codec unit/test_fe_codec.c)
# target_link_libraries(test_fe_codec PRIVATE sdf_fountain)
# add_test(NAME fe_codec COMMAND test_fe_codec)
