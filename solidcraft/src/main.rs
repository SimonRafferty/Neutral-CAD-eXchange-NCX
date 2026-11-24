// No console window for the user. Started from a console with arguments, it attaches to that console.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod export;
mod geom;
mod import;
mod ncx;
mod report;
mod sc;
mod snapshot;
mod style;

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

fn usage() -> ! {
    eprintln!(
        "NCX Translator {}\n\
         usage:\n  ncx-translator                      (the window)\n  \
         ncx-translator import <file.ncx.step> <design.sc> [--log] [--plain] [--cli <solidcraft-cli>]\n",
        env!("CARGO_PKG_VERSION")
    );
    std::process::exit(2)
}

/// The window icon: NCX's own mark, drawn here (an amber rounded square with a white "N").
fn icon() -> egui::IconData {
    const S: usize = 64;
    let mut rgba = vec![0u8; S * S * 4];
    for y in 0..S {
        for x in 0..S {
            let (fx, fy) = (x as f32 + 0.5, y as f32 + 0.5);
            // rounded square, radius 12
            let r = 12.0;
            let cx = fx.clamp(r, S as f32 - r);
            let cy = fy.clamp(r, S as f32 - r);
            let inside = ((fx - cx).powi(2) + (fy - cy).powi(2)).sqrt() <= r;
            // the N: two uprights and a diagonal
            let n = (16.0..24.0).contains(&fx) && (14.0..50.0).contains(&fy)
                || (40.0..48.0).contains(&fx) && (14.0..50.0).contains(&fy)
                || (14.0..50.0).contains(&fy) && ((fx - 16.0) - (fy - 14.0) * (24.0 / 36.0)).abs() < 5.0;
            let i = (y * S + x) * 4;
            if inside {
                let c = if n { [0xff, 0xff, 0xff] } else { [0xe8, 0xa3, 0x3d] };
                rgba[i..i + 4].copy_from_slice(&[c[0], c[1], c[2], 0xff]);
            }
        }
    }
    egui::IconData { rgba, width: S as u32, height: S as u32 }
}

#[cfg(windows)]
fn attach_console() {
    extern "system" {
        fn AttachConsole(pid: u32) -> i32;
    }
    const ATTACH_PARENT_PROCESS: u32 = 0xFFFF_FFFF;
    unsafe {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}
#[cfg(not(windows))]
fn attach_console() {}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // No arguments, or one file (dropped on the program or "Open with"): the window.
    if args.is_empty() || (args.len() == 1 && std::path::Path::new(&args[0]).is_file()) {
        let start_with = args.first().map(PathBuf::from);
        let options = eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_title("NCX Translator")
                .with_inner_size([660.0, 470.0])
                .with_min_inner_size([520.0, 400.0])
                .with_drag_and_drop(true)
                .with_icon(icon()),
            ..Default::default()
        };
        let _ = eframe::run_native("NCX Translator", options, Box::new(move |cc| Ok(Box::new(app::App::new(cc, start_with)))));
        return;
    }
    attach_console();
    let code = match run_cli(&args) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: {e:#}");
            1
        }
    };
    std::process::exit(code);
}

fn run_cli(args: &[String]) -> anyhow::Result<i32> {
    let mut pos: Vec<&str> = Vec::new();
    let (mut log, mut plain, mut cli) = (false, false, None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--log" => log = true,
            "--plain" => plain = true,
            "--cli" => cli = it.next().map(PathBuf::from),
            "-h" | "--help" => usage(),
            _ => pos.push(a),
        }
    }
    let cli = sc::find_cli(cli.as_deref()).ok_or_else(|| anyhow::anyhow!("SolidCraft's command-line program was not found"))?;
    match pos.as_slice() {
        ["import", input, output] => {
            let job = import::Job {
                input: PathBuf::from(input),
                output: std::path::absolute(output)?,
                cli,
                cancel: Arc::new(AtomicBool::new(false)),
                progress: Arc::new(Mutex::new(import::Progress::default())),
                log,
                plain_only: plain,
            };
            let out = import::run(&job)?;
            println!("{}", out.report.text());
            println!("{}", out.message);
            Ok(if out.problem { 3 } else { 0 })
        }
        ["export", input, output] => {
            let job = export::Job {
                input: std::path::absolute(input)?,
                output: std::path::absolute(output)?,
                cli,
                cancel: Arc::new(AtomicBool::new(false)),
                progress: Arc::new(Mutex::new(import::Progress::default())),
                log,
            };
            let out = export::run(&job)?;
            println!("{}", out.report.text());
            println!("{}", out.message);
            Ok(if out.problem { 3 } else { 0 })
        }
        _ => usage(),
    }
}
