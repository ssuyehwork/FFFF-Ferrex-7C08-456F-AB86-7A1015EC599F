## 第一部分：代码审查 — Jules 的实现与原始提示词的差距

### 严重偏差（Critical Gaps）

| 原始提示词要求 | Jules 的实现 | 评级 |
|---|---|---|
| 每个 crate 独立职责，CLI 有 `index` / `search` / `watch` 子命令 | CLI 已实现 ✅ | OK |
| mmap 加载 `.idx` 启动时不重扫 | `ferrex_cli` 已修复 ✅ | OK |
| GUI 加载所有卷的 `.idx`，多卷并行搜索 | GUI **只加载 `volumes[0]`**，完全忽略其他卷 | 严重 |
| USN Journal 监听后台线程 | **完全未实现**，`watch` 命令只打印一句话 | 严重 |
| FRN 关联建树，搜索结果显示**完整路径** | GUI 显示的只是**文件名**，没有路径 | 严重 |
| `sorted_idx` 预排序 + 二分查找 | **未实现**，只做线性扫描 | 中等 |
| Structure of Arrays 内存布局 | **未实现**，用的是 AoS | 中等 |
| LRU 路径缓存（容量 10000） | **未实现** | 中等 |
| 硬件白名单 `verify_hardware_wmi` | Jules 写了但 **永远返回 false**（bug） | 严重 |
| SVG 图标，无 Emoji | GUI 用的是内嵌 SVG 字符串但加载方式极其低效，每帧都 `from_bytes` | 严重 |
| 图标缓存池，提前预热 | **完全未实现** | 严重 |
| 盘符勾选筛选 | **完全未实现** | 严重 |
| 扩展名过滤输入框 | **完全未实现** | 严重 |

---

## 重要补充说明（用户最新指令）

1. **字体要求**：后期特意要求修改为 **微软雅黑** (Microsoft YaHei)，无需使用 JetBrains Mono 或 Barlow Condensed。
2. **图标要求**：支持使用 **Windows 系统图标 API** (SHGetFileInfoW)，无需使用 SVG 图标池。
3. **优先级**：以上两条补充指令优先级最高，覆盖下文中关于 SVG 和 字体 的旧有描述。

---

## 第二部分：UI 提示词（交给 Jules 实现）

```
# Task: Implement Ferrex GUI — High-Fidelity UI Redesign

## Overview
Replace the current `ferrex_gui/src/main.rs` entirely.
The new UI must be implemented using `eframe` + `egui` (already in Cargo.toml).
The visual design is strictly defined below. Jules must follow it exactly —
no improvisation, no "simplified version", no placeholder layouts.

---

## Aesthetic Direction: Precision Industrial Instrument
Think high-end oscilloscope control panel. Every pixel is intentional.
Dark, dense, information-rich. Orange accent (#FF8C00) on near-black backgrounds.
Monospace font throughout for data cells. Condensed font for labels and headers.
NO emojis anywhere in the codebase. SVG icons only, loaded from a pre-warmed cache.

---

## Font Setup
Load these two fonts at startup via `egui::FontDefinitions`:

1. **JetBrains Mono** — used for all file names, paths, sizes, timestamps, input fields
   - Load from embedded bytes: include the .ttf in `ferrex_gui/assets/JetBrainsMono-Regular.ttf`
   - Register as family name `"mono"`

2. **Barlow Condensed** — used for labels, column headers, status bar, drive chips, section titles
   - Load from embedded bytes: include the .ttf in `ferrex_gui/assets/BarlowCondensed-SemiBold.ttf`
   - Register as family name `"cond"`

If font files are unavailable, fall back to egui's built-in monospace.

---

## Color Palette (use these exact values everywhere)
```
BG        = Color32::from_rgb(7,   9,  11)   // #07090B — main background
PANEL     = Color32::from_rgb(13,  16,  20)  // #0D1014 — titlebar, statusbar
BG2       = Color32::from_rgb(17,  21,  25)  // #111519 — input backgrounds
BG3       = Color32::from_rgb(22,  27,  32)  // #161B20 — hover bg, drive chip bg
BORDER    = Color32::from_rgb(30,  37,  44)  // #1E252C — dividers
BORDER2   = Color32::from_rgb(37,  46,  55)  // #252E37 — widget borders
ACCENT    = Color32::from_rgb(255, 140,  0)  // #FF8C00 — primary accent (orange)
ACCENT2   = Color32::from_rgb(201, 110,  0)  // #C96E00 — darker orange (hover)
TEXT      = Color32::from_rgb(200, 212, 220) // #C8D4DC — primary text
TEXT2     = Color32::from_rgb(122, 143, 158) // #7A8F9E — secondary text
TEXT3     = Color32::from_rgb(61,  80,  96)  // #3D5060 — dim/hint text
SUCCESS   = Color32::from_rgb(46,  204, 113) // #2ECC71
DANGER    = Color32::from_rgb(231,  76,  60) // #E74C3C
```

---

## SVG Icon Cache Pool

Define a struct `IconCache` at the top of `main.rs`:

```rust
use std::collections::HashMap;

struct IconCache {
    map: HashMap<&'static str, egui::ImageSource<'static>>,
}

impl IconCache {
    fn new() -> Self {
        let mut map = HashMap::new();
        // Pre-load all icons at startup — never load inside render loop
        map.insert("file",   egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_file.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/file.svg")),
        });
        map.insert("folder", egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_folder.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/folder.svg")),
        });
        map.insert("exe",    egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_exe.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/exe.svg")),
        });
        map.insert("image",  egui::ImageSource::Bytes {
            uri: std::borrow::Cow::Borrowed("bytes://icon_image.svg"),
            bytes: egui::load::Bytes::Static(include_bytes!("../assets/icons/image.svg")),
        });
        map.insert("doc",    egui::ImageSource::Bytes {
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
```

Create these SVG files in `ferrex_gui/assets/icons/`:

**file.svg**
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="#7A8F9E" stroke-width="1.2" xmlns="http://www.w3.org/2000/svg">
  <path d="M9 1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5z" stroke-linejoin="round"/>
  <polyline points="9,1 9,5 13,5" stroke-linejoin="round"/>
</svg>
```

**folder.svg**
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="#C96E00" stroke-width="1.2" xmlns="http://www.w3.org/2000/svg">
  <path d="M1 4a1 1 0 0 1 1-1h4l2 2h6a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1z" stroke-linejoin="round"/>
</svg>
```

**exe.svg**
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="#7A8F9E" stroke-width="1.2" xmlns="http://www.w3.org/2000/svg">
  <rect x="2" y="2" width="12" height="12" rx="1"/>
  <path d="M5 8l2-2 2 2 2-2" stroke-linecap="round"/>
  <line x1="5" y1="11" x2="11" y2="11" stroke-linecap="round"/>
</svg>
```

**image.svg**
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="#7A8F9E" stroke-width="1.2" xmlns="http://www.w3.org/2000/svg">
  <rect x="1" y="3" width="14" height="10" rx="1"/>
  <circle cx="5.5" cy="6.5" r="1.2" fill="#7A8F9E" stroke="none"/>
  <polyline points="1,11 5,7.5 8,10 11,7 15,11" stroke-linejoin="round"/>
</svg>
```

**doc.svg**
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="#7A8F9E" stroke-width="1.2" xmlns="http://www.w3.org/2000/svg">
  <path d="M9 1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5z" stroke-linejoin="round"/>
  <polyline points="9,1 9,5 13,5"/>
  <line x1="5" y1="8" x2="11" y2="8" stroke-linecap="round"/>
  <line x1="5" y1="10.5" x2="11" y2="10.5" stroke-linecap="round"/>
  <line x1="5" y1="13" x2="8" y2="13" stroke-linecap="round"/>
</svg>
```

---

## Application State

```rust
struct FerrexApp {
    // Search
    query:          String,
    ext_filter:     String,   // extension filter — user types "rs", dot is prepended in display
    results:        Vec<SearchResult>,

    // Index stores — one per loaded volume
    stores:         Vec<(String, Arc<LoadedIndex>)>,  // (drive_letter, index)

    // Drive selector — which drives are active for search
    active_drives:  std::collections::HashSet<String>,

    // Status
    status_text:    String,
    total_records:  usize,

    // Icon cache — initialized once at startup
    icons:          IconCache,

    // Hardware lock
    hardware_ok:    bool,
}

struct SearchResult {
    drive:      String,
    name:       String,
    full_path:  String,
    size:       u64,
    timestamp:  u64,
    is_dir:     bool,
}
```

---

## Layout — Exact Panel Regions

The window is a fixed grid of horizontal strips from top to bottom.
Use `egui::TopBottomPanel` and `egui::CentralPanel` to implement this:

```
┌─────────────────────────────────────────────────────┐
│ TITLEBAR         height: 44px   bg: PANEL           │
│  [hex logo] FERREX   [status dot + record count]    │
├─────────────────────────────────────────────────────┤  ← 1px BORDER separator
│ DRIVE SELECTOR   height: 40px   bg: BG2             │
│  DRIVES  [C:] [G:] [H:] [I:] [Z:]     [全选/全清]  │
├─────────────────────────────────────────────────────┤  ← 1px BORDER separator
│ SEARCH BAR       height: 46px   bg: PANEL           │
│  [search icon][  filename input  ][.][ext][ SEARCH ]│
├─────────────────────────────────────────────────────┤  ← 1px BORDER separator
│ COLUMN HEADER    height: 30px   bg: BG2             │
│   [icon] NAME              PATH          SIZE  DATE │
├─────────────────────────────────────────────────────┤  ← 1px BORDER separator
│ RESULTS LIST     flex height    bg: BG              │
│   row × N                                           │
├─────────────────────────────────────────────────────┤  ← 1px BORDER separator
│ STATUS BAR       height: 26px   bg: PANEL           │
│  结果: N   耗时: Xms   盘符: C: G:       v0.1.0    │
└─────────────────────────────────────────────────────┘
```

---

## Titlebar Implementation

```rust
egui::TopBottomPanel::top("titlebar")
    .exact_height(44.0)
    .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
    .show(ctx, |ui| {
        ui.horizontal_centered(|ui| {
            // Hexagon logo SVG (inline, 22x22)
            // Draw using egui painter: a regular hexagon outline in ACCENT color
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

            // Logo text
            ui.label(
                egui::RichText::new("FERREX")
                    .font(egui::FontId::new(18.0, egui::FontFamily::Name("cond".into())))
                    .color(ACCENT)
                    .letter_spacing(2.5)
            );

            ui.add_space(14.0);
            // Badge
            ui.label(
                egui::RichText::new("NTFS INDEXER")
                    .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
                    .color(TEXT3)
            );

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                // Status dot + record count
                let dot_color = if self.total_records > 0 { SUCCESS } else { TEXT3 };
                ui.colored_label(dot_color, "●");
                ui.add_space(4.0);
                ui.label(
                    egui::RichText::new(format!("索引就绪 — {:>10} 条记录",
                        format_number(self.total_records)))
                        .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
                        .color(TEXT3)
                );
            });
        });
    });
```

---

## Drive Selector Implementation

Each drive chip is a clickable `egui::Button` styled as a compact tag:
- **Active state**: border = ACCENT, bg = ACCENT with alpha 20, text = ACCENT
- **Inactive state**: border = BORDER2, bg = BG3, text = TEXT2
- Show drive letter + record count (e.g., `C: 2.3M`)
- At least one drive must always remain active (prevent deselecting the last one)

```rust
egui::TopBottomPanel::top("drives")
    .exact_height(40.0)
    .frame(egui::Frame::none().fill(BG2).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
    .show(ctx, |ui| {
        ui.horizontal_centered(|ui| {
            ui.label(
                egui::RichText::new("DRIVES")
                    .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
                    .color(TEXT3)
            );
            ui.add_space(8.0);

            for (drive, store) in &self.stores {
                let is_active = self.active_drives.contains(drive);
                let count_str = format_count(store.record_count);
                let label = format!("{}:  {}", drive, count_str);

                let (bg, stroke_color, text_color) = if is_active {
                    (Color32::from_rgba_unmultiplied(255, 140, 0, 30), ACCENT, ACCENT)
                } else {
                    (BG3, BORDER2, TEXT2)
                };

                let btn = egui::Button::new(
                    egui::RichText::new(&label)
                        .font(egui::FontId::new(12.0, egui::FontFamily::Name("cond".into())))
                        .color(text_color)
                )
                .fill(bg)
                .stroke(egui::Stroke::new(1.0, stroke_color))
                .rounding(egui::Rounding::ZERO);

                if ui.add(btn).clicked() {
                    if is_active && self.active_drives.len() > 1 {
                        self.active_drives.remove(drive);
                        self.run_search();
                    } else if !is_active {
                        self.active_drives.insert(drive.clone());
                        self.run_search();
                    }
                }
                ui.add_space(4.0);
            }

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let all_active = self.active_drives.len() == self.stores.len();
                let label = if all_active { "全清" } else { "全选" };
                if ui.small_button(
                    egui::RichText::new(label)
                        .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
                        .color(TEXT3)
                ).clicked() {
                    if all_active && self.stores.len() > 1 {
                        let first = self.stores[0].0.clone();
                        self.active_drives = std::collections::HashSet::from([first]);
                    } else {
                        self.active_drives = self.stores.iter().map(|(d,_)| d.clone()).collect();
                    }
                    self.run_search();
                }
            });
        });
    });
```

---

## Search Bar Implementation

Three widgets in a single horizontal strip, visually connected (no gaps):
1. Search icon placeholder (18x36, bg=BG2, border-right=none)
2. Main text input — file name / keyword (flex width)
3. `.` separator label (fixed 24px, bg=BG3, color=ACCENT)
4. Extension input (fixed 80px, placeholder="ext")
5. SEARCH button (fixed, bg=ACCENT, text=black, font=cond bold)

```rust
egui::TopBottomPanel::top("searchbar")
    .exact_height(46.0)
    .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 5.0)))
    .show(ctx, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;

            // Search icon (simple magnifier drawn with painter)
            let (icon_rect, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
            ui.painter().rect_filled(icon_rect, egui::Rounding::ZERO, BG2);
            ui.painter().rect_stroke(icon_rect, egui::Rounding::ZERO, egui::Stroke::new(1.0, BORDER2));
            // Draw search circle + line
            let c = egui::pos2(icon_rect.center().x - 2.0, icon_rect.center().y - 2.0);
            ui.painter().circle_stroke(c, 5.5, egui::Stroke::new(1.5, TEXT3));
            ui.painter().line_segment(
                [egui::pos2(c.x + 4.0, c.y + 4.0), egui::pos2(c.x + 8.0, c.y + 8.0)],
                egui::Stroke::new(1.5, TEXT3)
            );

            // Main search input
            let search_response = ui.add_sized(
                egui::vec2(ui.available_width() - 80.0 - 24.0 - 80.0, 36.0),
                egui::TextEdit::singleline(&mut self.query)
                    .font(egui::FontId::new(13.0, egui::FontFamily::Name("mono".into())))
                    .hint_text(egui::RichText::new("文件名 / 关键词...").color(TEXT3))
                    .frame(true)
                    .text_color(TEXT)
                    .cursor_color(ACCENT)
            );

            // "." dot separator
            let (dot_rect, _) = ui.allocate_exact_size(egui::vec2(24.0, 36.0), egui::Sense::hover());
            ui.painter().rect_filled(dot_rect, egui::Rounding::ZERO, BG3);
            ui.painter().text(
                dot_rect.center(),
                egui::Align2::CENTER_CENTER,
                ".",
                egui::FontId::new(16.0, egui::FontFamily::Name("mono".into())),
                ACCENT,
            );

            // Extension input
            let ext_response = ui.add_sized(
                egui::vec2(80.0, 36.0),
                egui::TextEdit::singleline(&mut self.ext_filter)
                    .font(egui::FontId::new(13.0, egui::FontFamily::Name("mono".into())))
                    .hint_text(egui::RichText::new("扩展名").color(TEXT3))
                    .frame(true)
                    .text_color(TEXT)
            );

            ui.add_space(10.0);

            // Search button
            let search_btn = egui::Button::new(
                egui::RichText::new("搜索")
                    .font(egui::FontId::new(13.0, egui::FontFamily::Name("cond".into())))
                    .color(Color32::BLACK)
                    .strong()
            )
            .fill(ACCENT)
            .stroke(egui::Stroke::new(1.0, ACCENT))
            .rounding(egui::Rounding::ZERO)
            .min_size(egui::vec2(70.0, 36.0));

            if ui.add(search_btn).clicked()
                || search_response.changed()
                || ext_response.changed()
            {
                self.run_search();
            }
        });
    });
```

---

## Column Header

Fixed 30px strip with 5 columns matching result rows:

```
grid: [24px icon] [1fr name] [2fr path] [80px size right-aligned] [140px date]
```

Draw column labels using `"cond"` font, 10px, color=TEXT3, letter_spacing=1.5.
Draw a 1px BORDER horizontal separator below this panel.

---

## Results List — CentralPanel

Use `egui::ScrollArea::vertical()` wrapping the result rows.

**Each row** (`height: 30px`):
- Left border: 3px transparent normally, 3px ACCENT on hover
- Background: alternating BG / BG2, on hover always BG3
- Columns: icon (14x14) | name with drive tag | path | size | date
- Drive tag: small label `"C:"` in a box, color=TEXT3, border=BORDER2, font=cond 9px
- Name: font=mono 12.5px, color=TEXT, overflow=ellipsis
- Matched substring highlighted in ACCENT color
- Path: font=mono 11px, color=TEXT3, overflow=ellipsis
- Size: font=mono 11px, color=TEXT2, right-aligned, `"—"` for directories
- Date: font=mono 11px, color=TEXT3, format `YYYY-MM-DD HH:MM`

**Empty state** (no results):
- Centered vertically in the available space
- Draw magnifier SVG (36x36) in BORDER2 color using painter
- Label: `"无匹配结果"` font=cond 14px, color=TEXT3
- Sublabel: `"尝试更换关键词或扩大盘符范围"` font=mono 11px, color=TEXT3 with 60% opacity

---

## Status Bar

```rust
egui::TopBottomPanel::bottom("statusbar")
    .exact_height(26.0)
    .frame(egui::Frame::none().fill(PANEL).inner_margin(egui::Margin::symmetric(16.0, 0.0)))
    .show(ctx, |ui| {
        ui.horizontal_centered(|ui| {
            // Left items
            stat_item(ui, "结果", &format_number(self.results.len()));
            ui.add_space(16.0);
            stat_item(ui, "耗时", &format!("{:.1} ms", self.last_search_ms));
            ui.add_space(16.0);
            stat_item(ui, "盘符", &self.active_drives_str());

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.label(
                    egui::RichText::new("FERREX v0.1.0")
                        .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
                        .color(ACCENT)
                );
                ui.add_space(16.0);
                stat_item(ui, "上次索引", "2026-04-22 17:41");
            });
        });
    });

fn stat_item(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.label(egui::RichText::new(label)
        .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
        .color(TEXT3));
    ui.add_space(4.0);
    ui.label(egui::RichText::new(value)
        .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
        .color(TEXT2)
        .strong());
}
```

---

## Search Logic — `run_search()` method

```rust
fn run_search(&mut self) {
    let t0 = std::time::Instant::now();
    let query = self.query.trim().to_lowercase();
    let ext = self.ext_filter.trim().to_lowercase();

    self.results = self.stores.iter()
        .filter(|(d, _)| self.active_drives.contains(d.as_str()))
        .flat_map(|(drive, store)| {
            let records = store.get_records();
            let pool = store.get_string_pool();
            let searcher = Searcher::new(records, pool);
            searcher.search_with_ext(&query, if ext.is_empty() { None } else { Some(&ext) })
                .into_iter()
                .map(|idx| {
                    let rec = &records[idx];
                    let name = pool_get_name(pool, rec.name_offset as usize);
                    SearchResult {
                        drive: drive.clone(),
                        name: name.clone(),
                        full_path: format!("{}:\\{}", drive, name), // TODO: resolve from FRN
                        size: rec.size,
                        timestamp: rec.timestamp,
                        is_dir: rec.flags & 0x10 != 0,
                    }
                })
                .collect::<Vec<_>>()
        })
        .take(5000)
        .collect();

    self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;
}
```

---

## Helper Functions

```rust
fn format_number(n: usize) -> String {
    // Format with comma separators: 2338800 → "2,338,800"
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
    // ts is FILETIME (100ns intervals since 1601-01-01)
    // Convert to approximate YYYY-MM-DD HH:MM
    // Use a simple offset: subtract Windows epoch offset
    // If ts == 0, return "—"
    if ts == 0 { return "—".to_string(); }
    "—".to_string() // Implement full conversion or use chrono crate
}

fn pool_get_name(pool: &[u8], offset: usize) -> String {
    if offset >= pool.len() { return String::new(); }
    let slice = &pool[offset..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).to_string()
}
```

---

## Hardware Verification Fix

The `verify_hardware_wmi` function MUST NOT unconditionally return `false`.
Remove the final `false` and ensure the function returns `true` when any
whitelist entry matches. The existing logic above the final `false` is correct —
simply delete the final `false` and let the function end naturally returning `false`
only when NO match is found in the loop.

```rust
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
```

---

## Unauthorized Device Screen

When `hardware_ok == false`, show ONLY this centered in the window:

```
[red lock icon — draw with painter, 48x48]

未授权的硬件设备
[font: cond 16px, color: DANGER]

请联系开发者
[font: mono 12px, color: TEXT3]
```

No other UI elements visible. Window title still shows "Ferrex".

---

## Implementation Checklist for Jules
- [ ] Create `ferrex_gui/assets/` directory
- [ ] Download and place JetBrains Mono + Barlow Condensed .ttf files
- [ ] Create all 5 SVG icon files in `ferrex_gui/assets/icons/`
- [ ] Implement `IconCache` with `include_bytes!` pre-loading
- [ ] Register both fonts in `egui::FontDefinitions` before first frame
- [ ] Implement all 6 panel regions (titlebar, drives, searchbar, col-header, results, statusbar)
- [ ] Drive chips toggle correctly with at least-one-active enforcement
- [ ] Search triggers on input change AND button click
- [ ] Extension filter strips leading dot before matching
- [ ] Results capped at 5000, display capped at 200 rows for perf
- [ ] Fix `verify_hardware_wmi` — remove unconditional `false`
- [ ] All colors use the exact palette values above — NO hardcoded random colors
- [ ] NO emojis anywhere — SVG icons only
- [ ] Font "cond" for all labels/headers, "mono" for all data
```