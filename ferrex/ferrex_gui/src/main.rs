#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use egui_extras::install_image_loaders;
use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner};
use storage::{save_index, LoadedIndex, FileRecord};
use search::Searcher;
use std::sync::Arc;
use std::process::Command;

const SVG_FILE: &str = "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FF8C00\" stroke-width=\"2\"><path d=\"M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z\"></path><polyline points=\"13 2 13 9 20 9\"></polyline></svg>";
const SVG_FOLDER: &str = "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FF8C00\" stroke-width=\"2\"><path d=\"M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z\"></path></svg>";

struct FerrexApp {
    query: String,
    results: Vec<usize>,
    index_data: Option<Arc<LoadedIndex>>,
    status: String,
    hardware_verified: bool,
}

impl FerrexApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        install_image_loaders(&cc.egui_ctx);

        let mut fonts = egui::FontDefinitions::default();
        // 嵌入微软雅黑字体
        fonts.font_data.insert(
            "msyh".to_owned(),
            egui::FontData::from_static(include_bytes!("../../resources/msyh.ttc")),
        );
        fonts.families.get_mut(&egui::FontFamily::Proportional).unwrap().insert(0, "msyh".to_owned());
        fonts.families.get_mut(&egui::FontFamily::Monospace).unwrap().push("msyh".to_owned());
        cc.egui_ctx.set_fonts(fonts);

        let mut app = Self {
            query: String::new(),
            results: Vec::new(),
            index_data: None,
            status: "正在初始化系统...".to_string(),
            hardware_verified: false,
        };

        app.hardware_verified = app.verify_hardware_wmi();
        if app.hardware_verified {
            app.load_or_build_index();
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

    fn load_or_build_index(&mut self) {
        if let Err(_) = acquire_privileges() {
            self.status = "权限获取失败，请确认管理员权限。".to_string();
            return;
        }
        let volumes = get_ntfs_volumes();
        if volumes.is_empty() {
            self.status = "未发现可用驱动器。".to_string();
            return;
        }
        let vol = &volumes[0];
        let idx_path = format!("{}_drive.idx", vol.replace(":", ""));
        if let Ok(loaded) = LoadedIndex::load(&idx_path) {
            self.index_data = Some(Arc::new(loaded));
            self.status = format!("索引就绪: {} ({} 条记录)", vol, self.index_data.as_ref().unwrap().record_count);
        } else {
            self.status = format!("正在构建索引... ({})", vol);
        }
    }
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
                let response = ui.add(egui::TextEdit::singleline(&mut self.query).hint_text("输入搜索关键词..."));
                if response.changed() {
                    if let Some(ref data) = self.index_data {
                        let searcher = Searcher::new(data.get_records(), data.get_string_pool());
                        self.results = searcher.search(&self.query);
                    }
                }
            });
            ui.label(egui::RichText::new(&self.status).small().color(egui::Color32::GRAY));
            ui.separator();

            egui::ScrollArea::vertical().show(ui, |ui| {
                if let Some(ref data) = self.index_data {
                    let records = data.get_records();
                    let pool = data.get_string_pool();
                    for &idx in self.results.iter().take(100) {
                        let rec = &records[idx];
                        let offset = rec.name_offset as usize;
                        let name = String::from_utf8_lossy(&pool[offset..]).split('\0').next().unwrap_or("").to_string();

                        ui.horizontal(|ui| {
                            let svg_str = if (rec.flags & 0x10) != 0 { SVG_FOLDER } else { SVG_FILE };
                            let img_id = if (rec.flags & 0x10) != 0 { "folder.svg" } else { "file.svg" };

                            ui.add(egui::Image::from_bytes(img_id, svg_str.as_bytes().to_vec())
                                .fit_to_exact_size(egui::vec2(16.0, 16.0)));

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
    eframe::run_native("Ferrex", options, Box::new(|cc| Ok(Box::new(FerrexApp::new(cc)))))
}
