#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
use eframe::egui;
use egui_extras::install_image_loaders;
use indexer::{acquire_privileges, get_ntfs_volumes};
use storage::{LoadedIndex, FileRecord};
use search::Searcher;
use std::sync::Arc;
use std::process::Command;
use std::collections::HashMap;
use std::path::Path;

const SVG_FILE: &str = "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FF8C00\" stroke-width=\"2\"><path d=\"M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z\"></path><polyline points=\"13 2 13 9 20 9\"></polyline></svg>";
const SVG_FOLDER: &str = "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FF8C00\" stroke-width=\"2\"><path d=\"M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z\"></path></svg>";

struct IndexStore {
    drive: String,
    data: Arc<LoadedIndex>,
    frn_to_idx: HashMap<u64, usize>,
}

struct SearchResult {
    path: String,
    is_dir: bool,
}

struct FerrexApp {
    query: String,
    results: Vec<SearchResult>,
    indices: Vec<IndexStore>,
    status: String,
    hardware_verified: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        install_image_loaders(&cc.egui_ctx);

        let mut fonts = egui::FontDefinitions::default();
        fonts.font_data.insert(
            "msyh".to_owned(),
            egui::FontData::from_static(include_bytes!("../resources/msyh.ttc")),
        );
        fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().insert(0, "msyh".to_owned());
        fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().push("msyh".to_owned());
        cc.egui_ctx.set_fonts(fonts);

        let mut app = Self {
            query: String::new(),
            results: Vec::new(),
            indices: Vec::new(),
            status: "正在初始化系统...".to_string(),
            hardware_verified: false,
        };

        app.hardware_verified = app.verify_hardware_wmi();
        if app.hardware_verified {
            app.load_indices();
        } else {
            app.status = "未授权设备。".to_string();
        }

        app
    }

    fn verify_hardware_wmi(&self) -> bool {
        let whitelist = ["BFEBFBFF000306C3", "SGH412RF00", "494000PA0D9L", "PHYS825203NX480BGN", "NA5360WJ", "NA7G89GQ", "03000210052122072519"];
        for cmd in ["cpu get processorid", "baseboard get serialnumber", "bios get serialnumber"] {
            if let Ok(out) = Command::new("wmic").args(cmd.split_whitespace()).output() {
                let text = String::from_utf8_lossy(&out.stdout);
                let id = text.lines().nth(1).unwrap_or("").trim();
                if !id.is_empty() && whitelist.iter().any(|&w| w.eq_ignore_ascii_case(id)) { return true; }
            }
        }
        false
    }

    fn load_indices(&mut self) {
        let _ = acquire_privileges();
        let mut loaded_count = 0;
        let mut total_records = 0;

        for d in b'c'..=b'z' {
            let drive_char = d as char;
            let idx_path = format!("{}_drive.idx", drive_char);
            if Path::new(&idx_path).exists() {
                if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                    let records = loaded.get_records();
                    let mut frn_to_idx = HashMap::with_capacity(records.len());
                    for (i, rec) in records.iter().enumerate() {
                        frn_to_idx.insert(rec.frn, i);
                    }

                    total_records += loaded.record_count;
                    self.indices.push(IndexStore {
                        drive: drive_char.to_string().to_uppercase(),
                        data: Arc::new(loaded),
                        frn_to_idx,
                    });
                    loaded_count += 1;
                }
            }
        }

        if loaded_count > 0 {
            self.status = format!("已加载 {} 个驱动器索引 (共 {} 条记录)", loaded_count, total_records);
        } else {
            self.status = "未发现索引文件，请先运行 CLI 版进行 'index'。".to_string();
        }
    }

    fn perform_search(&mut self) {
        if self.query.is_empty() {
            self.results.clear();
            return;
        }

        let mut all_matches = Vec::new();
        for store in &self.indices {
            let records = store.data.get_records();
            let pool = store.data.get_string_pool();
            let searcher = Searcher::new(records, pool);
            let matches = searcher.search(&self.query);

            for &idx in matches.iter().take(200) { // Limit per volume for speed
                let full_path = resolve_full_path(&store.drive, idx, records, pool, &store.frn_to_idx);
                let is_dir = (records[idx].flags & 0x10) != 0;
                all_matches.push(SearchResult { path: full_path, is_dir });
            }
        }

        // Final UI limit
        all_matches.truncate(500);
        self.results = all_matches;
    }
}

fn resolve_full_path(drive: &str, idx: usize, records: &[FileRecord], pool: &[u8], frn_to_idx: &HashMap<u64, usize>) -> String {
    let mut parts = Vec::new();
    let mut curr = Some(idx);
    while let Some(i) = curr {
        let rec = &records[i];
        let name = String::from_utf8_lossy(&pool[rec.name_offset as usize..]).split('\0').next().unwrap_or("").to_string();
        parts.push(name);

        let c_frn = rec.frn;
        let p_frn = rec.parent_frn;
        if c_frn == 5 || p_frn == 0 { break; }
        curr = frn_to_idx.get(&p_frn).copied();
    }
    parts.reverse();
    format!("{}:\\{}", drive, parts.join("\\"))
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let mut visuals = egui::Visuals::dark();
        visuals.widgets.active.bg_fill = egui::Color32::from_rgb(255, 140, 0);
        ctx.set_visuals(visuals);

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(10.0);
                ui.heading(egui::RichText::new("Ferrex NTFS Indexer").color(egui::Color32::from_rgb(255, 140, 0)));
            });

            if !self.hardware_verified {
                ui.centered_and_justified(|ui| {
                    ui.colored_label(egui::Color32::RED, "未授权设备\n请联系 Telegram: SYQ_14");
                });
                return;
            }

            ui.horizontal(|ui| {
                ui.label("搜索:");
                if ui.text_edit_singleline(&mut self.query).changed() {
                    self.perform_search();
                }
            });
            ui.label(egui::RichText::new(&self.status).small().color(egui::Color32::GRAY));
            ui.separator();

            egui::ScrollArea::vertical().show(ui, |ui| {
                for res in &self.results {
                    ui.horizontal(|ui| {
                        let svg = if res.is_dir { SVG_FOLDER } else { SVG_FILE };
                        let img_id = if res.is_dir { "folder.svg" } else { "file.svg" };
                        ui.add(egui::Image::from_bytes(img_id, svg.as_bytes().to_vec()).fit_to_exact_size(egui::vec2(14.0, 14.0)));
                        ui.label(&res.path);
                    });
                }
            });
        });
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([800.0, 600.0]).with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
