// cbindgen emits the Rust view of the fountain ABI (plan W5.3):
// header diff against include/sdf/fountain.h is the seam gate.
fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    // the crate sits at src/rust/sdf-fountain; the config at the repo root
    let cfg = std::path::Path::new(&crate_dir)
        .join("../../..")
        .join("bindings")
        .join("cbindgen.toml");
    let config = cbindgen::Config::from_file(&cfg)
        .unwrap_or_else(|_| cbindgen::Config::default());
    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("cbindgen: unable to generate the fountain header")
        .write_to_file(std::path::Path::new(&crate_dir).join("sdf_fountain_generated.h"));
}
