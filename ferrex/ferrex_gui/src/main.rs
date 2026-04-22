#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner, RawEntry};
use storage::{save_index, LoadedIndex, FileRecord};
use search::Searcher;
use std::sync::Arc;
use std::process::Command;

struct FerrexApp {
    query: String,
    results: Vec<usize>,
    index_data: Option<Arc<LoadedIndex>>,
    status: String,
    hardware_verified: bool,
}

impl FerrexApp {
    fn new(_cc: &eframe::CreationContext<'_>) -> Self {
        let mut app = Self {
            query: String::new(),
            results: Vec::new(),
            index_data: None,
            status: "初始化中...".to_string(),
            hardware_verified: false,
        };

        app.hardware_verified = app.verify_hardware_wmi();
        if app.hardware_verified {
            app.load_or_build_index();
        } else {
            app.status = "未授权的硬件设备，请联系开发者。".to_string();
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
                let lines: Vec<&str> = text.lines().collect();
                if lines.len() >= 2 {
                    let id = lines[1].trim();
                    if !id.is_empty() && whitelist.iter().any(|&w| w.eq_ignore_ascii_case(id)) {
                        return true;
                    }
                }
            }
        }

        // Mock check for the sandbox environment if needed, but per "no brain-supplement"
        // we follow the template's logic strictly.
        false
    }

    fn load_or_build_index(&mut self) {
        if let Err(_) = acquire_privileges() {
            self.status = "权限获取失败，请以管理员身份运行。".to_string();
            return;
        }

        let volumes = get_ntfs_volumes();
        if volumes.is_empty() {
            self.status = "未发现 NTFS 卷。".to_string();
            return;
        }

        let vol = &volumes[0]; // 默认处理第一个卷
        let idx_path = format!("{}_drive.idx", vol.replace(":", ""));

        if let Ok(loaded) = LoadedIndex::load(&idx_path) {
            self.index_data = Some(Arc::new(loaded));
            self.status = format!("已加载索引: {} (文件总数: {})", vol, self.index_data.as_ref().unwrap().record_count);
        } else {
            self.status = format!("正在构建 {} 索引，请稍候...", vol);
            if let Ok(scanner) = MftScanner::new(vol) {
                if let Ok(raw_entries) = scanner.scan() {
                    let mut string_pool = Vec::new();
                    let mut records = Vec::with_capacity(raw_entries.len());
                    for entry in raw_entries {
                        let name_offset = string_pool.len() as u32;
                        string_pool.extend_from_slice(entry.name.as_bytes());
                        string_pool.push(0); // Null terminator per Phase 4
                        records.push(FileRecord {
                            frn: entry.frn,
                            parent_frn: entry.parent_frn,
                            size: entry.file_size,
                            timestamp: entry.modified,
                            name_offset,
                            flags: entry.flags,
                        });
                    }
                    let _ = save_index(&idx_path, &records, &string_pool, 0, 0);
                    if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                        self.index_data = Some(Arc::new(loaded));
                        self.status = format!("索引构建完成: {} (文件总数: {})", vol, self.index_data.as_ref().unwrap().record_count);
                    }
                }
            }
        }
    }
}

impl eframe::App for FerrexApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let mut visuals = egui::Visuals::dark();
        visuals.widgets.active.bg_fill = egui::Color32::from_rgb(255, 140, 0); // #FF8C00
        ctx.set_visuals(visuals);

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(10.0);
                ui.heading(egui::RichText::new("Ferrex NTFS Indexer").color(egui::Color32::from_rgb(255, 140, 0)));
                ui.add_space(10.0);
            });

            if !self.hardware_verified {
                ui.centered_and_justified(|ui| {
                    ui.colored_label(egui::Color32::RED, "未授权的硬件设备\n请联系开发者 Telegram: SYQ_14");
                });
                return;
            }

            ui.horizontal(|ui| {
                ui.label("搜索:");
                let response = ui.add(egui::TextEdit::singleline(&mut self.query).hint_text("输入文件名关键词..."));
                if response.changed() {
                    if let Some(ref data) = self.index_data {
                        let searcher = Searcher::new(data.get_records(), data.get_string_pool());
                        self.results = searcher.search(&self.query);
                    }
                }
            });

            ui.add_space(5.0);
            ui.label(egui::RichText::new(&self.status).small().color(egui::Color32::GRAY));

            ui.separator();

            egui::ScrollArea::vertical().auto_shrink([false; 2]).show(ui, |ui| {
                if let Some(ref data) = self.index_data {
                    let records = data.get_records();
                    let pool = data.get_string_pool();
                    for &idx in self.results.iter().take(200) {
                        let rec = &records[idx];
                        let offset = rec.name_offset as usize;
                        let name_slice = &pool[offset..];
                        let end = name_slice.iter().position(|&b| b == 0).unwrap_or(name_slice.len());
                        let name = String::from_utf8_lossy(&name_slice[..end]);

                        ui.horizontal(|ui| {
                            if rec.flags & 0x10 != 0 {
                                ui.label("📁");
                            } else {
                                ui.label("📄");
                            }
                            ui.label(name);
                        });
                    }
                }
            });
        });
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([700.0, 500.0])
            .with_title("Ferrex"),
        ..Default::default()
    };
    eframe::run_native(
        "Ferrex",
        options,
        Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))),
    )
}
