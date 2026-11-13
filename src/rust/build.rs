fn main() {
  println!("cargo:rerun-if-changed=src/sandbox");
  println!("cargo:rerun-if-changed=cbindgen.toml");
  println!("cargo:rerun-if-env-changed=ANT_SANDBOX_HEADER");
  #[cfg(feature = "sandbox")]
  generate_header();
}

#[cfg(feature = "sandbox")]
fn generate_header() {
  let out = std::env::var_os("ANT_SANDBOX_HEADER").map(std::path::PathBuf::from).unwrap_or_else(|| {
    std::path::PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR is set by cargo")).join("ant_sandbox_devices.h")
  });
  let config = cbindgen::Config::from_file("cbindgen.toml").expect("cbindgen.toml is valid");
  cbindgen::Builder::new()
    .with_config(config)
    .with_src("src/sandbox/mod.rs")
    .generate()
    .expect("sandbox FFI header can be generated")
    .write_to_file(out);
}
