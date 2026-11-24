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
        "NCX SolidCraft {}\n\
         usage:\n  ncx-solidcraft                      (the window)\n  \
         ncx-solidcraft import <file.ncx.step> <design.sc> [--log] [--plain] [--cli <solidcraft-cli>]\n",
        env!("CARGO_PKG_VERSION")
    );
    std::process::exit(2)
}

/// The window icon: the same artwork as the program's icon (assets/make_icon.py writes icon64.rgba).
fn icon() -> egui::IconData {
    egui::IconData { rgba: include_bytes!("../assets/icon64.rgba").to_vec(), width: 64, height: 64 }
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
                .with_title("NCX SolidCraft")
                .with_inner_size([660.0, 470.0])
                .with_min_inner_size([520.0, 400.0])
                .with_drag_and_drop(true)
                .with_icon(icon()),
            ..Default::default()
        };
        let _ = eframe::run_native("NCX SolidCraft", options, Box::new(move |cc| Ok(Box::new(app::App::new(cc, start_with)))));
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
    let mut cancel_after: Option<u64> = None;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--log" => log = true,
            "--plain" => plain = true,
            "--cli" => cli = it.next().map(PathBuf::from),
            // developer test of Cancel: press it after this many milliseconds
            "--cancel-after" => cancel_after = it.next().and_then(|v| v.parse::<u64>().ok()),
            "-h" | "--help" => usage(),
            _ => pos.push(a),
        }
    }
    let cli = sc::find_cli(cli.as_deref()).ok_or_else(|| anyhow::anyhow!("SolidCraft's command-line program was not found"))?;
    let cancel = Arc::new(AtomicBool::new(false));
    if let Some(ms) = cancel_after {
        let c = cancel.clone();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(ms));
            c.store(true, std::sync::atomic::Ordering::Relaxed);
        });
    }
    match pos.as_slice() {
        ["import", input, output] => {
            let job = import::Job {
                input: PathBuf::from(input),
                output: std::path::absolute(output)?,
                cli,
                cancel: cancel.clone(),
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
                cancel: cancel.clone(),
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
