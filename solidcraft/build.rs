//! Builds the shared NCX core (`../core`, C++17 behind a C ABI) into a static library linked into the
//! program, so the translator ships as one executable. The core's sources are used as they are: this
//! file only lists them (keep it in step with core/CMakeLists.txt).

use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("core");
    let core_sources = [
        "api", "codec", "container", "fingerprint", "matcher", "notice", "step21", "validate", "verify",
    ];

    // miniz (C): raw deflate for the payload.
    cc::Build::new()
        .file(root.join("third_party/miniz/miniz.c"))
        .include(root.join("third_party/miniz"))
        .warnings(false)
        .compile("ncx_miniz");

    // The core (C++17). NCX_CORE_BUILD makes ncx_core.h declare its functions for export; in a static
    // build that only affects the declarations, which Rust links by name.
    let mut b = cc::Build::new();
    b.cpp(true)
        .std("c++17")
        .define("NCX_CORE_BUILD", None)
        .include(root.join("include"))
        .include(root.join("src"))
        .include(root.join("third_party"))
        .include(root.join("third_party/miniz"))
        .warnings(false);
    if b.get_compiler().is_like_msvc() {
        b.flag("/utf-8").flag("/EHsc");
    }
    for s in core_sources {
        b.file(root.join("src").join(format!("{s}.cpp")));
    }
    b.compile("ncx_core");

    println!("cargo:rerun-if-changed={}", root.join("src").display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());
    println!("cargo:rerun-if-changed=build.rs");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        windows_resources();
    }
}

/// The icon and version information Explorer shows (assets/app.rc), compiled with the Windows SDK's rc.exe
/// and linked into the program. Without the SDK the program still builds, with no icon.
fn windows_resources() {
    println!("cargo:rerun-if-changed=assets/app.rc");
    println!("cargo:rerun-if-changed=assets/icon.ico");
    let Some(rc) = find_rc() else {
        println!("cargo:warning=rc.exe (Windows SDK) not found: the program has no icon");
        return;
    };
    let version = env!("CARGO_PKG_VERSION");
    let parts: Vec<&str> = version.split('.').collect();
    let comma = format!("{},{},{},0", parts[0], parts.get(1).unwrap_or(&"0"), parts.get(2).unwrap_or(&"0"));
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let assets = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("assets");
    // the template with the version and the icon's full path filled in
    let template = std::fs::read_to_string(assets.join("app.rc")).expect("assets/app.rc");
    let icon = assets.join("icon.ico").display().to_string().replace('\\', "\\\\");
    let rc_text = template
        .replace("NCX_VERSION_COMMA", &comma)
        .replace("NCX_VERSION_TEXT", &format!("\"{version}\""))
        .replace("\"icon.ico\"", &format!("\"{icon}\""));
    let rc_file = out_dir.join("app.rc");
    std::fs::write(&rc_file, rc_text).unwrap();
    let res = out_dir.join("app.res");
    let mut cmd = std::process::Command::new(&rc);
    cmd.arg("/nologo");
    // winver.h lives in the SDK's um and shared include folders, beside the bin folder rc.exe came from
    if let Some(ver) = rc.parent().and_then(|p| p.parent()).and_then(|p| p.file_name()) {
        let inc = PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10\Include").join(ver);
        for sub in ["um", "shared"] {
            cmd.arg("/i").arg(inc.join(sub));
        }
    }
    let status = cmd.arg("/fo").arg(&res).arg(&rc_file).status();
    match status {
        Ok(s) if s.success() => println!("cargo:rustc-link-arg-bins={}", res.display()),
        _ => println!("cargo:warning=rc.exe failed: the program has no icon"),
    }
}

fn find_rc() -> Option<PathBuf> {
    let kits = PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10\bin");
    let mut versions: Vec<PathBuf> = std::fs::read_dir(&kits).ok()?.filter_map(|e| e.ok().map(|e| e.path())).collect();
    versions.sort();
    versions.into_iter().rev().map(|v| v.join("x64").join("rc.exe")).find(|p| p.is_file())
}
