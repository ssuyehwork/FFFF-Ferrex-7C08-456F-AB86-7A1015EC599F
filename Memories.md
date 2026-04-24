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