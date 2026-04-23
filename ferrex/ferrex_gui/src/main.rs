#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use egui::{Color32, FontDefinitions, FontFamily, FontId, RichText};
use indexer::{acquire_privileges, get_ntfs_volumes, get_volume_serial, MftScanner};
use storage::{save_index_soa, LoadedIndex};
use search::{Searcher, get_name_from_pool, PathResolver};
use std::sync::Arc;
use std::process::Command;
use std::collections::{HashMap, HashSet};

// --- Color Palette ---
const BG: Color32 = Color32::from_rgb(7, 9, 11);
const PANEL: Color32 = Color32::from_rgb(13, 16, 20);
const BG2: Color32 = Color32::from_rgb(17, 21, 25);
const BG3: Color32 = Color32::from_rgb(22, 27, 32);
const BORDER2: Color32 = Color32::from_rgb(37, 46, 55);
const ACCENT: Color32 = Color32::from_rgb(255, 140, 0);
const TEXT: Color32 = Color32::from_rgb(200, 212, 220);
const TEXT2: Color32 = Color32::from_rgb(122, 143, 158);
const TEXT3: Color32 = Color32::from_rgb(61, 80, 96);
const SUCCESS: Color32 = Color32::from_rgb(46, 204, 113);
const DANGER: Color32 = Color32::from_rgb(231, 76, 60);

// --- Icon Cache ---
struct IconCache {
    map: HashMap<&'static str, egui::ImageSource<'static>>,
}

impl IconCache {
    fn new() -> Self {
        let mut map = HashMap::new();
        map.insert("file", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_file.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/file.svg")),
        });
        map.insert("folder", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_folder.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/folder.svg")),
        });
        map.insert("exe", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_exe.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/exe.svg")),
        });
        map.insert("image", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_image.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/image.svg")),
        });
        map.insert("doc", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_doc.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/doc.svg")),
        });
        Self { map }
    }

    fn get_for_entry(&self, name: &str, is_dir: bool) -> &egui::ImageSource<'static> {
        if is_dir { return self.map.get("folder").unwrap(); }
        let ext = name.rsplit('.').next().unwrap_or("").to_lowercase();
        match ext.as_str() {
            "exe" | "dll" | "msi" | "bat" | "cmd" => self.map.get("exe"),
            "png" | "jpg" | "jpeg" | "gif" | "bmp" | "ico" | "webp" => self.map.get("image"),
            "doc" | "docx" | "pdf" | "txt" | "md" | "xls" | "xlsx" | "ppt" | "pptx" => self.map.get("doc"),
            _ => self.map.get("file"),
        }
        .unwrap()
    }
}

// --- App State ---
struct SearchResult {
    drive: String,
    name: String,
    full_path: String,
    size: u64,
    timestamp: u64,
    is_dir: bool,
}

struct LoadedVolume {
    drive: String,
    store: Arc<LoadedIndex>,
    sorted_idx: Arc<Vec<u32>>,
    resolver: Arc<PathResolver<'static>>,
}

struct FerrexApp {
    query: String,
    ext_filter: String,
    results: Vec<SearchResult>,
    volumes: Vec<LoadedVolume>,
    active_drives: HashSet<String>,
    status_text: String,
    total_records: usize,
    last_search_ms: f64,
    icons: IconCache,
    hardware_ok: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        egui_extras::install_image_loaders(&cc.egui_ctx);

        let mut fonts = FontDefinitions::default();
        fonts.font_data.insert("mono_data".to_owned(), egui::FontData::from_static(include_bytes!("../assets/JetBrainsMono-Regular.ttf")));
        fonts.families.entry(FontFamily::Name("mono".into())).or_default().insert(0, "mono_data".to_owned());

        fonts.font_data.insert("cond_data".to_owned(), egui::FontData::from_static(include_bytes!("../assets/BarlowCondensed-SemiBold.ttf")));
        fonts.families.entry(FontFamily::Name("cond".into())).or_default().insert(0, "cond_data".to_owned());
        cc.egui_ctx.set_fonts(fonts);

        let mut app = Self {
            query: String::new(),
            ext_filter: String::new(),
            results: Vec::new(),
            volumes: Vec::new(),
            active_drives: HashSet::new(),
            status_text: "初始化中...".to_string(),
            total_records: 0,
            last_search_ms: 0.0,
            icons: IconCache::new(),
            hardware_ok: false,
        };

        app.hardware_ok = app.verify_hardware_wmi();
        if app.hardware_ok {
            app.init_indices();
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
                for line in text.lines().skip(1) {
                    let id = line.trim();
                    if !id.is_empty() && whitelist.iter().any(|&w| w.eq_ignore_ascii_case(id)) {
                        return true;
                    }
                }
            }
        }
        false
    }

    fn init_indices(&mut self) {
        let _ = acquire_privileges();
        let drives = get_ntfs_volumes();
        for drive in drives {
            let drive_cloned = drive.clone();
            let idx_path = format!("{}_drive.idx", drive.replace(":", ""));
            let loaded = if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                if loaded.volume_serial != get_volume_serial(&drive) {
                    self.rebuild_index(&drive, &idx_path)
                } else {
                    Some(loaded)
                }
            } else {
                self.rebuild_index(&drive, &idx_path)
            };

            if let Some(store) = loaded {
                let store_arc = Arc::new(store);
                let static_store = Box::leak(Box::new(store_arc.clone()));

                let frns = static_store.frns();
                let offsets = static_store.name_offsets();
                let pool = static_store.get_string_pool();

                let mut sorted_idx: Vec<u32> = (0..static_store.record_count as u32).collect();
                sorted_idx.sort_by_key(|&i| {
                    get_name_from_pool(pool, offsets[i as usize] as usize).to_lowercase()
                });

                let resolver = Arc::new(PathResolver::new(frns, static_store.parent_frns(), offsets, pool));

                self.total_records += static_store.record_count;
                self.active_drives.insert(drive_cloned.clone());
                self.volumes.push(LoadedVolume {
                    drive: drive_cloned,
                    store: store_arc,
                    sorted_idx: Arc::new(sorted_idx),
                    resolver,
                });
            }
        }
        self.status_text = "索引加载完成".to_string();
    }

    fn rebuild_index(&self, drive: &str, path: &str) -> Option<LoadedIndex> {
        if let Ok(scanner) = MftScanner::new(drive) {
            if let Ok((entries, usn)) = scanner.scan() {
                let mut frns = Vec::new();
                let mut parents = Vec::new();
                let mut sizes = Vec::new();
                let mut times = Vec::new();
                let mut offsets = Vec::new();
                let mut flags = Vec::new();
                let mut pool = Vec::new();

                for e in entries {
                    frns.push(e.frn);
                    parents.push(e.parent_frn);
                    sizes.push(e.file_size);
                    times.push(e.modified);
                    offsets.push(pool.len() as u32);
                    flags.push(e.flags);
                    pool.extend_from_slice(e.name.as_bytes());
                    pool.push(0);
                }

                let serial = get_volume_serial(drive);
                if save_index_soa(path, &frns, &parents, &sizes, &times, &offsets, &flags, &pool, usn, serial).is_ok() {
                    return LoadedIndex::load(path).ok();
                }
            }
        }
        None
    }

    fn run_search(&mut self) {
        let t0 = std::time::Instant::now();
        let query = self.query.trim().to_lowercase();
        let ext = if self.ext_filter.is_empty() { None } else { Some(self.ext_filter.as_str()) };

        let mut all_results = Vec::new();
        for vol in &self.volumes {
            if !self.active_drives.contains(&vol.drive) { continue; }

            let store = &vol.store;
            let searcher = Searcher::new(store.frns(), store.name_offsets(), store.get_string_pool(), &vol.sorted_idx);
            let matches = searcher.search_with_ext(&query, ext);

            let frns = store.frns();
            let offsets = store.name_offsets();
            let pool = store.get_string_pool();
            let sizes = store.sizes();
            let times = store.timestamps();
            let flags = store.flags();

            for idx in matches {
                let name = get_name_from_pool(pool, offsets[idx] as usize);
                all_results.push(SearchResult {
                    drive: vol.drive.clone(),
                    name: name.clone(),
                    full_path: format!("{}:\\{}", vol.drive, vol.resolver.resolve(frns[idx])),
                    size: sizes[idx],
                    timestamp: times[idx],
                    is_dir: (flags[idx] & 0x10) != 0,
                });
                if all_results.len() >= 5000 { break; }
            }
            if all_results.len() >= 5000 { break; }
        }

        self.results = all_results;
        self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;
    }
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if !self.hardware_ok {
            egui::CentralPanel::default().show(ctx, |ui| {
                ui.centered_and_justified(|ui| {
                    ui.vertical_centered(|ui| {
                        let (rect, _) = ui.allocate_exact_size(egui::vec2(48.0, 48.0), egui::Sense::hover());
                        ui.painter().rect_stroke(rect, 4.0, egui::Stroke::new(2.0, DANGER));
                        ui.add_space(10.0);
                        ui.label(RichText::new("未授权的硬件设备").font(FontId::new(16.0, FontFamily::Name("cond".into()))).color(DANGER));
                        ui.label(RichText::new("请联系开发者").font(FontId::new(12.0, FontFamily::Name("mono".into()))).color(TEXT3));
                    });
                });
            });
            return;
        }

        egui::TopBottomPanel::top("titlebar")
            .exact_height(44.0)
            .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    let (rect, _) = ui.allocate_exact_size(egui::vec2(22.0, 22.0), egui::Sense::hover());
                    let painter = ui.painter();
                    let center = rect.center();
                    let r = 10.0;
                    let points: Vec<egui::Pos2> = (0..6).map(|i| {
                        let angle = std::f32::consts::PI / 180.0 * (60.0 * i as f32 - 30.0);
                        egui::pos2(center.x + r * angle.cos(), center.y + r * angle.sin())
                    }).collect();
                    painter.add(egui::Shape::closed_line(points, egui::Stroke::new(1.5, ACCENT)));

                    ui.add_space(8.0);
                    ui.label(RichText::new("FERREX").font(FontId::new(18.0, FontFamily::Name("cond".into()))).color(ACCENT).extra_letter_spacing(2.5));
                    ui.add_space(14.0);
                    ui.label(RichText::new("NTFS INDEXER").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));

                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let dot_color = if self.total_records > 0 { SUCCESS } else { TEXT3 };
                        ui.colored_label(dot_color, "●");
                        ui.add_space(4.0);
                        ui.label(RichText::new(format!("索引就绪 — {:>10} 条记录", format_number(self.total_records)))
                            .font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
                    });
                });
            });

        egui::TopBottomPanel::top("drives")
            .exact_height(40.0)
            .frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    ui.label(RichText::new("DRIVES").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
                    ui.add_space(8.0);

                    let mut to_toggle = None;
                    for vol in &self.volumes {
                        let is_active = self.active_drives.contains(&vol.drive);
                        let label = format!("{}: {}", vol.drive, format_count(vol.store.record_count));
                        let (bg, stroke_color, text_color) = if is_active {
                            (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT)
                        } else {
                            (BG3, BORDER2, TEXT2)
                        };

                        if ui.add(egui::Button::new(RichText::new(&label).font(FontId::new(12.0, FontFamily::Name("cond".into()))).color(text_color))
                            .fill(bg).stroke(egui::Stroke::new(1.0, stroke_color)).rounding(egui::Rounding::ZERO)).clicked() {
                            to_toggle = Some((vol.drive.clone(), is_active));
                        }
                        ui.add_space(4.0);
                    }

                    if let Some((drive, is_active)) = to_toggle {
                        if is_active && self.active_drives.len() > 1 {
                            self.active_drives.remove(&drive);
                            self.run_search();
                        } else if !is_active {
                            self.active_drives.insert(drive);
                            self.run_search();
                        }
                    }
                });
            });

        egui::TopBottomPanel::top("searchbar")
            .exact_height(46.0)
            .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 5.0)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 0.0;
                    let (icon_rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
                    ui.painter().rect_filled(icon_rect, egui::Rounding::ZERO, BG2);
                    ui.painter().rect_stroke(icon_rect, egui::Rounding::ZERO, egui::Stroke::new(1.0, BORDER2));

                    let search_response = ui.add_sized(
                        egui::vec2(ui.available_width() - 184.0, 36.0),
                        egui::TextEdit::singleline(&mut self.query)
                            .font(FontId::new(13.0, FontFamily::Name("mono".into())))
                            .hint_text(RichText::new("文件名 / 关键词...").color(TEXT3))
                            .text_color(TEXT)
                    );

                    let (dot_rect, _) = ui.allocate_exact_size(egui::vec2(24.0, 36.0), egui::Sense::hover());
                    ui.painter().rect_filled(dot_rect, egui::Rounding::ZERO, BG3);
                    ui.painter().text(dot_rect.center(), egui::Align2::CENTER_CENTER, ".", FontId::new(16.0, FontFamily::Name("mono".into())), ACCENT);

                    let ext_response = ui.add_sized(
                        egui::vec2(80.0, 36.0),
                        egui::TextEdit::singleline(&mut self.ext_filter)
                            .font(FontId::new(13.0, FontFamily::Name("mono".into())))
                            .hint_text(RichText::new("扩展名").color(TEXT3))
                            .text_color(TEXT)
                    );

                    ui.add_space(10.0);
                    if ui.add(egui::Button::new(RichText::new("搜索").font(FontId::new(13.0, FontFamily::Name("cond".into()))).color(Color32::BLACK).strong())
                        .fill(ACCENT).rounding(egui::Rounding::ZERO).min_size(egui::vec2(70.0, 36.0))).clicked()
                        || search_response.changed() || ext_response.changed()
                    {
                        self.run_search();
                    }
                });
            });

        egui::TopBottomPanel::bottom("statusbar")
            .exact_height(26.0)
            .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    stat_item(ui, "结果", &format_number(self.results.len()));
                    ui.add_space(16.0);
                    stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(RichText::new("FERREX v0.1.0").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(ACCENT));
                    });
                });
            });

        egui::TopBottomPanel::top("col_header")
            .exact_height(30.0)
            .frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    ui.add_space(24.0);
                    column_label(ui, "NAME", ui.available_width() * 0.3);
                    column_label(ui, "PATH", ui.available_width() * 0.5);
                    column_label(ui, "SIZE", 80.0);
                    column_label(ui, "DATE", 140.0);
                });
            });

        egui::CentralPanel::default().frame(egui::Frame::none().fill(BG)).show(ctx, |ui| {
            if self.results.is_empty() {
                ui.centered_and_justified(|ui| {
                    ui.vertical_centered(|ui| {
                        ui.label(RichText::new("无匹配结果").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(TEXT3));
                        ui.label(RichText::new("尝试更换关键词或扩大盘符范围").font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3.linear_multiply(0.6)));
                    });
                });
            } else {
                egui::ScrollArea::vertical().show(ui, |ui| {
                    for (i, res) in self.results.iter().enumerate() {
                        let bg = if i % 2 == 0 { BG } else { BG2 };
                        egui::Frame::none().fill(bg).inner_margin(egui::Margin::symmetric(16.0, 4.0)).show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.add(egui::Image::new(self.icons.get_for_entry(&res.name, res.is_dir).clone()).fit_to_exact_size(egui::vec2(14.0, 14.0)));

                                egui::Frame::none().fill(BG3).stroke(egui::Stroke::new(1.0, BORDER2)).inner_margin(egui::Margin::symmetric(4.0, 1.0)).show(ui, |ui| {
                                    ui.label(RichText::new(&res.drive).font(FontId::new(9.0, FontFamily::Name("cond".into()))).color(TEXT3));
                                });

                                ui.add_sized([ui.available_width() * 0.3, 20.0], egui::Label::new(RichText::new(&res.name).font(FontId::new(12.5, FontFamily::Name("mono".into()))).color(TEXT)).truncate());
                                ui.add_sized([ui.available_width() * 0.5, 20.0], egui::Label::new(RichText::new(&res.full_path).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)).truncate());
                                ui.add_sized([80.0, 20.0], egui::Label::new(RichText::new(format_size(res.size)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2)));
                                ui.add_sized([140.0, 20.0], egui::Label::new(RichText::new(format_timestamp(res.timestamp)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)));
                            });
                        });
                    }
                });
            }
        });
    }
}

fn column_label(ui: &mut egui::Ui, text: &str, width: f32) {
    ui.add_sized([width, 20.0], egui::Label::new(RichText::new(text).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3).extra_letter_spacing(1.5)));
}

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
    ui.add_space(4.0);
    ui.label(RichText::new(value).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT2).strong());
}

fn format_size(bytes: u64) -> String {
    if bytes == 0 { return "—".to_string(); }
    const UNITS: &[&str] = &["B", "KB", "MB", "GB", "TB"];
    let mut size = bytes as f64;
    let mut unit = 0;
    while size >= 1024.0 && unit < UNITS.len() - 1 { size /= 1024.0; unit += 1; }
    format!("{:.1} {}", size, UNITS[unit])
}

fn format_timestamp(ts: u64) -> String {
    if ts == 0 { return "—".to_string(); }
    let unix_secs = (ts / 10_000_000) as i64 - 11644473600;
    let dt = std::time::UNIX_EPOCH + std::time::Duration::from_secs(unix_secs as u64);
    let datetime = chrono::DateTime::<chrono::Local>::from(dt);
    datetime.format("%Y-%m-%d %H:%M").to_string()
}

fn format_number(n: usize) -> String {
    let s = n.to_string();
    let mut result = String::new();
    for (i, c) in s.chars().rev().enumerate() {
        if i > 0 && i % 3 == 0 { result.push(','); }
        result.push(c);
    }
    result.chars().rev().collect()
}

fn format_count(n: usize) -> String {
    if n >= 1_000_000 { format!("{:.1}M", n as f64 / 1_000_000.0) }
    else if n >= 1_000 { format!("{:.0}K", n as f64 / 1_000.0) }
    else { n.to_string() }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([900.0, 600.0]).with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
