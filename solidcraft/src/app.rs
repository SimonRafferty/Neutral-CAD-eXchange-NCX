//! The window: drop or pick a file, watch the progress, read the outcome.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use egui::{Align, Layout, RichText, Sense};

use crate::import::{Progress, ProgressHandle};
use crate::{export, import, sc, style};

/// What a finished job reports.
struct Done {
    message: String,
    problem: bool,
    output: Option<PathBuf>,
    report: Option<PathBuf>,
    is_import: bool,
}

enum State {
    Idle,
    Working { title: String, progress: ProgressHandle, cancel: Arc<AtomicBool>, result: Arc<Mutex<Option<Done>>> },
    Finished(Done),
}

pub struct App {
    state: State,
    cli: Option<PathBuf>,
    cli_override: Option<PathBuf>,
    pending: Option<PathBuf>,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>, start_with: Option<PathBuf>) -> App {
        style::apply(&cc.egui_ctx);
        let cli_override = cc.storage.and_then(|s| s.get_string("solidcraft_cli")).map(PathBuf::from);
        let cli = sc::find_cli(cli_override.as_deref());
        // developer aid for NCX_UI_SNAPSHOT: show a finished or a working state
        let state = match std::env::var("NCX_UI_STATE").as_deref() {
            Ok("done") => State::Finished(Done {
                message: "Your model has been imported.".into(),
                problem: false,
                output: Some(PathBuf::from(r"C:\Users\you\Documents\P02_block_features.sc")),
                report: Some(PathBuf::from("report.txt")),
                is_import: true,
            }),
            Ok("working") => {
                let progress: ProgressHandle = Arc::new(Mutex::new(Progress {
                    phase: "Rebuilding P02_block_features".into(),
                    detail: "Fillet-Corners".into(),
                    fraction: 0.62,
                }));
                State::Working {
                    title: "Importing P02_block_features.ncx.step".into(),
                    progress,
                    cancel: Arc::new(AtomicBool::new(false)),
                    result: Arc::new(Mutex::new(None)),
                }
            }
            _ => State::Idle,
        };
        App { state, cli, cli_override, pending: start_with }
    }

    fn start(&mut self, input: PathBuf, ctx: &egui::Context) {
        let Some(cli) = self.cli.clone() else {
            self.state = State::Finished(Done {
                message: "SolidCraft was not found on this computer. Install SolidCraft, or show the translator where it is (Settings, below).".into(),
                problem: true,
                output: None,
                report: None,
                is_import: true,
            });
            return;
        };
        let is_import = !is_design(&input);
        let output = free_name(&output_for(&input, is_import));
        let progress: ProgressHandle = Arc::new(Mutex::new(Progress::default()));
        let cancel = Arc::new(AtomicBool::new(false));
        let result: Arc<Mutex<Option<Done>>> = Arc::new(Mutex::new(None));
        let title = format!(
            "{} {}",
            if is_import { "Importing" } else { "Exporting" },
            input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
        );
        {
            let (progress, cancel, result, ctx) = (progress.clone(), cancel.clone(), result.clone(), ctx.clone());
            std::thread::spawn(move || {
                let done = if is_import {
                    let job = import::Job { input, output: output.clone(), cli, cancel, progress, log: false, plain_only: false };
                    match import::run(&job) {
                        Ok(o) => Done { message: o.message, problem: o.problem, output: Some(output).filter(|p| p.exists()), report: o.report_path, is_import },
                        Err(e) => Done { message: format!("The file could not be imported:\n{e:#}"), problem: true, output: None, report: None, is_import },
                    }
                } else {
                    let job = export::Job { input, output: output.clone(), cli, cancel, progress, log: false };
                    match export::run(&job) {
                        Ok(o) => Done { message: o.message, problem: o.problem, output: Some(output).filter(|p| p.exists()), report: o.report_path, is_import },
                        Err(e) => Done { message: format!("The design could not be exported:\n{e:#}"), problem: true, output: None, report: None, is_import },
                    }
                };
                *result.lock().unwrap() = Some(done);
                ctx.request_repaint();
            });
        }
        self.state = State::Working { title, progress, cancel, result };
    }

    fn pick_file(&mut self, ctx: &egui::Context) {
        if let Some(p) = rfd::FileDialog::new()
            .set_title("Choose an NCX file or a SolidCraft design")
            .add_filter("NCX files and SolidCraft designs", &["step", "stp", "sc"])
            .add_filter("NCX files (STEP)", &["step", "stp"])
            .add_filter("SolidCraft designs", &["sc"])
            .pick_file()
        {
            self.start(p, ctx);
        }
    }
}

fn is_design(p: &Path) -> bool {
    p.extension().map(|e| e.eq_ignore_ascii_case("sc")).unwrap_or(false)
}

/// file.ncx.step -> file.sc; design.sc -> design.ncx.step (same folder).
fn output_for(input: &Path, is_import: bool) -> PathBuf {
    let name = input.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "model".into());
    let lower = name.to_lowercase();
    let stem = ["", ".ncx"]
        .iter()
        .flat_map(|n| [".step", ".stp", ".sc"].map(move |e| format!("{n}{e}")))
        .filter(|suffix| !suffix.is_empty() && lower.ends_with(&suffix.to_lowercase()))
        .max_by_key(|s| s.len())
        .map(|suffix| name[..name.len() - suffix.len()].to_string())
        .unwrap_or(name);
    input.with_file_name(if is_import { format!("{stem}.sc") } else { format!("{stem}.ncx.step") })
}

/// Never overwrite: "name.sc", then "name (2).sc", "name (3).sc"...
fn free_name(p: &Path) -> PathBuf {
    if !p.exists() {
        return p.to_path_buf();
    }
    let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let (base, ext) = match name.find('.') {
        Some(i) => (name[..i].to_string(), name[i..].to_string()),
        None => (name.clone(), String::new()),
    };
    (2..1000).map(|i| p.with_file_name(format!("{base} ({i}){ext}"))).find(|c| !c.exists()).unwrap_or_else(|| p.to_path_buf())
}

fn open_path(p: &Path) {
    #[cfg(windows)]
    let _ = std::process::Command::new("explorer").arg(p).spawn();
    #[cfg(target_os = "macos")]
    let _ = std::process::Command::new("open").arg(p).spawn();
    #[cfg(all(unix, not(target_os = "macos")))]
    let _ = std::process::Command::new("xdg-open").arg(p).spawn();
}

fn show_in_folder(p: &Path) {
    #[cfg(windows)]
    let _ = std::process::Command::new("explorer").arg(format!("/select,{}", p.display())).spawn();
    #[cfg(not(windows))]
    if let Some(d) = p.parent() {
        open_path(d);
    }
}

/// Opens a design in SolidCraft (the app beside its command-line program).
fn open_in_solidcraft(cli: &Path, design: &Path) {
    let app = cli.with_file_name(if cfg!(windows) { "solidcraft.exe" } else { "solidcraft" });
    if app.is_file() {
        let _ = std::process::Command::new(app).arg(design).spawn();
    } else {
        open_path(design);
    }
}

impl eframe::App for App {
    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        if let Some(p) = &self.cli_override {
            storage.set_string("solidcraft_cli", p.to_string_lossy().into_owned());
        }
    }

    fn ui(&mut self, root: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = &root.ctx().clone();
        crate::snapshot::tick(ctx);
        // a file dropped on the window, or given on the command line
        if matches!(self.state, State::Idle | State::Finished(_)) {
            let dropped = ctx.input(|i| i.raw.dropped_files.first().map(|f| f.path().to_path_buf()));
            if let Some(p) = dropped.or_else(|| self.pending.take()) {
                self.start(p, ctx);
            }
        }
        // a finished job
        let finished = match &self.state {
            State::Working { result, .. } => result.lock().unwrap().take(),
            _ => None,
        };
        if let Some(done) = finished {
            self.state = State::Finished(done);
        }

        egui::Panel::bottom("footer").frame(egui::Frame::new().fill(style::PANEL).inner_margin(10)).show(root, |ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new(format!("NCX Translator {}", env!("CARGO_PKG_VERSION"))).small().color(style::DIM));
                ui.label(RichText::new("·").small().color(style::DIM));
                match &self.cli {
                    Some(p) => ui.label(RichText::new("SolidCraft found").small().color(style::DIM)).on_hover_text(p.display().to_string()),
                    None => ui.label(RichText::new("SolidCraft not found").small().color(style::BAD)),
                };
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.small_button("Where is SolidCraft?").on_hover_text("Choose SolidCraft's command-line program, solidcraft-cli").clicked() {
                        if let Some(p) = rfd::FileDialog::new().set_title("Choose solidcraft-cli").pick_file() {
                            self.cli_override = Some(p.clone());
                            self.cli = sc::find_cli(Some(&p));
                        }
                    }
                    ui.hyperlink_to(RichText::new("About NCX").small(), "https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX");
                });
            });
        });

        egui::CentralPanel::no_frame().frame(egui::Frame::new().fill(style::BG).inner_margin(24)).show(root, |ui| {
            ui.heading("NCX Translator");
            ui.label(RichText::new("Turns NCX files into SolidCraft designs, with their history, and SolidCraft designs into NCX files.").color(style::DIM));
            ui.add_space(16.0);

            let mut start_pick = false;
            let mut back_to_idle = false;
            match &self.state {
                State::Idle => {
                    start_pick = drop_zone(ui, "Drop an NCX file or a SolidCraft design here", "or click to choose a file");
                }
                State::Working { title, progress, cancel, .. } => {
                    let p = progress.lock().map(|g| g.clone()).unwrap_or_default();
                    ui.label(RichText::new(title).strong());
                    ui.add_space(6.0);
                    ui.label(&p.phase);
                    ui.label(RichText::new(&p.detail).color(style::DIM));
                    ui.add_space(6.0);
                    let bar = if p.fraction >= 0.0 { egui::ProgressBar::new(p.fraction) } else { egui::ProgressBar::new(0.0).animate(true) };
                    ui.add(bar.fill(style::ACCENT));
                    ui.add_space(10.0);
                    let cancelling = cancel.load(Ordering::Relaxed);
                    if ui.add_enabled(!cancelling, egui::Button::new(if cancelling { "Cancelling…" } else { "Cancel" })).clicked() {
                        cancel.store(true, Ordering::Relaxed);
                    }
                    ctx.request_repaint_after(std::time::Duration::from_millis(150));
                }
                State::Finished(done) => {
                    let colour = if done.problem { style::BAD } else { style::GOOD };
                    egui::Frame::new().fill(style::PANEL).corner_radius(8).inner_margin(16).stroke(egui::Stroke::new(1.0, colour)).show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.label(RichText::new(&done.message).color(style::TEXT));
                        ui.add_space(10.0);
                        ui.horizontal(|ui| {
                            if let Some(out) = &done.output {
                                if done.is_import {
                                    if ui.add(style::primary("Open in SolidCraft")).clicked() {
                                        if let Some(cli) = &self.cli {
                                            open_in_solidcraft(cli, out);
                                        }
                                    }
                                }
                                if ui.button("Show in folder").clicked() {
                                    show_in_folder(out);
                                }
                            }
                            if let Some(r) = &done.report {
                                if ui.button("Show report").clicked() {
                                    open_path(r);
                                }
                            }
                        });
                        if let Some(out) = &done.output {
                            ui.add_space(4.0);
                            ui.label(RichText::new(format!("Saved as {}", out.display())).small().color(style::DIM));
                        }
                    });
                    ui.add_space(14.0);
                    if drop_zone(ui, "Drop another file here", "or click to choose one") {
                        start_pick = true;
                    }
                    let _ = &mut back_to_idle;
                }
            }
            if start_pick {
                self.pick_file(ctx);
            }
        });
    }
}

/// The big drop target; true when clicked.
fn drop_zone(ui: &mut egui::Ui, title: &str, hint: &str) -> bool {
    let hovering = ui.ctx().input(|i| !i.raw.hovered_files.is_empty());
    let size = egui::vec2(ui.available_width(), 170.0);
    let (rect, resp) = ui.allocate_exact_size(size, Sense::click());
    let lit = hovering || resp.hovered();
    let painter = ui.painter_at(rect);
    painter.rect(
        rect,
        10,
        if lit { style::RAISED } else { style::PANEL },
        egui::Stroke::new(if lit { 2.0 } else { 1.0 }, if lit { style::ACCENT } else { style::LINE }),
        egui::StrokeKind::Inside,
    );
    let c = rect.center();
    painter.text(c - egui::vec2(0.0, 14.0), egui::Align2::CENTER_CENTER, title, egui::FontId::proportional(17.0), style::TEXT);
    painter.text(c + egui::vec2(0.0, 14.0), egui::Align2::CENTER_CENTER, hint, egui::FontId::proportional(13.5), style::DIM);
    if resp.hovered() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    resp.clicked()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn output_names() {
        assert_eq!(output_for(Path::new("C:/a/P01.ncx.step"), true), PathBuf::from("C:/a/P01.sc"));
        assert_eq!(output_for(Path::new("C:/a/part.STEP"), true), PathBuf::from("C:/a/part.sc"));
        assert_eq!(output_for(Path::new("C:/a/design.sc"), false), PathBuf::from("C:/a/design.ncx.step"));
    }
}
