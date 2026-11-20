// cbindgen emits the Rust view of the ARC ABI (plan W6.18):
// the header diff against include/sdf/arc.h is the seam gate.
fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    // the crate sits at src/rust/sdf-arc; the config lives under bindings/
    let cfg = std::path::Path::new(&crate_dir)
        .join("../../..")
        .join("bindings")
        .join("cbindgen-arc.toml");
    let config = cbindgen::Config::from_file(&cfg).unwrap_or_else(|_| cbindgen::Config::default());
    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("cbindgen: unable to generate the ARC header")
        .write_to_file(std::path::Path::new(&crate_dir).join("sdf_arc_generated.h"));
}
