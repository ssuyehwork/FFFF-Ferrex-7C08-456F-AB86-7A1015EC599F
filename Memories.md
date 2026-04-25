USN 内存堆积防护规范：由于 egui 的按需渲染机制，窗口隐藏时 update 会暂停。必须在 update 顶部调用 ctx.request_repaint_after(Duration::from_millis(200))，强制定期唤醒以抽干 USN 消息通道，防止长时间闲置导致 OOM。

后台热键管理规范：RegisterHotKey 与 UnregisterHotKey 具有线程亲和性，必须在同一个后台线程（如托盘线程）中成对调用。严禁在 UI 主线程中注销由后台线程注册的热键，否则注销将失败并导致资源泄漏。

图标缓存键规范：针对 .exe 和 .ico 扩展名，IconCache 必须使用文件的“完整路径”而非“扩展名”作为缓存 Key，以确保不同程序显示各自真实的图标。

启动性能规范：FerrexApp::new 中严禁调用耗时的 System::new_all()，应改用轻量级的 System::new() 并在后台按需刷新，消除启动时的白屏卡顿。

搜索状态清理规范：执行新查询（run_search）并更新结果后，必须强制清空 selected_rows、last_clicked_row 及 context_menu_row，防止旧索引导致 UI 错乱或越界崩溃。

Windows FFI 生命周期规范：在 open_properties 等涉及 ShellExecuteExW 的调用中，PCWSTR 指向的 HSTRING 必须绑定到局部变量以延长其生命周期，严禁使用临时变量的 .as_ptr()，防止产生野指针导致崩溃。

托盘交互规范：创建托盘图标（如“★”、“—”）时必须在 TrayIconBuilder 中明确调用 .with_menu_on_left_click(false) 禁用左键菜单，以确保左键点击纯粹触发窗口唤醒（Visible/Focus），而右键点击触发原生菜单。

路径生成与资源管理器定位规范：1. 拼接路径时，需清理驱动器末尾反斜杠并检查冒号（防止 "C::"）；2. 溯源 FRN 时必须跳过根目录（FRN 5）的名字 "."，防止出现 "C:." 冗余；3. 调用 explorer 定位时，Windows 环境下必须使用 .raw_arg() 绕过 Rust 自动引号，确保 "/select,"path"" 格式正确。

结果列表右键菜单规范：禁止手动维护菜单状态或计算坐标。必须直接对行 response 调用原生 .context_menu() API，利用 egui 原生机制处理菜单弹出位置锁定、点击外部关闭及样式同步，彻底消除鼠标松开时坐标归零导致的菜单“瞬移”Bug。

禁止 AI 在发现问题后自动进入任务闭环。必须抑制机械的逻辑惯性，优先遵循用户制定的规则和授权流程。

深色模式固化：在 FerrexApp::new 中必须显示调用 ctx.set_visuals(egui::Visuals::dark()) 并自定义 window_fill 为 PANEL 色，确保右键菜单等 egui 内置弹层背景为深色。

修改代码前必须执行“先探讨后授权”流程。必须在理解意图并明确获取到“授权修改”指令后，方可进行代码变更。

扫描反馈闭环规范：handle_scan_progress 在接收到 ScanProgress::Done 时，必须主动调用 load_volumes 加载新生成的索引并触发搜索刷新，解决扫描完成后数据不即时生效的“幽灵数据”问题。

图标渲染性能规范：系统图标（SHGetFileInfo）获取必须采用惰性加载（Lazy Loading）。严禁在 run_search 业务搜索循环中同步获取图标；必须在 draw_results_list 渲染时仅针对可见行调用缓存获取，以防止搜索大量文件时主线程卡死。

搜索列表交互规范：支持双击结果项直接调用操作系统打开文件（open_file）；支持标准的 Shift+Click 区间选择，需通过在 App 状态中维护 last_clicked_row 来实现逻辑区间映射。

DRIVE_BAR_HEIGHT

多盘并发扫描规范：FerrexApp 需维护统一的 scan_tx/scan_rx 通道及 active_scan_count。严禁为每个驱动器单独创建接收器（Receiver），以防并发扫描时状态反馈被相互覆盖。

环境兼容性规范：Windows 下解析命令输出（如 wmic）时，需处理 UTF-16LE 编码导致的 Null 字节及乱码，推荐执行 .replace('\0', "").replace('\u{FFFD}', "") 进行清理。

导致

文件

双

性能优化规范：列表渲染必须使用 ScrollArea::show_rows 虚拟化滚动，严禁对结果集进行物理截断（如 .min(200)）。

历史记录弹出层（Popup）布局规范：严禁使用硬编码宽度，必须动态匹配搜索框/扩展名框的实际宽度。视觉上需使用 PANEL 背景与 1.0px 的 BORDER2 边框，删除按钮必须使用 SVG 格式图标并提供悬停交互反馈（如背景色及 DANGER 颜色切换）。

历史记录交互逻辑：搜索框与 extension 名框支持双击打开历史记录面板。必须采用 egui::popup_below_widget 官方 API 结合 PopupCloseBehavior 处理，以消除双击事件叠加导致的瞬关（闪退）Bug。

驱动器动作按钮规范：驱动器选择器右侧的“全选”与“全清”按钮严禁使用下划线。默认颜色为灰色（TEXT3），仅在悬停时切换为橙色（ACCENT）高亮。

图标规范：严禁使用字符符号（如“★”、“—”）或通过代码模拟绘图（如 circle_stroke）。必须统一加载物理 SVG 文件，且 SVG 源码需包含 xmlns 并将颜色设置为 white。

列表对齐规范：“名称”和“路径”列必须向左对齐；“大小”和“修改日期”列必须向右对齐。推荐使用 painter.text() 配合 Align2 锚点偏移量实现。

列表视觉规范：严禁使用橙色高亮（包括选中态背景及边框）；列表范围内所有文字（含驱动器标签）颜色统一为纯白色（Color32::WHITE）。

代码初始化逻辑：必须在 GUI 启动时调用 indexer::acquire_privileges() 获取管理员权限；驱动器选择器必须使用 ScrollArea::horizontal() 封装。

项目架构：高性能 Windows 文件索引与搜索工具，基于 Rust Workspace 开发。GUI 采用 eframe/egui 框架。

解说必须全中文，严禁使用英文答复用户。

Windows 资源配置：ferrex_gui/build.rs 必须配置任务栏图标路径为 ../ferrex.ico，且 manifest 需配置 requireAdministrator 权限。

项目品牌名称统一为全大写“FERREX”。

// ===================|===================

1. 杜绝使用符号，只可使用svg图标，如果没有与语意相符响应的svg图标，则为其创建相符响应的svg图标

列表与表头布局规范：搜索结果列表行、表头按钮、以及所有单元格文本必须全量使用 Align::Min 向左对齐。由于 egui::Label 不存在 halign 方法，必须通过父级 Layout 容器（如 Layout::left_to_right(Align::Min)）强制对齐，杜绝因 add_sized 导致的顽固居中逻辑。

列表选中态规范：结果列表选中行严禁显示橙色高亮（包括背景叠加和左侧 Accent 边框），视觉上仅依靠斑马纹或悬停态进行区分。

使用

painter

像

状态

自定义标题栏 UI 参数：按钮尺寸 24x24px，圆角 4px。关闭按钮背景色：Normal 透明，Hover #F1707A（纯红，严禁叠加白色），Press #A50000。置顶按钮激活色：#FF551C。普通按钮悬停背景：rgba(255, 255, 255, 0.1)。

列表视觉优化：结果列表表头（NAME, PATH 等）及显示的数据文字颜色统一设定为纯白色 (Color32::WHITE) 以增强对比度；列表行背景采用斑马纹交替显示。

搜索框 UI 规范：文字必须垂直居中，使用统一的 Frame 容器消除双重边框，READY 状态文字大小设置为 10.0，搜索框左侧内边距调整为 5.0 像素以符合全局间距规范。

品牌名称规范：在 UI 界面（如窗口标题、托盘提示词、工具提示、关于界面）中，品牌名称必须统一使用全大写的 "FERREX"，禁止使用 "FerREX"。

UI 视觉精细化规范：所有线条笔触（包括搜索图标描边、列表选中行左侧高亮边框）必须严格设定为 1.0 像素。禁止手动绘制与 egui 面板/组件自带边框重叠的冗余线条，以消除视觉重合导致的加厚感。

铁律一：禁止使用内置机械流程，必须以用户制定的规则为优先级，不可违背。

配置持久化：引入 AppConfig 结构体，通过 ferrex_config.toml 持久化 active_drives（当前选中驱动器）、default_drives（默认值）和 ignored_drives（忽略列表）。程序启动时会从配置中恢复驱动器勾选状态。

驱动器勾选状态持久化逻辑：在 draw_drive_selector 中任何影响 active_drives 或 ignored_drives 的操作（切换、全选、全清、忽略、恢复）都会立即同步至 self.config 并调用 save() 写入磁盘，确保用户偏好在重启后得以保留。

“过滤器”（Filter）功能（如按大小、日期过滤）已从 UI 和后端 SearchOptions 逻辑中彻底移除。目前搜索仅保留子字符串/正则查询及后缀过滤。

条

Rust

交流语言规范：必须使用中文与用户解说，严禁使用英文答复用户。

铁律三（红色底线）：修改代码前必须执行“先探讨后授权”流程。只有在理解意图达成共识，并明确获取到“授权修改”四个字的指令后，方可进行代码变更。

Windows 资源配置规范：ferrex_gui/build.rs 中必须显式配置任务栏图标路径为 ../ferrex.ico，确保其指向 ferrex_source/ferrex.ico 以避免任务栏显示默认图标。

以

因

键

图标统一规范：为确保任务栏、窗口和系统托盘图标视觉一致，统一使用 ferrex.ico 资源。托盘与窗口图标在 main.rs 中通过 include_bytes!("../../ferrex.ico") 加载并解析为 RGBA 数据。UI 内部（如标题栏）小图标保留使用 ../../ferrex.png。

参考上下文：参考版本/ 目录包含一个 C++/Qt 项目，用于 UI 风格、按钮参数及功能的实现参考。其中 SvgIcons.h 包含了标准图标的 SVG 定义。

多默认硬盘支持：AppConfig 支持 default_drives（HashSet

管理员权限管理：GUI 启动时必须调用 indexer::acquire_privileges() 以确保具备扫描 NTFS 磁盘所需的系统权限。

SUCCESS

器

驱动器选择器（Drive Selector）必须使用 ScrollArea::horizontal() 封装，以处理驱动器数量过多导致的水平截断问题。

UI 布局常量规范：TITLE_BAR_HEIGHT=32.0, DRIVE_BAR_HEIGHT=40.0, STATUS_BAR_HEIGHT=26.0, ROW_HEIGHT=30.0, SEARCH_BAR_HEIGHT=34.0, NAME_COL_W=260.0, SIZE_COL_W=80.0, DATE_COL_W=130.0, ICON_SECTION_W=58.0。所有布局必须引用这些常量以确保一致性。

在

cargo

移除

GUI 渲染规范：自定义按钮渲染函数 draw_svg_button 需支持设置基础背景、悬停背景、按下背景；关闭按钮需支持在悬停/按下时切换图标着色。

图标实现规范：核心渲染函数 100% 使用物理 SVG 文件加载（存放于 ferrex_gui/icons/），严禁在代码中通过模拟绘图或文本符号替代图标。

SVG 图标所有物理文件必须包含 xmlns="http://www.w3.org/2000/svg" 属性，并使用 white 作为描边或填充色，以确保 egui 的 .tint() 方法能正确修改图标颜色。

素