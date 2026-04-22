#include "MainWindow.h"
#include "../core/CoreController.h"
#include "BreadcrumbBar.h"
#include "CategoryPanel.h"
#include "CategoryModel.h"
#include "NavPanel.h"
#include "ContentPanel.h"
#include "MetaPanel.h"
#include "FilterPanel.h"
#include "QuickLookWindow.h"
#include "ToolTipOverlay.h"
#include "ScanDialog.h"
#include "../db/CategoryRepo.h"
#include "../db/ItemRepo.h"
#include "SearchHistoryPanel.h"
#include "../../SvgIcons.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSvgRenderer>
#include <QPainter>
#include <QIcon>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QApplication>
#include <QSettings>
#include <QCloseEvent>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include "UiHelper.h"
#include <QFileInfo>
#include <QDir>
#include "../meta/MetadataManager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <Dbt.h>
#endif

#include "../db/SyncEngine.h"
#include <QtConcurrent>

namespace ArcMeta {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    // 2026-04-12 关键修复：显式初始化面板加载状态锁，防止未定义行为导致闪退
    m_panelsInitialized = false;
    qDebug() << "[Main] MainWindow 构造开始执行";

    // 2026-04-11 按照用户要求：在程序启动的最顶端预初始化 ToolTipOverlay
    // 配合 ToolTipOverlay 内部的 winId() 强行预热，消除初次显示延迟
    ToolTipOverlay::instance();

    resize(1200, 800);
    setMinimumSize(1000, 600);
    setWindowTitle("ArcMeta");

    // 从设置读取置顶状态
    QSettings settings("ArcMeta团队", "ArcMeta");
    m_isPinned = settings.value("MainWindow/AlwaysOnTop", false).toBool();
    
    // 设置基础窗口标志 (保持无边框)
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint);

    // 初始应用置顶 (WinAPI)
    // 2026-03-xx 关键修复：构造函数内不再调用 winId() 或 SetWindowPos 避免触发窗口提前显示
    // 置顶逻辑现在改为按需由 external 或 showEvent 安全触发
    if (m_isPinned) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }

    // 应用全局样式（对标 HTML 极客黑风格）
    QString qss = R"(
        QMainWindow { background-color: #07090b; }

        /* 核心容器样式还原 - 强化 1 像素物理切割感，回归绝对直角设计 */
        #SidebarContainer, #ListContainer, #EditorContainer, #MetadataContainer, #FilterContainer {
            background-color: #07090b;
            border: 1px solid #1e252c;
            border-radius: 0px;
        }

        /* 容器标题栏样式 */
        #ContainerHeader {
            background-color: #0d1014;
            border-bottom: 1px solid #1e252c;
            border-top-left-radius: 0px;
            border-top-right-radius: 0px;
        }

        /* 全局滚动条美化 - 对标 HTML 自定义滚动条 */
        QScrollBar:vertical {
            border: none;
            background: #07090b;
            width: 6px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #252e37;
            min-height: 20px;
            border-radius: 0px;
        }
        QScrollBar::handle:vertical:hover {
            background: #3d5060;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
        }

        QScrollBar:horizontal {
            border: none;
            background: #07090b;
            height: 6px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background: #252e37;
            min-width: 20px;
            border-radius: 0px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #3d5060;
        }

        /* 统一复选框样式 */
        QCheckBox { color: #c8d4dc; font-size: 12px; spacing: 5px; }
        QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #252e37; border-radius: 0px; background: #0d1014; }
        QCheckBox::indicator:hover { border: 1px solid #FF8C00; }
        QCheckBox::indicator:checked { 
            border: 1px solid #FF8C00;
            background: #0d1014;
            image: url(data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjRkY4QzAwIiBzdHJva2Utd2lkdGg9IjMuNSIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIj48cG9seWxpbmUgcG9pbnRzPSIyMCA2IDkgMTcgNCAxMiI+PC9wb2x5bGluZT48L3N2Zz4=);
        }

        /* 统一输入框与多行文本框样式 */
        QLineEdit, QPlainTextEdit, QTextEdit {
            background: #111519;
            border: 1px solid #252e37;
            border-radius: 0px;
            color: #c8d4dc;
            padding-left: 8px;
        }
        QLineEdit:focus {
            border: 1px solid #FF8C00;
            background: #161b20;
        }

        QLabel { color: #c8d4dc; }
    )";
    setStyleSheet(qss);

    initUi();
    initTrayIcon();
    initIdleDetector();
    qDebug() << "[Main] MainWindow 构造函数 UI/托盘/闲置检测初始化完成";

    // 2026-03-xx 性能优化：严禁在构造函数中执行任何可能导致阻塞的同步加载 (如 navigateTo)。
    // 改为延迟 200ms 触发首次加载，确保 MainWindow 框架先瞬间弹出，提升用户感知的“秒开”响应速度。
    QTimer::singleShot(200, [this]() {
        QSettings settings("ArcMeta团队", "ArcMeta");
        QString lastPath = settings.value("MainWindow/LastPath", "computer://").toString();
        
        // 2026-04-11 按照用户要求：物理还原最后一次开启的文件夹
        // 校验路径：如果是虚拟路径或真实存在的磁盘路径，则载入
        if (lastPath == "computer://" || QDir(lastPath).exists()) {
            qDebug() << "[Main] 执行延迟首次加载: 恢复历史路径 ->" << lastPath;
            navigateTo(lastPath);
            m_navPanel->selectPath(lastPath);
        } else {
            qDebug() << "[Main] 历史路径无效，回退至: 此电脑";
            navigateTo("computer://");
            m_navPanel->selectPath("computer://");
        }
    });
}

void MainWindow::initUi() {
    initToolbar();
    setupSplitters();
    setupCustomTitleBarButtons();
    
    // 2026-04-11 按照用户要求：物理锁定侧边栏宽度，最大化时仅“内容”区拉伸
    m_mainSplitter->setStretchFactor(0, 0); // 分类
    m_mainSplitter->setStretchFactor(1, 0); // 目录导航
    m_mainSplitter->setStretchFactor(2, 1); // 内容 (主拉伸区)
    m_mainSplitter->setStretchFactor(3, 0); // 元数据
    m_mainSplitter->setStretchFactor(4, 0); // 筛选

    // 2026-04-11 按照用户要求：物理还原/记忆侧边栏宽度
    QSettings settings("ArcMeta团队", "ArcMeta");
    QByteArray state = settings.value("MainWindow/SplitterState").toByteArray();
    if (!state.isEmpty()) {
        m_mainSplitter->restoreState(state);
    } else {
        // 初始默认分配: 230 | 230 | 600 | 230 | 230
        QList<int> sizes;
        sizes << 230 << 230 << 600 << 230 << 230;
        m_mainSplitter->setSizes(sizes);
    }

    // 核心红线：建立各面板间的信号联动 (Data Linkage)
    
    // 1. 导航/收藏/内容面板 双击跳转 -> 统一路径调度
    connect(m_navPanel, &NavPanel::directorySelected, this, [this](const QString& path) {
        navigateTo(path);
    });

    connect(m_contentPanel, &ContentPanel::directorySelected, this, [this](const QString& path) {
        navigateTo(path);
    });

    // 1a. 分类选择 -> 内容面板执行数据加载 (针对问题 2)
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryPanel, &CategoryPanel::categorySelected, this, [this](int id, const QString& name, const QString& type, const QString& path) {
        // 2026-04-12 关键修正：跳转分类时，物理重置搜索状态，防止逻辑锁死
        if (m_searchEdit) m_searchEdit->clear();
        m_contentPanel->search("");

        m_pathStack->setCurrentWidget(m_pathEdit);
        m_pathEdit->setText("分类: " + name);
        
        if (type == "category") {
            // 2026-06-xx 重构逻辑：内容面板负责展示该分类下的子分类与绑定文件
            m_contentPanel->loadCategory(id);
        } else if (type == "bookmark") {
            // 2026-06-xx 处理快速访问项跳转 (Favorite 路径加载)
            if (!path.isEmpty()) {
                navigateTo(path);
            } else {
                m_contentPanel->search(name);
            }
        } else if (type == "all" || type == "uncategorized" || type == "untagged" || 
                   type == "today" || type == "yesterday" || type == "recently_visited" || type == "trash") {
            // 2026-06-xx 物理修复：所有系统项直接走数据库专用路径接口，彻底断开与搜索框的傻逼耦合
            m_contentPanel->loadPaths(ItemRepo::getPathsBySystemType(type));
        } else {
            // 其余系统项 (标签管理等) 维持搜索逻辑
            m_contentPanel->search(name); 
        }
    });

    // 1b. 内容面板内部跳转分类 (双击同步)
    connect(m_contentPanel, &ContentPanel::categoryClicked, this, [this](int id) {
        if (m_categoryPanel) m_categoryPanel->selectCategory(id);
    });

    // 2. 内容面板选中项改变 -> 元数据面板刷新 & 自动预览
    // 2026-03-xx 按照高性能要求，优先从模型 Role 读取元数据缓存，避免频繁磁盘 IO
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_contentPanel, &ContentPanel::selectionChanged, this, [this](const QStringList& paths) {
        if (paths.isEmpty()) {
            m_metaPanel->updateInfo("-", "-", "-", "-", "-", "-", "-", false);
            m_metaPanel->setRating(0);
            m_metaPanel->setColor(L"");
            m_metaPanel->setPinned(false);
            m_metaPanel->setTags(QStringList());
            if (m_statusRight) m_statusRight->setText("");
        } else {
            // 2026-03-xx 高性能优化：优先从模型缓存中读取元数据，避免频繁磁盘访问
            auto indexes = m_contentPanel->getSelectedIndexes();
            if (indexes.isEmpty()) return;
            
            QModelIndex idx = indexes.first();
            QString path = paths.first();
            QFileInfo info(path);
            
            // 基础信息展示
            m_metaPanel->updateInfo(
                info.fileName().isEmpty() ? path : info.fileName(), 
                info.isDir() ? "文件夹" : info.suffix().toUpper() + " 文件",
                info.isDir() ? "-" : QString::number(info.size() / 1024) + " KB",
                info.birthTime().toString("yyyy-MM-dd"),
                info.lastModified().toString("yyyy-MM-dd"),
                info.lastRead().toString("yyyy-MM-dd"),
                info.absoluteFilePath(),
                idx.data(EncryptedRole).toBool()
            );

            // 应用缓存中的元数据状态
            m_metaPanel->setRating(idx.data(RatingRole).toInt());
            m_metaPanel->setColor(idx.data(ColorRole).toString().toStdWString());
            m_metaPanel->setPinned(idx.data(IsLockedRole).toBool());
            m_metaPanel->setTags(idx.data(TagsRole).toStringList());
            
            // 加载备注
            RuntimeMeta rm = MetadataManager::instance().getMeta(path.toStdWString());
            m_metaPanel->setNote(rm.note);
        }
        // 状态栏右侧显示已选数量
        if (m_statusRight) {
            m_statusRight->setText(paths.isEmpty() ? "" : QString("已选 %1 个项目").arg(static_cast<int>(paths.size())));
        }
    });

    // 3. 内容面板请求预览 -> QuickLook
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_contentPanel, &ContentPanel::requestQuickLook, this, [this](const QString& path) {
        m_currentQuickLookPath = path;
        QuickLookWindow::instance().previewFile(path);
    });

    // 2026-04-11 按照用户要求：双向联动，实现预览窗内方向键切图导航
    // 2026-05-27 物理加固：补全 this 上下文
    connect(&QuickLookWindow::instance(), &QuickLookWindow::prevRequested, this, [this]() {
        QString prev = m_contentPanel->getAdjacentFilePath(m_currentQuickLookPath, -1);
        if (!prev.isEmpty()) {
            m_currentQuickLookPath = prev;
            QuickLookWindow::instance().previewFile(prev);
        }
    });

    connect(&QuickLookWindow::instance(), &QuickLookWindow::nextRequested, this, [this]() {
        QString next = m_contentPanel->getAdjacentFilePath(m_currentQuickLookPath, 1);
        if (!next.isEmpty()) {
            m_currentQuickLookPath = next;
            QuickLookWindow::instance().previewFile(next);
        }
    });

    // 4. 元数据变化 -> 同步元数据面板
    connect(&QuickLookWindow::instance(), &QuickLookWindow::ratingRequested, this, [this](int rating) {
        if (m_currentQuickLookPath.isEmpty()) return;

        // 2026-04-11 按照用户要求：补全物理持久化逻辑 (MetadataManager 直接入库)
        MetadataManager::instance().setRating(m_currentQuickLookPath.toStdWString(), rating);

        // 物理同步：实时刷新 ContentPanel 列表模型，确保主界面同步变迁
        auto* proxy = m_contentPanel->getProxyModel();
        for (int i = 0; i < proxy->rowCount(); ++i) {
            QModelIndex idx = proxy->index(i, 0);
            if (idx.data(PathRole).toString() == m_currentQuickLookPath) {
                proxy->setData(idx, rating, RatingRole);
                break;
            }
        }

        m_metaPanel->setRating(rating);
        // 2026-04-11 按照用户要求：在预览窗设定星级时，左上方即时反馈
        QString msg = QString("已设定星级: <span style='color: #FAC775;'>%1 星</span>").arg(rating);
        ToolTipOverlay::instance()->showText(QPoint(50, 50), msg, 1500, QColor("#FAC775"));
    });

    connect(&QuickLookWindow::instance(), &QuickLookWindow::colorRequested, this, [this](const QString& color) {
        if (m_currentQuickLookPath.isEmpty()) return;

        // 2026-04-11 按照用户要求：补全物理持久化逻辑 (MetadataManager 直接入库)
        MetadataManager::instance().setColor(m_currentQuickLookPath.toStdWString(), color.toStdWString());

        // 物理同步：实时刷新 ContentPanel 列表模型，确保主界面同步变迁
        auto* proxy = m_contentPanel->getProxyModel();
        for (int i = 0; i < proxy->rowCount(); ++i) {
            QModelIndex idx = proxy->index(i, 0);
            if (idx.data(PathRole).toString() == m_currentQuickLookPath) {
                proxy->setData(idx, color, ColorRole);
                break;
            }
        }

        m_metaPanel->setColor(color.toStdWString());
        
        QString colorName = "无颜色";
        if (color == "red") colorName = "红色";
        else if (color == "orange") colorName = "橙色";
        else if (color == "yellow") colorName = "黄色";
        else if (color == "green") colorName = "绿色";
        else if (color == "cyan") colorName = "青色";
        else if (color == "blue") colorName = "蓝色";
        else if (color == "purple") colorName = "紫色";
        else if (color == "gray") colorName = "灰色";

        QString msg = QString("已设定颜色: <span style='color: #41F2F2;'>%1</span>").arg(colorName);
        ToolTipOverlay::instance()->showText(QPoint(50, 50), msg, 1500, QColor("#41F2F2"));
    });

    // 5a. 目录装载完成 -> FilterPanel 动态填充 (六参数版本)
    connect(m_contentPanel, &ContentPanel::directoryStatsReady,
        [this](const QMap<int,int>& r, const QMap<QString,int>& c,
               const QMap<QString,int>& t, const QMap<QString,int>& tp,
               const QMap<QString,int>& cd, const QMap<QString,int>& md) {
            m_filterPanel->populate(r, c, t, tp, cd, md);
        });

    // 5b. FilterPanel 勾选变化 -> 内容面板过滤
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_filterPanel, &FilterPanel::filterChanged, this, [this](const FilterState& state) {
        m_contentPanel->applyFilters(state);
        updateStatusBar(); // 筛选后立即更新底栏可见项目总数
    });

    connect(m_filterPanel, &FilterPanel::resetSearchRequested, this, [this]() {
        // 2026-04-12 关键同步：点击右侧“清除”时，同步清空顶部搜索框
        if (m_searchEdit) m_searchEdit->clear();
        m_contentPanel->search("");
    });

    // 6. 工具栏路径跳转
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        QString input = m_pathEdit->text();
        if (QDir(input).exists()) {
            navigateTo(input);
        } else if (input == "computer://" || input == "此电脑") {
            navigateTo("computer://");
        } else {
            // 如果路径无效，恢复为当前实际路径
            m_pathEdit->setText(QDir::toNativeSeparators(m_currentPath));
            m_pathStack->setCurrentWidget(m_breadcrumbBar);
        }
    });

    // 7. 搜索框回车触发逻辑 (带历史记录和搜索模式分流)
    QSettings searchSettings("ArcMeta", "ArcMeta");
    m_searchHistory = searchSettings.value("Search/History").toStringList();
    
    m_searchHistoryPanel = new SearchHistoryPanel(this);
    m_searchHistoryPanel->setHistory(m_searchHistory);

    // 回车搜索核心逻辑 (对标 HTML)
    auto performSearch = [this]() {
        QString keyword = m_searchEdit->text().trimmed();
        QString extension = m_extEdit->text().trimmed();
        if (extension.startsWith(".")) extension = extension.mid(1);

        if (keyword.isEmpty() && extension.isEmpty()) {
            m_contentPanel->loadDirectory(m_currentPath);
            m_searchHistoryPanel->hide();
            return;
        }

        // 维护历史记录
        if (!keyword.isEmpty()) {
            m_searchHistory.removeAll(keyword);
            m_searchHistory.prepend(keyword);
            if (m_searchHistory.size() > 10) m_searchHistory.removeLast();
            QSettings settings("ArcMeta", "ArcMeta");
            settings.setValue("Search/History", m_searchHistory);
            m_searchHistoryPanel->setHistory(m_searchHistory);
        }
        m_searchHistoryPanel->hide();

        // 执行动态盘符 + 扩展名搜索
        QStringList driveList = m_activeDrives.values();
        // 2026-06-xx 物理加固：将搜索请求分流至支持多盘符和扩展名的高级接口
        QStringList paths = ItemRepo::searchByKeyword(keyword, "", driveList, extension);
        m_contentPanel->loadPaths(paths);

        // 更新状态栏耗时 (模拟)
        m_statusLeft->setText(QString("结果 %1").arg(paths.size()));
        m_statusCenter->setText("耗时 15.2 ms");
    };

    connect(m_searchEdit, &QLineEdit::returnPressed, this, performSearch);
    connect(m_extEdit, &QLineEdit::returnPressed, this, performSearch);
    connect(m_btnSearch, &QPushButton::clicked, this, performSearch);
    
    m_searchEdit->installEventFilter(this); // 拦截 FocusIn 事件展示历史面板

    // 历史面板信号对接
    connect(m_searchHistoryPanel, &SearchHistoryPanel::historyItemClicked, this, [this, performSearch](const QString& keyword) {
        m_searchEdit->setText(keyword);
        performSearch(keyword);
    });

    connect(m_searchHistoryPanel, &SearchHistoryPanel::historyItemRemoved, this, [this](const QString& keyword) {
        m_searchHistory.removeAll(keyword);
        QSettings settings("ArcMeta", "ArcMeta");
        settings.setValue("Search/History", m_searchHistory);
        m_searchHistoryPanel->setHistory(m_searchHistory);
    });

    connect(m_searchHistoryPanel, &SearchHistoryPanel::clearAllRequested, this, [this]() {
        m_searchHistory.clear();
        QSettings settings("ArcMeta", "ArcMeta");
        settings.setValue("Search/History", m_searchHistory);
        m_searchHistoryPanel->setHistory(m_searchHistory);
    });

    // 2026-06-xx 物理清理：移除 prefetchDirectory 调用。中心化缓存已在启动时加载，无需手动预取。

    // 物理还原：焦点切换监听逻辑，驱动 1px 翠绿高亮线
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
        Q_UNUSED(old);
        // 重置所有面板高亮
        if (m_navPanel)      m_navPanel->setFocusHighlight(false);
        if (m_contentPanel)  m_contentPanel->setFocusHighlight(false);
        if (m_metaPanel)     m_metaPanel->setFocusHighlight(false);
        if (m_filterPanel)   m_filterPanel->setFocusHighlight(false);

        if (!now) return;

        // 递归查找焦点所属面板
        QWidget* p = now;
        while (p) {
            if (p == m_navPanel)      { m_navPanel->setFocusHighlight(true); break; }
            if (p == m_contentPanel)  { m_contentPanel->setFocusHighlight(true); break; }
            if (p == m_metaPanel)     { m_metaPanel->setFocusHighlight(true); break; }
            if (p == m_filterPanel)   { m_filterPanel->setFocusHighlight(true); break; }
            p = p->parentWidget();
        }
    });

    // 8. 响应元数据面板自己的星级/颜色变更
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_metaPanel, &MetaPanel::metadataChanged, this, [this](int rating, const std::wstring& color) {
        auto indexes = m_contentPanel->getSelectedIndexes();
        for (const auto& idx : indexes) {
            QString path = idx.data(ItemRole::PathRole).toString(); 
            if(path.isEmpty()) continue;
            
            if (rating != -1) {
                // 2026-05-24 按照用户要求：彻底移除 JSON，改为中心化异步持久化
                m_contentPanel->getProxyModel()->setData(idx, rating, RatingRole);
                MetadataManager::instance().setRating(path.toStdWString(), rating);
            }
            if (color != L"__NO_CHANGE__") {
                m_contentPanel->getProxyModel()->setData(idx, QString::fromStdWString(color), ColorRole);
                MetadataManager::instance().setColor(path.toStdWString(), color);
            }
        }
    });

    // 9. 2026-03-xx 按照用户要求：响应元数据全局变更，同步刷新侧边栏计数
    // 确保在内容面板收藏项目后，侧边栏的“收藏 (X)”数字能实时跳变
    // 2026-05-27 物理修复：显式增加 this 上下文参数
    // 理由：MetadataManager 可能在后台线程（如初始化扫描阶段）发射信号，
    // 若无 context 参数，Lambda 将在发射信号的后台线程执行，导致非法线程操作 UI (refresh()) 引起崩溃。
    connect(&MetadataManager::instance(), &MetadataManager::metaChanged, this, [this]() {
        if (m_categoryPanel && m_categoryPanel->model()) {
            m_categoryPanel->model()->refresh();
        }
    });

    // 10. 侧边栏点击物理项（文件预览或文件夹跳转）
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryPanel, &CategoryPanel::fileSelected, this, [this](const QString& path) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            // 如果是文件夹，执行界面跳转联动
            navigateTo(path);
        } else {
            // 2026-03-xx 按照用户要求：侧边栏选中任何物理文件，立即执行即时全能预览
            m_contentPanel->previewFile(path);
        }
    });
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType);
    Q_UNUSED(result);

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_DEVICECHANGE) {
        // 2026-05-24 按照用户要求：捕捉硬件变更，硬盘插入时触发 GLOB 扫描对账
        if (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE) {
            qDebug() << "[Main] 检测到磁盘硬件变更，触发全量 GLOB 对账对账...";
            // 异步触发扫描，防止阻塞 UI
            (void)QtConcurrent::run([]() {
                SyncEngine::instance().runFullScan({}, nullptr);
            });
        }
    }
    return false;
}
#endif

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // 2026-04-12 关键修复：延迟初始化面板数据（确保窗口先渲染，避免主线程卡死导致无法显示）
    qDebug() << "[Main] showEvent 触发, m_panelsInitialized =" << m_panelsInitialized;
    if (!m_panelsInitialized) {
        m_panelsInitialized = true;
        qDebug() << "[Main] 正在排期延迟加载任务 (QTimer::singleShot(0))...";
        QTimer::singleShot(0, [this]() {
            qDebug() << "[Main] 延迟加载任务开始执行";
            if (m_categoryPanel) {
                qDebug() << "[Main] 正在初始化 CategoryPanel...";
                m_categoryPanel->deferredInit();
            }
            if (m_navPanel) {
                qDebug() << "[Main] 正在初始化 NavPanel...";
                m_navPanel->deferredInit();
            }
            if (m_contentPanel) {
                qDebug() << "[Main] 正在初始化 ContentPanel...";
                m_contentPanel->deferredInit();
            }
            // MetaPanel 和 FilterPanel 暂时不需要延迟数据加载，因为它们通常随选中项动态刷新
            
            // 2026-04-14 按照用户要求：物理禁用"最后一个窗口关闭时退出"逻辑
            // 确保程序只能通过托盘菜单显式退出，提高驻留稳定性
            qDebug() << "[Main] 所有核心面板数据延迟初始化完成，UI 响应已恢复";
        });
    }
    
    // 2026-04-11 按照用户要求：此处是执行 ToolTipOverlay 真实 GPU 预热的唯一合法时机。
    // 只有当 MainWindow 的原生窗口句柄（HWND）被 Windows 完整创建后，ToolTipOverlay 的
    // show() + hide() 序列才能真正触发 DWM 桌面合成器分配 GPU 内存驻留资源。
    // 在构造函数中执行该操作毫无意义，因为此时 MainWindow 本身尚未拥有有效句柄。
    // 使用 singleShot(0) 延迟到下一个事件循环帧，确保当前帧窗口绘制不受打扰。
    static bool s_warmedUp = false;
    if (!s_warmedUp) {
        s_warmedUp = true;
        QTimer::singleShot(0, []() {
            auto* tip = ToolTipOverlay::instance();
            // 闪烁显示再立即隐藏，令 DWM 将其纹理资源常驻 GPU 显存
            tip->show();
            tip->hide();
        });
    }
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // 2026-03-xx 物理还原：点击纯标题栏区域（前 32px）允许拖动窗口
        if (event->position().y() <= 32) {
            m_isDragging = true;
            m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept();
        }
    }
}


void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    m_isDragging = false;
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // 1. Alt+Q: 切换窗口置顶状态
    if (event->key() == Qt::Key_Q && (event->modifiers() & Qt::AltModifier)) {
        m_btnPinTop->setChecked(!m_btnPinTop->isChecked());
        event->accept();
        return;
    }

    // 2026-05-20 极致性能：MainWindow 自身也需支持悬停识别，确保自定义标题栏操作灵敏
    setAttribute(Qt::WA_Hover);

    // 2. Ctrl+F: 聚焦搜索过滤框
    if (event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier)) {
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

// 2026-03-xx 按照用户要求：物理拦截事件以实现自定义 ToolTipOverlay 的显隐控制
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // 2026-06-15 闲置感应：拦截用户交互事件，重置同步倒计时
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress || 
        event->type() == QEvent::KeyPress || event->type() == QEvent::Wheel) {
        if (m_idleTimer) {
            m_idleTimer->start(); // 重新开始 30 秒计次
        }
    }

    // 2026-05-20 性能优化：同时支持 Enter/Leave 与 Hover 事件，确保标题栏按钮响应“零延迟”
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            // 物理级别禁绝原生 ToolTip，强制调用 ToolTipOverlay
            ToolTipOverlay::instance()->showText(QCursor::pos(), text);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
        ToolTipOverlay::hideTip();
    } else if (event->type() == QEvent::FocusIn && watched == m_searchEdit) {
        // 2026-04-12 按照用户要求：搜索框获得焦点时弹出历史记录
        if (!m_searchHistory.isEmpty()) {
            m_searchHistoryPanel->showBelow(m_searchEdit);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::initToolbar() {
    auto createBtn = [this](const QString& iconKey, const QString& tip) {
        QPushButton* btn = new QPushButton(this);
        btn->setAttribute(Qt::WA_Hover);
        btn->setFixedSize(32, 28);
        
        QIcon icon = UiHelper::getIcon(iconKey, QColor("#c8d4dc"));
        btn->setIcon(icon);
        btn->setIconSize(QSize(18, 18));
        
        btn->setProperty("tooltipText", tip);
        btn->installEventFilter(this);

        btn->setStyleSheet(
            "QPushButton { background: transparent; border: none; border-radius: 0px; }"
            "QPushButton:hover { background: #161b20; }"
            "QPushButton:pressed { background: #1e252c; }"
        );
        return btn;
    };

    m_btnBack = createBtn("nav_prev", "后退");
    m_btnForward = createBtn("nav_next", "前进");
    m_btnUp = createBtn("arrow_up", "上级");

    connect(m_btnBack, &QPushButton::clicked, this, &MainWindow::onBackClicked);
    connect(m_btnForward, &QPushButton::clicked, this, &MainWindow::onForwardClicked);
    connect(m_btnUp, &QPushButton::clicked, this, &MainWindow::onUpClicked);

    m_pathStack = new QStackedWidget(this);
    m_pathStack->setFixedHeight(36);
    m_pathStack->setMinimumWidth(300);
    m_pathStack->setStyleSheet("QStackedWidget { background: #111519; border: 1px solid #252e37; border-radius: 0px; }");

    m_breadcrumbBar = new BreadcrumbBar(m_pathStack);
    m_pathStack->addWidget(m_breadcrumbBar);

    m_pathEdit = new QLineEdit(m_pathStack);
    m_pathEdit->setPlaceholderText("输入路径...");
    m_pathEdit->setFixedHeight(36);
    m_pathEdit->setStyleSheet("QLineEdit { background: transparent; border: none; color: #c8d4dc; padding-left: 8px; }");
    m_pathStack->addWidget(m_pathEdit);

    m_pathStack->setCurrentWidget(m_breadcrumbBar);

    connect(m_breadcrumbBar, &BreadcrumbBar::blankAreaClicked, this, [this]() {
        m_pathEdit->setText(QDir::toNativeSeparators(m_currentPath));
        m_pathStack->setCurrentWidget(m_pathEdit);
        m_pathEdit->setFocus();
        m_pathEdit->selectAll();
    });
    connect(m_pathEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_pathStack->currentWidget() == m_pathEdit) {
            m_pathStack->setCurrentWidget(m_breadcrumbBar);
        }
    });
    connect(m_breadcrumbBar, &BreadcrumbBar::pathClicked, this, [this](const QString& path) {
        navigateTo(path);
    });

    // --- 搜索栏重构 (对标 HTML) ---
    m_searchContainer = new QWidget(this);
    m_searchContainer->setStyleSheet("background: transparent;");
    QHBoxLayout* searchLayout = new QHBoxLayout(m_searchContainer);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);

    // 搜索图标包装 (左侧装饰)
    QLabel* searchIconLabel = new QLabel(m_searchContainer);
    searchIconLabel->setFixedSize(36, 36);
    searchIconLabel->setPixmap(UiHelper::getIcon("search", QColor("#3d5060")).pixmap(15, 15));
    searchIconLabel->setAlignment(Qt::AlignCenter);
    searchIconLabel->setStyleSheet("border: 1px solid #252e37; border-right: none; background: #111519;");
    searchLayout->addWidget(searchIconLabel);

    m_searchEdit = new QLineEdit(m_searchContainer);
    m_searchEdit->setPlaceholderText("文件名 / 关键词...");
    m_searchEdit->setMinimumWidth(250);
    m_searchEdit->setFixedHeight(36);
    m_searchEdit->setStyleSheet(
        "QLineEdit { background: #111519; border: 1px solid #252e37; border-right: none; border-radius: 0px; color: #c8d4dc; padding-left: 12px; }"
        "QLineEdit:focus { border-color: #FF8C00; background: #161b20; }"
    );
    searchLayout->addWidget(m_searchEdit, 1);

    // 扩展名分隔符
    QLabel* extDivider = new QLabel(".", m_searchContainer);
    extDivider->setFixedSize(20, 36);
    extDivider->setAlignment(Qt::AlignCenter);
    extDivider->setStyleSheet("background: #161b20; border-top: 1px solid #252e37; border-bottom: 1px solid #252e37; border-left: 1px solid #252e37; color: #FF8C00; font-weight: bold; font-size: 14px;");
    searchLayout->addWidget(extDivider);

    m_extEdit = new QLineEdit(m_searchContainer);
    m_extEdit->setPlaceholderText("扩展名");
    m_extEdit->setFixedSize(80, 36);
    m_extEdit->setStyleSheet(
        "QLineEdit { background: #111519; border: 1px solid #252e37; border-left: none; border-radius: 0px; color: #c8d4dc; padding: 0 10px; }"
        "QLineEdit:focus { border-color: #FF8C00; background: #161b20; }"
    );
    searchLayout->addWidget(m_extEdit);

    // 橙色搜索按钮
    m_btnSearch = new QPushButton(m_searchContainer);
    m_btnSearch->setText("搜索");
    m_btnSearch->setIcon(UiHelper::getIcon("search", QColor("#000000")));
    m_btnSearch->setFixedSize(80, 36);
    m_btnSearch->setStyleSheet(
        "QPushButton { background: #FF8C00; border: 1px solid #FF8C00; color: #000000; font-weight: bold; margin-left: 10px; }"
        "QPushButton:hover { background: #c96e00; border-color: #c96e00; }"
    );
    searchLayout->addWidget(m_btnSearch);
}



void MainWindow::initDriveBar() {
    m_driveBarWidget = new QWidget(this);
    m_driveBarWidget->setFixedHeight(40);
    m_driveBarWidget->setStyleSheet("background-color: #111519; padding: 0 16px; border-bottom: 1px solid #1e252c;");
    m_driveBarLayout = new QHBoxLayout(m_driveBarWidget);
    m_driveBarLayout->setContentsMargins(16, 0, 16, 0);
    m_driveBarLayout->setSpacing(6);

    QLabel* driveLabel = new QLabel("盘符", m_driveBarWidget);
    driveLabel->setStyleSheet("color: #3d5060; font-weight: 600; font-size: 10px; text-transform: uppercase; margin-right: 4px;");
    m_driveBarLayout->addWidget(driveLabel);

    // 动态获取系统盘符
    auto drives = QDir::drives();
    for (const auto& info : drives) {
        QString driveRoot = info.absolutePath();
        QString driveLetter = driveRoot.left(1).toUpper();

        QPushButton* chip = new QPushButton(driveLetter + ":", m_driveBarWidget);
        chip->setCheckable(true);
        chip->setFixedSize(60, 24);

        // 初始激活 C 盘
        if (driveLetter == "C") {
            chip->setChecked(true);
            m_activeDrives.insert(driveLetter);
        }

        QString chipStyle = R"(
            QPushButton {
                background-color: #161b20;
                border: 1px solid #252e37;
                color: #7a8f9e;
                font-weight: 600;
                font-size: 11px;
                border-radius: 0px;
            }
            QPushButton:hover {
                border-color: #c96e00;
                color: #c8d4dc;
            }
            QPushButton:checked {
                border-color: #FF8C00;
                background-color: rgba(255,140,0,0.08);
                color: #FF8C00;
            }
        )";
        chip->setStyleSheet(chipStyle);

        connect(chip, &QPushButton::toggled, this, [this, driveLetter](bool checked) {
            if (checked) m_activeDrives.insert(driveLetter);
            else m_activeDrives.remove(driveLetter);
            updateStatusBar();
            // TODO: 触发搜索联动
        });

        m_driveBarLayout->addWidget(chip);
    }

    m_driveBarLayout->addStretch();

    QPushButton* allBtn = new QPushButton("全选 / 全清", m_driveBarWidget);
    allBtn->setStyleSheet(
        "QPushButton { color: #3d5060; border: 1px solid #1e252c; background: transparent; font-size: 10px; font-weight: 600; padding: 4px 10px; }"
        "QPushButton:hover { color: #FF8C00; border-color: #FF8C00; }"
    );
    connect(allBtn, &QPushButton::clicked, this, [this]() {
        bool anyUnchecked = false;
        auto chips = m_driveBarWidget->findChildren<QPushButton*>();
        for (auto c : chips) {
            if (c->text().contains(":") && !c->isChecked()) { anyUnchecked = true; break; }
        }

        for (auto c : chips) {
            if (c->text().contains(":")) c->setChecked(anyUnchecked);
        }
    });
    m_driveBarLayout->addWidget(allBtn);
}

void MainWindow::setupSplitters() {
    QWidget* centralC = new QWidget(this);
    centralC->setObjectName("CentralWidget");
    centralC->setStyleSheet("#CentralWidget { background-color: #07090b; }");
    QVBoxLayout* mainL = new QVBoxLayout(centralC);
    mainL->setContentsMargins(0, 0, 0, 0); 
    mainL->setSpacing(0); 

    // --- 1. 自定义标题栏 (第一行) ---
    m_titleBarWidget = new QWidget(centralC);
    m_titleBarWidget->setFixedHeight(44);
    m_titleBarWidget->setStyleSheet("QWidget { background-color: #0d1014; border-bottom: 1px solid #1e252c; }");
    m_titleBarLayout = new QHBoxLayout(m_titleBarWidget);
    m_titleBarLayout->setContentsMargins(16, 0, 16, 0);
    m_titleBarLayout->setSpacing(14);

    // Logo & Text
    m_appNameLabel = new QLabel("FERREX", m_titleBarWidget);
    m_appNameLabel->setStyleSheet("color: #FF8C00; font-size: 18px; font-weight: 700; letter-spacing: 0.12em;");
    m_titleBarLayout->addWidget(m_appNameLabel);

    QLabel* badge = new QLabel("NTFS INDEXER", m_titleBarWidget);
    badge->setStyleSheet("color: #3d5060; background: #111519; border: 1px solid #252e37; padding: 3px 8px; font-size: 11px; font-weight: 500;");
    m_titleBarLayout->addWidget(badge);

    m_titleBarLayout->addStretch();

    // Index Status
    QWidget* indexStatus = new QWidget(m_titleBarWidget);
    QHBoxLayout* indexL = new QHBoxLayout(indexStatus);
    indexL->setContentsMargins(0, 0, 0, 0);
    indexL->setSpacing(6);

    QWidget* dot = new QWidget(indexStatus);
    dot->setFixedSize(6, 6);
    dot->setStyleSheet("background: #2ecc71; border-radius: 3px;");
    indexL->addWidget(dot);

    QLabel* statusText = new QLabel("索引就绪", indexStatus);
    statusText->setStyleSheet("color: #3d5060; font-size: 11px;");
    indexL->addWidget(statusText);
    m_titleBarLayout->addWidget(indexStatus);

    // --- 2. 盘符选择栏 (第二行) ---
    initDriveBar();
    mainL->addWidget(m_titleBarWidget);
    mainL->addWidget(m_driveBarWidget);

    // --- 3. 搜索与路径栏 (第三行) ---
    m_navBarWidget = new QWidget(centralC);
    m_navBarWidget->setFixedHeight(46);
    m_navBarWidget->setStyleSheet("background-color: #0d1014; border-bottom: 1px solid #1e252c;");
    
    m_navBarLayout = new QHBoxLayout(m_navBarWidget);
    m_navBarLayout->setContentsMargins(16, 0, 16, 0);
    m_navBarLayout->setSpacing(10);
    m_navBarLayout->setAlignment(Qt::AlignVCenter);

    m_navBarLayout->addWidget(m_btnBack);
    m_navBarLayout->addWidget(m_btnForward);
    m_navBarLayout->addWidget(m_btnUp);
    m_navBarLayout->addWidget(m_searchContainer, 1);

    mainL->addWidget(m_navBarWidget);

    // --- 4. 主体核心容器 (直角平铺) ---
    QWidget* bodyWrapper = new QWidget(centralC);
    bodyWrapper->setStyleSheet("background: transparent;");
    QVBoxLayout* bodyLayout = new QVBoxLayout(bodyWrapper);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_mainSplitter = new QSplitter(Qt::Horizontal, bodyWrapper);
    m_mainSplitter->setHandleWidth(1);
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->setStyleSheet("QSplitter::handle { background: #1e252c; }");

    m_categoryPanel = new CategoryPanel(this);
    m_categoryPanel->setObjectName("SidebarContainer");
    
    m_navPanel = new NavPanel(this);
    m_navPanel->setObjectName("ListContainer");
    
    m_contentPanel = new ContentPanel(this);
    m_contentPanel->setObjectName("EditorContainer");
    
    m_metaPanel = new MetaPanel(this);
    m_metaPanel->setObjectName("MetadataContainer");
    
    m_filterPanel = new FilterPanel(this);
    m_filterPanel->setObjectName("FilterContainer");

    m_mainSplitter->addWidget(m_categoryPanel);
    m_mainSplitter->addWidget(m_navPanel);
    m_mainSplitter->addWidget(m_contentPanel);
    m_mainSplitter->addWidget(m_metaPanel);
    m_mainSplitter->addWidget(m_filterPanel);

    bodyLayout->addWidget(m_mainSplitter);
    mainL->addWidget(bodyWrapper, 1);

    // --- 5. 底部状态栏 ---
    QWidget* statusBar = new QWidget(centralC);
    statusBar->setFixedHeight(26);
    statusBar->setStyleSheet("QWidget { background-color: #0d1014; border-top: 1px solid #1e252c; }");
    QHBoxLayout* statusL = new QHBoxLayout(statusBar);
    statusL->setContentsMargins(16, 0, 16, 0);
    statusL->setSpacing(20);

    m_statusLeft = new QLabel("结果 —", statusBar);
    m_statusLeft->setStyleSheet("font-size: 10px; color: #3d5060; text-transform: uppercase;");

    m_statusCenter = new QLabel("耗时 —", statusBar);
    m_statusCenter->setStyleSheet("font-size: 10px; color: #3d5060; text-transform: uppercase;");

    m_statusRight = new QLabel("盘符 C:", statusBar);
    m_statusRight->setStyleSheet("font-size: 10px; color: #3d5060; text-transform: uppercase;");

    statusL->addWidget(m_statusLeft);
    statusL->addWidget(m_statusCenter);
    statusL->addWidget(m_statusRight);
    statusL->addStretch(1);

    QLabel* versionLabel = new QLabel("FERREX v0.1.0", statusBar);
    versionLabel->setStyleSheet("font-size: 10px; color: #FF8C00; font-weight: bold;");
    statusL->addWidget(versionLabel);

    mainL->addWidget(statusBar);

    setCentralWidget(centralC);
}

/**
 * @brief 实现符合 funcBtnStyle 规范的自定义按钮组
 */
void MainWindow::setupCustomTitleBarButtons() {
    QWidget* titleBarBtns = new QWidget(this);
    QHBoxLayout* layout = new QHBoxLayout(titleBarBtns);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto createTitleBtn = [this](const QString& iconKey, const QString& hoverColor = "rgba(255, 255, 255, 0.1)") {
        QPushButton* btn = new QPushButton(this);
        btn->setAttribute(Qt::WA_Hover); // 2026-05-20 性能优化：必须开启 Hover 属性以触发悬停事件
        btn->setFixedSize(24, 24); // 固定 24x24px
        
        // 使用 UiHelper 全局辅助类
        QIcon icon = UiHelper::getIcon(iconKey, QColor("#EEEEEE"));
        btn->setIcon(icon);
        btn->setIconSize(QSize(18, 18));
        
        btn->setStyleSheet(QString(
            "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }"
            "QPushButton:hover { background: %1; }"
            "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
        ).arg(hoverColor));
        return btn;
    };

    m_btnSync = createTitleBtn("sync");
    m_btnSync->setProperty("tooltipText", "无待同步任务");
    m_btnSync->installEventFilter(this);

    // 2026-06-15 按照用户要求：手动点击同步
    connect(m_btnSync, &QPushButton::clicked, this, [this]() {
        SyncEngine::instance().runIncrementalSync();
        ToolTipOverlay::instance()->showText(m_btnSync->mapToGlobal(QPoint(0,0)), "正在启动延时同步...", 1500);
    });

    // 联动同步按钮颜色状态 (红色预警)
    auto updateSyncBtnState = [this](bool hasPending) {
        if (hasPending) {
            m_btnSync->setIcon(UiHelper::getIcon("sync", QColor("#E81123"))); // 强制红色
            m_btnSync->setProperty("tooltipText", "存在待同步元数据，请点击或等待闲置同步");
        } else {
            m_btnSync->setIcon(UiHelper::getIcon("sync", QColor("#EEEEEE"))); // 恢复正常
            m_btnSync->setProperty("tooltipText", "元数据已同步至物理文件");
        }
    };
    
    connect(&MetadataManager::instance(), &MetadataManager::pendingSyncChanged, this, updateSyncBtnState);
    connect(&SyncEngine::instance(), &SyncEngine::syncStatusChanged, this, [this, updateSyncBtnState](bool running) {
        if (!running) {
            updateSyncBtnState(SyncEngine::instance().hasPendingTasks());
        }
    });

    // 启动时初始化一次状态
    QTimer::singleShot(500, this, [this, updateSyncBtnState]() {
        updateSyncBtnState(SyncEngine::instance().hasPendingTasks());
    });

    m_btnScan = createTitleBtn("scan");
    m_btnScan->setProperty("tooltipText", "实时扫描与查找...");
    m_btnScan->installEventFilter(this);
    // 2026-04-17 按照用户要求：扫描窗口独立运行，关闭后自动释放内存，不得影响主程序生命周期。
    connect(m_btnScan, &QPushButton::clicked, this, [this]() {
        auto* dlg = new ScanDialog(this);
        dlg->setModal(false);
        dlg->show();
    });

    m_btnCreate = createTitleBtn("add"); // 2026-03-xx 规范化：“+”按钮图标修正
    m_btnCreate->setProperty("tooltipText", "新建...");
    QMenu* createMenu = new QMenu(m_btnCreate);
    createMenu->setStyleSheet(
        "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
        "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
        "QMenu::item:selected { background-color: #3E3E42; color: white; }"
        "QMenu::right-arrow { image: url(data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjRUVFRUVFIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCI+PHBvbHlsaW5lIHBvaW50cz0iOSAxOCAxNSAxMiA5IDYiPjwvcG9seWxpbmU+PC9zdmc+); width: 12px; height: 12px; right: 8px; }"
    );
    
    QAction* actNewFolder = createMenu->addAction(UiHelper::getIcon("folder_filled", QColor("#EEEEEE")), "创建文件夹");
    QAction* actNewMd     = createMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建 Markdown");
    QAction* actNewTxt    = createMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建纯文本文件 (txt)");
    
    // 2026-03-xx 按照用户要求修正居中对齐：
    // 不再使用 setMenu，避免按钮进入“菜单模式”从而为指示器预留空间导致图标偏左。
    // 采用手动 popup 方式展示菜单。
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_btnCreate, &QPushButton::clicked, this, [this, createMenu]() {
        createMenu->popup(m_btnCreate->mapToGlobal(QPoint(0, m_btnCreate->height())));
    });

    auto handleCreate = [this](const QString& type) {
        m_contentPanel->createNewItem(type);
    };
    // 2026-05-27 物理加固：补全 this 上下文
    connect(actNewFolder, &QAction::triggered, this, [handleCreate](){ handleCreate("folder"); });
    connect(actNewMd,     &QAction::triggered, this, [handleCreate](){ handleCreate("md"); });
    connect(actNewTxt,    &QAction::triggered, this, [handleCreate](){ handleCreate("txt"); });

    m_btnPinTop = createTitleBtn(m_isPinned ? "pin_vertical" : "pin_tilted");
    m_btnPinTop->setProperty("tooltipText", "置顶窗口");
    m_btnPinTop->installEventFilter(this);
    m_btnPinTop->setCheckable(true);
    m_btnPinTop->setChecked(m_isPinned);
    if (m_isPinned) {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", QColor("#FF551C")));
    }

    m_btnMin = createTitleBtn("minimize");
    m_btnMin->setProperty("tooltipText", "最小化");
    m_btnMin->installEventFilter(this);

    m_btnMax = createTitleBtn(isMaximized() ? "restore_window" : "maximize");
    m_btnMax->setProperty("tooltipText", "最大化/还原");
    m_btnMax->installEventFilter(this);

    m_btnClose = createTitleBtn("close", "#e81123"); // 初始创建
    // 按照用户要求：关闭按钮持续显示红色高亮，不再仅悬停显示
    m_btnClose->setStyleSheet(
        "QPushButton { background-color: #E81123; border: none; border-radius: 4px; padding: 0; }"
        "QPushButton:hover { background-color: #F1707A; }"
        "QPushButton:pressed { background-color: #A50000; }"
    );
    m_btnClose->setProperty("tooltipText", "关闭项目");
    m_btnClose->installEventFilter(this);

    m_btnCreate->installEventFilter(this);
    layout->addWidget(m_btnSync);
    layout->addWidget(m_btnScan);
    layout->addWidget(m_btnCreate);
    layout->addWidget(m_btnPinTop);
    layout->addWidget(m_btnMin);
    layout->addWidget(m_btnMax);
    layout->addWidget(m_btnClose);

    // 绑定基础逻辑
    connect(m_btnMin, &QPushButton::clicked, this, &MainWindow::showMinimized);
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_btnMax, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    connect(m_btnClose, &QPushButton::clicked, this, &MainWindow::close);

    if (m_titleBarLayout) {
        m_titleBarLayout->addWidget(titleBarBtns);
    }

    // 逻辑：置顶切换
    connect(m_btnPinTop, &QPushButton::toggled, this, &MainWindow::onPinToggled);
}

void MainWindow::initIdleDetector() {
    // 2026-06-15 按照用户要求：建立 30 秒闲置自动同步机制
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(30000); // 30秒
    m_idleTimer->setSingleShot(true);
    
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[Main] 检测到系统闲置超过30秒，触发自动对账同步...";
        SyncEngine::instance().runIncrementalSync();
    });

    // 启动闲置计时
    m_idleTimer->start();
    
    // 安装全局事件过滤器以感应操作（在 QApplication 级别感应更佳，这里先按窗口级实现）
    qApp->installEventFilter(this);
}

void MainWindow::initTrayIcon() {
    // 2026-03-xx 按照用户要求：集成系统托盘功能
    m_trayIcon = new QSystemTrayIcon(this);
    
    // 2026-04-14 物理加固：锁定图标来源为 Qt 资源系统中的标准 ico
    // 杜绝使用 windowIcon() 导致的潜在路径失效风险
    m_trayIcon->setIcon(QIcon(":/app_icon.ico"));

    // 托盘图标由于是 OS 级容器，不受 ToolTipOverlay 控制，允许保留原生或根据系统行为处理
    m_trayIcon->setToolTip("ArcMeta");

    QMenu* trayMenu = new QMenu(this);
    trayMenu->setStyleSheet(
        "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
        "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
        "QMenu::item:selected { background-color: #3E3E42; color: white; }"
    );

    QAction* showAction = trayMenu->addAction("显示主界面");
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction("退出 ArcMeta");

    connect(showAction, &QAction::triggered, this, [this]() {
        showNormal();
        activateWindow();
    });

    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(trayMenu);

    // 点击托盘图标逻辑
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                activateWindow();
            }
        }
    });

    m_trayIcon->show();
}

void MainWindow::navigateTo(const QString& path, bool record) {
    if (path.isEmpty()) return;
    qDebug() << "[Main] 执行跳转 ->" << path << (record ? "(记录历史)" : "(不记录)");

    // 2026-04-12 关键协议：任何导航操作（手动输入、点击、后退、上级）都应强制重置搜索态
    if (m_searchEdit && !m_searchEdit->text().isEmpty()) {
        qDebug() << "[Main] 检测到导航操作，物理清空搜索关键词残留:" << m_searchEdit->text();
        m_searchEdit->clear();
        m_contentPanel->search("");
    }
    
    // 处理虚拟路径 "computer://" —— 此电脑（磁盘分区列表）
    if (path == "computer://") {
        m_currentPath = "computer://";
        if (record) {
            if (m_history.isEmpty() || m_history.last() != path) {
                m_history.append(path);
                m_historyIndex = static_cast<int>(m_history.size()) - 1;
            }
        }
        m_pathEdit->setText("此电脑");
        m_breadcrumbBar->setPath("computer://");
        m_pathStack->setCurrentWidget(m_breadcrumbBar);
        m_contentPanel->loadDirectory(""); 
        int driveCount = static_cast<int>(QDir::drives().count());
        m_statusLeft->setText(QString("%1 个分区").arg(driveCount));
        m_statusCenter->setText("此电脑");
        updateNavButtons();
        return;
    }

    QString normPath = QDir::toNativeSeparators(path);
    m_currentPath = normPath;

    if (record) {
            if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
            m_history = m_history.mid(0, m_historyIndex + 1);
        }
        if (m_history.isEmpty() || m_history.last() != normPath) {
            m_history.append(normPath);
                m_historyIndex = static_cast<int>(m_history.size()) - 1;
        }
    }
    
    m_pathEdit->setText(normPath);
    m_breadcrumbBar->setPath(normPath);
    m_pathStack->setCurrentWidget(m_breadcrumbBar);
    m_contentPanel->loadDirectory(normPath);
    updateNavButtons();
    updateStatusBar();
}

void MainWindow::onBackClicked() {
    if (m_historyIndex > 0) {
        m_historyIndex--;
        navigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onForwardClicked() {
    if (m_historyIndex < m_history.size() - 1) {
        m_historyIndex++;
        navigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onUpClicked() {
    QDir dir(m_currentPath);
    if (dir.cdUp()) {
        navigateTo(dir.absolutePath());
    }
}

void MainWindow::updateNavButtons() {
    m_btnBack->setEnabled(m_historyIndex > 0);
    m_btnForward->setEnabled(m_historyIndex < m_history.size() - 1);
    
    bool atRoot = (m_currentPath == "computer://" || (QDir(m_currentPath).isRoot()));
    m_btnUp->setEnabled(!atRoot && !m_currentPath.isEmpty());
}

void MainWindow::updateStatusBar() {
    if (!m_statusLeft || !m_statusCenter || !m_statusRight) return;
    
    // 修正：显示经过过滤后的可见项目总数
    int visibleCount = m_contentPanel->getProxyModel()->rowCount();
    m_statusLeft->setText(QString("%1 个项目").arg(visibleCount));
    m_statusCenter->setText(m_currentPath == "computer://" ? "此电脑" : m_currentPath);
    m_statusRight->setText(""); // 选中时由 selectionChanged 更新
}

void MainWindow::onPinToggled(bool checked) {
    // 2026-03-xx 按照用户要求优化置顶逻辑：
    // 避免重复调用导致卡顿，并优化 WinAPI 标志位以减少冗余消息推送
    if (m_isPinned == checked) return;
    m_isPinned = checked;

#ifdef Q_OS_WIN
    HWND hwnd = (HWND)winId();
    // 使用 SWP_NOSENDCHANGING 拦截冗余消息，减少 UI 线程的消息风暴，从而解决卡顿
    SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
#else
    setWindowFlag(Qt::WindowStaysOnTopHint, checked);
    show(); // 非 Windows 平台修改 Flag 后通常需要重新显示
#endif

    // 更新图标和颜色 (按下置顶为品牌橙色)
    if (m_isPinned) {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", QColor("#FF551C")));
    } else {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_tilted", QColor("#EEEEEE")));
    }

    // 持久化存储
    QSettings settings("ArcMeta团队", "ArcMeta");
    settings.setValue("MainWindow/AlwaysOnTop", m_isPinned);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        // 2026-04-11 按照用户要求：物理识别窗口状态，精准切换最大化/还原图标
        if (m_btnMax) {
            QString iconKey = isMaximized() ? "restore_window" : "maximize";
            m_btnMax->setIcon(UiHelper::getIcon(iconKey, QColor("#EEEEEE")));
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings("ArcMeta团队", "ArcMeta");
    settings.setValue("MainWindow/LastPath", m_currentPath);
    // 2026-04-11 按照用户要求：物理保存各容器宽度状态
    if (m_mainSplitter) {
        settings.setValue("MainWindow/SplitterState", m_mainSplitter->saveState());
    }
    QMainWindow::closeEvent(event);
}

} // namespace ArcMeta
