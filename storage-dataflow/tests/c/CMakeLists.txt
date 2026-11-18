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

# Plan W0.3 — strict TOML loader (ADR-0005). The source tree path is
# baked in so the shipped examples resolve from any CWD.
add_executable(test_config unit/test_config.c)
target_link_libraries(test_config PRIVATE sdf_common)
target_compile_definitions(test_config PRIVATE "SDF_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME config COMMAND test_config)

# Plan W0.4 — golden-vector parity gate (ADR-0009).
add_executable(test_parity test_parity.c)
target_link_libraries(test_parity PRIVATE sdf_common sdf_wm)
target_compile_definitions(test_parity PRIVATE "SDF_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME parity COMMAND test_parity)

# Future targets (enable once the corresponding C sources are implemented):
#   test_config   — plan W0.3 (strict TOML loader)
#   test_parity   — plan W0.4 (golden-vector harness)
#   test_metrics  — plan W0.5 (metric series pinning)
#   test_fluid_gate / test_fe_codec / test_arc_* — plan W3/W4
#
# add_executable(test_fluid_gate unit/test_fluid_gate.c)
# target_link_libraries(test_fluid_gate PRIVATE sdf_fluid)
# add_test(NAME fluid_gate COMMAND test_fluid_gate)
#
# add_executable(test_arc_pacer unit/test_arc_offset.c unit/test_arc_heap.c)
# target_link_libraries(test_arc_pacer PRIVATE sdf_arc)
# add_test(NAME arc_pacer COMMAND test_arc_pacer)
#
# add_executable(test_fe_codec unit/test_fe_codec.c)
# target_link_libraries(test_fe_codec PRIVATE sdf_fountain)
# add_test(NAME fe_codec COMMAND test_fe_codec)
