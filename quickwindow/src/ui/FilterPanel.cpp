#include "FilterPanel.h"
#include "QuickWindow.h"
#include "../core/DatabaseManager.h"
#include "IconHelper.h"
#include "ToolTipOverlay.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QApplication>
#include <QTimer>
#include <QtConcurrent>
#include <QStyledItemDelegate>

// [NEW] 2026-04-xx 按照用户要求：1:1 复刻侧边栏高亮逻辑，解决“脑补参数”导致的视觉碎片化
class FilterDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!index.isValid()) return;

        bool selected = option.state & QStyle::State_Selected;
        bool hover = option.state & QStyle::State_MouseOver;
        bool isSelectable = index.flags() & Qt::ItemIsSelectable;

        if (isSelectable && (selected || hover)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            // [MATCH] 1:1 对齐侧边栏逻辑：选中态高亮颜色与当前分类联动，非固定蓝色
            QColor highlightColor("#4a90e2"); // 默认蓝
            QuickWindow* qw = qobject_cast<QuickWindow*>(option.widget->window());
            if (qw) {
                QString c = qw->currentCategoryColor();
                if (!c.isEmpty() && QColor::isValidColorName(c)) highlightColor = QColor(c);
            }

            QColor bg = selected ? highlightColor : QColor("#2a2d2e");
            if (selected) bg.setAlphaF(0.2); 
            
            QStyle* style = option.widget ? option.widget->style() : QApplication::style();
            // 获取复选框、图标和文字的物理矩形
            QRect checkRect = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, option.widget);
            QRect decoRect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, option.widget);
            QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
            
            // 联合内容区域：确保高亮块完整包裹视觉元素
            QRect contentRect = checkRect.united(decoRect).united(textRect);
            contentRect = contentRect.intersected(option.rect);
            
            // [RESTORED] 严格遵循侧边栏物理边界：右侧缩进 5px，左侧不再使用负偏移防止视觉碎片
            contentRect.setRight(option.rect.right() - 5);
            
            // 上下保留 1px 间隙以体现圆角呼吸感
            contentRect.adjust(0, 1, 0, -1);
            
            painter->setBrush(bg);
            painter->setPen(Qt::NoPen);
            // 2026-04-xx 按照用户要求：修正圆角设计，统一校准为 4px 以对齐侧边栏
            painter->drawRoundedRect(contentRect, 4, 4);
            painter->restore();
        }

        // 绘制原内容 (Checkbox, Icon, Text)
        QStyleOptionViewItem opt = option;
        opt.state &= ~QStyle::State_Selected;
        opt.state &= ~QStyle::State_MouseOver;
        
        if (selected) {
            opt.palette.setColor(QPalette::Text, Qt::white);
            opt.palette.setColor(QPalette::HighlightedText, Qt::white);
        }
        
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

FilterPanel::FilterPanel(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(true);
    // 2026-04-xx 按照用户要求：直接照搬侧边栏分类宽度的参数 (163px)
    setMinimumSize(163, 350);
    initUI();
    setupTree();

    // 应用自定义代理，接管高亮绘制逻辑
    m_tree->setItemDelegate(new FilterDelegate(this));

    connect(&m_statsWatcher, &QFutureWatcher<QVariantMap>::finished, this, &FilterPanel::onStatsReady);
}

void FilterPanel::initUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 内容容器
    auto* contentWidget = new QWidget();
    contentWidget->setStyleSheet(
        "QWidget { "
        "  background-color: transparent; "
        "  border: none; "
        "  border-bottom-left-radius: 0px; "
        "  border-bottom-right-radius: 0px; "
        "}"
    );
    auto* contentLayout = new QVBoxLayout(contentWidget);
    // 2026-04-xx 按照用户要求：控制按钮组整体向下偏移 5 像素。
    // 物理逻辑：将 contentLayout 底部边距从 5px 降至 0px，并配合 2px 的 bottomLayout 边距，
    // 实现按钮中心点相对于底部的精准下沉。
    contentLayout->setContentsMargins(7, 5, 0, 0);
    contentLayout->setSpacing(7); // 增加树形与按钮间的自然间距 (2px -> 7px)

    // 树形筛选器
    m_tree = new QTreeWidget();
    m_tree->setHeaderHidden(true);
    // 2026-04-06 按照用户要求：精准对齐侧边栏缩进参数 (12px)
    m_tree->setIndentation(12);
    m_tree->setFocusPolicy(Qt::NoFocus);
    // 2026-04-06 按照用户要求：显示展开箭头，复刻侧边栏层级感
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setStyleSheet(
        "QTreeWidget {"
        "  background-color: transparent;"
        "  color: #ddd;"
        "  border: none;"
        "  font-size: 12px;"
        "  outline: none;"
        "}"
        "QTreeWidget::branch:has-children:closed { image: url(:/icons/arrow_right.svg); }"
        "QTreeWidget::branch:has-children:open   { image: url(:/icons/arrow_down.svg); }"
        "QTreeWidget::item {"
        "  height: 22px;" 
        "  border-radius: 4px;"
        "  padding: 0px;"
        "  border: none;"
        "}"
        "/* 高亮逻辑已交由 Delegate 处理，此处屏蔽 QSS 默认背景防止冲突 */"
        "QTreeWidget::item:hover, QTreeWidget::item:selected { background-color: transparent; }" 
        "QTreeWidget::branch:hover, QTreeWidget::branch:selected { background-color: transparent; }" 
        "QTreeWidget::indicator {"
        "  width: 14px;"
        "  height: 14px;"
        "}"
        "QScrollBar:vertical { border: none; background: transparent; width: 6px; margin: 0px; }"
        "QScrollBar::handle:vertical { background: #444; border-radius: 3px; min-height: 20px; }"
        "QScrollBar::handle:vertical:hover { background: #555; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
    );
    connect(m_tree, &QTreeWidget::itemChanged, this, &FilterPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemClicked, this, &FilterPanel::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &FilterPanel::onItemDoubleClicked);
    m_tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // [MODIFIED] 2026-04-xx 按照用户要求：修正按钮悬浮 Bug。
    // 明确指定树形控件占据全部剩余空间 (Stretch=1)，移除多余的 Vertical Stretch，
    // 从而保证按钮组始终紧贴面板底部，且不产生非预期的中段悬浮感。
    contentLayout->addWidget(m_tree, 1);

    // 底部区域
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(0, 0, 0, 2);
    bottomLayout->setSpacing(8);

    bottomLayout->addStretch(); // 左侧弹簧

    // 2026-04-xx 按照用户要求：标准化按钮样式，移除文字，仅保留图标
    QString btnStyle = "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0px; } "
                       "QPushButton:hover { background-color: #3e3e42; }";

    m_btnReset = new QPushButton();
    m_btnReset->setIcon(IconHelper::getIcon("refresh", "#aaaaaa", 18));
    m_btnReset->setFixedSize(24, 24);
    m_btnReset->setStyleSheet(btnStyle);
    // 2026-04-06 按照用户要求：物理移除错误的 F5 快捷键提示，F5 仅限用于刷新数据
    m_btnReset->setProperty("tooltipText", "重置所有筛选条件"); m_btnReset->installEventFilter(this);
    connect(m_btnReset, &QPushButton::clicked, this, &FilterPanel::resetFilters);
    bottomLayout->addWidget(m_btnReset);

    auto* btnCollapse = new QPushButton();
    btnCollapse->setIcon(IconHelper::getIcon("chevrons_up", "#aaaaaa", 18));
    btnCollapse->setFixedSize(24, 24);
    btnCollapse->setStyleSheet(btnStyle);
    // 2026-04-xx 按照宪法规范：补全提示文本。注：Ctrl + G 为全局切换逻辑，此处仅保留功能描述以防歧义
    btnCollapse->setProperty("tooltipText", "全部折叠"); btnCollapse->installEventFilter(this);
    connect(btnCollapse, &QPushButton::clicked, this, [this](){
        for(auto* root : m_roots) root->setExpanded(false);
    });
    bottomLayout->addWidget(btnCollapse);

    auto* btnExpand = new QPushButton();
    btnExpand->setIcon(IconHelper::getIcon("chevrons_down", "#aaaaaa", 18));
    btnExpand->setFixedSize(24, 24);
    btnExpand->setStyleSheet(btnStyle);
    // 2026-04-xx 按照宪法规范：补全提示文本
    btnExpand->setProperty("tooltipText", "全部展开"); btnExpand->installEventFilter(this);
    connect(btnExpand, &QPushButton::clicked, this, [this](){
        for(auto* root : m_roots) root->setExpanded(true);
    });
    bottomLayout->addWidget(btnExpand);

    bottomLayout->addStretch();

    contentLayout->addLayout(bottomLayout);
    mainLayout->addWidget(contentWidget);
}

void FilterPanel::setupTree() {
    struct Section {
        QString key;
        QString label;
        QString icon;
        QString color;
    };

    // 2026-04-08 [MODIFIED] 按照用户要求：重构主选项排序顺序
    // 物理顺序：评级 -> 类型 -> 标签 -> 字数 -> 创建日期 -> 修改日期
    QList<Section> sections = {
        {"stars", "评级", "star_filled", "#f39c12"},
        {"types", "类型", "folder", "#3498db"},
        {"tags", "标签", "tag", "#e67e22"},
        {"word_count", "字数", "type", "#3498db"},
        {"date_create", "创建日期", "today", "#2ecc71"},
        {"date_update", "修改日期", "clock", "#9b59b6"},
        {"colors", "颜色", "palette", "#e91e63"}
    };

    QFont headerFont = m_tree->font();
    headerFont.setBold(true);

    for (const auto& sec : sections) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, sec.label);
        item->setIcon(0, IconHelper::getIcon(sec.icon, sec.color));
        item->setExpanded(true);
        item->setFlags(Qt::ItemIsEnabled);
        item->setFont(0, headerFont);
        item->setForeground(0, QBrush(Qt::gray));
        m_roots[sec.key] = item;
    }
}

void FilterPanel::updateStats(const QString& keyword, const QString& type, const QVariant& value) {
    // [PERF] 性能优化：将耗时的 FTS5 聚合统计移至后台线程，防止搜索时 UI 线程假死。
    if (m_statsWatcher.isRunning()) {
        m_statsWatcher.cancel();
    }

    m_pendingKeyword = keyword;
    m_pendingType = type;
    m_pendingValue = value;

    auto future = QtConcurrent::run([keyword, type, value]() {
        return DatabaseManager::instance().getFilterStats(keyword, type, value);
    });
    m_statsWatcher.setFuture(future);
}

void FilterPanel::onStatsReady() {
    QVariantMap stats = m_statsWatcher.result();
    if (stats.isEmpty()) return;

    m_tree->blockSignals(true);
    m_blockItemClick = true;

    // 1. 评级
    // 2026-04-xx 按照用户要求：重构评级排序顺序，无星级置顶，随后1-5星升序
    QList<QVariantMap> starData;
    QVariantMap starStats = stats["stars"].toMap();
    
    // 无评级优先
    if (starStats.contains("0")) {
        int count = starStats["0"].toInt();
        if (count > 0) {
            QVariantMap item;
            item["key"] = "0";
            item["label"] = "无评级";
            item["count"] = count;
            starData.append(item);
        }
    }
    
    // 1-5星升序
    for (int i = 1; i <= 5; ++i) {
        int count = starStats[QString::number(i)].toInt();
        if (count > 0) {
            QVariantMap item;
            item["key"] = QString::number(i);
            item["label"] = QString(i, QChar(0x2605)); // ★
            item["count"] = count;
            starData.append(item);
        }
    }
    refreshNode("stars", starData);

    // 1.5 字数区间展示 (2026-04-xx 按照用户要求：极致精准展示)
    QList<QVariantMap> wordData;
    QVariantMap wordStats = stats["word_count"].toMap();
    // 按照 10, 20, ..., 101 的顺序强制物理遍历，确保 UI 排序的绝对有序性
    for (int i = 10; i <= 110; i += 10) {
        QString key = (i == 110) ? "101" : QString::number(i);
        if (wordStats.contains(key)) {
            int count = wordStats[key].toInt();
            if (count > 0) {
                QVariantMap item;
                item["key"] = key;
                QString label;
                if (i == 10) label = "0-10 字";
                else if (i == 110) label = "101 字以上";
                else label = QString("%1-%2 字").arg(i - 9).arg(i);
                item["label"] = label;
                item["count"] = count;
                wordData.append(item);
            }
        }
    }
    refreshNode("word_count", wordData);

    // 2. 颜色
    QList<QVariantMap> colorData;
    QVariantMap colorStats = stats["colors"].toMap();
    for (auto it = colorStats.begin(); it != colorStats.end(); ++it) {
        int count = it.value().toInt();
        if (count > 0) {
            QVariantMap item;
            item["key"] = it.key();
            item["label"] = it.key();
            item["count"] = count;
            colorData.append(item);
        }
    }
    refreshNode("colors", colorData, true);

    // 3. 扁平化类型展示 (2026-04-08 按照用户要求：不再区分大类，直接显示扩展名)
    QList<QVariantMap> typeData;
    QVariantMap typeStats = stats["types"].toMap();
    
    QStringList sortedLabels = typeStats.keys();
    // [OPTIMIZATION] 2026-04-08 使用 localeAwareCompare 实现不区分大小写的自然排序
    // 解决 "Link" (大写L) 可能因为 ASCII 排序特性排在小写字母前的问题
    std::sort(sortedLabels.begin(), sortedLabels.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });
    
    // [USER_REQUEST] 2026-04-08 按照用户要求：将“文件夹”固定排在最顶部
    // 物理确保其余项（包括 Link）严格排在“文件夹”之后
    if (sortedLabels.contains("文件夹")) {
        sortedLabels.removeAll("文件夹");
        sortedLabels.prepend("文件夹");
    }
    
    for (const QString& label : sortedLabels) {
        QVariantMap item;
        item["key"] = label;
        item["label"] = label;
        item["count"] = typeStats[label].toInt();
        typeData.append(item);
    }
    refreshNode("types", typeData);

    // 4. 标签
    QList<QVariantMap> tagData;
    QVariantMap tagStats = stats["tags"].toMap();
    for (auto it = tagStats.begin(); it != tagStats.end(); ++it) {
        QVariantMap item;
        item["key"] = it.key();
        item["label"] = it.key();
        item["count"] = it.value().toInt();
        tagData.append(item);
    }
    refreshNode("tags", tagData);

    // 5. 创建日期与修改日期辅助逻辑
    QDate today = QDate::currentDate();
    auto processDateStats = [&](const QString& key, const QString& statsKey) {
        QList<QVariantMap> dateData;
        QVariantMap dateStats = stats[statsKey].toMap();
        QStringList sortedDates = dateStats.keys();
        std::sort(sortedDates.begin(), sortedDates.end(), std::greater<QString>());

        for (const QString& dateStr : sortedDates) {
            QDate date = QDate::fromString(dateStr, "yyyy-MM-dd");
            QString label;
            qint64 daysTo = date.daysTo(today);
            if (daysTo == 0) label = "今天";
            else if (daysTo == 1) label = "昨天";
            else if (daysTo == 2) label = "2 天前";
            else label = date.toString("yyyy/M/d");

            QVariantMap item;
            item["key"] = dateStr;
            item["label"] = label;
            item["count"] = dateStats[dateStr].toInt();
            dateData.append(item);
        }
        refreshNode(key, dateData);
    };

    processDateStats("date_create", "date_create");
    processDateStats("date_update", "date_update");

    m_blockItemClick = false;
    m_tree->blockSignals(false);
}

void FilterPanel::refreshNode(const QString& key, const QList<QVariantMap>& items, bool isCol) {
    if (!m_roots.contains(key)) return;
    auto* root = m_roots[key];

    // 建立现有的 key -> item 映射
    QMap<QString, QTreeWidgetItem*> existingItems;
    for (int i = 0; i < root->childCount(); ++i) {
        auto* child = root->child(i);
        existingItems[child->data(0, Qt::UserRole).toString()] = child;
    }

    QSet<QString> currentKeys;
    for (int i = 0; i < items.size(); ++i) {
        const auto& data = items[i];
        QString itemKey = data["key"].toString();
        QString label = data["label"].toString();
        int count = data["count"].toInt();
        currentKeys.insert(itemKey);

        QString newText = QString("%1 (%2)").arg(label).arg(count);
        QTreeWidgetItem* child = nullptr;

        if (existingItems.contains(itemKey)) {
            child = existingItems[itemKey];
            if (child->text(0) != newText) {
                child->setText(0, newText);
            }
            
            // 2026-04-xx 按照用户要求：物理级修复评级排序逻辑，确保项的显示顺序与统计列表严格一致
            int currentIndex = root->indexOfChild(child);
            if (currentIndex != i) {
                root->takeChild(currentIndex);
                root->insertChild(i, child);
            }
        } else {
            child = new QTreeWidgetItem();
            child->setText(0, newText);
            child->setData(0, Qt::UserRole, itemKey);
            child->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            child->setCheckState(0, Qt::Unchecked);
            
            if (isCol) {
                child->setIcon(0, IconHelper::getIcon("circle_filled", itemKey));
            }
            root->insertChild(i, child);
        }
        
        // [MODIFIED] 2026-04-08 物理清理：不再处理任何子项嵌套逻辑。
    }

    // 移除不再需要的项目
    for (int i = root->childCount() - 1; i >= 0; --i) {
        auto* child = root->child(i);
        if (!currentKeys.contains(child->data(0, Qt::UserRole).toString())) {
            delete root->takeChild(i);
        }
    }
}


QVariantMap FilterPanel::getCheckedCriteria() const {
    QVariantMap criteria;
    for (auto it = m_roots.begin(); it != m_roots.end(); ++it) {
        QStringList checked;
        QTreeWidgetItem* root = it.value();
        for (int i = 0; i < root->childCount(); ++i) {
            auto* item = root->child(i);
            if (item->checkState(0) == Qt::Checked) {
                checked << item->data(0, Qt::UserRole).toString();
            }
            // [MODIFIED] 2026-04-08 扁平化逻辑：不再处理 PartiallyChecked，因为已无子项
        }
        if (!checked.isEmpty()) {
            criteria[it.key()] = checked;
        }
    }
    return criteria;
}

void FilterPanel::resetFilters() {
    m_tree->blockSignals(true);
    for (auto* root : m_roots) {
        for (int i = 0; i < root->childCount(); ++i) {
            auto* item = root->child(i);
            item->setCheckState(0, Qt::Unchecked);
        }
    }
    m_tree->blockSignals(false);
    emit filterChanged();
}

void FilterPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (m_blockItemClick || !item || !(item->flags() & Qt::ItemIsUserCheckable)) return;

    // [MODIFIED] 2026-04-08 扁平化逻辑：移除所有层级联动逻辑（递归更新父子状态），因为已改为单层结构。
    
    // 记录最近改变的项，用于防止 onItemClicked 重复处理
    m_lastChangedItem = item;
    QTimer::singleShot(100, [this]() { m_lastChangedItem = nullptr; });
    
    emit filterChanged();
}

void FilterPanel::onItemClicked(QTreeWidgetItem* item, int column) {
    if (!item) return;

    // 如果该项刚刚由 Qt 原生机制改变了状态（点击了复选框），则忽略此次点击事件
    if (m_lastChangedItem == item) return;

    // [CRITICAL] 2026-04-08 傻逼逻辑修复：物理隔离单击与双击职责。
    // 单击仅允许切换勾选状态，严禁触发展开/折叠。

    // 处理点击文字时的勾选状态切换逻辑
    if (item->flags() & Qt::ItemIsUserCheckable) {
        // 切换逻辑：若当前未全选（包括部分选中），则变为全选；若已全选，则取消勾选
        Qt::CheckState newState = (item->checkState(0) == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        item->setCheckState(0, newState);
        // 注：由于连接了 itemChanged 信号，具体的联动递归逻辑已在 onItemChanged 中实现，此处仅需触发状态变更
    }
}

void FilterPanel::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (!item) return;
    
    // [USER_REQUEST] 2026-04-08：只有双击才允许切换展开/折叠状态。
    if (item->childCount() > 0) {
        item->setExpanded(!item->isExpanded());
    }
}

bool FilterPanel::eventFilter(QObject* watched, QEvent* event) {
    // 2026-04-xx 修正：物理级拦截并重定向 ToolTip 到项目指定的 ToolTipOverlay
    if (event->type() == QEvent::ToolTip) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, 2000);
        }
        return true; 
    }
    return QWidget::eventFilter(watched, event);
}

void FilterPanel::toggleAllGroups() {
    // 2026-04-xx 按照用户要求：快捷键触发各组折叠/展开切换（循环逻辑）
    if (m_roots.isEmpty()) return;
    
    bool anyCollapsed = false;
    for (auto* root : m_roots) {
        if (!root->isExpanded()) {
            anyCollapsed = true;
            break;
        }
    }
    
    // 如果有任何一个组是折叠的，则全部展开；否则全部折叠
    for (auto* root : m_roots) {
        root->setExpanded(anyCollapsed);
    }
}
