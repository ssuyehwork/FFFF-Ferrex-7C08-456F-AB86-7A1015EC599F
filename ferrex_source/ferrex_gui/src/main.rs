use eframe::egui;
use egui::{Color32, RichText, FontId, FontFamily};
use std::sync::Arc;
use std::collections::{HashSet, HashMap};
use std::process::Command;
use std::time::{Instant, Duration};
use storage::{IndexStore, LoadedIndex, MappedIndex, pool_get_name, pool_get_name_lower, resolve_path};
use search::{Searcher, SearchOptions};
use indexer::{UsnEvent, spawn_usn_watcher, get_ntfs_volumes};
use lru::LruCache;
use sysinfo::{System, SystemExt, CpuExt};
use windows::Win32::Foundation::HWND;
use windows::Win32::UI::Input::KeyboardAndMouse::{RegisterHotKey, UnregisterHotKey, MOD_ALT};
use windows::Win32::UI::WindowsAndMessaging::{PeekMessageW, PM_REMOVE, MSG, WM_HOTKEY};
use windows::Win32::System::Registry::*;
use windows::core::{HSTRING, w, PCWSTR};

const BG: Color32 = Color32::from_rgb(7, 9, 11);
const PANEL: Color32 = Color32::from_rgb(13, 16, 20);
const BG2: Color32 = Color32::from_rgb(17, 21, 25);
const BG3: Color32 = Color32::from_rgb(22, 27, 32);
const BORDER: Color32 = Color32::from_rgb(30, 37, 44);
const BORDER2: Color32 = Color32::from_rgb(37, 46, 55);
const ACCENT: Color32 = Color32::from_rgb(255, 140, 0);
const ACCENT2: Color32 = Color32::from_rgb(201, 110, 0);
const TEXT: Color32 = Color32::from_rgb(200, 212, 220);
const TEXT2: Color32 = Color32::from_rgb(122, 143, 158);
const TEXT3: Color32 = Color32::from_rgb(61, 80, 96);
const SUCCESS: Color32 = Color32::from_rgb(46, 204, 113);
const DANGER: Color32 = Color32::from_rgb(231, 76, 60);

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
        .unwrap_or(self.map.get("file").unwrap())
    }
}

struct VolumeStore {
    drive: String,
    index: Arc<LoadedIndex>,
    sorted_idx: Vec<u32>,
    frn_map: HashMap<u64, u32>,
    path_cache: LruCache<u64, String>,
    usn_rx: Option<std::sync::mpsc::Receiver<UsnEvent>>,
    record_count: usize,
}

struct SearchResult {
    drive: String,
    store_idx: usize,
    rec_idx: u32,
    name: String,
    full_path: String,
    size: u64,
    timestamp: u64,
    is_dir: bool,
}

#[derive(PartialEq, Clone, Copy)]
enum SortColumn { Name, Path, Size, Date }

enum ScanProgress {
    Progress { done: usize, total: usize },
    Done { drive: String, idx_path: String },
    Error(String),
}

struct FerrexApp {
    stores: Vec<VolumeStore>,
    active_drives: HashSet<String>,
    query: String,
    ext_filter: String,
    use_regex: bool,
    min_size_str: String,
    max_size_str: String,
    date_from_str: String,
    date_to_str: String,
    show_hidden: bool,
    show_system: bool,
    dirs_only: bool,
    results: Vec<SearchResult>,
    selected_rows: HashSet<usize>,
    last_search_ms: f64,
    sort_col: SortColumn,
    sort_asc: bool,
    search_history: Vec<String>,
    show_history: bool,
    hovered_row: Option<usize>,
    preview_pos: Option<egui::Pos2>,
    icons: IconCache,
    status_text: String,
    is_scanning: bool,
    scan_progress: f32,
    scan_rx: Option<std::sync::mpsc::Receiver<ScanProgress>>,
    hardware_ok: bool,
    exclude_paths: Vec<String>,
    sysinfo: System,
    last_sys_poll: Instant,
    mem_usage_mb: f32,
    cpu_usage: f32,
    tray: Option<tray_icon::TrayIcon>,
    context_menu_row: Option<usize>,
    show_filters: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        setup_fonts(&cc.egui_ctx);
        
        let mut app = Self {
            stores: Vec::new(),
            active_drives: HashSet::new(),
            query: String::new(),
            ext_filter: String::new(),
            use_regex: false,
            min_size_str: String::new(),
            max_size_str: String::new(),
            date_from_str: String::new(),
            date_to_str: String::new(),
            show_hidden: false,
            show_system: false,
            dirs_only: false,
            results: Vec::new(),
            selected_rows: HashSet::new(),
            last_search_ms: 0.0,
            sort_col: SortColumn::Name,
            sort_asc: true,
            search_history: Vec::new(),
            show_history: false,
            hovered_row: None,
            preview_pos: None,
            icons: IconCache::new(),
            status_text: "准备就绪".to_string(),
            is_scanning: false,
            scan_progress: 0.0,
            scan_rx: None,
            hardware_ok: false,
            exclude_paths: Vec::new(),
            sysinfo: System::new_all(),
            last_sys_poll: Instant::now(),
            mem_usage_mb: 0.0,
            cpu_usage: 0.0,
            tray: None,
            context_menu_row: None,
            show_filters: false,
        };

        app.hardware_ok = app.verify_hardware_wmi();
        if app.hardware_ok {
            app.load_volumes();
            app.setup_hotkey();
            app.setup_tray();
        }

        app
    }

    fn verify_hardware_wmi(&self) -> bool {
        let whitelist = [
            "BFEBFBFF000306C3", "SGH412RF00", "494000PA0D9L",
            "PHYS825203NX480BGN", "NA5360WJ", "NA7G89GQ", "03000210052122072519"
        ];
        let cmds = [
            "cpu get processorid",
            "baseboard get serialnumber",
            "bios get serialnumber",
        ];
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

    fn load_volumes(&mut self) {
        let volumes = get_ntfs_volumes();
        for vol in volumes {
            let drive_letter = vol.chars().next().unwrap().to_lowercase().to_string();
            let idx_path = format!("{}_drive.idx", drive_letter);
            
            if let Ok(mapped) = MappedIndex::load(&idx_path) {
                let mut store = IndexStore::new();
                let records_ptr = unsafe { mapped.mmap.as_ptr().add(storage::HEADER_SIZE) as *const storage::FileRecord };
                for i in 0..mapped.record_count {
                    let rec = unsafe { &*records_ptr.add(i) };
                    store.frns.push(rec.frn);
                    store.parent_frns.push(rec.parent_frn);
                    store.sizes.push(rec.size);
                    store.timestamps.push(rec.timestamp);
                    store.name_offsets.push(rec.name_offset);
                    store.flags.push(rec.flags);
                }
                let pool_ptr = unsafe { mapped.mmap.as_ptr().add(mapped.string_pool_offset) };
                let pool_len = mapped.mmap.len() - mapped.string_pool_offset;
                store.string_pool = unsafe { std::slice::from_raw_parts(pool_ptr, pool_len).to_vec() };

                let sorted_idx = store.build_sorted_idx();
                let frn_map = store.build_frn_map();
                let usn_rx = spawn_usn_watcher(&vol).ok();

                self.active_drives.insert(vol.clone());
                self.stores.push(VolumeStore {
                    drive: vol,
                    record_count: store.frns.len(),
                    index: Arc::new(store),
                    sorted_idx,
                    frn_map,
                    path_cache: LruCache::new(std::num::NonZeroUsize::new(10000).unwrap()),
                    usn_rx,
                });
            } else {
                let (tx, rx) = std::sync::mpsc::channel();
                self.scan_rx = Some(rx);
                self.is_scanning = true;
                spawn_scan(vol, tx);
            }
        }
    }

    fn setup_hotkey(&self) {
        unsafe {
            let _ = RegisterHotKey(HWND(0), 1001, MOD_ALT, 0x20); // Alt+Space
        }
    }

    fn setup_tray(&mut self) {
        // Placeholder for tray setup
    }

    fn process_usn_events(&mut self) {
        for store in &mut self.stores {
            if let Some(ref rx) = store.usn_rx {
                while let Ok(event) = rx.try_recv() {
                    match event {
                        UsnEvent::Renamed { old_frn, .. } | UsnEvent::Modified { frn: old_frn } => {
                            store.path_cache.pop(&old_frn);
                        }
                        _ => {}
                    }
                }
            }
        }
    }

    fn run_search(&mut self) {
        let t0 = Instant::now();
        
        let opts = SearchOptions {
            query: &self.query,
            use_regex: self.use_regex,
            ext_filter: if self.ext_filter.is_empty() { None } else { Some(&self.ext_filter) },
            min_size: parse_size(&self.min_size_str),
            max_size: parse_size(&self.max_size_str),
            include_dirs: !self.dirs_only,
            include_hidden: self.show_hidden,
            include_system: self.show_system,
            ..Default::default()
        };

        let mut all_results = Vec::new();
        for (idx, store) in self.stores.iter_mut().enumerate() {
            if !self.active_drives.contains(&store.drive) { continue; }

            let searcher = Searcher::new(
                &store.index.frns,
                &store.index.parent_frns,
                &store.index.sizes,
                &store.index.timestamps,
                &store.index.flags,
                &store.index.name_offsets,
                &store.index.string_pool,
            );

            let matches = searcher.search(&opts);
            for rec_idx in matches.into_iter().take(5000) {
                let name = pool_get_name(&store.index.string_pool, store.index.name_offsets[rec_idx] as usize);
                let full_path = resolve_path(
                    &store.drive, rec_idx as u32, &store.index.frns, &store.index.parent_frns,
                    &store.index.name_offsets, &store.index.string_pool, &store.frn_map, &mut store.path_cache
                );

                all_results.push(SearchResult {
                    drive: store.drive.clone(),
                    store_idx: idx,
                    rec_idx: rec_idx as u32,
                    name,
                    full_path,
                    size: store.index.sizes[rec_idx],
                    timestamp: store.index.timestamps[rec_idx],
                    is_dir: store.index.flags[rec_idx] & 0x10 != 0,
                });
                if all_results.len() >= 5000 { break; }
            }
            if all_results.len() >= 5000 { break; }
        }

        self.results = all_results;
        self.apply_sort();
        self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;

        if !self.query.is_empty() {
            self.search_history.retain(|h| h != &self.query);
            self.search_history.insert(0, self.query.clone());
            self.search_history.truncate(20);
        }
    }

    fn apply_sort(&mut self) {
        let asc = self.sort_asc;
        match self.sort_col {
            SortColumn::Name => self.results.sort_by(|a, b| {
                let c = a.name.to_lowercase().cmp(&b.name.to_lowercase());
                if asc { c } else { c.reverse() }
            }),
            SortColumn::Path => self.results.sort_by(|a, b| {
                let c = a.full_path.to_lowercase().cmp(&b.full_path.to_lowercase());
                if asc { c } else { c.reverse() }
            }),
            SortColumn::Size => self.results.sort_by(|a, b| {
                let c = a.size.cmp(&b.size);
                if asc { c } else { c.reverse() }
            }),
            SortColumn::Date => self.results.sort_by(|a, b| {
                let c = a.timestamp.cmp(&b.timestamp);
                if asc { c } else { c.reverse() }
            }),
        }
    }
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if !self.hardware_ok {
            self.show_lock_screen(ctx);
            return;
        }

        self.process_usn_events();
        self.handle_hotkey(ctx);
        self.update_stats();
        self.handle_scan_progress();

        egui::TopBottomPanel::top("titlebar").exact_height(44.0).frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0))).show(ctx, |ui| {
            self.draw_titlebar(ui);
        });

        egui::TopBottomPanel::top("drives").exact_height(40.0).frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0))).show(ctx, |ui| {
            self.draw_drive_selector(ui);
        });

        egui::TopBottomPanel::top("searchbar_area").frame(egui::Frame::none().fill(PANEL)).show(ctx, |ui| {
            ui.set_height(if self.show_filters { 86.0 } else { 46.0 });
            egui::Frame::none().inner_margin(egui::Margin::symmetric(16.0, 5.0)).show(ui, |ui| {
                self.draw_search_bar(ui);
                if self.show_filters {
                    ui.add_space(8.0);
                    self.draw_filter_strip(ui);
                }
            });
            if self.is_scanning {
                ui.add(egui::ProgressBar::new(self.scan_progress).text(RichText::new(&self.status_text).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(ACCENT)).fill(ACCENT).desired_height(2.0).rounding(egui::Rounding::ZERO));
            }
        });

        egui::TopBottomPanel::bottom("statusbar").exact_height(26.0).frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0))).show(ctx, |ui| {
            self.draw_status_bar(ui);
        });

        egui::CentralPanel::default().frame(egui::Frame::none().fill(BG)).show(ctx, |ui| {
            self.draw_column_header(ui);
            ui.add(egui::Separator::default().spacing(0.0).color(BORDER));
            self.draw_results_list(ui);
            self.draw_hover_preview(ctx);
        });
    }
}

impl FerrexApp {
    fn show_lock_screen(&self, ctx: &egui::Context) {
        egui::CentralPanel::default().frame(egui::Frame::none().fill(BG)).show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(ui.available_height() / 2.0 - 60.0);
                ui.label(RichText::new("未授权的硬件设备").font(FontId::new(18.0, FontFamily::Name("cond".into()))).color(DANGER));
                ui.add_space(8.0);
                ui.label(RichText::new("请联系开发者").font(FontId::new(12.0, FontFamily::Name("mono".into()))).color(TEXT3));
            });
        });
    }

    fn draw_titlebar(&self, ui: &mut egui::Ui) {
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
            ui.label(RichText::new("FERREX").font(FontId::new(18.0, FontFamily::Name("cond".into()))).color(ACCENT).letter_spacing(2.5));
            ui.add_space(14.0);
            ui.label(RichText::new("NTFS INDEXER").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let total_records: usize = self.stores.iter().map(|s| s.record_count).sum();
                let dot_color = if total_records > 0 { SUCCESS } else { TEXT3 };
                ui.colored_label(dot_color, "●");
                ui.add_space(4.0);
                ui.label(RichText::new(format!("索引就绪 — {:>10} 条记录", format_number(total_records))).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            });
        });
    }

    fn draw_drive_selector(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_centered(|ui| {
            ui.label(RichText::new("DRIVES").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            ui.add_space(8.0);

            for store in &self.stores {
                let is_active = self.active_drives.contains(&store.drive);
                let label = format!("{}:  {}", store.drive, format_count(store.record_count));

                let (bg, stroke_color, text_color) = if is_active {
                    (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT)
                } else {
                    (BG3, BORDER2, TEXT2)
                };

                let btn = egui::Button::new(RichText::new(&label).font(FontId::new(12.0, FontFamily::Name("cond".into()))).color(text_color))
                    .fill(bg).stroke(egui::Stroke::new(1.0, stroke_color)).rounding(egui::Rounding::ZERO);

                if ui.add(btn).clicked() {
                    if is_active && self.active_drives.len() > 1 {
                        self.active_drives.remove(&store.drive);
                    } else if !is_active {
                        self.active_drives.insert(store.drive.clone());
                    }
                    self.run_search();
                }
                ui.add_space(4.0);
            }
        });
    }

    fn draw_search_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;

            let (icon_rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
            ui.painter().rect_filled(icon_rect, egui::Rounding::ZERO, BG2);
            ui.painter().rect_stroke(icon_rect, egui::Rounding::ZERO, egui::Stroke::new(1.0, BORDER2));
            let c = egui::pos2(icon_rect.center().x - 2.0, icon_rect.center().y - 2.0);
            ui.painter().circle_stroke(c, 5.5, egui::Stroke::new(1.5, TEXT3));
            ui.painter().line_segment([egui::pos2(c.x + 4.0, c.y + 4.0), egui::pos2(c.x + 8.0, c.y + 8.0)], egui::Stroke::new(1.5, TEXT3));

            let search_response = ui.add_sized(
                egui::vec2(ui.available_width() - 80.0 - 24.0 - 80.0 - 100.0, 36.0),
                egui::TextEdit::singleline(&mut self.query)
                    .font(FontId::new(13.0, FontFamily::Name("mono".into())))
                    .hint_text(RichText::new("文件名 / 关键词...").color(TEXT3))
                    .frame(true).text_color(TEXT).cursor_color(ACCENT)
            );

            let (dot_rect, _) = ui.allocate_exact_size(egui::vec2(24.0, 36.0), egui::Sense::hover());
            ui.painter().rect_filled(dot_rect, egui::Rounding::ZERO, BG3);
            ui.painter().text(dot_rect.center(), egui::Align2::CENTER_CENTER, ".", FontId::new(16.0, FontFamily::Name("mono".into())), ACCENT);

            let ext_response = ui.add_sized(
                egui::vec2(80.0, 36.0),
                egui::TextEdit::singleline(&mut self.ext_filter)
                    .font(FontId::new(13.0, FontFamily::Name("mono".into())))
                    .hint_text(RichText::new("扩展名").color(TEXT3))
                    .frame(true).text_color(TEXT)
            );

            ui.add_space(10.0);

            if ui.add(egui::Button::new(RichText::new("搜索").font(FontId::new(13.0, FontFamily::Name("cond".into()))).color(Color32::BLACK).strong())
                .fill(ACCENT).min_size(egui::vec2(70.0, 36.0))).clicked() || search_response.changed() || ext_response.changed() {
                self.run_search();
            }

            ui.add_space(8.0);
            if ui.add(egui::Button::new(RichText::new("过滤器").font(FontId::new(11.0, FontFamily::Name("cond".into()))).color(if self.show_filters { ACCENT } else { TEXT3 }))
                .stroke(egui::Stroke::new(1.0, if self.show_filters { ACCENT } else { BORDER2 }))
                .fill(Color32::TRANSPARENT)).clicked() {
                self.show_filters = !self.show_filters;
            }
        });
    }

    fn draw_filter_strip(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 12.0;

            if filter_toggle(ui, "正则", &mut self.use_regex) { self.run_search(); }

            ui.label(RichText::new("大小 ≥").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(egui::TextEdit::singleline(&mut self.min_size_str).desired_width(60.0)).changed() { self.run_search(); }

            ui.label(RichText::new("≤").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(egui::TextEdit::singleline(&mut self.max_size_str).desired_width(60.0)).changed() { self.run_search(); }

            if filter_toggle(ui, "隐藏", &mut self.show_hidden) { self.run_search(); }
            if filter_toggle(ui, "系统", &mut self.show_system) { self.run_search(); }
            if filter_toggle(ui, "仅目录", &mut self.dirs_only) { self.run_search(); }
        });
    }

    fn draw_column_header(&self, ui: &mut egui::Ui) {
        ui.add_sized([ui.available_width(), 30.0], |ui: &mut egui::Ui| {
            ui.horizontal_centered(|ui| {
                ui.add_space(16.0 + 24.0);
                header_label(ui, "NAME", 300.0);
                header_label(ui, "PATH", ui.available_width() - 240.0);
                header_label(ui, "SIZE", 80.0);
                header_label(ui, "DATE", 140.0);
            }).response
        });
    }

    fn draw_results_list(&mut self, ui: &mut egui::Ui) {
        self.hovered_row = None;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            if self.results.is_empty() {
                self.draw_empty_state(ui);
                return;
            }

            for (idx, result) in self.results.iter().enumerate().take(200) {
                let is_selected = self.selected_rows.contains(&idx);
                let (rect, response) = ui.allocate_at_least(egui::vec2(ui.available_width(), 30.0), egui::Sense::click());

                let is_hovered = response.hovered();
                let bg = if is_selected { Color32::from_rgba_unmultiplied(255, 140, 0, 25) }
                         else if is_hovered { BG3 }
                         else if idx % 2 == 0 { BG }
                         else { BG2 };

                ui.painter().rect_filled(rect, egui::Rounding::ZERO, bg);

                if is_selected || is_hovered {
                    ui.painter().line_segment([rect.left_top(), rect.left_bottom()], egui::Stroke::new(3.0, ACCENT));
                }

                if is_hovered {
                    self.hovered_row = Some(idx);
                    self.preview_pos = Some(rect.right_top());
                }

                if response.clicked() {
                    let modifiers = ui.input(|i| i.modifiers);
                    if modifiers.shift {
                        // Range select (simplified)
                        self.selected_rows.insert(idx);
                    } else if modifiers.command {
                        if is_selected { self.selected_rows.remove(&idx); }
                        else { self.selected_rows.insert(idx); }
                    } else {
                        self.selected_rows.clear();
                        self.selected_rows.insert(idx);
                    }
                }

                if response.secondary_clicked() {
                    self.context_menu_row = Some(idx);
                }

                let mut ui = ui.child_ui(rect, egui::Layout::left_to_right(egui::Align::Center));
                ui.add_space(8.0);
                ui.add(egui::Image::new(self.icons.get_for_entry(&result.name, result.is_dir)).max_size(egui::vec2(14.0, 14.0)));
                ui.add_space(8.0);

                let (tag_rect, _) = ui.allocate_exact_size(egui::vec2(22.0, 14.0), egui::Sense::hover());
                ui.painter().rect_stroke(tag_rect, 1.0, egui::Stroke::new(1.0, BORDER2));
                ui.painter().text(tag_rect.center(), egui::Align2::CENTER_CENTER, &result.drive[..2], FontId::new(9.0, FontFamily::Name("cond".into())), TEXT3);
                ui.add_space(6.0);

                ui.add_sized([260.0, 20.0], egui::Label::new(RichText::new(&result.name).font(FontId::new(12.5, FontFamily::Name("mono".into()))).color(TEXT)).truncate(true));
                ui.add_sized([ui.available_width() - 230.0, 20.0], egui::Label::new(RichText::new(&result.full_path).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)).truncate(true));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.add_space(16.0);
                    ui.add_sized([130.0, 20.0], egui::Label::new(RichText::new(format_timestamp(result.timestamp)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)));
                    ui.add_sized([80.0, 20.0], egui::Label::new(RichText::new(format_size(result.size)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2)));
                });
            }
        });

        if let Some(idx) = self.context_menu_row {
            egui::Window::new("Context Menu").title_bar(false).resizable(false).show(ui.ctx(), |ui| {
                if ui.button("打开文件").clicked() { self.context_menu_row = None; }
                if ui.button("在资源管理器中定位").clicked() { self.context_menu_row = None; }
                if ui.button("复制路径").clicked() { self.context_menu_row = None; }
                if ui.button("属性").clicked() { self.context_menu_row = None; }
            });
            if ui.input(|i| i.pointer.any_click()) { self.context_menu_row = None; }
        }
    }

    fn draw_hover_preview(&self, ctx: &egui::Context) {
        if let (Some(idx), Some(pos)) = (self.hovered_row, self.preview_pos) {
            let result = &self.results[idx];
            egui::Area::new(egui::Id::new("preview")).fixed_pos(pos + egui::vec2(12.0, 0.0)).order(egui::Order::Tooltip).show(ctx, |ui| {
                egui::Frame::none().fill(PANEL).stroke(egui::Stroke::new(1.0, BORDER2)).inner_margin(egui::Margin::same(12.0)).show(ui, |ui| {
                    ui.set_min_width(260.0);
                    ui.label(RichText::new(&result.name).font(FontId::new(13.0, FontFamily::Name("mono".into()))).color(ACCENT));
                    ui.add_space(6.0);
                    preview_row(ui, "路径", &result.full_path);
                    preview_row(ui, "大小", &format_size(result.size));
                    preview_row(ui, "修改时间", &format_timestamp(result.timestamp));
                    preview_row(ui, "类型", if result.is_dir { "目录" } else { "文件" });
                });
            });
        }
    }

    fn draw_empty_state(&self, ui: &mut egui::Ui) {
        ui.vertical_centered(|ui| {
            ui.add_space(100.0);
            ui.label(RichText::new("无匹配结果").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(TEXT3));
            ui.label(RichText::new("尝试更换关键词或扩大盘符范围").font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3.gamma_multiply(0.6)));
        });
    }

    fn draw_status_bar(&self, ui: &mut egui::Ui) {
        ui.horizontal_centered(|ui| {
            stat_item(ui, "结果", &format_number(self.results.len()));
            ui.add_space(16.0);
            stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));

            if !self.selected_rows.is_empty() {
                ui.add_space(16.0);
                stat_item(ui, "已选", &format_number(self.selected_rows.len()));
            }

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.label(RichText::new("FERREX v0.1.0").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(ACCENT));
                ui.add_space(16.0);
                stat_item(ui, "CPU", &format!("{:.1}%", self.cpu_usage));
                ui.add_space(16.0);
                stat_item(ui, "MEM", &format!("{:.0}MB", self.mem_usage_mb));
            });
        });
    }

    fn handle_hotkey(&self, ctx: &egui::Context) {
        unsafe {
            let mut msg = MSG::default();
            while PeekMessageW(&mut msg, HWND(0), WM_HOTKEY, WM_HOTKEY, PM_REMOVE).as_bool() {
                if msg.wParam.0 == 1001 {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Focus);
                }
            }
        }
    }

    fn update_stats(&mut self) {
        if self.last_sys_poll.elapsed() >= Duration::from_secs(2) {
            self.sysinfo.refresh_memory();
            self.sysinfo.refresh_cpu_usage();
            self.mem_usage_mb = self.sysinfo.used_memory() as f32 / 1024.0 / 1024.0;
            self.cpu_usage = self.sysinfo.global_cpu_usage();
            self.last_sys_poll = Instant::now();
        }
    }

    fn handle_scan_progress(&mut self) {
        if let Some(ref rx) = self.scan_rx {
            while let Ok(msg) = rx.try_recv() {
                match msg {
                    ScanProgress::Progress { done, total } => {
                        self.scan_progress = if total == 0 { 0.0 } else { done as f32 / total as f32 };
                        self.status_text = format!("正在扫描 {}/{}", done, total);
                    }
                    ScanProgress::Done { drive, .. } => {
                        self.is_scanning = false;
                        self.scan_rx = None;
                        self.status_text = "扫描完成".to_string();
                    }
                    ScanProgress::Error(e) => {
                        self.status_text = format!("错误: {}", e);
                        self.is_scanning = false;
                    }
                }
            }
        }
    }
}

fn header_label(ui: &mut egui::Ui, text: &str, width: f32) {
    ui.add_sized([width, 20.0], egui::Label::new(RichText::new(text).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3).letter_spacing(1.5)));
}

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
    ui.add_space(4.0);
    ui.label(RichText::new(value).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT2).strong());
}

fn filter_toggle(ui: &mut egui::Ui, text: &str, value: &mut bool) -> bool {
    let color = if *value { ACCENT } else { TEXT3 };
    let stroke = egui::Stroke::new(1.0, if *value { ACCENT } else { BORDER2 });
    let btn = egui::Button::new(RichText::new(text).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(color))
        .fill(Color32::TRANSPARENT).stroke(stroke).rounding(egui::Rounding::ZERO);
    if ui.add(btn).clicked() {
        *value = !*value;
        true
    } else {
        false
    }
}

fn preview_row(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.horizontal(|ui| {
        ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
        ui.add_space(8.0);
        ui.label(RichText::new(value).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2));
    });
}

fn setup_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    #[cfg(windows)]
    {
        if let Ok(font_bytes) = std::fs::read("C:\\Windows\\Fonts\\msyh.ttc") {
            fonts.font_data.insert("msyh".to_owned(), egui::FontData::from_owned(font_bytes));
            fonts.families.get_mut(&FontFamily::Proportional).unwrap().insert(0, "msyh".to_owned());
            fonts.families.insert(FontFamily::Name("cond".into()), vec!["msyh".to_owned()]);
            fonts.families.insert(FontFamily::Name("mono".into()), vec!["msyh".to_owned()]);
        }
    }
    fonts.families.entry(FontFamily::Name("cond".into())).or_insert_with(|| vec![FontFamily::Proportional.to_string()]);
    fonts.families.entry(FontFamily::Name("mono".into())).or_insert_with(|| vec![FontFamily::Monospace.to_string()]);
    ctx.set_fonts(fonts);
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

fn format_timestamp(ts: u64) -> String {
    if ts == 0 { return "—".to_string(); }
    let secs = (ts / 10_000_000) as i64 - 11644473600;
    if let Some(dt) = chrono::DateTime::from_timestamp(secs, 0) {
        return dt.format("%Y-%m-%d %H:%M").to_string();
    }
    "—".to_string()
}

fn parse_size(s: &str) -> Option<u64> {
    s.parse().ok()
}

fn spawn_scan(vol: String, tx: std::sync::mpsc::Sender<ScanProgress>) {
    std::thread::spawn(move || {
        let scanner = match indexer::MftScanner::new(&vol) {
            Ok(s) => s,
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); return; }
        };
        match scanner.scan() {
            Ok(entries) => {
                let mut store = IndexStore::new();
                for entry in entries {
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
                let drive_letter = vol.chars().next().unwrap().to_lowercase().to_string();
                let idx_path = format!("{}_drive.idx", drive_letter);
                let _ = storage::save_index(&idx_path, &store);
                let _ = tx.send(ScanProgress::Done { drive: vol, idx_path });
            }
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); }
        }
    });
}

fn main() -> eframe::Result<()> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1000.0, 700.0])
            .with_min_inner_size([800.0, 500.0])
            .with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native(
        "ferrex",
        native_options,
        Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))),
    )
}
