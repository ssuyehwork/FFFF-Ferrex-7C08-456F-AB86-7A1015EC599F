#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use egui_extras::install_image_loaders;
use indexer::{acquire_privileges, get_ntfs_volumes};
use storage::LoadedIndex;
use search::Searcher;
use std::sync::Arc;
use std::process::Command;
use chrono::{DateTime, Utc};

// Colors from UI界面风格.md
const COLOR_BG: egui::Color32 = egui::Color32::from_rgb(7, 9, 11);
const COLOR_BG1: egui::Color32 = egui::Color32::from_rgb(13, 16, 20);
const COLOR_BG2: egui::Color32 = egui::Color32::from_rgb(17, 21, 25);
const COLOR_BG3: egui::Color32 = egui::Color32::from_rgb(22, 27, 32);
const COLOR_BORDER2: egui::Color32 = egui::Color32::from_rgb(37, 46, 55);
const COLOR_ACCENT: egui::Color32 = egui::Color32::from_rgb(255, 140, 0);
const COLOR_TEXT2: egui::Color32 = egui::Color32::from_rgb(122, 143, 158);
const COLOR_TEXT3: egui::Color32 = egui::Color32::from_rgb(61, 80, 96);
const COLOR_SUCCESS: egui::Color32 = egui::Color32::from_rgb(46, 204, 113);

// SVG Icons
const SVG_LOGO: &str = r#"<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><polygon points="12,2 21,7 21,17 12,22 3,17 3,7"/><line x1="12" y1="2" x2="12" y2="22"/><line x1="3" y1="7" x2="21" y2="17"/><line x1="21" y1="7" x2="3" y2="17"/></svg>"#;
const SVG_SEARCH: &str = r#"<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="11" cy="11" r="7"/><line x1="16.5" y1="16.5" x2="22" y2="22"/></svg>"#;
const SVG_FILE: &str = r#"<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"><path d="M9 1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5z"/><polyline points="9,1 9,5 13,5"/></svg>"#;
const SVG_DIR: &str = r#"<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"><path d="M1 4a1 1 0 0 1 1-1h4l2 2h6a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1z"/></svg>"#;

struct VolumeData {
    letter: String,
    index: Option<Arc<LoadedIndex>>,
    active: bool,
    record_count: usize,
}

struct SearchResult {
    volume_letter: String,
    record_idx: usize,
}

struct FerrexApp {
    query: String,
    ext_query: String,
    volumes: Vec<VolumeData>,
    results: Vec<SearchResult>,
    hardware_verified: bool,
    total_records: usize,
    search_time_ms: f64,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        install_image_loaders(&cc.egui_ctx);
        setup_yahei_font(&cc.egui_ctx);

        let mut app = Self {
            query: String::new(),
            ext_query: String::new(),
            volumes: Vec::new(),
            results: Vec::new(),
            hardware_verified: false,
            total_records: 0,
            search_time_ms: 0.0,
        };
        
        app.hardware_verified = app.verify_hardware_wmi();
        if app.hardware_verified {
            app.load_all_indexes();
        }
        
        app
    }

    fn verify_hardware_wmi(&self) -> bool {
        let whitelist = [
            "BFEBFBFF000306C3", "SGH412RF00", "494000PA0D9L", 
            "PHYS825203NX480BGN", "NA5360WJ", "NA7G89GQ", "03000210052122072519"
        ];
        let cmds = ["cpu get processorid", "baseboard get serialnumber", "bios get serialnumber"];
        for cmd_args in cmds {
            if let Ok(output) = Command::new("wmic").args(cmd_args.split_whitespace()).output() {
                let text = String::from_utf8_lossy(&output.stdout);
                let lines: Vec<&str> = text.lines().collect();
                if lines.len() >= 2 {
                    let id = lines[1].trim();
                    if !id.is_empty() && whitelist.iter().any(|&w| w.eq_ignore_ascii_case(id)) {
                        return true;
                    }
                }
            }
        }
        false 
    }

    fn load_all_indexes(&mut self) {
        if let Err(_) = acquire_privileges() { return; }
        let ntfs_letters = get_ntfs_volumes();
        let mut total = 0;
        for vol in ntfs_letters {
            let idx_path = format!("{}_drive.idx", vol.chars().next().unwrap().to_lowercase());
            let (index, count) = if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                let c = loaded.record_count;
                total += c;
                (Some(Arc::new(loaded)), c)
            } else {
                (None, 0)
            };

            let is_active = index.is_some();
            self.volumes.push(VolumeData {
                letter: vol,
                index,
                active: is_active,
                record_count: count,
            });
        }
        self.total_records = total;
    }

    fn perform_search(&mut self) {
        let start = std::time::Instant::now();
        let mut all_results = Vec::new();
        for vol_data in &self.volumes {
            if !vol_data.active { continue; }
            if let Some(ref idx) = vol_data.index {
                let searcher = Searcher::new(idx.get_records(), idx.get_string_pool());
                let matches = searcher.search(&self.query, &self.ext_query);
                for m_idx in matches {
                    all_results.push(SearchResult { volume_letter: vol_data.letter.clone(), record_idx: m_idx });
                }
            }
        }
        self.results = all_results;
        self.search_time_ms = start.elapsed().as_secs_f64() * 1000.0;
    }
}

fn format_size(size: u64) -> String {
    if size == 0 { return "—".to_string(); }
    let kb = size as f64 / 1024.0;
    if kb < 1024.0 { return format!("{:.1} KB", kb); }
    let mb = kb / 1024.0;
    if mb < 1024.0 { return format!("{:.2} MB", mb); }
    let gb = mb / 1024.0;
    format!("{:.2} GB", gb)
}

fn setup_yahei_font(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let yahei_path = "C:\\Windows\\Fonts\\msyh.ttc";
    if let Ok(font_data) = std::fs::read(yahei_path) {
         fonts.font_data.insert("yahei".to_owned(), egui::FontData::from_owned(font_data).into());
         fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().clear();
         fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().push("yahei".to_owned());
         fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().clear();
         fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().push("yahei".to_owned());
    } else if let Ok(linux_font) = std::fs::read("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc") {
         fonts.font_data.insert("yahei".to_owned(), egui::FontData::from_owned(linux_font).into());
         fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().clear();
         fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().push("yahei".to_owned());
         fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().clear();
         fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().push("yahei".to_owned());
    }
    ctx.set_fonts(fonts);
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let mut visuals = egui::Visuals::dark();
        visuals.panel_fill = COLOR_BG;
        visuals.widgets.noninteractive.bg_fill = COLOR_BG1;
        visuals.widgets.inactive.bg_fill = COLOR_BG2;
        visuals.widgets.hovered.bg_fill = COLOR_BG3;
        visuals.widgets.active.bg_fill = COLOR_ACCENT;
        visuals.selection.bg_fill = COLOR_ACCENT.linear_multiply(0.2);
        ctx.set_visuals(visuals);

        let mut trigger_search = false;

        // 1. Titlebar
        egui::TopBottomPanel::top("tb").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(8.0)).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 10.0;
                ui.add(egui::Image::from_bytes("bytes://logo.svg", SVG_LOGO.as_bytes().to_vec()).tint(COLOR_ACCENT).fit_to_exact_size(egui::vec2(20.0, 20.0)));
                ui.label(egui::RichText::new("FERREX").size(17.0).strong().color(COLOR_ACCENT));
                egui::Frame::none().fill(COLOR_BG2).stroke(egui::Stroke::new(1.0, COLOR_BORDER2)).inner_margin(egui::Margin::symmetric(6.0, 2.0)).show(ui, |ui| {
                    ui.label(egui::RichText::new("NTFS INDEXER").size(9.0).color(COLOR_TEXT3));
                });
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        ui.painter().circle_filled(ui.cursor().min + egui::vec2(-8.0, 8.0), 3.0, COLOR_SUCCESS);
                        ui.label(egui::RichText::new(format!("索引就绪 — {} 条记录", self.total_records)).size(11.0).color(COLOR_TEXT3));
                    });
                });
            });
        });

        // 2. Drive Bar
        egui::TopBottomPanel::top("db").frame(egui::Frame::none().fill(COLOR_BG2).inner_margin(egui::Margin::symmetric(16.0, 6.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new("盘符").size(10.0).color(COLOR_TEXT3).strong());
                ui.add_space(10.0);
                for vol in &mut self.volumes {
                    let fill = if vol.active { COLOR_ACCENT.linear_multiply(0.1) } else { COLOR_BG3 };
                    let stroke = egui::Stroke::new(1.0, if vol.active { COLOR_ACCENT } else { COLOR_BORDER2 });
                    let response = egui::Frame::none().fill(fill).stroke(stroke).inner_margin(egui::Margin::symmetric(8.0, 3.0)).show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.painter().circle_filled(ui.cursor().min + egui::vec2(0.0, 7.0), 2.5, if vol.active { COLOR_ACCENT } else { COLOR_TEXT3 });
                            ui.add_space(10.0);
                            let text_color = if vol.active { COLOR_ACCENT } else { COLOR_TEXT2 };
                            ui.label(egui::RichText::new(format!("{}:", vol.letter.replace(":", ""))).size(12.0).strong().color(text_color));
                            ui.add_space(2.0);
                            let count_text = if vol.record_count > 1000000 { format!("{:.1}M", vol.record_count as f64 / 1000000.0) }
                                            else if vol.record_count > 1000 { format!("{}K", vol.record_count / 1000) }
                                            else { format!("{}", vol.record_count) };
                            ui.label(egui::RichText::new(count_text).size(9.0).color(text_color.linear_multiply(0.6)));
                        });
                    }).response.interact(egui::Sense::click());
                    if response.clicked() && vol.index.is_some() { vol.active = !vol.active; trigger_search = true; }
                }
            });
        });

        // 3. Search Bar
        egui::TopBottomPanel::top("sb").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(egui::Margin::symmetric(16.0, 10.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 0.0;
                let (rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
                ui.painter().rect_stroke(rect, 0.0, egui::Stroke::new(1.0, COLOR_BORDER2));
                ui.put(rect.shrink(10.0), egui::Image::from_bytes("bytes://search.svg", SVG_SEARCH.as_bytes().to_vec()).tint(COLOR_TEXT3));

                let edit = egui::TextEdit::singleline(&mut self.query).hint_text("文件名 / 关键词...").margin(egui::vec2(12.0, 8.0));
                let resp = ui.add_sized([ui.available_width() - 240.0, 36.0], edit);
                if resp.changed() { trigger_search = true; }

                let (rect, _) = ui.allocate_exact_size(egui::vec2(18.0, 36.0), egui::Sense::hover());
                ui.painter().rect_filled(rect, 0.0, COLOR_BG3);
                ui.painter().rect_stroke(rect, 0.0, egui::Stroke::new(1.0, COLOR_BORDER2));
                ui.painter().circle_filled(rect.center(), 1.5, COLOR_ACCENT);

                let ext_edit = egui::TextEdit::singleline(&mut self.ext_query).hint_text("扩展名").margin(egui::vec2(10.0, 8.0));
                let resp_ext = ui.add_sized([100.0, 36.0], ext_edit);
                if resp_ext.changed() { trigger_search = true; }

                ui.add_space(10.0);
                let (rect, response) = ui.allocate_at_least(egui::vec2(90.0, 36.0), egui::Sense::click());
                ui.painter().rect_filled(rect, 0.0, COLOR_ACCENT);
                let mut btn_ui = ui.child_ui(rect, egui::Layout::left_to_right(egui::Align::Center), None);
                btn_ui.add_space(15.0);
                btn_ui.add(egui::Image::from_bytes("bytes://search_w.svg", SVG_SEARCH.as_bytes().to_vec()).tint(egui::Color32::BLACK).fit_to_exact_size(egui::vec2(14.0, 14.0)));
                btn_ui.label(egui::RichText::new(" 搜索").strong().color(egui::Color32::BLACK));
                if response.clicked() { trigger_search = true; }
            });
        });

        // 4. Headers
        egui::TopBottomPanel::top("hd").frame(egui::Frame::none().fill(COLOR_BG2).inner_margin(egui::Margin::symmetric(16.0, 4.0))).show(ctx, |ui| {
             ui.horizontal(|ui| {
                ui.add_space(28.0);
                ui.add_sized([ui.available_width() * 0.4, 20.0], egui::Label::new(egui::RichText::new("名称 ▲").size(10.0).color(COLOR_TEXT3).strong()));
                ui.add_sized([ui.available_width() * 0.4, 20.0], egui::Label::new(egui::RichText::new("路径").size(10.0).color(COLOR_TEXT3).strong()));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.add_sized([120.0, 20.0], egui::Label::new(egui::RichText::new("修改时间").size(10.0).color(COLOR_TEXT3).strong()));
                    ui.add_sized([80.0, 20.0], egui::Label::new(egui::RichText::new("大小").size(10.0).color(COLOR_TEXT3).strong()));
                });
            });
        });

        // 5. Results
        egui::CentralPanel::default().show(ctx, |ui| {
            if self.results.is_empty() && (!self.query.is_empty() || !self.ext_query.is_empty()) {
                ui.centered_and_justified(|ui| { ui.label(egui::RichText::new("无匹配结果").color(COLOR_TEXT3).size(20.0)); });
            } else {
                egui::ScrollArea::vertical().auto_shrink([false; 2]).show(ui, |ui| {
                    ui.spacing_mut().item_spacing.y = 0.0;
                    for (i, res) in self.results.iter().take(200).enumerate() {
                        let vol_data = self.volumes.iter().find(|v| v.letter == res.volume_letter).unwrap();
                        let rec = &vol_data.index.as_ref().unwrap().get_records()[res.record_idx];
                        let pool = vol_data.index.as_ref().unwrap().get_string_pool();
                        let name = String::from_utf8_lossy(&pool[rec.name_offset as usize..]).split('\0').next().unwrap_or("").to_string();
                        
                        let (rect, response) = ui.allocate_at_least(egui::vec2(ui.available_width(), 30.0), egui::Sense::click());
                        let bg = if response.hovered() { COLOR_BG2 } else if i % 2 == 0 { COLOR_BG } else { COLOR_BG1.linear_multiply(0.5) };
                        ui.painter().rect_filled(rect, 0.0, bg);
                        if response.hovered() { ui.painter().line_segment([rect.left_top(), rect.left_bottom()], egui::Stroke::new(3.0, COLOR_ACCENT)); }

                        let icon_svg = if (rec.flags & 0x10) != 0 { SVG_DIR } else { SVG_FILE };
                        ui.put(egui::Rect::from_min_size(rect.left_top() + egui::vec2(8.0, 7.0), egui::vec2(16.0, 16.0)),
                            egui::Image::from_bytes("bytes://i.svg", icon_svg.as_bytes().to_vec()).tint(COLOR_TEXT3));

                        ui.painter().text(rect.left_top() + egui::vec2(40.0, 15.0), egui::Align2::LEFT_CENTER, format!("{}:", res.volume_letter.replace(":", "")), egui::FontId::proportional(9.0), COLOR_TEXT3);
                        ui.painter().text(rect.left_top() + egui::vec2(60.0, 15.0), egui::Align2::LEFT_CENTER, &name, egui::FontId::proportional(13.0), COLOR_ACCENT);

                        let name_width = ui.fonts(|f| f.glyph_width(&egui::FontId::proportional(13.0), ' ')) * name.len() as f32;
                        ui.painter().text(rect.left_top() + egui::vec2(60.0 + name_width + 15.0, 15.0), egui::Align2::LEFT_CENTER, "C:\\Users\\...", egui::FontId::proportional(11.0), COLOR_TEXT3);

                        let dt = DateTime::<Utc>::from_timestamp(rec.timestamp as i64 / 10_000_000 - 11_644_473_600, 0).unwrap_or_default();
                        ui.painter().text(rect.right_top() + egui::vec2(-10.0, 15.0), egui::Align2::RIGHT_CENTER, dt.format("%Y-%m-%d %H:%M").to_string(), egui::FontId::proportional(11.0), COLOR_TEXT3);
                        ui.painter().text(rect.right_top() + egui::vec2(-140.0, 15.0), egui::Align2::RIGHT_CENTER, format_size(rec.size), egui::FontId::proportional(11.0), COLOR_TEXT2);
                    }
                });
            }
        });

        // 6. Statusbar
        egui::TopBottomPanel::bottom("st").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(egui::Margin::symmetric(16.0, 4.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new("结果").size(10.0).color(COLOR_TEXT3));
                ui.label(egui::RichText::new(format!("{}", self.results.len())).size(10.0).color(COLOR_TEXT2).strong());
                ui.add_space(20.0);
                ui.label(egui::RichText::new("耗时").size(10.0).color(COLOR_TEXT3));
                ui.label(egui::RichText::new(format!("{:.1} ms", self.search_time_ms)).size(10.0).color(COLOR_TEXT2).strong());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(egui::RichText::new("FERREX v0.1.0").size(10.0).color(COLOR_ACCENT).strong());
                    ui.add_space(20.0);
                    ui.label(egui::RichText::new("上次索引 2026-04-22 17:41").size(10.0).color(COLOR_TEXT3));
                });
            });
        });

        if trigger_search { self.perform_search(); }
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions { viewport: egui::ViewportBuilder::default().with_inner_size([1000.0, 700.0]).with_title("Ferrex"), ..Default::default() };
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
