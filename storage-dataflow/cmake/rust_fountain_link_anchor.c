/*
 * W6.8 build dependency anchor: the generated copy is touched after Cargo
 * rebuilds the LT staticlib. Its object makes CMake re-archive sdf_fountain,
 * whose POST_BUILD step then merges the fresh Rust archive. No ABI symbol.
 */
typedef int sdf_fountain_rust_link_anchor_t;
