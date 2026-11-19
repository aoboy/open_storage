# find_package(sdf) config — the whole stack, imported targets per lib.
# Consumers pick what they call: sdf::sdf_wm, sdf::sdf_fountain, ...
if(NOT TARGET sdf::sdf_common)
    add_library(sdf::sdf_common STATIC IMPORTED)
    set_target_properties(sdf::sdf_common PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libsdf_common.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
    find_package(Threads REQUIRED)
    set_target_properties(sdf::sdf_common PROPERTIES
        INTERFACE_LINK_LIBRARIES "Threads::Threads")
endif()

foreach(_lib sdf_origin sdf_origin_loopback sdf_origin_http sdf_wm
             sdf_fountain sdf_fluid sdf_arc)
    if(NOT TARGET sdf::${_lib})
        add_library(sdf::${_lib} STATIC IMPORTED)
        set_target_properties(sdf::${_lib} PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../lib${_lib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
    endif()
endforeach()

# Vendored RaptorQ archive: the fountain arm's private dep, exported so
# sdf::sdf_fountain links from the install tree (W6.4).
if(NOT TARGET sdf::nanorq)
    add_library(sdf::nanorq STATIC IMPORTED)
    set_target_properties(sdf::nanorq PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libnanorq.a")
endif()

# dependency chain (flat; m where math is used)
set_target_properties(sdf::sdf_origin_loopback PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_origin;sdf::sdf_common")
set_target_properties(sdf::sdf_origin_http PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_origin;sdf::sdf_common")
set_target_properties(sdf::sdf_wm PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_common;sdf::sdf_origin;sdf::sdf_origin_loopback")
set_target_properties(sdf::sdf_fountain PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_common;sdf::nanorq;m")
set_target_properties(sdf::sdf_fluid PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_common;sdf::sdf_wm;sdf::sdf_fountain;sdf::sdf_origin_loopback;m")
set_target_properties(sdf::sdf_arc PROPERTIES
    INTERFACE_LINK_LIBRARIES "sdf::sdf_common;m")
