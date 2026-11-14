// The launcher's version is the game's: port/include/version.h is the one place it is written.
fn main() {
    let header = "../../include/version.h";
    println!("cargo:rerun-if-changed={header}");
    let text = std::fs::read_to_string(header).expect("port/include/version.h");
    let version = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("#define DHD_VERSION "))
        .map(|v| v.trim().trim_matches('"').to_string())
        .expect("DHD_VERSION in version.h");
    println!("cargo:rustc-env=DHD_VERSION={version}");
    println!("cargo:rerun-if-changed=../../host/disc_check.c");
    println!("cargo:rerun-if-changed=../../host/disc_check.h");
    cc::Build::new().file("../../host/disc_check.c").compile("disc_check");
    println!("cargo:rerun-if-changed=../../host/launcher_link.h");
    println!("cargo:rerun-if-changed=launcher_link.c");
    cc::Build::new().file("launcher_link.c").compile("launcher_link");
    tauri_build::build()
}
