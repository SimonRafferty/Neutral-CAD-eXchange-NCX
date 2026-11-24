//! NCX Translator's look: egui (the toolkit SolidCraft is built with) and IBM Plex Sans (SIL Open Font
//! License, assets/fonts/OFL.txt), with NCX's own colours. SolidCraft's own theme, colours and icons are
//! not used: they are under SolidCraft's Brand and Interface Licence.

use egui::{Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Stroke, TextStyle, Visuals};

pub const BG: Color32 = Color32::from_rgb(0x17, 0x1a, 0x1f);
pub const PANEL: Color32 = Color32::from_rgb(0x20, 0x24, 0x2b);
pub const RAISED: Color32 = Color32::from_rgb(0x2a, 0x2f, 0x38);
pub const LINE: Color32 = Color32::from_rgb(0x3a, 0x41, 0x4c);
pub const TEXT: Color32 = Color32::from_rgb(0xe6, 0xe8, 0xeb);
pub const DIM: Color32 = Color32::from_rgb(0x9a, 0xa3, 0xae);
/// NCX accent: a warm amber.
pub const ACCENT: Color32 = Color32::from_rgb(0xe8, 0xa3, 0x3d);
pub const ACCENT_DIM: Color32 = Color32::from_rgb(0x7a, 0x58, 0x25);
pub const GOOD: Color32 = Color32::from_rgb(0x6c, 0xc4, 0x8a);
pub const BAD: Color32 = Color32::from_rgb(0xe5, 0x6b, 0x5d);

pub fn apply(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    fonts.font_data.insert(
        "plex".into(),
        FontData::from_static(include_bytes!("../assets/fonts/IBMPlexSans-Regular.ttf")).into(),
    );
    fonts.font_data.insert(
        "plex-semibold".into(),
        FontData::from_static(include_bytes!("../assets/fonts/IBMPlexSans-SemiBold.ttf")).into(),
    );
    fonts.families.entry(FontFamily::Proportional).or_default().insert(0, "plex".into());
    fonts.families.insert(FontFamily::Name("strong".into()), vec!["plex-semibold".into(), "plex".into()]);
    ctx.set_fonts(fonts);

    let mut style = (*ctx.style_of(egui::Theme::Dark)).clone();
    style.text_styles = [
        (TextStyle::Heading, FontId::new(22.0, FontFamily::Name("strong".into()))),
        (TextStyle::Body, FontId::new(14.5, FontFamily::Proportional)),
        (TextStyle::Button, FontId::new(14.5, FontFamily::Proportional)),
        (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
        (TextStyle::Monospace, FontId::new(13.0, FontFamily::Monospace)),
    ]
    .into();
    style.spacing.item_spacing = egui::vec2(10.0, 8.0);
    style.spacing.button_padding = egui::vec2(14.0, 6.0);

    let mut v = Visuals::dark();
    v.panel_fill = BG;
    v.window_fill = PANEL;
    v.extreme_bg_color = PANEL;
    v.override_text_color = Some(TEXT);
    v.hyperlink_color = ACCENT;
    v.selection.bg_fill = ACCENT_DIM;
    v.selection.stroke = Stroke::new(1.0, ACCENT);
    let r = CornerRadius::same(6);
    for w in [&mut v.widgets.noninteractive, &mut v.widgets.inactive, &mut v.widgets.hovered, &mut v.widgets.active, &mut v.widgets.open] {
        w.corner_radius = r;
    }
    v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, LINE);
    v.widgets.inactive.weak_bg_fill = RAISED;
    v.widgets.inactive.bg_fill = RAISED;
    v.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x34, 0x3a, 0x45);
    v.widgets.hovered.bg_stroke = Stroke::new(1.0, ACCENT);
    v.widgets.active.weak_bg_fill = ACCENT_DIM;
    style.visuals = v;
    // always this look, whatever the system's light or dark setting
    ctx.set_theme(egui::Theme::Dark);
    ctx.set_style_of(egui::Theme::Dark, style.clone());
    ctx.set_style_of(egui::Theme::Light, style);
}

/// The accent-filled button for the main action.
pub fn primary(text: &str) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.to_string()).color(Color32::from_rgb(0x1a, 0x14, 0x08)).strong())
        .fill(ACCENT)
        .min_size(egui::vec2(120.0, 32.0))
}
