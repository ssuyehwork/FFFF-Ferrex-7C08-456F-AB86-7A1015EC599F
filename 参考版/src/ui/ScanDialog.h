#pragma once

#include "FramelessDialog.h"
#include <QListWidget>
#include <QCheckBox>
#include <QProgressBar>
#include <QFuture>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QLineEdit>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QDateTime>
#include <functional>
#include <QHash>
#include <QReadWriteLock>
#include <atomic>
#include <memory>

namespace ArcMeta {

struct ScanFilterState {
    QStringList types;
    QStringList suffixes;

    bool isEmpty() const { return types.isEmpty() && suffixes.isEmpty(); }
};

/**
 * @brief 紧凑型索引条目
 */
struct IndexedEntry {
    QString name;
    int64_t size = 0;
    int64_t modifyTime = 0;
    int parentIndex = -1; 
    bool isDir = false;

    QString suffix() const;
};

/**
 * @brief 虚拟化表格模型
 */
class ScanTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ScanTableModel(QObject* parent = nullptr);
    ~ScanTableModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setEntries(QVector<IndexedEntry>&& entries);
    QString getFullPath(int row) const;
    QString getPathByEntryIndex(int entryIndex) const;

    void setFilterText(const QString& text);
    void setFilterState(const ScanFilterState& state);
    int totalCount() const { return m_allEntries.size(); }
    const QVector<IndexedEntry>& allEntries() const { return m_allEntries; }

signals:
    void filterFinished(int count);

private:
    void startAsyncRebuild();
    QVector<int> performRebuild(const QString& text, const ScanFilterState& state);

    QVector<IndexedEntry> m_allEntries;
    QVector<int> m_filteredIndices;
    QString m_filterText;
    ScanFilterState m_filterState;

    QFutureWatcher<QVector<int>> m_filterWatcher;
    mutable QHash<int, QString> m_pathCache;
    mutable QReadWriteLock m_pathLock;
};

class ScanDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit ScanDialog(QWidget* parent = nullptr);
    ~ScanDialog() override;

private slots:
    void onStartClicked();
    void onCancelClicked();
    void onSearchTriggered();
    void updateProgress(int current, int total, const QString& status);
    void onScanFinished();

private:
    void initContent();
    bool collectSelectedDrives(QStringList& drives) const;
    int scanDriveEntries(const QString& driveRoot, 
                        std::atomic_bool& cancelRequested, 
                        QVector<IndexedEntry>& outEntries,
                        QHash<QString, int>& typeCounts,
                        QHash<QString, int>& suffixCounts,
                        const std::function<void(int)>& progressCallback);

    void updateUiStateForScanning(bool scanning);
    void clearIndexData();
    void updateActionButtonsForState();
    void closeEvent(QCloseEvent* event) override;
    
    void rebuildFilterPanel();
    void addFilterRow(QVBoxLayout* layout, const QString& label, int count, const QString& category, const QString& value);

    QLineEdit* m_searchEdit = nullptr;
    QScrollArea* m_driveScrollArea = nullptr;
    QWidget* m_driveRowWidget = nullptr;
    QHBoxLayout* m_driveRowLayout = nullptr;
    QList<QCheckBox*> m_driveChecks;
    QTableView* m_resultView = nullptr;
    QCheckBox* m_selectAll = nullptr;
    QPushButton* m_btnCancel = nullptr;
    QPushButton* m_btnStart = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;

    ScanTableModel* m_tableModel = nullptr;
    QWidget* m_filterPanel = nullptr;
    QVBoxLayout* m_filterContainerLayout = nullptr;
    ScanFilterState m_currentFilter;
    
    QVector<IndexedEntry> m_indexEntries;
    
    bool m_isScanning = false;
    bool m_scanCompleted = false;
    std::atomic_bool m_cancelRequested{false};
    QFuture<void> m_scanFuture;
};

} // namespace ArcMeta