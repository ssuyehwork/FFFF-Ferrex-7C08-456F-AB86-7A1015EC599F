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
const COLOR_ACCENT2: egui::Color32 = egui::Color32::from_rgb(201, 110, 0);
const COLOR_TEXT: egui::Color32 = egui::Color32::from_rgb(200, 212, 220);
const COLOR_TEXT2: egui::Color32 = egui::Color32::from_rgb(122, 143, 158);
const COLOR_TEXT3: egui::Color32 = egui::Color32::from_rgb(61, 80, 96);
const COLOR_SUCCESS: egui::Color32 = egui::Color32::from_rgb(46, 204, 113);

struct VolumeData {
    letter: String,
    index: Arc<LoadedIndex>,
    active: bool,
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
    status: String,
    hardware_verified: bool,
    total_records: usize,
    search_time_ms: f64,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        install_image_loaders(&cc.egui_ctx);
        setup_custom_fonts(&cc.egui_ctx);

        let mut app = Self {
            query: String::new(),
            ext_query: String::new(),
            volumes: Vec::new(),
            results: Vec::new(),
            status: "初始化中...".to_string(),
            hardware_verified: false,
            total_records: 0,
            search_time_ms: 0.0,
        };
        
        app.hardware_verified = app.verify_hardware_wmi();
        if app.hardware_verified {
            app.load_all_indexes();
        } else {
            app.status = "未授权设备。".to_string();
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
        if let Err(_) = acquire_privileges() {
            self.status = "权限不足".to_string();
            return;
        }
        let volume_letters = get_ntfs_volumes();
        let mut total = 0;
        for vol in volume_letters {
            let idx_path = format!("{}_drive.idx", vol.replace(":", ""));
            if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                total += loaded.record_count as usize;
                self.volumes.push(VolumeData {
                    letter: vol,
                    index: Arc::new(loaded),
                    active: true,
                });
            }
        }
        self.total_records = total;
        if self.volumes.is_empty() {
            self.status = "未发现可用索引".to_string();
        } else {
            self.status = "就绪".to_string();
        }
    }

    fn perform_search(&mut self) {
        let start = std::time::Instant::now();
        let mut all_results = Vec::new();

        for vol_data in &self.volumes {
            if !vol_data.active { continue; }

            let searcher = Searcher::new(vol_data.index.get_records(), vol_data.index.get_string_pool());
            let matches = searcher.search(&self.query, &self.ext_query);

            for idx in matches {
                all_results.push(SearchResult {
                    volume_letter: vol_data.letter.clone(),
                    record_idx: idx,
                });
            }
        }

        self.results = all_results;
        self.search_time_ms = start.elapsed().as_secs_f64() * 1000.0;
    }
}

fn setup_custom_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();

    // Load WenQuanYi Zen Hei for Chinese support
    if let Ok(font_data) = std::fs::read("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc") {
         fonts.font_data.insert(
            "zenhei".to_owned(),
            egui::FontData::from_owned(font_data).into(),
        );
        fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap()
            .insert(0, "zenhei".to_owned());
        fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap()
            .push("zenhei".to_owned());
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
        visuals.widgets.active.fg_stroke = egui::Stroke::new(1.0, egui::Color32::BLACK);
        visuals.selection.bg_fill = COLOR_ACCENT.linear_multiply(0.3);
        ctx.set_visuals(visuals);

        let mut trigger_search = false;

        // 1. Titlebar
        egui::TopBottomPanel::top("title_bar").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(8.0)).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 12.0;

                // Logo Icon (Hexagon)
                let (rect, _) = ui.allocate_exact_size(egui::vec2(22.0, 22.0), egui::Sense::hover());
                let pts = vec![
                        rect.center() + egui::vec2(0.0, -10.0),
                        rect.center() + egui::vec2(9.0, -5.0),
                        rect.center() + egui::vec2(9.0, 5.0),
                        rect.center() + egui::vec2(0.0, 10.0),
                        rect.center() + egui::vec2(-9.0, 5.0),
                        rect.center() + egui::vec2(-9.0, -5.0),
                        rect.center() + egui::vec2(0.0, -10.0),
                ];
                ui.painter().add(egui::Shape::line(pts, egui::Stroke::new(1.5, COLOR_ACCENT)));

                ui.label(egui::RichText::new("FERREX").font(egui::FontId::proportional(18.0)).strong().color(COLOR_ACCENT));

                ui.add_space(4.0);
                ui.group(|ui| {
                    ui.style_mut().visuals.widgets.noninteractive.bg_fill = COLOR_BG2;
                    ui.label(egui::RichText::new("NTFS INDEXER").size(10.0).color(COLOR_TEXT3));
                });

                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    // Fake WM buttons
                    ui.button(egui::RichText::new("✕").color(COLOR_TEXT3));
                    ui.button(egui::RichText::new("□").color(COLOR_TEXT3));
                    ui.button(egui::RichText::new("—").color(COLOR_TEXT3));

                    ui.add_space(12.0);
                    ui.horizontal(|ui| {
                        ui.painter().circle_filled(ui.cursor().min + egui::vec2(0.0, 8.0), 3.0, COLOR_SUCCESS);
                        ui.add_space(8.0);
                        ui.label(egui::RichText::new(format!("索引就绪 — {} 条记录", self.total_records)).size(11.0).color(COLOR_TEXT3));
                    });
                });
            });
        });

        // 2. Drive Selector
        egui::TopBottomPanel::top("drive_selector").frame(egui::Frame::none().fill(COLOR_BG2).inner_margin(egui::Margin::symmetric(16.0, 8.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new("盘符").size(10.0).strong().color(COLOR_TEXT3));
                ui.add_space(8.0);

                for vol in &mut self.volumes {
                    let mut text = egui::RichText::new(format!("{}:", vol.letter.replace(":", ""))).size(12.0).strong();
                    if vol.active {
                        text = text.color(COLOR_ACCENT);
                    } else {
                        text = text.color(COLOR_TEXT2);
                    }

                    let resp = ui.selectable_label(vol.active, text);
                    if resp.clicked() {
                        vol.active = !vol.active;
                        trigger_search = true;
                    }
                }

                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.button(egui::RichText::new("全选 / 全清").size(10.0).color(COLOR_TEXT3)).clicked() {
                        let all_active = self.volumes.iter().all(|v| v.active);
                        for v in &mut self.volumes { v.active = !all_active; }
                        trigger_search = true;
                    }
                });
            });
        });

        // 3. Search Bar
        egui::TopBottomPanel::top("search_bar").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(egui::Margin::symmetric(16.0, 10.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 0.0;

                // Icon wrap
                let (rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
                ui.painter().rect_stroke(rect, 0.0, egui::Stroke::new(1.0, COLOR_BORDER2));
                ui.painter().text(rect.center(), egui::Align2::CENTER_CENTER, "🔍", egui::FontId::proportional(14.0), COLOR_TEXT3);

                // Main search input
                let edit = egui::TextEdit::singleline(&mut self.query)
                    .hint_text("文件名 / 关键词...")
                    .margin(egui::vec2(12.0, 8.0));

                let resp = ui.add_sized([ui.available_width() - 220.0, 36.0], edit);
                if resp.changed() { trigger_search = true; }

                // Extension divider
                let (rect, _) = ui.allocate_exact_size(egui::vec2(20.0, 36.0), egui::Sense::hover());
                ui.painter().rect_filled(rect, 0.0, COLOR_BG3);
                ui.painter().rect_stroke(rect, 0.0, egui::Stroke::new(1.0, COLOR_BORDER2));
                ui.painter().text(rect.center(), egui::Align2::CENTER_CENTER, ".", egui::FontId::monospace(14.0), COLOR_ACCENT);

                // Extension input
                let ext_edit = egui::TextEdit::singleline(&mut self.ext_query)
                    .hint_text("扩展名")
                    .margin(egui::vec2(10.0, 8.0));
                let resp_ext = ui.add_sized([100.0, 36.0], ext_edit);
                if resp_ext.changed() { trigger_search = true; }

                ui.add_space(10.0);

                // Search button
                if ui.add_sized([80.0, 36.0], egui::Button::new(egui::RichText::new("搜索").strong().color(egui::Color32::BLACK)).fill(COLOR_ACCENT)).clicked() {
                    trigger_search = true;
                }
            });
        });

        // 4. Column Header
        egui::TopBottomPanel::top("col_header").frame(egui::Frame::none().fill(COLOR_BG2).inner_margin(egui::Margin::symmetric(16.0, 4.0))).show(ctx, |ui| {
             ui.horizontal(|ui| {
                ui.add_space(28.0);
                ui.add_sized([200.0, 20.0], egui::Label::new(egui::RichText::new("名称").size(10.0).color(COLOR_TEXT3).strong()));
                ui.add_sized([ui.available_width() - 210.0, 20.0], egui::Label::new(egui::RichText::new("路径").size(10.0).color(COLOR_TEXT3).strong()));
                ui.add_sized([80.0, 20.0], egui::Label::new(egui::RichText::new("大小").size(10.0).color(COLOR_TEXT3).strong()));
                ui.add_sized([130.0, 20.0], egui::Label::new(egui::RichText::new("修改时间").size(10.0).color(COLOR_TEXT3).strong()));
            });
        });

        // 5. Results List
        egui::CentralPanel::default().frame(egui::Frame::none().fill(COLOR_BG)).show(ctx, |ui| {
            if self.results.is_empty() && (!self.query.is_empty() || !self.ext_query.is_empty()) {
                ui.centered_and_justified(|ui| {
                    ui.vertical_centered(|ui| {
                        ui.label(egui::RichText::new("无匹配结果").color(COLOR_TEXT3).size(20.0));
                        ui.label(egui::RichText::new("尝试更换关键词或扩大盘符范围").color(COLOR_TEXT3).size(12.0));
                    });
                });
            } else {
                egui::ScrollArea::vertical().auto_shrink([false; 2]).show(ui, |ui| {
                    for (i, res) in self.results.iter().take(200).enumerate() {
                        let vol_data = self.volumes.iter().find(|v| v.letter == res.volume_letter).unwrap();
                        let rec = &vol_data.index.get_records()[res.record_idx];
                        let pool = vol_data.index.get_string_pool();
                        let offset = rec.name_offset as usize;
                        let name = String::from_utf8_lossy(&pool[offset..]).split('\0').next().unwrap_or("").to_string();
                        
                        let bg = if i % 2 == 0 { COLOR_BG } else { COLOR_BG1.linear_multiply(0.4) };

                        let (rect, response) = ui.allocate_at_least(egui::vec2(ui.available_width(), 30.0), egui::Sense::click());
                        if response.hovered() {
                            ui.painter().rect_filled(rect, 0.0, COLOR_BG2);
                            ui.painter().line_segment([rect.left_top(), rect.left_bottom()], egui::Stroke::new(3.0, COLOR_ACCENT));
                        } else {
                            ui.painter().rect_filled(rect, 0.0, bg);
                        }

                        let icon = if (rec.flags & 0x10) != 0 { "📁" } else { "📄" };
                        let icon_color = if (rec.flags & 0x10) != 0 { COLOR_ACCENT2 } else { COLOR_TEXT3 };

                        ui.painter().text(rect.left_top() + egui::vec2(16.0, 15.0), egui::Align2::CENTER_CENTER, icon, egui::FontId::proportional(14.0), icon_color);

                        // Drive tag + Name
                        let name_rect = egui::Rect::from_min_size(rect.left_top() + egui::vec2(44.0, 0.0), egui::vec2(200.0, 30.0));
                        let drive_tag = format!("{}:", res.volume_letter.replace(":", ""));
                        ui.painter().text(name_rect.left_center(), egui::Align2::LEFT_CENTER, format!("{} {}", drive_tag, name), egui::FontId::monospace(12.5), COLOR_TEXT);

                        // Path (Fake for now as we don't have full path resolution yet, but will show name)
                        let path_rect = egui::Rect::from_min_size(rect.left_top() + egui::vec2(250.0, 0.0), egui::vec2(rect.width() - 460.0, 30.0));
                        ui.painter().text(path_rect.left_center(), egui::Align2::LEFT_CENTER, "...", egui::FontId::monospace(11.0), COLOR_TEXT3);

                        // Size
                        let size_text = if (rec.flags & 0x10) != 0 { "—" } else { "—" }; // Placeholder
                        let size_rect = egui::Rect::from_min_size(rect.right_top() - egui::vec2(210.0, 0.0), egui::vec2(80.0, 30.0));
                        ui.painter().text(size_rect.right_center(), egui::Align2::RIGHT_CENTER, size_text, egui::FontId::monospace(11.0), COLOR_TEXT2);

                        // Date
                        let date_rect = egui::Rect::from_min_size(rect.right_top() - egui::vec2(130.0, 0.0), egui::vec2(130.0, 30.0));
                        let dt = DateTime::<Utc>::from_timestamp(rec.timestamp as i64 / 10_000_000 - 11_644_473_600, 0).unwrap_or_default();
                        ui.painter().text(date_rect.left_center(), egui::Align2::LEFT_CENTER, dt.format("%Y-%m-%d %H:%M").to_string(), egui::FontId::monospace(11.0), COLOR_TEXT3);
                    }
                });
            }
        });

        // 6. Statusbar
        egui::TopBottomPanel::bottom("status_bar").frame(egui::Frame::none().fill(COLOR_BG1).inner_margin(egui::Margin::symmetric(16.0, 4.0))).show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new("结果").size(10.0).color(COLOR_TEXT3));
                ui.label(egui::RichText::new(format!("{}", self.results.len())).size(10.0).color(COLOR_TEXT2).strong());

                ui.add_space(20.0);
                ui.label(egui::RichText::new("耗时").size(10.0).color(COLOR_TEXT3));
                ui.label(egui::RichText::new(format!("{:.1} ms", self.search_time_ms)).size(10.0).color(COLOR_TEXT2).strong());

                ui.add_space(20.0);
                ui.label(egui::RichText::new("盘符").size(10.0).color(COLOR_TEXT3));
                let active_drives: Vec<String> = self.volumes.iter().filter(|v| v.active).map(|v| v.letter.clone()).collect();
                ui.label(egui::RichText::new(active_drives.join(" ")).size(10.0).color(COLOR_TEXT2).strong());

                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(egui::RichText::new("FERREX v0.1.0").size(10.0).color(COLOR_ACCENT).strong());
                });
            });
        });

        if trigger_search {
            self.perform_search();
        }
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([900.0, 600.0])
            .with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
