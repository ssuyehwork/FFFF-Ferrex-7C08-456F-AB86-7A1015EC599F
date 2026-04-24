#include "ScanDialog.h"
#include "UiHelper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QDir>
#include <QDirIterator>
#include <QStorageInfo>
#include <QtConcurrent>
#include <QMetaObject>
#include <QFileInfo>
#include <QHeaderView>
#include <QPointer>
#include <QByteArray>
#include <QDateTime>
#include <QSettings>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace ArcMeta {

// --- ScanTableModel Implementation ---

ScanTableModel::ScanTableModel(QObject* parent)
    : QAbstractTableModel(parent) {
    connect(&m_filterWatcher, &QFutureWatcher<QVector<int>>::finished, this, [this]() {
        beginResetModel();
        m_filteredIndices = m_filterWatcher.result();
        endResetModel();
        emit filterFinished(m_filteredIndices.size());
    });
}

ScanTableModel::~ScanTableModel() {
    m_filterWatcher.cancel();
    m_filterWatcher.waitForFinished();
}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_filteredIndices.size();
}

int ScanTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return 4;
}

QString ScanTableModel::getFullPath(int row) const {
    if (row < 0 || row >= m_filteredIndices.size()) return QString();
    return getPathByEntryIndex(m_filteredIndices[row]);
}

QString ScanTableModel::getPathByEntryIndex(int entryIndex) const {
    if (entryIndex < 0 || entryIndex >= m_allEntries.size()) return QString();
    
    // 2026-xx-xx 按照用户要求：引入路径缓存机制，并增加读写锁以支持多线程过滤安全
    {
        QReadLocker locker(&m_pathLock);
        if (m_pathCache.contains(entryIndex)) return m_pathCache[entryIndex];
    }

    QStringList parts;
    int curr = entryIndex;
    int depth = 0;
    int firstCached = -1;

    while (curr != -1 && depth < 100) {
        if (m_pathCache.contains(curr)) {
            firstCached = curr;
            break;
        }
        parts.prepend(m_allEntries[curr].name);
        curr = m_allEntries[curr].parentIndex;
        depth++;
    }
    
    QString path;
    if (firstCached != -1) {
        QString base = m_pathCache[firstCached];
        if (base.endsWith('\\') || base.endsWith('/')) {
            path = base + parts.join("/");
        } else {
            path = base + "/" + parts.join("/");
        }
    } else {
        path = parts.join("/");
        if (path.length() == 2 && path.endsWith(':')) path += "/";
    }
    
    QString nativePath = QDir::toNativeSeparators(path);
    {
        QWriteLocker locker(&m_pathLock);
        m_pathCache[entryIndex] = nativePath;
    }
    return nativePath;
}

QString IndexedEntry::suffix() const {
    if (isDir) return QString();
    int pos = name.lastIndexOf('.');
    if (pos == -1) return QString();
    return name.mid(pos + 1).toLower();
}

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_filteredIndices.size()) return QVariant();

    int actualIndex = m_filteredIndices[index.row()];
    const auto& entry = m_allEntries[actualIndex];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return entry.name;
            case 1: return getFullPath(index.row());
            case 2: {
                if (entry.isDir) return QVariant();
                if (entry.size <= 0) return "0 B";
                if (entry.size < 1024) return QString("%1 B").arg(entry.size);
                if (entry.size < 1024 * 1024) return QString("%1 KB").arg(entry.size / 1024.0, 0, 'f', 1);
                return QString("%1 MB").arg(entry.size / (1024.0 * 1024.0), 0, 'f', 1);
            }
            case 3: return QDateTime::fromMSecsSinceEpoch(entry.modifyTime).toString("yyyy-MM-dd HH:mm");
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        if (entry.isDir) {
            return UiHelper::getIcon("folder_filled", QColor("#3498db"), 16);
        }
        return UiHelper::getFileIcon(entry.name, 16);
    } else if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    } else if (role == Qt::UserRole) {
        return actualIndex;
    }

    return QVariant();
}

QVariant ScanTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole) {
            switch (section) {
                case 0: return "名称";
                case 1: return "路径";
                case 2: return "大小";
                case 3: return "修改日期";
            }
        } else if (role == Qt::TextAlignmentRole) {
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    return QVariant();
}

void ScanTableModel::setEntries(QVector<IndexedEntry>&& entries) {
    m_filterWatcher.cancel();
    m_filterWatcher.waitForFinished();

    beginResetModel();
    m_allEntries = std::move(entries);
    m_pathCache.clear();
    // 预填充根节点的路径缓存以加速后续递归
    for (int i = 0; i < m_allEntries.size(); ++i) {
        if (m_allEntries[i].parentIndex == -1) {
            QString rootPath = QDir::toNativeSeparators(m_allEntries[i].name);
            if (rootPath.length() == 2 && rootPath.endsWith(':')) rootPath += "/";
            m_pathCache[i] = rootPath;
        }
    }
    m_filteredIndices.clear();
    endResetModel();
    startAsyncRebuild();
}

void ScanTableModel::setFilterText(const QString& text) {
    if (m_filterText == text) return;
    m_filterText = text;
    startAsyncRebuild();
}

void ScanTableModel::setFilterState(const ScanFilterState& state) {
    m_filterState = state;
    startAsyncRebuild();
}

void ScanTableModel::startAsyncRebuild() {
    m_filterWatcher.cancel();
    
    // 2026-xx-xx 按照用户要求：将过滤逻辑异步化，防止大数据量下搜索卡死 UI
    QFuture<QVector<int>> future = QtConcurrent::run([this, text = m_filterText, state = m_filterState]() {
        return performRebuild(text, state);
    });
    m_filterWatcher.setFuture(future);
}

QVector<int> ScanTableModel::performRebuild(const QString& filterText, const ScanFilterState& filterState) {
    QVector<int> result;
    // 2026-xx-xx 按照用户要求：在后台筛选线程中使用快照或直接访问受保护数据，此函数执行时 UI 线程已通过 waitForFinished 保证 entries 稳定
    if (m_allEntries.isEmpty()) return result;

    result.reserve(m_allEntries.size() / (filterText.isEmpty() ? 1 : 10));
    bool hasSlash = filterText.contains('/') || filterText.contains('\\');

    for (int i = 0; i < m_allEntries.size(); ++i) {
        // 检查是否已请求取消。注意：此处检查应针对当前正在运行的 Future 状态
        if (m_filterWatcher.isCanceled()) return result;

        const IndexedEntry& e = m_allEntries[i];
        bool textMatch = true;
        if (!filterText.isEmpty()) {
            textMatch = e.name.contains(filterText, Qt::CaseInsensitive);
            if (!textMatch) {
                if (!hasSlash) {
                    int curr = e.parentIndex;
                    while (curr != -1) {
                        if (m_allEntries[curr].name.contains(filterText, Qt::CaseInsensitive)) {
                            textMatch = true;
                            break;
                        }
                        curr = m_allEntries[curr].parentIndex;
                    }
                } else {
                    // 全路径匹配模式
                    QString fullPath = getPathByEntryIndex(i);
                    textMatch = fullPath.contains(filterText, Qt::CaseInsensitive);
                }
            }
        }
        if (!textMatch) continue;

        if (!filterState.isEmpty()) {
            if (!filterState.types.isEmpty()) {
                QString typeStr = e.isDir ? "文件夹" : "文件";
                if (!filterState.types.contains(typeStr)) continue;
            }
            if (!filterState.suffixes.isEmpty()) {
                if (e.isDir) continue; 
                QString sfx = e.suffix();
                if (sfx.isEmpty()) sfx = "无后缀";
                if (!filterState.suffixes.contains(sfx)) continue;
            }
        }

        result.push_back(i);
    }
    return result;
}

// --- ScanDialog Implementation ---

#ifdef Q_OS_WIN
namespace {
QString normalizeDriveRoot(const QString& root) {
    QString normalized = QDir::fromNativeSeparators(root).trimmed();
    if (!normalized.endsWith('/')) { normalized += '/'; }
    return normalized;
}

QString toVolumeDevicePath(const QString& driveRoot) {
    const QString normalized = normalizeDriveRoot(driveRoot);
    if (normalized.size() < 2 || normalized[1] != ':') { return QString(); }
    return QStringLiteral("\\\\.\\") + normalized.left(2);
}

int64_t win32FileTimeToUnixMs(LARGE_INTEGER ft) {
    return (ft.QuadPart - 116444736000000000LL) / 10000;
}
}
#endif

ScanDialog::ScanDialog(QWidget* parent)
    : FramelessDialog("实时扫描与查找", parent) 
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1200, 760);
    initContent();
}

ScanDialog::~ScanDialog() {
    m_cancelRequested.store(true);
    if (m_scanFuture.isRunning()) {
        m_scanFuture.waitForFinished(); 
    }
}

void ScanDialog::initContent() {
    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    m_selectAll = new QCheckBox("扫描全部硬盘");
    m_selectAll->setStyleSheet("QCheckBox { color: #378ADD; font-weight: bold; }");

    m_driveScrollArea = new QScrollArea(m_contentArea);
    m_driveScrollArea->setWidgetResizable(true);
    m_driveScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_driveScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_driveScrollArea->setFixedHeight(42);
    m_driveScrollArea->setStyleSheet("QScrollArea { background: #2D2D2D; border: 1px solid #444; border-radius: 6px; } QWidget { background: transparent; color: #EEE; }");

    m_driveRowWidget = new QWidget(m_driveScrollArea);
    m_driveRowLayout = new QHBoxLayout(m_driveRowWidget);
    m_driveRowLayout->setContentsMargins(8, 4, 8, 4);
    m_driveRowLayout->setSpacing(14);
    m_driveRowLayout->addStretch();

    for (const QFileInfo& drive : QDir::drives()) {
        QStorageInfo storage(drive.absoluteFilePath());
        if (storage.isValid() && storage.isReady()) {
            QString drivePath = drive.absoluteFilePath();
            auto* driveCheck = new QCheckBox(QString("%1 (%2)").arg(drivePath).arg(storage.displayName()), m_driveRowWidget);
            driveCheck->setProperty("drivePath", drivePath);
            driveCheck->setStyleSheet("QCheckBox { color: #EEE; font-size: 12px; background: transparent; }");
            m_driveChecks.push_back(driveCheck);
            m_driveRowLayout->insertWidget(m_driveRowLayout->count() - 1, driveCheck);
        }
    }
    m_driveScrollArea->setWidget(m_driveRowWidget);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addWidget(m_selectAll);
    headerRow->addWidget(m_driveScrollArea, 1);

    m_btnCancel = new QPushButton();
    m_btnCancel->setAutoDefault(false);
    m_btnCancel->setDefault(false);
    m_btnCancel->setFixedSize(32, 32);
    m_btnCancel->setIcon(UiHelper::getIcon("close", QColor("#EEEEEE"), 16));
    m_btnCancel->setIconSize(QSize(16, 16));
    m_btnCancel->setProperty("tooltipText", "取消扫描");
    m_btnCancel->setStyleSheet("QPushButton { background: transparent; color: #888; border: 1px solid #444; border-radius: 4px; } QPushButton:hover { background: #333; color: #EEE; }");
    connect(m_btnCancel, &QPushButton::clicked, this, &ScanDialog::onCancelClicked);
    headerRow->addWidget(m_btnCancel);

    m_btnStart = new QPushButton();
    m_btnStart->setAutoDefault(false);
    m_btnStart->setDefault(false);
    m_btnStart->setFixedSize(32, 32);
    m_btnStart->setIcon(UiHelper::getIcon("scan", QColor("#FFFFFF"), 16));
    m_btnStart->setIconSize(QSize(16, 16));
    m_btnStart->setProperty("tooltipText", "开始扫描");
    m_btnStart->setStyleSheet("QPushButton { background: #378ADD; color: white; border: none; border-radius: 4px; } QPushButton:hover { background: #4FACFE; } QPushButton:disabled { background: #444; }");
    connect(m_btnStart, &QPushButton::clicked, this, &ScanDialog::onStartClicked);
    headerRow->addWidget(m_btnStart);
    layout->addLayout(headerRow);

    connect(m_selectAll, &QCheckBox::toggled, [this](bool checked) {
        for (QCheckBox* driveCheck : m_driveChecks) if (driveCheck) driveCheck->setChecked(checked);
    });

    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("输入关键词后按回车执行搜索...");
    m_searchEdit->setMinimumHeight(34);
    m_searchEdit->setStyleSheet("QLineEdit { background: #2D2D2D; border: 1px solid #444; border-radius: 6px; color: #EEE; padding: 0 10px; } QLineEdit:focus { border: 1px solid #378ADD; }");
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ScanDialog::onSearchTriggered);
    layout->addWidget(m_searchEdit);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, m_contentArea);
    splitter->setStyleSheet("QSplitter::handle { background: #333; width: 1px; }");

    m_resultView = new QTableView(splitter);
    m_resultView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultView->setAlternatingRowColors(false);
    m_resultView->setShowGrid(false);
    m_resultView->verticalHeader()->setVisible(false);
    
    // 【深度修复：彻底移除傻逼滚动条】强制关闭滚动条策略，并从 QSS 中清除样式，实现视觉无缝对齐
    m_resultView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_resultView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 【深度修复：铲除原生恶心虚线】
    m_resultView->setFocusPolicy(Qt::NoFocus);

    // 【深度修复1】强制锁死列表头高度为 30px
    m_resultView->horizontalHeader()->setFixedHeight(30);

    // 【深度修复2】重构样式，彻底铲除滚动条样式定义，优化表格边框以对接筛选器面板，并强制禁用虚线框
    QString tableStyle = 
        "QTableView { background: #1F1F1F; border: 1px solid #333; border-right: none; border-radius: 0px; color: #EEE; gridline-color: #333; outline: none; }"
        "QHeaderView::section { background: #252526; color: #AAA; font-size: 12px; border: none; border-right: 1px solid #333; border-bottom: 1px solid #333; padding: 0 8px; text-align: left; }"
        "QTableView::item:selected { background: #2F5E8D; color: white; }"
        "QTableView::item:focus { background: #2F5E8D; outline: none; }";
    m_resultView->setStyleSheet(tableStyle);
    
    m_tableModel = new ScanTableModel(this);
    m_resultView->setModel(m_tableModel);

    connect(m_tableModel, &ScanTableModel::filterFinished, this, [this](int count) {
        m_summaryLabel->setText(QString("索引总数: %1 | 命中: %2").arg(m_tableModel->totalCount()).arg(count));
    });

    m_resultView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_resultView->horizontalHeader()->setStretchLastSection(true);
    
    m_resultView->setColumnWidth(0, 300);
    m_resultView->setColumnWidth(1, 400);
    m_resultView->setColumnWidth(2, 100);
    m_resultView->setColumnWidth(3, 150);

    QSettings settings("ArcMeta", "ScanDialog");
    if (settings.contains("HeaderState")) {
        m_resultView->horizontalHeader()->restoreState(settings.value("HeaderState").toByteArray());
    }

    m_filterPanel = new QWidget(splitter);
    m_filterPanel->setFixedWidth(260);
    m_filterPanel->setObjectName("FilterPanel");
    m_filterPanel->setStyleSheet("QWidget#FilterPanel { background-color: #252526; border: 1px solid #333; border-left: 1px solid #333; }");
    
    QVBoxLayout* fl = new QVBoxLayout(m_filterPanel);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(0);

    // 【深度修复1同步】统一筛选器标题栏与表格标题栏的高度与视觉风格
    QWidget* fHeader = new QWidget(m_filterPanel);
    fHeader->setFixedHeight(30); 
    fHeader->setObjectName("FilterHeader");
    fHeader->setStyleSheet("QWidget#FilterHeader { background: #252526; border-bottom: 1px solid #333; border-left: none; }");
    QHBoxLayout* fhl = new QHBoxLayout(fHeader);
    fhl->setContentsMargins(12, 0, 12, 0);
    QLabel* fTitle = new QLabel("筛选器", fHeader);
    fTitle->setStyleSheet("color: #AAA; font-size: 12px; font-weight: bold; border: none;"); // 字体与列标题大小统一
    fhl->addWidget(fTitle);
    fl->addWidget(fHeader);

    QScrollArea* sa = new QScrollArea(m_filterPanel);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    // 【交互补正】针对筛选器面板恢复必要的垂直滚动条，以应对海量后缀名显示
    sa->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); 
    sa->setStyleSheet(
        "QScrollArea { background: #252526; border: none; }"
        "QScrollBar:vertical { width: 8px; background: transparent; }"
        "QScrollBar::handle:vertical { background: #444; border-radius: 4px; min-height: 20px; }"
        "QScrollBar::handle:vertical:hover { background: #555; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
    );
    QWidget* sc = new QWidget();
    sc->setStyleSheet("background: transparent;");
    m_filterContainerLayout = new QVBoxLayout(sc);
    m_filterContainerLayout->setContentsMargins(0, 8, 0, 10);
    m_filterContainerLayout->setSpacing(4);
    m_filterContainerLayout->addStretch();
    sa->setWidget(sc);
    fl->addWidget(sa);

    splitter->addWidget(m_resultView);
    splitter->addWidget(m_filterPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    layout->addWidget(splitter, 1);

    // 【深度修复3】底部状态栏整体结构重建，告别难看的布局重叠和细线进度条
    QWidget* bottomBar = new QWidget(m_contentArea);
    bottomBar->setFixedHeight(32);
    QHBoxLayout* bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(4, 0, 4, 0);

    m_summaryLabel = new QLabel("索引未建立", bottomBar);
    m_summaryLabel->setStyleSheet("color: #AAAAAA; font-size: 11px;");
    bottomLayout->addWidget(m_summaryLabel);

    bottomLayout->addStretch(1);

    m_progressBar = new QProgressBar(bottomBar);
    m_progressBar->setFixedHeight(14); // 进度条标准厚度
    m_progressBar->setFixedWidth(300); // 居中固定宽度
    m_progressBar->setTextVisible(true);
    m_progressBar->setAlignment(Qt::AlignCenter);
    m_progressBar->setStyleSheet(
        "QProgressBar { background: #1E1E1E; border: 1px solid #444; border-radius: 0px; color: #EEE; font-size: 10px; }"
        "QProgressBar::chunk { background: #378ADD; border-radius: 0px; }"
    );
    m_progressBar->hide(); // 默认隐藏，不占空隙
    bottomLayout->addWidget(m_progressBar, 0, Qt::AlignCenter);

    bottomLayout->addStretch(1);

    m_statusLabel = new QLabel("就绪", bottomBar);
    m_statusLabel->setStyleSheet("color: #AAAAAA; font-size: 11px;");
    bottomLayout->addWidget(m_statusLabel);

    layout->addWidget(bottomBar);
}

void ScanDialog::onStartClicked() {
    if (m_isScanning) return;
    QStringList drives;
    if (!collectSelectedDrives(drives)) return;
    clearIndexData();
    updateUiStateForScanning(true);
    m_statusLabel->setText("正在进行多核并发扫描...");
    QPointer<ScanDialog> safeThis(this);
    m_scanFuture = QtConcurrent::run([safeThis, drives]() {
        if (!safeThis) return;
        QVector<IndexedEntry> allEntries;
        std::atomic<int> finishedDrives{0};
        
        struct DriveResult {
            QVector<IndexedEntry> entries;
            QHash<QString, int> typeCounts;
            QHash<QString, int> suffixCounts;
        };

        QList<QFuture<DriveResult>> futures;
        for (const QString& drive : drives) {
            futures.append(QtConcurrent::run([safeThis, drive, &finishedDrives, total = drives.size()]() {
                DriveResult dr;
                if (!safeThis) return dr;
                safeThis->scanDriveEntries(drive, safeThis->m_cancelRequested, dr.entries, dr.typeCounts, dr.suffixCounts, [safeThis, drive, &finishedDrives, total](int count) {
                    if (count % 10000 == 0) {
                        int currentFinished = finishedDrives.load();
                        QMetaObject::invokeMethod(safeThis,[safeThis, drive, count, currentFinished, total]() {
                            if (safeThis) safeThis->updateProgress(currentFinished, total, QString("正在扫描 %1 (已发现 %2)...").arg(drive).arg(count));
                        });
                    }
                });
                finishedDrives++;
                return dr;
            }));
        }
        QHash<QString, int> totalTypeCounts;
        QHash<QString, int> totalSuffixCounts;
        for (auto& f : futures) {
            DriveResult res = f.result();
            if (res.entries.isEmpty()) continue;
            
            // 合并统计结果
            for (auto it = res.typeCounts.begin(); it != res.typeCounts.end(); ++it) totalTypeCounts[it.key()] += it.value();
            for (auto it = res.suffixCounts.begin(); it != res.suffixCounts.end(); ++it) totalSuffixCounts[it.key()] += it.value();

            int offset = allEntries.size();
            for (auto& entry : res.entries) { if (entry.parentIndex != -1) entry.parentIndex += offset; }
            allEntries += res.entries;
        }
        QMetaObject::invokeMethod(safeThis,[safeThis, allEntries = std::move(allEntries), totalTypeCounts, totalSuffixCounts]() mutable {
            if (safeThis) { 
                safeThis->m_indexEntries = std::move(allEntries); 
                // 暂时利用属性或成员传递统计结果，避免修改 onScanFinished 签名导致更大范围变动
                safeThis->setProperty("tempTypeCounts", QVariant::fromValue(totalTypeCounts));
                safeThis->setProperty("tempSuffixCounts", QVariant::fromValue(totalSuffixCounts));
                safeThis->onScanFinished(); 
            }
        });
    });
}

void ScanDialog::updateProgress(int current, int total, const QString& status) {
    if (m_progressBar->isHidden()) m_progressBar->show(); // 扫描开始自动显示巨型进度条
    m_progressBar->setRange(0, total);
    m_progressBar->setValue(current);
    m_statusLabel->setText(status);
}

void ScanDialog::onScanFinished() {
    updateUiStateForScanning(false);
    m_progressBar->hide(); // 扫描完毕后功成身退自动隐藏
    m_tableModel->setEntries(std::move(m_indexEntries));
    m_scanCompleted = true;
    m_statusLabel->setText("扫描完成");
    m_summaryLabel->setText(QString("索引总数: %1").arg(m_tableModel->totalCount()));
    
    rebuildFilterPanel();
    updateActionButtonsForState();
}

bool ScanDialog::collectSelectedDrives(QStringList& drives) const {
    for (auto* cb : m_driveChecks) if (cb && cb->isChecked()) drives.append(cb->property("drivePath").toString());
    return !drives.isEmpty();
}

int ScanDialog::scanDriveEntries(const QString& driveRoot, std::atomic_bool& cancelRequested, 
                                 QVector<IndexedEntry>& outEntries, 
                                 QHash<QString, int>& typeCounts,
                                 QHash<QString, int>& suffixCounts,
                                 const std::function<void(int)>& progressCallback) {
    const QString normalizedRoot = QDir::fromNativeSeparators(driveRoot);
    const QStorageInfo storage(normalizedRoot);
    int rootIdx = outEntries.size();
    IndexedEntry rootEntry;
    rootEntry.name = normalizedRoot.left(2);
    rootEntry.isDir = true;
    rootEntry.parentIndex = -1;
    rootEntry.modifyTime = QDateTime::currentMSecsSinceEpoch();
    outEntries.push_back(rootEntry);

#ifdef Q_OS_WIN
    if (storage.fileSystemType() == "NTFS") {
        const QString devicePath = toVolumeDevicePath(normalizedRoot);
        HANDLE volume = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(devicePath).utf16()),
                                   GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (volume != INVALID_HANDLE_VALUE) {
            std::unordered_map<DWORDLONG, int> frnToIndex;
            frnToIndex.reserve(1500000); 

            MFT_ENUM_DATA_V0 med = { 0, 0, MAXLONGLONG };
            std::vector<BYTE> buffer(4 * 1024 * 1024); 
            int count = 0;
            struct TempNode { DWORDLONG parentFrn; int entryIdx; };
            std::vector<TempNode> tempNodes;
            tempNodes.reserve(1500000);
            outEntries.reserve(1500000);

            while (!cancelRequested.load()) {
                DWORD bytesReturned = 0;
                if (!DeviceIoControl(volume, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr) || bytesReturned <= sizeof(USN)) break;
                BYTE* ptr = buffer.data() + sizeof(USN);
                BYTE* end = buffer.data() + bytesReturned;
                while (ptr < end) {
                    auto* rec = reinterpret_cast<USN_RECORD_V2*>(ptr);
                    if (rec->RecordLength == 0) break;
                    QString name = QString::fromWCharArray(rec->FileName, rec->FileNameLength / sizeof(WCHAR));
                    if (name != "." && name != "..") {
                        IndexedEntry e;
                        e.name = name;
                        e.isDir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        e.modifyTime = (rec->TimeStamp.QuadPart > 0) ? win32FileTimeToUnixMs(rec->TimeStamp) : rootEntry.modifyTime;
                        
                        // 2026-xx-xx 按照用户要求：取消扫描时的实时 IO 获取大小逻辑，这是性能杀手
                        // 仅保留基础属性统计，后续可考虑通过解析 MFT 结构或空闲时异步补全大小
                        if (!e.isDir) {
                            typeCounts["文件"]++;
                            QString sfx = e.suffix();
                            if (sfx.isEmpty()) sfx = "无后缀";
                            suffixCounts[sfx]++;
                        } else {
                            typeCounts["文件夹"]++;
                        }

                        int currIdx = outEntries.size();
                        frnToIndex[rec->FileReferenceNumber] = currIdx;
                        tempNodes.push_back({ rec->ParentFileReferenceNumber, currIdx });
                        outEntries.push_back(e);
                        count++;
                        if (progressCallback && count % 10000 == 0) progressCallback(count);
                    }
                    ptr += rec->RecordLength;
                }
                med.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buffer.data());
                if (med.StartFileReferenceNumber == 0) break;
            }
            CloseHandle(volume);
            for (const auto& node : tempNodes) {
                auto it = frnToIndex.find(node.parentFrn);
                outEntries[node.entryIdx].parentIndex = (it != frnToIndex.end()) ? it->second : rootIdx;
            }
            return count;
        }
    }
#endif

    QHash<QString, int> pathToIndex;
    pathToIndex[QDir(normalizedRoot).absolutePath()] = rootIdx;
    QDirIterator it(normalizedRoot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    int count = 0;
    while (it.hasNext() && !cancelRequested.load()) {
        it.next();
        QFileInfo info = it.fileInfo();
        IndexedEntry e;
        e.name = info.fileName();
        e.isDir = info.isDir();
        e.size = info.size();
        e.modifyTime = info.lastModified().toMSecsSinceEpoch();
        
        if (!e.isDir) {
            typeCounts["文件"]++;
            QString sfx = e.suffix();
            if (sfx.isEmpty()) sfx = "无后缀";
            suffixCounts[sfx]++;
        } else {
            typeCounts["文件夹"]++;
        }

        QString parentPath = info.absolutePath();
        e.parentIndex = pathToIndex.contains(parentPath) ? pathToIndex[parentPath] : rootIdx;
        int currIdx = outEntries.size();
        if (e.isDir) pathToIndex[info.absoluteFilePath()] = currIdx;
        outEntries.push_back(e);
        count++;
        if (progressCallback && count % 1000 == 0) progressCallback(count);
    }
    return count;
}

void ScanDialog::updateUiStateForScanning(bool scanning) {
    m_isScanning = scanning;
    m_btnStart->setEnabled(!scanning || m_scanCompleted);
    m_searchEdit->setEnabled(!scanning);
    m_driveScrollArea->setEnabled(!scanning);
    m_selectAll->setEnabled(!scanning);
    updateActionButtonsForState();
}

void ScanDialog::clearIndexData() {
    m_indexEntries.clear();
    m_tableModel->setEntries(QVector<IndexedEntry>());
    m_currentFilter = ScanFilterState();
    m_tableModel->setFilterState(m_currentFilter);
    rebuildFilterPanel();
    m_scanCompleted = false;
}

void ScanDialog::onSearchTriggered() {
    m_tableModel->setFilterText(m_searchEdit->text());
    // 异步模式下，rowCount 此时可能还没更新，需等待信号
    m_summaryLabel->setText(QString("正在筛选 %1 ...").arg(m_searchEdit->text()));
}

void ScanDialog::onCancelClicked() {
    if (m_isScanning) {
        m_cancelRequested.store(true);
        m_statusLabel->setText("正在请求取消扫描...");
    }
}

void ScanDialog::updateActionButtonsForState() {
    if (!m_btnStart || !m_btnCancel) return;
    if (m_isScanning) {
        m_btnStart->setIcon(UiHelper::getIcon("sync", QColor("#FFFFFF"), 16));
        m_btnStart->setProperty("tooltipText", "正在扫描...");
        m_btnStart->setEnabled(false);
        m_btnCancel->setIcon(UiHelper::getIcon("close", QColor("#EEEEEE"), 16));
        m_btnCancel->setProperty("tooltipText", "取消扫描");
        m_btnCancel->setEnabled(true);
        return;
    }
    m_btnStart->setIcon(UiHelper::getIcon("scan", QColor("#FFFFFF"), 16));
    m_btnStart->setProperty("tooltipText", "开始扫描");
    m_btnStart->setEnabled(true);
    
    // 【逻辑纠正】取消/停止按钮在非扫描态下强制禁用，Tooltop 保持语义一致，严禁执行关闭操作
    m_btnCancel->setIcon(UiHelper::getIcon("close", QColor("#888888"), 16));
    m_btnCancel->setProperty("tooltipText", "取消/停止扫描");
    m_btnCancel->setEnabled(false);
}

void ScanDialog::closeEvent(QCloseEvent* event) {
    if (m_isScanning) { event->ignore(); return; }
    if (m_resultView && m_resultView->horizontalHeader()) {
        QSettings settings("ArcMeta", "ScanDialog");
        settings.setValue("HeaderState", m_resultView->horizontalHeader()->saveState());
    }
    event->accept();
}

void ScanDialog::rebuildFilterPanel() {
    if (!m_filterContainerLayout || !m_tableModel) return;

    while (m_filterContainerLayout->count() > 1) {
        QLayoutItem* item = m_filterContainerLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    // 2026-xx-xx 按照用户要求：优先从预统计缓存中读取，杜绝扫描结束后对百万条数据的二次全量遍历
    QHash<QString, int> typeCounts = property("tempTypeCounts").value<QHash<QString, int>>();
    QHash<QString, int> suffixCounts = property("tempSuffixCounts").value<QHash<QString, int>>();

    if (typeCounts.isEmpty() && suffixCounts.isEmpty()) {
        const auto& entries = m_tableModel->allEntries();
        if (entries.isEmpty()) return;

        for (int i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.isDir) {
                typeCounts["文件夹"]++;
            } else {
                typeCounts["文件"]++;
                QString sfx = entry.suffix();
                if (sfx.isEmpty()) sfx = "无后缀";
                suffixCounts[sfx]++;
            }
        }
    }

    auto addCategoryLabel =[this](const QString& text) {
        QLabel* lbl = new QLabel(text);
        lbl->setStyleSheet("color: #888; font-size: 11px; font-weight: bold; padding: 6px 16px 2px 16px; border: none; background: transparent;");
        m_filterContainerLayout->addWidget(lbl);
    };

    if (!typeCounts.isEmpty()) {
        addCategoryLabel("文件类型");
        if (typeCounts.contains("文件夹")) addFilterRow(m_filterContainerLayout, "文件夹", typeCounts["文件夹"], "type", "文件夹");
        if (typeCounts.contains("文件")) addFilterRow(m_filterContainerLayout, "文件", typeCounts["文件"], "type", "文件");
    }

    if (!suffixCounts.isEmpty()) {
        addCategoryLabel("常见后缀名");
        std::vector<std::pair<int, QString>> sortedList;
        sortedList.reserve(suffixCounts.size());
        
        static auto isGarbageSuffix = [](const QString& s) {
            if (s.length() > 10) {
                bool ok;
                s.toLongLong(&ok);
                if (ok) return true; // 超过10位的纯数字后缀视为垃圾数据
            }
            return false;
        };

        for (auto it = suffixCounts.begin(); it != suffixCounts.end(); ++it) {
            if (isGarbageSuffix(it.key())) continue;
            sortedList.push_back({it.value(), it.key()});
        }
        
        // 【逻辑纠正】筛选器必须按“频率降序”排列，确保高价值后缀名（如 .jpg, .pdf）置顶，解决图片中数字霸屏的脑残体验
        std::sort(sortedList.begin(), sortedList.end(),[](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

        // 扩充显示上限，配合滚动条提升可用性
        int limit = std::min<int>(100, (int)sortedList.size());
        for (int i = 0; i < limit; ++i) {
            addFilterRow(m_filterContainerLayout, sortedList[i].second, sortedList[i].first, "suffix", sortedList[i].second);
        }
    }
}

void ScanDialog::addFilterRow(QVBoxLayout* layout, const QString& label, int count, const QString& category, const QString& value) {
    QWidget* row = new QWidget();
    row->setFixedHeight(26); 
    QHBoxLayout* hl = new QHBoxLayout(row);
    hl->setContentsMargins(16, 0, 16, 0); 
    hl->setSpacing(8);

    QCheckBox* cb = new QCheckBox();
    cb->setStyleSheet(
        "QCheckBox::indicator { width: 14px; height: 14px; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #666; background: #1E1E1E; }"
        "QCheckBox::indicator:checked { border: 1px solid #378ADD; background: #378ADD; }"
    );
    
    if (category == "type") cb->setChecked(m_currentFilter.types.contains(value));
    else if (category == "suffix") cb->setChecked(m_currentFilter.suffixes.contains(value));

    // 严防样式污染：明确指明背景透明且无边框
    QLabel* lbl = new QLabel(label);
    lbl->setStyleSheet("color: #CCC; font-size: 12px; border: none; background: transparent;");
    
    QLabel* cnt = new QLabel(QString("(%1)").arg(count));
    cnt->setStyleSheet("color: #777; font-size: 11px; border: none; background: transparent;");

    hl->addWidget(cb);
    hl->addWidget(lbl);
    
    hl->addStretch();
    hl->addWidget(cnt); 

    auto updateFilter =[this, category, value](bool checked) {
        if (category == "type") {
            if (checked) m_currentFilter.types.append(value);
            else m_currentFilter.types.removeAll(value);
        } else if (category == "suffix") {
            if (checked) m_currentFilter.suffixes.append(value);
            else m_currentFilter.suffixes.removeAll(value);
        }
        m_tableModel->setFilterState(m_currentFilter);
        onSearchTriggered();
    };

    connect(cb, &QCheckBox::toggled, updateFilter);

    layout->addWidget(row);
}

} // namespace ArcMeta