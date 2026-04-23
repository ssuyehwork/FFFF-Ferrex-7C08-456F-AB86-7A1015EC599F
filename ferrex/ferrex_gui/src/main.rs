#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use egui::{Color32, TextureHandle, ColorImage};
use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner};
use storage::{IndexStore, MappedIndex};
use search::Searcher;
use std::sync::Arc;
use std::collections::{HashMap, HashSet};
use std::process::Command;
use std::time::Instant;

// Windows API imports
#[cfg(target_os = "windows")]
use windows::{
    core::*,
    Win32::UI::Shell::*,
    Win32::Graphics::Gdi::*,
    Win32::UI::WindowsAndMessaging::*,
    Win32::Foundation::*,
    Win32::Storage::FileSystem::*,
};

// --- Colors ---
const PANEL: Color32 = Color32::from_rgb(13, 16, 20);
const BG: Color32 = Color32::from_rgb(7, 9, 11);
const BG2: Color32 = Color32::from_rgb(17, 21, 25);
const BG3: Color32 = Color32::from_rgb(22, 27, 32);
const BORDER2: Color32 = Color32::from_rgb(37, 46, 55);
const ACCENT: Color32 = Color32::from_rgb(255, 140, 0);
const TEXT: Color32 = Color32::from_rgb(200, 212, 220);
const TEXT2: Color32 = Color32::from_rgb(122, 143, 158);
const TEXT3: Color32 = Color32::from_rgb(61, 80, 96);
const SUCCESS: Color32 = Color32::from_rgb(46, 204, 113);
const DANGER: Color32 = Color32::from_rgb(231, 76, 60);

/// Icon Cache Pool for Windows Native Icons
struct IconCache {
    textures: HashMap<String, TextureHandle>,
}

impl IconCache {
    fn new() -> Self {
        Self {
            textures: HashMap::new(),
        }
    }

    fn get(&mut self, ctx: &egui::Context, name: &str, is_dir: bool) -> &TextureHandle {
        let key = if is_dir {
            "__folder__".to_string()
        } else {
            name.rsplit('.').next().unwrap_or("").to_lowercase()
        };

        if !self.textures.contains_key(&key) {
            let image = self.load_system_icon(&key, is_dir);
            let handle = ctx.load_texture(
                format!("icon_{}", key),
                image,
                Default::default()
            );
            self.textures.insert(key.clone(), handle);
        }
        self.textures.get(&key).unwrap()
    }

    #[cfg(target_os = "windows")]
    fn load_system_icon(&self, extension: &str, is_dir: bool) -> ColorImage {
        unsafe {
            let mut shfi = SHFILEINFOW::default();
            let flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
            let attr = if is_dir { FILE_ATTRIBUTE_DIRECTORY } else { FILE_ATTRIBUTE_NORMAL };
            let path = if is_dir {
                w!("C:\\Windows")
            } else {
                HSTRING::from(format!("file.{}", extension))
            };

            SHGetFileInfoW(
                PCWSTR(path.as_ptr()),
                attr,
                Some(&mut shfi),
                std::mem::size_of::<SHFILEINFOW>() as u32,
                flags,
            );

            if shfi.hIcon.is_invalid() {
                return ColorImage::new([16, 16], Color32::TRANSPARENT);
            }

            let image = hicon_to_color_image(shfi.hIcon);
            let _ = DestroyIcon(shfi.hIcon);
            image
        }
    }

    #[cfg(not(target_os = "windows"))]
    fn load_system_icon(&self, _ext: &str, is_dir: bool) -> ColorImage {
        if is_dir {
            ColorImage::new([16, 16], Color32::from_rgb(201, 110, 0))
        } else {
            ColorImage::new([16, 16], Color32::from_rgb(122, 143, 158))
        }
    }
}

#[cfg(target_os = "windows")]
unsafe fn hicon_to_color_image(h_icon: HICON) -> ColorImage {
    let mut icon_info = ICONINFO::default();
    if !GetIconInfo(h_icon, &mut icon_info).as_bool() {
        return ColorImage::new([16, 16], Color32::TRANSPARENT);
    }

    let h_dc = GetDC(None);
    let mut bmi = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: 16,
            biHeight: -16,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB as u32,
            ..Default::default()
        },
        ..Default::default()
    };

    let mut pixels = vec![0u8; 16 * 16 * 4];
    GetDIBits(
        h_dc,
        icon_info.hbmColor,
        0,
        16,
        Some(pixels.as_mut_ptr() as *mut _),
        &mut bmi,
        DIB_RGB_COLORS,
    );

    let _ = DeleteObject(icon_info.hbmColor);
    let _ = DeleteObject(icon_info.hbmMask);
    let _ = ReleaseDC(None, h_dc);

    for chunk in pixels.chunks_exact_mut(4) {
        chunk.swap(0, 2);
    }

    ColorImage::from_rgba_unmultiplied([16, 16], &pixels)
}

struct SearchResult {
    drive: String,
    name: String,
    full_path: String,
    size: u64,
    _timestamp: u64,
    is_dir: bool,
}

struct FerrexApp {
    query: String,
    ext_filter: String,
    results: Vec<SearchResult>,
    stores: Vec<(String, Arc<IndexStore>)>,
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

        let mut fonts = egui::FontDefinitions::default();
        fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().clear();
        fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().clear();

        let yahei_paths = [
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\msyhl.ttc",
        ];
        for path in yahei_paths {
            if let Ok(font_data) = std::fs::read(path) {
                fonts.font_data.insert("yahei".to_owned(), egui::FontData::from_owned(font_data));
                fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().push("yahei".to_owned());
                fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().push("yahei".to_owned());
                break;
            }
        }
        cc.egui_ctx.set_fonts(fonts);

        let mut app = Self {
            query: String::new(),
            ext_filter: String::new(),
            results: Vec::new(),
            stores: Vec::new(),
            active_drives: HashSet::new(),
            status_text: "正在初始化系统...".to_string(),
            total_records: 0,
            last_search_ms: 0.0,
            icons: IconCache::new(),
            hardware_ok: false,
        };

        app.hardware_ok = app.verify_hardware_wmi();
        if app.hardware_ok {
            app.load_or_build_all_indices();
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

    fn load_or_build_all_indices(&mut self) {
        if let Err(_) = acquire_privileges() {
            self.status_text = "权限不足，请确认以管理员身份运行。".to_string();
            return;
        }
        let volumes = get_ntfs_volumes();
        for vol in volumes {
            let drive_letter = vol.replace(":", "");
            let idx_path = format!("{}_drive.idx", drive_letter);

            let mut store = IndexStore::new();
            if let Ok(mapped) = MappedIndex::load(&idx_path) {
                let record_raw_ptr = unsafe {
                    mapped.mmap.as_ptr().add(storage::HEADER_SIZE) as *const storage::FileRecord
                };
                let records = unsafe { std::slice::from_raw_parts(record_raw_ptr, mapped.record_count) };
                let pool = &mapped.mmap[mapped.string_pool_offset..];
                for r in records {
                    store.frns.push(r.frn);
                    store.parent_frns.push(r.parent_frn);
                    store.sizes.push(r.size);
                    store.timestamps.push(r.timestamp);
                    store.flags.push(r.flags);
                    store.name_offsets.push(r.name_offset);
                }
                store.string_pool = pool.to_vec();
                store.usn_watermark = mapped.usn_watermark;
                store.volume_serial = mapped.volume_serial;
            } else {
                if let Ok(scanner) = MftScanner::new(&vol) {
                    if let Ok(raw_entries) = scanner.scan() {
                        for entry in raw_entries {
                            let offset = store.string_pool.len() as u32;
                            store.string_pool.extend_from_slice(entry.name.as_bytes());
                            store.string_pool.push(0);
                            store.frns.push(entry.frn);
                            store.parent_frns.push(entry.parent_frn);
                            store.sizes.push(entry.file_size);
                            store.timestamps.push(entry.modified);
                            store.name_offsets.push(offset);
                            store.flags.push(entry.flags);
                        }
                        let _ = storage::save_index(&idx_path, &store);
                    }
                }
            }

            if !store.frns.is_empty() {
                store.rebuild_lookups();
                let count = store.frns.len();
                self.stores.push((drive_letter.clone(), Arc::new(store)));
                self.active_drives.insert(drive_letter);
                self.total_records += count;
            }
        }
        self.status_text = if self.stores.is_empty() { "未发现可用驱动器".to_string() } else { format!("就绪 - {} 卷", self.stores.len()) };
    }

    fn run_search(&mut self) {
        let t0 = Instant::now();
        let query = self.query.trim().to_lowercase();
        let ext = if self.ext_filter.trim().is_empty() { None } else { Some(self.ext_filter.trim()) };

        let mut results = Vec::new();
        for (drive, store) in &self.stores {
            if !self.active_drives.contains(drive) { continue; }
            let searcher = Searcher::new(
                &store.frns, &store.parent_frns, &store.sizes, &store.timestamps,
                &store.flags, &store.name_offsets, &store.string_pool
            );
            let matches = searcher.search_with_ext(&query, ext);
            for idx in matches {
                let name = pool_get_name(&store.string_pool, store.name_offsets[idx] as usize);
                let path = store.get_path(idx);
                results.push(SearchResult {
                    drive: drive.clone(),
                    name: name.clone(),
                    full_path: format!("{}:\\{}", drive, path),
                    size: store.sizes[idx],
                    _timestamp: store.timestamps[idx],
                    is_dir: (store.flags[idx] & 0x10) != 0,
                });
            }
        }
        self.results = results;
        self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;
    }
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let mut visuals = egui::Visuals::dark();
        visuals.override_text_color = Some(TEXT);
        visuals.widgets.noninteractive.bg_fill = BG;
        ctx.set_visuals(visuals);

        if !self.hardware_ok {
            egui::CentralPanel::default().show(ctx, |ui| {
                ui.centered_and_justified(|ui| {
                    ui.vertical_centered(|ui| {
                        let (rect, _) = ui.allocate_exact_size(egui::vec2(48.0, 48.0), egui::Sense::hover());
                        let painter = ui.painter();
                        let c = rect.center();
                        painter.rect_stroke(egui::Rect::from_center_size(egui::pos2(c.x, c.y + 4.0), egui::vec2(24.0, 18.0)), 2.0, egui::Stroke::new(2.0, DANGER));
                        painter.circle_stroke(egui::pos2(c.x, c.y - 4.0), 8.0, egui::Stroke::new(2.0, DANGER));
                        ui.add_space(16.0);
                        ui.heading(egui::RichText::new("未授权的硬件设备").color(DANGER));
                        ui.label(egui::RichText::new("请联系开发者").color(TEXT3));
                    });
                });
            });
            return;
        }

        // 1. TITLEBAR
        egui::TopBottomPanel::top("titlebar").exact_height(44.0).frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
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
                    ui.label(egui::RichText::new("FERREX").color(ACCENT).strong().extra_letter_spacing(2.5));
                    ui.add_space(14.0);
                    ui.label(egui::RichText::new("NTFS INDEXER").size(10.0).color(TEXT3));
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let (dot_rect, _) = ui.allocate_exact_size(egui::vec2(12.0, 12.0), egui::Sense::hover());
                        ui.painter().circle_filled(dot_rect.center(), 4.0, if self.total_records > 0 { SUCCESS } else { TEXT3 });
                        ui.label(egui::RichText::new(format!("索引就绪 — {} 条记录", format_number(self.total_records))).size(10.0).color(TEXT3));
                    });
                });
            });

        // 2. DRIVE SELECTOR
        egui::TopBottomPanel::top("drives").exact_height(40.0).frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    ui.label(egui::RichText::new("DRIVES").size(10.0).color(TEXT3));
                    ui.add_space(8.0);
                    let mut toggled_drive = None;
                    for (drive, store) in &self.stores {
                        let is_active = self.active_drives.contains(drive);
                        let (bg, stroke, text_color) = if is_active { (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT) } else { (BG3, BORDER2, TEXT2) };
                        if ui.add(egui::Button::new(egui::RichText::new(format!("{}: {}", drive, format_count(store.frns.len()))).color(text_color).size(12.0)).fill(bg).stroke(egui::Stroke::new(1.0, stroke)).rounding(0.0)).clicked() {
                            toggled_drive = Some((drive.clone(), is_active));
                        }
                    }
                    if let Some((drive, was_active)) = toggled_drive {
                        if was_active && self.active_drives.len() > 1 { self.active_drives.remove(&drive); }
                        else if !was_active { self.active_drives.insert(drive); }
                        self.run_search();
                    }
                });
            });

        // 3. SEARCH BAR
        egui::TopBottomPanel::top("searchbar").exact_height(46.0).frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 5.0)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 0.0;
                    let (icon_rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
                    ui.painter().rect_filled(icon_rect, 0.0, BG2);
                    ui.painter().rect_stroke(icon_rect, 0.0, egui::Stroke::new(1.0, BORDER2));
                    let c = icon_rect.center();
                    ui.painter().circle_stroke(egui::pos2(c.x-2.0, c.y-2.0), 5.5, egui::Stroke::new(1.5, TEXT3));
                    ui.painter().line_segment([egui::pos2(c.x+2.0, c.y+2.0), egui::pos2(c.x+6.0, c.y+6.0)], egui::Stroke::new(1.5, TEXT3));
                    let search_res = ui.add_sized(egui::vec2(ui.available_width() - 184.0, 36.0), egui::TextEdit::singleline(&mut self.query).hint_text("文件名 / 关键词...").frame(true));
                    let (dot_rect, _) = ui.allocate_exact_size(egui::vec2(24.0, 36.0), egui::Sense::hover());
                    ui.painter().rect_filled(dot_rect, 0.0, BG3);
                    ui.painter().text(dot_rect.center(), egui::Align2::CENTER_CENTER, ".", egui::FontId::monospace(16.0), ACCENT);
                    let ext_res = ui.add_sized(egui::vec2(80.0, 36.0), egui::TextEdit::singleline(&mut self.ext_filter).hint_text("扩展名").frame(true));
                    ui.add_space(10.0);
                    if ui.add(egui::Button::new(egui::RichText::new("搜索").color(Color32::BLACK).strong()).fill(ACCENT).min_size(egui::vec2(70.0, 36.0))).clicked() || search_res.changed() || ext_res.changed() {
                        self.run_search();
                    }
                });
            });

        // 4. COLUMN HEADER
        egui::TopBottomPanel::top("col_header").exact_height(30.0).frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    ui.add_space(18.0);
                    ui.label(egui::RichText::new("名称").size(10.0).color(TEXT3).extra_letter_spacing(1.5));
                    ui.add_space(200.0);
                    ui.label(egui::RichText::new("路径").size(10.0).color(TEXT3).extra_letter_spacing(1.5));
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(egui::RichText::new("大小").size(10.0).color(TEXT3).extra_letter_spacing(1.5));
                    });
                });
            });

        // 5. STATUS BAR
        egui::TopBottomPanel::bottom("statusbar").exact_height(26.0).frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    stat_item(ui, "结果", &format_number(self.results.len()));
                    ui.add_space(16.0);
                    stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(egui::RichText::new("FERREX v0.1.0").color(ACCENT).size(10.0));
                    });
                });
            });

        // 6. RESULTS LIST
        egui::CentralPanel::default().show(ctx, |ui| {
            if self.results.is_empty() && !self.query.is_empty() {
                ui.centered_and_justified(|ui| {
                    ui.vertical_centered(|ui| {
                        ui.label(egui::RichText::new("无匹配结果").color(TEXT3).size(14.0));
                        ui.label(egui::RichText::new("尝试更换关键词或扩大盘符范围").color(TEXT3).size(11.0));
                    });
                });
            } else {
                egui::ScrollArea::vertical().show(ui, |ui| {
                    for (i, res) in self.results.iter().take(200).enumerate() {
                        let bg = if i % 2 == 0 { BG } else { BG2 };
                        egui::Frame::none().fill(bg).show(ui, |ui| {
                            ui.horizontal(|ui| {
                                let icon_handle = self.icons.get(ctx, &res.name, res.is_dir);
                                ui.add(egui::Image::new(icon_handle).fit_to_exact_size(egui::vec2(14.0, 14.0)));

                                ui.add(egui::Label::new(egui::RichText::new(format!("{}:", res.drive)).size(9.0).color(TEXT3)).truncate());
                                ui.add_sized(egui::vec2(200.0, 18.0), egui::Label::new(egui::RichText::new(&res.name).color(TEXT)).truncate());
                                ui.label(egui::RichText::new(&res.full_path).color(TEXT3).size(11.0));

                                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                    ui.label(egui::RichText::new(format_size(res.size)).color(TEXT2).size(11.0));
                                });
                            });
                        });
                    }
                });
            }
        });
    }
}

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.label(egui::RichText::new(label).size(10.0).color(TEXT3));
    ui.add_space(4.0);
    ui.label(egui::RichText::new(value).size(10.0).color(TEXT2).strong());
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

fn format_size(bytes: u64) -> String {
    if bytes == 0 { return "—".to_string(); }
    const UNITS: &[&str] = &["B", "KB", "MB", "GB"];
    let mut size = bytes as f64;
    let mut unit = 0;
    while size >= 1024.0 && unit < UNITS.len() - 1 { size /= 1024.0; unit += 1; }
    format!("{:.2} {}", size, UNITS[unit])
}

fn pool_get_name(pool: &[u8], offset: usize) -> String {
    if offset >= pool.len() { return String::new(); }
    let slice = &pool[offset..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).to_string()
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([750.0, 550.0]).with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
