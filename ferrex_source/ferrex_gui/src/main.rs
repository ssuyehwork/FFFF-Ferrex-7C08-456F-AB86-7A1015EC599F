#![windows_subsystem = "windows"]

use eframe::egui;
use egui::{
    Color32, RichText, FontId, FontFamily, Pos2, Vec2, Margin, Frame, Sense, Layout, 
    Align, Align2, Stroke, Rounding, Label, Image, TextEdit, ScrollArea, Area, 
    Order, ViewportCommand, Id
};
use std::sync::Arc;
use std::collections::{HashSet, HashMap};
use std::process::Command;
use std::time::{Instant, Duration};
use std::num::NonZeroUsize;

use storage::{IndexStore, LoadedIndex, MappedIndex, pool_get_name, resolve_path};
use search::{Searcher, SearchOptions};
use indexer::{UsnEvent, spawn_usn_watcher, get_ntfs_volumes};
use lru::LruCache;
use sysinfo::System;

#[cfg(windows)]
use windows::Win32::Foundation::HWND;
#[cfg(windows)]
use windows::Win32::UI::Input::KeyboardAndMouse::{RegisterHotKey, MOD_ALT, UnregisterHotKey};
#[cfg(windows)]
use windows::Win32::UI::WindowsAndMessaging::{PeekMessageW, PM_REMOVE, MSG, WM_HOTKEY};
#[cfg(windows)]
use windows::Win32::UI::Shell::{SHGetFileInfoW, SHGFI_ICON, SHGFI_SMALLICON, SHGFI_USEFILEATTRIBUTES, SHFILEINFOW, ShellExecuteExW, SHELLEXECUTEINFOW, SEE_MASK_INVOKEIDLIST};

const BG: Color32 = Color32::from_rgb(7, 9, 11);
const PANEL: Color32 = Color32::from_rgb(13, 16, 20);
const BG2: Color32 = Color32::from_rgb(17, 21, 25);
const BG3: Color32 = Color32::from_rgb(22, 27, 32);
const BORDER2: Color32 = Color32::from_rgb(37, 46, 55);
const ACCENT: Color32 = Color32::from_rgb(255, 140, 0);
const SUCCESS: Color32 = Color32::from_rgb(46, 204, 113);
const DANGER: Color32 = Color32::from_rgb(231, 76, 60);
const TEXT: Color32 = Color32::from_rgb(200, 212, 220);
const TEXT2: Color32 = Color32::from_rgb(122, 143, 158);
const TEXT3: Color32 = Color32::from_rgb(61, 80, 96);

struct IconCache {
    textures: HashMap<String, egui::TextureHandle>,
}

impl IconCache {
    fn new() -> Self {
        Self { textures: HashMap::new() }
    }

    fn get_for_path(&mut self, ctx: &egui::Context, path: &str, is_dir: bool) -> egui::ImageSource<'static> {
        let ext = if is_dir { "folder".to_string() } else {
            path.rsplit('.').next().unwrap_or("").to_lowercase()
        };

        if let Some(texture) = self.textures.get(&ext) {
            return texture.into();
        }

        if let Some(texture) = self.load_system_icon(ctx, path, is_dir) {
            self.textures.insert(ext.clone(), texture.clone());
            return (&self.textures[&ext]).into();
        }

        egui::include_image!("../../ferrex.png") // Fallback
    }

    fn load_system_icon(&self, ctx: &egui::Context, path: &str, is_dir: bool) -> Option<egui::TextureHandle> {
        #[cfg(windows)]
        unsafe {
            use windows::core::HSTRING;
            use windows::Win32::Graphics::Gdi::{GetDIBits, BITMAPINFO, DIB_RGB_COLORS, GetDC, ReleaseDC, DeleteObject};
            use windows::Win32::UI::WindowsAndMessaging::{DestroyIcon, GetIconInfo};
            let mut shfi = SHFILEINFOW::default();
            let flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
            let attr = if is_dir { windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_DIRECTORY.0 } else { windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_NORMAL.0 };
            
            SHGetFileInfoW(
                &HSTRING::from(path),
                windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES(attr),
                Some(&mut shfi),
                std::mem::size_of::<SHFILEINFOW>() as u32,
                flags,
            );

            if shfi.hIcon.is_invalid() { return None; }

            let mut icon_info = windows::Win32::UI::WindowsAndMessaging::ICONINFO::default();
            if GetIconInfo(shfi.hIcon, &mut icon_info).is_err() {
                let _ = DestroyIcon(shfi.hIcon);
                return None;
            }

            let h_dc = GetDC(HWND(0));
            let mut bmi = BITMAPINFO::default();
            bmi.bmiHeader.biSize = std::mem::size_of::<windows::Win32::Graphics::Gdi::BITMAPINFOHEADER>() as u32;
            bmi.bmiHeader.biWidth = 16;
            bmi.bmiHeader.biHeight = -16; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = windows::Win32::Graphics::Gdi::BI_RGB.0;

            let mut pixels = vec![0u8; 16 * 16 * 4];
            let _ = GetDIBits(h_dc, icon_info.hbmColor, 0, 16, Some(pixels.as_mut_ptr() as *mut _), &mut bmi, DIB_RGB_COLORS);
            
            let _ = ReleaseDC(HWND(0), h_dc);
            let _ = DeleteObject(icon_info.hbmColor);
            let _ = DeleteObject(icon_info.hbmMask);
            let _ = DestroyIcon(shfi.hIcon);

            // BGRA to RGBA
            for i in 0..(pixels.len() / 4) {
                pixels.swap(i * 4, i * 4 + 2);
            }

            let color_image = egui::ColorImage::from_rgba_unmultiplied([16, 16], &pixels);
            Some(ctx.load_texture(format!("icon_{}", path), color_image, Default::default()))
        }
        #[cfg(not(windows))]
        { let _ = (ctx, path, is_dir); None }
    }
}

struct VolumeStore {
    drive: String,
    index: Arc<LoadedIndex>,
    frn_map: HashMap<u64, u32>,
    path_cache: LruCache<u64, String>,
    usn_rx: Option<std::sync::mpsc::Receiver<UsnEvent>>,
    record_count: usize,
}

#[derive(Clone)]
struct SearchResult {
    drive: String,
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
    #[allow(dead_code)]
    search_history: Vec<String>,
    hovered_row: Option<usize>,
    preview_pos: Option<Pos2>,
    icons: IconCache,
    status_text: String,
    is_scanning: bool,
    scan_progress: f32,
    scan_rx: Option<std::sync::mpsc::Receiver<ScanProgress>>,
    hardware_ok: bool,
    sysinfo: System,
    last_sys_poll: Instant,
    mem_usage_mb: f32,
    cpu_usage: f32,
    context_menu_row: Option<usize>,
    show_filters: bool,
    #[cfg(windows)]
    tray: Option<tray_icon::TrayIcon>,
    auto_startup: bool,
    is_pinned: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        setup_fonts(&cc.egui_ctx);
        egui_extras::install_image_loaders(&cc.egui_ctx);
        
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
            hovered_row: None,
            preview_pos: None,
            icons: IconCache::new(),
            status_text: "准备就绪".to_string(),
            is_scanning: false,
            scan_progress: 0.0,
            scan_rx: None,
            hardware_ok: false,
            sysinfo: System::new_all(),
            last_sys_poll: Instant::now(),
            mem_usage_mb: 0.0,
            cpu_usage: 0.0,
            context_menu_row: None,
            show_filters: false,
            #[cfg(windows)]
            tray: None,
            auto_startup: false,
            is_pinned: false,
        };

        app.hardware_ok = app.verify_hardware_wmi();
        if app.hardware_ok {
            app.load_volumes();
            #[cfg(windows)]
            app.setup_hotkey();
            #[cfg(windows)]
            app.setup_tray();
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
            let mut cmd = Command::new("wmic");
            cmd.args(cmd_args.split_whitespace());
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
            }
            if let Ok(output) = cmd.output() {
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
            let drive_letter = vol.chars().next().unwrap_or('c').to_lowercase().to_string();
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
                
                let frn_map = store.build_frn_map();
                let usn_rx = spawn_usn_watcher(&vol).ok();
                
                self.active_drives.insert(vol.clone());
                self.stores.push(VolumeStore {
                    drive: vol,
                    record_count: store.frns.len(),
                    index: Arc::new(store),
                    frn_map,
                    path_cache: LruCache::new(NonZeroUsize::new(10000).expect("Capacity is non-zero")),
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

    #[cfg(windows)]
    fn setup_hotkey(&self) {
        unsafe { let _ = RegisterHotKey(HWND(0), 1001, MOD_ALT, 0x20); }
    }

    #[cfg(windows)]
    fn setup_tray(&mut self) {
        let tray = tray_icon::TrayIconBuilder::new()
            .with_tooltip("Ferrex")
            .build()
            .ok();
        self.tray = tray;
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

    fn run_search(&mut self, ctx: &egui::Context) {
        let t0 = Instant::now();
        let opts = SearchOptions {
            query: &self.query,
            use_regex: self.use_regex,
            ext_filter: if self.ext_filter.is_empty() { None } else { Some(&self.ext_filter) },
            min_size: parse_size(&self.min_size_str),
            max_size: parse_size(&self.max_size_str),
            date_from: parse_date_to_filetime(&self.date_from_str),
            date_to: parse_date_to_filetime(&self.date_to_str),
            include_dirs: !self.dirs_only,
            include_hidden: self.show_hidden,
            include_system: self.show_system,
        };

        let mut all_results = Vec::new();
        for store in self.stores.iter_mut() {
            if !self.active_drives.contains(&store.drive) { continue; }
            let searcher = Searcher::new(
                &store.index.frns, &store.index.parent_frns, &store.index.sizes, 
                &store.index.timestamps, &store.index.flags, &store.index.name_offsets, 
                &store.index.string_pool
            );
            let matches = searcher.search(&opts);
            for rec_idx in matches.into_iter().take(5000) {
                let name = pool_get_name(&store.index.string_pool, store.index.name_offsets[rec_idx] as usize);
                let full_path = resolve_path(
                    &store.drive, rec_idx as u32, &store.index.frns, &store.index.parent_frns, 
                    &store.index.name_offsets, &store.index.string_pool, &store.frn_map, &mut store.path_cache
                );
                
                self.icons.get_for_path(ctx, &name, store.index.flags[rec_idx] & 0x10 != 0);

                all_results.push(SearchResult {
                    drive: store.drive.clone(),
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
    }

    fn apply_sort(&mut self) {
        let asc = self.sort_asc;
        match self.sort_col {
            SortColumn::Name => self.results.sort_by(|a, b| { let c = a.name.to_lowercase().cmp(&b.name.to_lowercase()); if asc { c } else { c.reverse() } }),
            SortColumn::Path => self.results.sort_by(|a, b| { let c = a.full_path.to_lowercase().cmp(&b.full_path.to_lowercase()); if asc { c } else { c.reverse() } }),
            SortColumn::Size => self.results.sort_by(|a, b| { let c = a.size.cmp(&b.size); if asc { c } else { c.reverse() } }),
            SortColumn::Date => self.results.sort_by(|a, b| { let c = a.timestamp.cmp(&b.timestamp); if asc { c } else { c.reverse() } }),
        }
    }

    fn export_selected_csv(&self) {
        #[cfg(target_os = "windows")]
        {
            let path = rfd::FileDialog::new()
                .set_file_name("ferrex_export.csv")
                .add_filter("CSV", &["csv"])
                .save_file();
            if let Some(p) = path {
                let mut content = String::from("名称,路径,大小(字节),修改时间,类型\n");
                for &idx in &self.selected_rows {
                    if let Some(r) = self.results.get(idx) {
                        content.push_str(&format!("{},{},{},{},{}\n",
                            r.name, r.full_path, r.size,
                            format_timestamp(r.timestamp),
                            if r.is_dir { "目录" } else { "文件" }));
                    }
                }
                let _ = std::fs::write(p, content);
            }
        }
    }
}

impl Drop for FerrexApp {
    fn drop(&mut self) {
        #[cfg(windows)]
        unsafe { let _ = UnregisterHotKey(HWND(0), 1001); }
    }
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if !self.hardware_ok {
            self.show_lock_screen(ctx);
            return;
        }

        self.process_usn_events();
        #[cfg(windows)]
        self.handle_hotkey(ctx);
        self.update_stats();
        self.handle_scan_progress();

        if ctx.input(|i| i.viewport().close_requested()) {
            ctx.send_viewport_cmd(ViewportCommand::CancelClose);
            ctx.send_viewport_cmd(ViewportCommand::Visible(false));
        }

        egui::TopBottomPanel::top("titlebar")
            .exact_height(32.0)
            .frame(Frame::none().fill(PANEL).inner_margin(Margin::symmetric(8.0, 0.0)))
            .show(ctx, |ui| { self.draw_titlebar(ui); });

        egui::TopBottomPanel::top("drives")
            .exact_height(40.0)
            .frame(Frame::none().fill(BG2).inner_margin(Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| { self.draw_drive_selector(ui, ctx); });

        egui::TopBottomPanel::top("searchbar_area").frame(Frame::none().fill(PANEL)).show(ctx, |ui| {
            Frame::none().inner_margin(Margin::symmetric(16.0, 5.0)).show(ui, |ui| {
                self.draw_search_bar(ui, ctx);
                if self.show_filters { ui.add_space(8.0); self.draw_filter_strip(ui, ctx); }
            });
            if self.is_scanning { 
                ui.add(egui::ProgressBar::new(self.scan_progress)
                    .text(RichText::new(&self.status_text).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(ACCENT))
                    .fill(ACCENT).desired_height(4.0).rounding(Rounding::ZERO)); 
            }
        });

        egui::TopBottomPanel::bottom("statusbar")
            .exact_height(26.0)
            .frame(Frame::none().fill(PANEL).inner_margin(Margin::symmetric(16.0, 0.0)))
            .show(ctx, |ui| { self.draw_status_bar(ui); });

        egui::CentralPanel::default().frame(Frame::none().fill(BG)).show(ctx, |ui| {
            self.draw_column_header(ui);
            ui.add(egui::Separator::default().spacing(0.0));
            self.draw_results_list(ui);
            self.draw_hover_preview(ctx);
        });
    }
}

impl FerrexApp {
    fn show_lock_screen(&self, ctx: &egui::Context) {
        egui::CentralPanel::default().frame(Frame::none().fill(BG)).show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(ui.available_height() / 2.0 - 60.0);
                ui.label(RichText::new("未授权的硬件设备").font(FontId::new(24.0, FontFamily::Name("cond".into()))).color(DANGER).strong());
                ui.add_space(8.0);
                ui.label(RichText::new("请联系开发者以获取授权").font(FontId::new(14.0, FontFamily::Name("mono".into()))).color(TEXT3));
                ui.add_space(20.0);
                let (rect, _) = ui.allocate_exact_size(Vec2::new(64.0, 64.0), Sense::hover());
                ui.painter().rect_stroke(rect, 8.0, Stroke::new(2.0, DANGER));
                ui.painter().circle_stroke(rect.center() - Vec2::new(0.0, 10.0), 12.0, Stroke::new(2.0, DANGER));
            });
        });
    }

    fn draw_titlebar(&mut self, ui: &mut egui::Ui) {
        let rect = ui.available_rect_before_wrap();
        // 绘制底部边框，百分之百还原参考版的物理边缘感
        ui.painter().line_segment(
            [rect.left_bottom(), rect.right_bottom()],
            Stroke::new(1.0, Color32::from_rgb(68, 68, 68)) // 参考版 #444
        );

        ui.horizontal_centered(|ui| {
            ui.add_space(8.0);
            ui.add(egui::Image::new(egui::include_image!("../../ferrex.png")).max_size(Vec2::new(18.0, 18.0)));
            ui.add_space(8.0);
            ui.label(RichText::new("FERREX").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(ACCENT).extra_letter_spacing(1.5));
            ui.add_space(12.0);

            // 恢复被移除的核心 UX：记录总数显示
            let total_records: usize = self.stores.iter().map(|s| s.record_count).sum();
            ui.label(RichText::new(format!("READY — {}", format_number(total_records))).font(FontId::new(9.0, FontFamily::Name("cond".into()))).color(TEXT3));

            // 中间拖拽区域（物理分离按钮区域，避免交互冲突）
            let drag_rect = ui.available_rect_before_wrap();
            let drag_response = ui.interact(drag_rect, ui.id().with("drag"), Sense::click_and_drag());
            if drag_response.drag_started_by(egui::PointerButton::Primary) {
                ui.ctx().send_viewport_cmd(ViewportCommand::StartDrag);
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 0.0;

                // 关闭按钮 - 物理加载 close.svg
                let close_response = self.draw_svg_button(ui, egui::include_image!("../icons/close.svg"), Color32::from_rgb(232, 17, 35), Color32::from_rgb(241, 112, 122), Color32::from_rgb(165, 0, 0), Color32::WHITE);
                if close_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Close);
                }

                // 最大化/还原 - 物理加载
                let is_max = ui.ctx().input(|i| i.viewport().maximized.unwrap_or(false));
                let max_img = if is_max { egui::include_image!("../icons/restore.svg") } else { egui::include_image!("../icons/maximize.svg") };
                let max_response = self.draw_svg_button(ui, max_img, Color32::TRANSPARENT, Color32::from_rgba_unmultiplied(255, 255, 255, 25), Color32::from_rgba_unmultiplied(255, 255, 255, 51), Color32::from_rgb(238, 238, 238));
                if max_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Maximized(!is_max));
                }

                // 最小化 - 物理加载 minimize.svg
                let min_response = self.draw_svg_button(ui, egui::include_image!("../icons/minimize.svg"), Color32::TRANSPARENT, Color32::from_rgba_unmultiplied(255, 255, 255, 25), Color32::from_rgba_unmultiplied(255, 255, 255, 51), Color32::from_rgb(238, 238, 238));
                if min_response.clicked() {
                    ui.ctx().send_viewport_cmd(ViewportCommand::Minimized(true));
                }

                // 置顶 - 物理加载 pin.svg
                let pin_color = if self.is_pinned { Color32::from_rgb(255, 85, 28) } else { Color32::from_rgb(238, 238, 238) };
                let pin_response = self.draw_svg_button(ui, egui::include_image!("../icons/pin.svg"), Color32::TRANSPARENT, Color32::from_rgba_unmultiplied(255, 255, 255, 25), Color32::from_rgba_unmultiplied(255, 255, 255, 51), pin_color);
                if pin_response.clicked() {
                    self.is_pinned = !self.is_pinned;
                    let level = if self.is_pinned { egui::WindowLevel::AlwaysOnTop } else { egui::WindowLevel::Normal };
                    ui.ctx().send_viewport_cmd(ViewportCommand::WindowLevel(level));
                }
            });
        });
    }

    /// 核心渲染函数：100% 使用物理 SVG 文件加载（禁止模拟绘图）
    fn draw_svg_button(
        &self,
        ui: &mut egui::Ui,
        img_source: egui::ImageSource<'static>,
        base_bg: Color32,
        hover_bg: Color32,
        press_bg: Color32,
        icon_tint: Color32
    ) -> egui::Response {
        // 规格：24x24px 容器 (参考版 setFixedSize(24, 24))
        let (rect, response) = ui.allocate_at_least(Vec2::splat(24.0), Sense::click());

        let fill = if response.is_pointer_button_down_on() { press_bg }
                   else if response.hovered() { hover_bg }
                   else { base_bg };

        if fill != Color32::TRANSPARENT {
            ui.painter().rect_filled(rect, Rounding::same(4.0), fill);
        }

        // 渲染真实的物理 SVG 图标
        let mut img = egui::Image::new(img_source)
            .max_size(Vec2::splat(14.0)) // 严格控制图标在按钮中心的比例
            .tint(icon_tint);

        let mut child_ui = ui.child_ui(rect, Layout::centered_and_justified(Direction::LeftToRight), None);
        child_ui.add(img);

        response
    }

    fn draw_drive_selector(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        let mut toggle_drive = None;
        ui.horizontal_centered(|ui| {
            ui.label(RichText::new("DRIVES").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            ui.add_space(8.0);
            for store in &self.stores {
                let is_active = self.active_drives.contains(&store.drive);
                let label = format!("{}:  {}", store.drive, format_count(store.record_count));
                let (bg, stroke_color, text_color) = if is_active { (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT) } else { (BG3, BORDER2, TEXT2) };
                let btn = egui::Button::new(RichText::new(&label).font(FontId::new(12.0, FontFamily::Name("cond".into()))).color(text_color)).fill(bg).stroke(Stroke::new(1.0, stroke_color)).rounding(Rounding::ZERO);
                if ui.add(btn).clicked() {
                    toggle_drive = Some((store.drive.clone(), is_active));
                }
                ui.add_space(4.0);
            }
        });

        if let Some((drive, was_active)) = toggle_drive {
            if was_active && self.active_drives.len() > 1 { self.active_drives.remove(&drive); }
            else if !was_active { self.active_drives.insert(drive); }
            self.run_search(ctx);
        }
    }

    fn draw_search_bar(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            let (icon_rect, _) = ui.allocate_exact_size(Vec2::new(36.0, 36.0), Sense::hover());
            ui.painter().rect_filled(icon_rect, Rounding::ZERO, BG2);
            ui.painter().rect_stroke(icon_rect, Rounding::ZERO, Stroke::new(1.0, BORDER2));
            let c = Pos2::new(icon_rect.center().x - 2.0, icon_rect.center().y - 2.0);
            ui.painter().circle_stroke(c, 5.5, Stroke::new(1.5, TEXT3));
            ui.painter().line_segment([Pos2::new(c.x + 4.0, c.y + 4.0), Pos2::new(c.x + 8.0, c.y + 8.0)], Stroke::new(1.5, TEXT3));
            
            let search_edit = TextEdit::singleline(&mut self.query)
                .font(FontId::new(13.0, FontFamily::Name("mono".into())))
                .hint_text(RichText::new("文件名 / 关键词...").color(TEXT3))
                .frame(true)
                .text_color(TEXT);

            let search_response = ui.add_sized(Vec2::new(ui.available_width() - 80.0 - 24.0 - 80.0 - 100.0, 36.0), search_edit);
            
            let (dot_rect, _) = ui.allocate_exact_size(Vec2::new(24.0, 36.0), Sense::hover());
            ui.painter().rect_filled(dot_rect, Rounding::ZERO, BG3);
            ui.painter().text(dot_rect.center(), Align2::CENTER_CENTER, ".", FontId::new(16.0, FontFamily::Name("mono".into())), ACCENT);
            
            let ext_response = ui.add_sized(Vec2::new(80.0, 36.0), TextEdit::singleline(&mut self.ext_filter).font(FontId::new(13.0, FontFamily::Name("mono".into()))).hint_text(RichText::new("扩展名").color(TEXT3)).frame(true).text_color(TEXT));
            
            ui.add_space(10.0);
            if ui.add(egui::Button::new(RichText::new("搜索").font(FontId::new(13.0, FontFamily::Name("cond".into()))).color(Color32::BLACK).strong()).fill(ACCENT).min_size(Vec2::new(70.0, 36.0))).clicked() || search_response.changed() || ext_response.changed() { self.run_search(ctx); }
            ui.add_space(8.0);
            if ui.add(egui::Button::new(RichText::new("过滤器").font(FontId::new(11.0, FontFamily::Name("cond".into()))).color(if self.show_filters { ACCENT } else { TEXT3 })).stroke(Stroke::new(1.0, if self.show_filters { ACCENT } else { BORDER2 })).fill(Color32::TRANSPARENT)).clicked() { self.show_filters = !self.show_filters; }
        });
    }

    fn draw_filter_strip(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 12.0;
            if filter_toggle(ui, "正则", &mut self.use_regex) { self.run_search(ctx); }
            ui.label(RichText::new("大小 ≥").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(TextEdit::singleline(&mut self.min_size_str).desired_width(60.0)).changed() { self.run_search(ctx); }
            ui.label(RichText::new("≤").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(TextEdit::singleline(&mut self.max_size_str).desired_width(60.0)).changed() { self.run_search(ctx); }
            
            ui.label(RichText::new("日期 从").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(TextEdit::singleline(&mut self.date_from_str).hint_text("YYYY-MM-DD").desired_width(80.0)).changed() { self.run_search(ctx); }
            ui.label(RichText::new("至").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3));
            if ui.add(TextEdit::singleline(&mut self.date_to_str).hint_text("YYYY-MM-DD").desired_width(80.0)).changed() { self.run_search(ctx); }

            if filter_toggle(ui, "隐藏", &mut self.show_hidden) { self.run_search(ctx); }
            if filter_toggle(ui, "系统", &mut self.show_system) { self.run_search(ctx); }
            if filter_toggle(ui, "仅目录", &mut self.dirs_only) { self.run_search(ctx); }
            
            ui.add_space(8.0);
            if filter_toggle(ui, "开机自启", &mut self.auto_startup) {
                #[cfg(windows)]
                set_startup(self.auto_startup);
            }
        });
    }

    fn draw_column_header(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_centered(|ui| {
            ui.add_space(16.0 + 24.0);
            if header_button(ui, "NAME", 300.0, self.sort_col == SortColumn::Name) { self.sort_col = SortColumn::Name; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if header_button(ui, "PATH", ui.available_width() - 240.0, self.sort_col == SortColumn::Path) { self.sort_col = SortColumn::Path; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if header_button(ui, "SIZE", 80.0, self.sort_col == SortColumn::Size) { self.sort_col = SortColumn::Size; self.sort_asc = !self.sort_asc; self.apply_sort(); }
            if header_button(ui, "DATE", 140.0, self.sort_col == SortColumn::Date) { self.sort_col = SortColumn::Date; self.sort_asc = !self.sort_asc; self.apply_sort(); }
        });
    }

    fn draw_results_list(&mut self, ui: &mut egui::Ui) {
        if self.results.is_empty() { self.draw_empty_state(ui); return; }

        let mut new_hovered = None;
        let mut new_preview_pos = None;
        let mut new_context_row = None;
        let mut new_selected = None;

        let available_width = ui.available_width();
        let results_to_show = self.results.len().min(200);

        ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            for idx in 0..results_to_show {
                let is_selected = self.selected_rows.contains(&idx);
                let (rect, response) = ui.allocate_at_least(Vec2::new(available_width, 30.0), Sense::click());
                let is_hovered = response.hovered();
                
                let bg = if is_selected { Color32::from_rgba_unmultiplied(255, 140, 0, 25) } else if is_hovered { BG3 } else if idx % 2 == 0 { BG } else { BG2 };
                ui.painter().rect_filled(rect, Rounding::ZERO, bg);
                if is_selected || is_hovered { ui.painter().line_segment([rect.left_top(), rect.left_bottom()], Stroke::new(3.0, ACCENT)); }
                
                if is_hovered { new_hovered = Some(idx); new_preview_pos = Some(rect.right_top()); }
                if response.clicked() { new_selected = Some(idx); }
                if response.secondary_clicked() { new_context_row = Some(idx); }
                
                let mut child_ui = ui.child_ui(rect, Layout::left_to_right(Align::Center), None);
                child_ui.add_space(8.0);
                
                let result = &self.results[idx];
                child_ui.add(Image::new(self.icons.get_for_path(ui.ctx(), &result.name, result.is_dir)).max_size(Vec2::new(14.0, 14.0)));
                child_ui.add_space(8.0);
                
                let (tag_rect, _) = child_ui.allocate_exact_size(Vec2::new(22.0, 14.0), Sense::hover());
                child_ui.painter().rect_stroke(tag_rect, 1.0, Stroke::new(1.0, BORDER2));
                child_ui.painter().text(tag_rect.center(), Align2::CENTER_CENTER, &result.drive[..2], FontId::new(9.0, FontFamily::Name("cond".into())), TEXT3);
                child_ui.add_space(6.0);
                
                child_ui.add_sized([260.0, 20.0], Label::new(RichText::new(&result.name).font(FontId::new(12.5, FontFamily::Name("mono".into()))).color(TEXT)).truncate());
                child_ui.add_sized([child_ui.available_width() - 230.0, 20.0], Label::new(RichText::new(&result.full_path).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)).truncate());
                
                child_ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.add_space(16.0);
                    ui.add_sized([130.0, 20.0], Label::new(RichText::new(format_timestamp(result.timestamp)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT3)));
                    ui.add_sized([80.0, 20.0], Label::new(RichText::new(format_size(result.size)).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2)));
                });
            }
        });

        self.hovered_row = new_hovered;
        self.preview_pos = new_preview_pos;
        if let Some(idx) = new_selected {
            let modifiers = ui.input(|i| i.modifiers);
            if modifiers.command {
                if !self.selected_rows.insert(idx) { self.selected_rows.remove(&idx); }
            } else if modifiers.shift {
                self.selected_rows.insert(idx);
            } else {
                self.selected_rows.clear();
                self.selected_rows.insert(idx);
            }
        }
        
        if let Some(idx) = new_context_row { self.context_menu_row = Some(idx); }
        if let Some(idx) = self.context_menu_row {
            let result = self.results[idx].clone();
            let mut close_menu = false;
            let menu_pos = ui.input(|i| i.pointer.interact_pos().unwrap_or_default());
            Area::new(Id::new("ctx_menu")).fixed_pos(menu_pos).order(Order::Foreground).show(ui.ctx(), |ui| {
                Frame::none().fill(PANEL).stroke(Stroke::new(1.0, BORDER2)).inner_margin(Margin::same(4.0)).show(ui, |ui| {
                    ui.set_min_width(180.0);
                    if menu_item(ui, "打开文件") { open_file(&result.full_path); close_menu = true; }
                    if menu_item(ui, "在资源管理器中定位") { reveal_in_explorer(&result.full_path); close_menu = true; }
                    if menu_item(ui, "复制路径") { ui.ctx().output_mut(|o| o.copied_text = result.full_path.clone()); close_menu = true; }
                    if menu_item(ui, "复制文件名") { ui.ctx().output_mut(|o| o.copied_text = result.name.clone()); close_menu = true; }
                    ui.separator();
                    if menu_item(ui, "属性") { open_properties(&result.full_path); close_menu = true; }
                });
            });
            if close_menu || (ui.input(|i| i.pointer.any_click()) && !ui.input(|i| i.pointer.secondary_down())) { self.context_menu_row = None; }
        }
    }

    fn draw_hover_preview(&self, ctx: &egui::Context) {
        if let (Some(idx), Some(pos)) = (self.hovered_row, self.preview_pos) {
            if let Some(result) = self.results.get(idx) {
                Area::new(Id::new("preview")).fixed_pos(pos + Vec2::new(12.0, 0.0)).order(Order::Tooltip).show(ctx, |ui| {
                    Frame::none().fill(PANEL).stroke(Stroke::new(1.0, BORDER2)).inner_margin(Margin::same(12.0)).show(ui, |ui| {
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
    }

    fn draw_empty_state(&self, ui: &mut egui::Ui) {
        ui.vertical_centered(|ui| {
            ui.add_space(100.0);
            ui.label(RichText::new("无匹配结果").font(FontId::new(14.0, FontFamily::Name("cond".into()))).color(TEXT3));
        });
    }

    fn draw_status_bar(&self, ui: &mut egui::Ui) {
        ui.horizontal_centered(|ui| {
            if self.selected_rows.len() > 1 {
                let total_size: u64 = self.selected_rows.iter().filter_map(|&idx| self.results.get(idx)).map(|r| r.size).sum();
                stat_item(ui, "已选", &format!("{} 项", self.selected_rows.len()));
                ui.add_space(8.0);
                stat_item(ui, "合计大小", &format_size(total_size));
                ui.add_space(16.0);
                if ui.link(RichText::new("导出所选为 CSV").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(ACCENT)).clicked() {
                    self.export_selected_csv();
                }
            } else {
                stat_item(ui, "结果", &format_number(self.results.len()));
                ui.add_space(16.0);
                stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.label(RichText::new("FERREX v0.1.0").font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(ACCENT));
                ui.add_space(16.0);
                stat_item(ui, "CPU", &format!("{:.1}%", self.cpu_usage));
                ui.add_space(16.0);
                stat_item(ui, "MEM", &format!("{:.0}MB", self.mem_usage_mb));
            });
        });
    }

    #[cfg(windows)]
    fn handle_hotkey(&self, ctx: &egui::Context) {
        unsafe {
            let mut msg = MSG::default();
            while PeekMessageW(&mut msg, HWND(0), WM_HOTKEY, WM_HOTKEY, PM_REMOVE).as_bool() {
                if msg.wParam.0 == 1001 {
                    ctx.send_viewport_cmd(ViewportCommand::Focus);
                    ctx.send_viewport_cmd(ViewportCommand::Visible(true));
                }
            }
        }
    }

    fn update_stats(&mut self) {
        if self.last_sys_poll.elapsed() >= Duration::from_secs(2) {
            self.sysinfo.refresh_memory();
            self.sysinfo.refresh_cpu_usage();
            self.mem_usage_mb = self.sysinfo.used_memory() as f32 / 1024.0 / 1024.0;
            self.cpu_usage = self.sysinfo.global_cpu_info().cpu_usage();
            self.last_sys_poll = Instant::now();
        }
    }

    fn handle_scan_progress(&mut self) {
        let mut drop_rx = false;
        if let Some(ref rx) = self.scan_rx {
            while let Ok(msg) = rx.try_recv() {
                match msg {
                    ScanProgress::Progress { done, total } => { 
                        self.scan_progress = if total == 0 { 0.0 } else { done as f32 / total as f32 }; 
                        self.status_text = format!("正在扫描 {}/{}", done, total); 
                    }
                    ScanProgress::Done { .. } => { 
                        self.is_scanning = false; 
                        drop_rx = true;
                        self.status_text = "扫描完成".to_string(); 
                    }
                    ScanProgress::Error(e) => { 
                        self.status_text = format!("错误: {}", e); 
                        self.is_scanning = false; 
                    }
                }
            }
        }
        if drop_rx { self.scan_rx = None; }
    }
}

fn header_button(ui: &mut egui::Ui, text: &str, width: f32, active: bool) -> bool {
    let color = if active { ACCENT } else { TEXT3 };
    let response = ui.add_sized([width, 20.0], egui::Button::new(RichText::new(text).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(color).extra_letter_spacing(1.5)).fill(Color32::TRANSPARENT));
    response.clicked()
}

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) { 
    ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3)); 
    ui.add_space(4.0); 
    ui.label(RichText::new(value).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT2).strong()); 
}

fn filter_toggle(ui: &mut egui::Ui, text: &str, value: &mut bool) -> bool {
    let color = if *value { ACCENT } else { TEXT3 };
    let btn = egui::Button::new(RichText::new(text).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(color))
        .fill(Color32::TRANSPARENT).stroke(Stroke::new(1.0, if *value { ACCENT } else { BORDER2 })).rounding(Rounding::ZERO);
    if ui.add(btn).clicked() { *value = !*value; true } else { false }
}

fn preview_row(ui: &mut egui::Ui, label: &str, value: &str) { 
    ui.horizontal(|ui| { 
        ui.label(RichText::new(label).font(FontId::new(10.0, FontFamily::Name("cond".into()))).color(TEXT3)); 
        ui.add_space(8.0); 
        ui.label(RichText::new(value).font(FontId::new(11.0, FontFamily::Name("mono".into()))).color(TEXT2)); 
    }); 
}

fn menu_item(ui: &mut egui::Ui, label: &str) -> bool {
    ui.add(egui::Button::new(RichText::new(label).font(FontId::new(11.0, FontFamily::Name("cond".into()))).color(TEXT2)).fill(Color32::TRANSPARENT)).clicked()
}

fn open_file(path: &str) {
    let mut cmd = std::process::Command::new("cmd");
    cmd.args(["/c", "start", "", path]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    let _ = cmd.spawn();
}

fn reveal_in_explorer(path: &str) {
    let mut cmd = std::process::Command::new("explorer");
    cmd.args(["/select,", path]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    let _ = cmd.spawn();
}

#[allow(unused_variables)]
fn open_properties(path: &str) {
    #[cfg(windows)]
    unsafe {
        use windows::core::HSTRING;
        let info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            lpVerb: windows::core::PCWSTR(HSTRING::from("properties").as_ptr()),
            lpFile: windows::core::PCWSTR(HSTRING::from(path).as_ptr()),
            nShow: 1,
            fMask: SEE_MASK_INVOKEIDLIST,
            ..Default::default()
        };
        let _ = ShellExecuteExW(&mut { info });
    }
}

#[cfg(windows)]
fn set_startup(enabled: bool) {
    use windows::Win32::System::Registry::*;
    use windows::core::{HSTRING, w};
    unsafe {
        let key_path = w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        let mut h_key = HKEY::default();
        let _ = RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_SET_VALUE, &mut h_key);
        if enabled {
            let exe = std::env::current_exe().unwrap_or_default();
            let val = HSTRING::from(exe.to_string_lossy().as_ref());
            let bytes = val.as_wide();
            let _ = RegSetValueExW(
                h_key, w!("Ferrex"), 0, REG_SZ,
                Some(std::slice::from_raw_parts(bytes.as_ptr() as *const u8, bytes.len()*2))
            );
        } else {
            let _ = RegDeleteValueW(h_key, w!("Ferrex"));
        }
        let _ = RegCloseKey(h_key);
    }
}

fn setup_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    #[allow(unused_mut)]
    let mut msyh_loaded = false;

    #[cfg(windows)]
    if let Ok(font_bytes) = std::fs::read("C:\\Windows\\Fonts\\msyh.ttc") {
        fonts.font_data.insert("msyh".to_owned(), egui::FontData::from_owned(font_bytes));
        msyh_loaded = true;
    }

    if msyh_loaded {
        if let Some(v) = fonts.families.get_mut(&FontFamily::Proportional) {
            v.insert(0, "msyh".to_owned());
        }
        if let Some(v) = fonts.families.get_mut(&FontFamily::Monospace) {
            v.insert(0, "msyh".to_owned());
        }
        fonts.families.insert(FontFamily::Name("cond".into()), vec!["msyh".to_owned()]);
        fonts.families.insert(FontFamily::Name("mono".into()), vec!["msyh".to_owned()]);
    } else {
        let default_sans = fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default();
        let default_mono = fonts.families.get(&FontFamily::Monospace).cloned().unwrap_or_default();
        fonts.families.insert(FontFamily::Name("cond".into()), default_sans);
        fonts.families.insert(FontFamily::Name("mono".into()), default_mono);
    }
    ctx.set_fonts(fonts);
}

fn format_number(n: usize) -> String { 
    let s = n.to_string(); 
    let mut result = String::new(); 
    for (i, c) in s.chars().rev().enumerate() { if i > 0 && i % 3 == 0 { result.push(','); } result.push(c); } 
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
    use chrono::{TimeZone, Utc};
    if let Some(dt) = Utc.timestamp_opt(secs, 0).single() {
        return dt.format("%Y-%m-%d %H:%M").to_string();
    }
    "—".to_string() 
}

fn parse_size(s: &str) -> Option<u64> {
    let s = s.trim().to_lowercase();
    if s.is_empty() { return None; }
    let (num_part, unit) = if s.ends_with("kb") { (&s[..s.len()-2], 1024) }
    else if s.ends_with("mb") { (&s[..s.len()-2], 1024*1024) }
    else if s.ends_with("gb") { (&s[..s.len()-2], 1024*1024*1024) }
    else if s.ends_with("k") { (&s[..s.len()-1], 1024) }
    else if s.ends_with("m") { (&s[..s.len()-1], 1024*1024) }
    else if s.ends_with("g") { (&s[..s.len()-1], 1024*1024*1024) }
    else { (s.as_str(), 1) };
    num_part.trim().parse::<u64>().ok().map(|n| n * unit)
}

fn parse_date_to_filetime(s: &str) -> Option<u64> {
    use chrono::{NaiveDate, Utc, TimeZone};
    let date = NaiveDate::parse_from_str(s, "%Y-%m-%d").ok()?;
    let datetime = date.and_hms_opt(0, 0, 0)?;
    let dt_utc = Utc.from_local_datetime(&datetime).single()?;
    let unix_secs = dt_utc.timestamp();
    let filetime_secs = unix_secs + 11644473600;
    Some((filetime_secs as u64) * 10_000_000)
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
                let drive_letter = vol.chars().next().unwrap_or('c').to_lowercase().to_string();
                let idx_path = format!("{}_drive.idx", drive_letter);
                let _ = storage::save_index(&idx_path, &store);
                let _ = tx.send(ScanProgress::Done { drive: vol, idx_path: idx_path });
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
            .with_title("Ferrex")
            .with_decorations(false),
        ..Default::default() 
    };
    eframe::run_native(
        "ferrex",
        native_options,
        Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)) as Box<dyn eframe::App>)),
    )
}
