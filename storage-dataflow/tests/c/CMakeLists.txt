# C tests

add_executable(test_abi test_abi.c)
target_link_libraries(test_abi PRIVATE sdf_common)
add_test(NAME abi_smoke COMMAND test_abi)

add_executable(test_wm_select unit/test_wm_select.c)
target_link_libraries(test_wm_select PRIVATE sdf_wm)
add_test(NAME wm_select COMMAND test_wm_select)

add_executable(test_wm_profile unit/test_wm_profile.c)
target_link_libraries(test_wm_profile PRIVATE sdf_wm)
add_test(NAME wm_profile COMMAND test_wm_profile)

# Future targets (enable once the corresponding C sources are implemented):
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
