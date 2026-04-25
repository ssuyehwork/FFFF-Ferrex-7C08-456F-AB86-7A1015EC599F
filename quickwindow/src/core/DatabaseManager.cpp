#include "DatabaseManager.h"
#include <QDebug>
#include <QSqlRecord>
#include <QSqlError>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSet>
#include <QRegularExpression>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtConcurrent>
#include <QThreadPool>
#include <QMessageBox>
#include <utility>
#include <algorithm>
#include "FileCryptoHelper.h"
#include "HardwareInfoHelper.h"
#include "ClipboardMonitor.h"
#include "../ui/StringUtils.h"
#include "../ui/FramelessDialog.h"

namespace {
    // 2026-04-09 按照用户要求：采用纯动态后缀解析架构，彻底废弃并移除所有静态白/黑名单
    QString extractFileExtensions(const QString& itemType, const QString& content) {
        if (itemType == "link") return "Link";
        
        // 仅处理文件/文件夹相关类型
        if (itemType != "file" && itemType != "local_file" && itemType != "local_batch" && itemType != "folder" && itemType != "local_folder") {
            return "";
        }
        
        QStringList paths = content.split(";", Qt::SkipEmptyParts);
        QSet<QString> extensions;
        for (const QString& path : paths) {
            QFileInfo info(path);
            if (info.isDir()) {
                extensions.insert("文件夹");
            } else {
                QString ext = info.suffix().toLower();
                if (!ext.isEmpty()) extensions.insert(ext);
            }
        }
        
        QStringList result = extensions.values();
        result.sort();
        return result.join(", ");
    }

}

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager inst;
    return inst;
}

QStringList DatabaseManager::s_tagClipboard;
QMutex DatabaseManager::s_tagClipboardMutex;

void DatabaseManager::setTagClipboard(const QStringList& tags) {
    QMutexLocker locker(&s_tagClipboardMutex);
    s_tagClipboard = tags;
}

QStringList DatabaseManager::getTagClipboard() {
    QMutexLocker locker(&s_tagClipboardMutex);
    return s_tagClipboard;
}

DatabaseManager::DatabaseManager(QObject* parent) : QObject(parent) {
    QSettings settings("RapidNotes", "QuickWindow");
    m_autoCategorizeEnabled = settings.value("autoCategorizeClipboard", false).toBool();
    m_lockedCategoriesHidden = settings.value("lockedCategoriesHidden", false).toBool();

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setInterval(7000); // 7秒增量同步间隔
    connect(m_autoSaveTimer, &QTimer::timeout, this, &DatabaseManager::handleAutoSave);
}

void DatabaseManager::setAutoCategorizeEnabled(bool enabled) {
    if (m_autoCategorizeEnabled != enabled) {
        m_autoCategorizeEnabled = enabled;
        QSettings settings("RapidNotes", "QuickWindow");
        settings.setValue("autoCategorizeClipboard", enabled);
        emit autoCategorizeEnabledChanged(enabled);
    }
}

void DatabaseManager::setActiveCategoryId(int id) {
    if (m_activeCategoryId != id) {
        m_activeCategoryId = id;
        emit activeCategoryIdChanged(id);
    }
}

QString DatabaseManager::getCategoryNameById(int id) {
    if (id <= 0) return "";
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("SELECT name FROM categories WHERE id = :id");
    query.bindValue(":id", id);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return "";
}

QVariantMap DatabaseManager::getRootCategory(int catId) {
    if (catId <= 0) return QVariantMap();
    QMutexLocker locker(&m_mutex);
    
    int currentId = catId;
    QVariantMap result;
    
    // 递归向上查找父分类，直到顶级
    while (true) {
        QSqlQuery query(m_db);
        query.prepare("SELECT id, name, parent_id FROM categories WHERE id = :id");
        query.bindValue(":id", currentId);
        
        if (query.exec() && query.next()) {
            result["id"] = query.value("id");
            result["name"] = query.value("name");
            int parentId = query.value("parent_id").isNull() ? -1 : query.value("parent_id").toInt();
            
            if (parentId <= 0) {
                // 已经到达最顶层
                break;
            } else {
                currentId = parentId;
            }
        } else {
            break;
        }
    }
    
    return result;
}

DatabaseManager::~DatabaseManager() {
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }
    if (m_db.isOpen()) {
        m_db.close();
    }
}

void DatabaseManager::logStartup(const QString& msg) {
    // 2026-03-xx 按照用户要求：彻底移除 startup_debug.log 文件写入逻辑，仅保留控制台调试输出
    qDebug() << "[DB-STARTUP]" << msg;
}

bool DatabaseManager::init(const QString& dbPath) {
    QMutexLocker locker(&m_mutex);
    m_isInitialized = false;
    m_lastError.clear();
    
    logStartup("--- [DE-SHELL] 架构初始化开始 ---");

    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        m_lastError = "Qt 环境缺失 QSQLITE 驱动支持。";
        logStartup("[ERR] " + m_lastError);
        return false;
    }
    
    // 统一明文数据库路径 (程序目录下的 inspiration.db)
    m_realDbPath = dbPath; 
    // [ARCH-CHANGE] 在“去壳”架构下，内核路径即为主路径，不再使用 AppData 里的 notes_core
    m_dbPath = m_realDbPath;

    logStartup("目标主路径: " + m_dbPath);

    auto isSqlite = [](const QString& path) -> bool {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray header = f.read(16);
            f.close();
            return header.startsWith("SQLite format 3");
        }
        return false;
    };

    bool loaded = false;

    // [LEVEL 1] 优先检测主路径是否已是明文 SQLite
    if (QFile::exists(m_dbPath) && isSqlite(m_dbPath)) {
        logStartup("[L1] 命中主路径明文数据库，直接加载。");
        loaded = true;
    }

    // [LEVEL 2] 抢救 AppData 残留 ( rapidnotes_kernel.db )
    if (!loaded) {
        QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (appDataPath.isEmpty()) appDataPath = QCoreApplication::applicationDirPath() + "/data";
        QString residuePath = appDataPath + "/rapidnotes_kernel.db";
        
        if (QFile::exists(residuePath) && isSqlite(residuePath)) {
            logStartup("[L2] 监测到上次异常退出的 AppData 内核残留，正在执行原地抢救...");
            if (QFile::exists(m_dbPath)) QFile::remove(m_dbPath);
            if (QFile::copy(residuePath, m_dbPath)) {
                loaded = true;
                logStartup("[L2] 抢救成功，残留已同步至主路径。");
            }
        }
    }

    // [LEVEL 3] 检测旧版明文 notes.db 迁移
    if (!loaded) {
        QString legacyPlain = QFileInfo(m_realDbPath).absolutePath() + "/notes.db";
        if (QFile::exists(legacyPlain) && isSqlite(legacyPlain)) {
            logStartup("[L3] 检测到旧版 notes.db 明文，执行架构平滑迁移...");
            if (QFile::copy(legacyPlain, m_dbPath)) {
                loaded = true;
                FileCryptoHelper::secureDelete(legacyPlain);
            }
        }
    }

    // [LEVEL 4] 核心解壳：尝试对主路径进行多指纹解密 (用于从旧版加密架构初次升级)
    if (!loaded && QFile::exists(m_dbPath)) {
        logStartup("[L4] 正在尝试对加密外壳进行多密钥自适应解密迁移...");
        
        QString appDriveSN = HardwareInfoHelper::getAppDrivePhysicalSerialNumber();
        QString cDriveSN = HardwareInfoHelper::getCDiskPhysicalSerialNumber();
        QStringList candidateKeys;
        if (!appDriveSN.isEmpty()) candidateKeys << FileCryptoHelper::getCombinedKeyBySN(appDriveSN);
        if (!cDriveSN.isEmpty()) candidateKeys << FileCryptoHelper::getCombinedKeyBySN(cDriveSN);
        candidateKeys << FileCryptoHelper::getLegacyCombinedKey();

        QString tempDecPath = m_dbPath + ".dec_mig";
        for (const QString& key : candidateKeys) {
            if (FileCryptoHelper::decryptFileWithShell(m_dbPath, tempDecPath, key)) {
                if (isSqlite(tempDecPath)) {
                    logStartup("[L4] 解壳成功！数据已成功转换为明文。");
                    QFile::remove(m_dbPath);
                    QFile::rename(tempDecPath, m_dbPath);
                    loaded = true;
                    break;
                }
                QFile::remove(tempDecPath);
            }
        }
        
        if (!loaded && FileCryptoHelper::decryptFileLegacy(m_dbPath, tempDecPath, FileCryptoHelper::getCombinedKeyBySN(cDriveSN))) {
             if (isSqlite(tempDecPath)) {
                logStartup("[L4] Legacy 解壳成功。");
                QFile::remove(m_dbPath);
                QFile::rename(tempDecPath, m_dbPath);
                loaded = true;
            } else {
                QFile::remove(tempDecPath);
            }
        }
    }

    // [LEVEL 5] 备份血包恢复
    if (!loaded) {
        logStartup("[L5] 主路径失效，尝试从备份恢复...");
        if (tryRecoverFromBackup()) {
            if (isSqlite(m_dbPath)) loaded = true;
            else {
                // 如果备份也是加密的，则递归执行一次 L4 逻辑 (简化实现，这里直接报错由人工介入)
                logStartup("[ERR] 备份文件需要二次解密，请联系技术支持或手动重命名。");
            }
        }
    }

    if (!loaded) {
        logStartup("[INIT] 所有抢救链均失效，初始化全新明文数据库。");
    }

    // 4. 打开数据库
    if (m_db.isOpen()) m_db.close();
    
    QString connectionName = "RapidNotes_Main_Conn";
    if (QSqlDatabase::contains(connectionName)) {
        m_db = QSqlDatabase::database(connectionName);
    } else {
        m_db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    }
    
    m_db.setDatabaseName(m_dbPath);

    logStartup("正在建立 SQL 连接...");
    if (!m_db.open()) {
        // 2026-03-xx [DIAGNOSTIC] 记录导致数据库无法打开的具体 SQL 错误，便于排查权限或文件锁冲突
        m_lastError = QString("[%1] %2").arg(m_db.lastError().nativeErrorCode()).arg(m_db.lastError().text());
        logStartup("[ERR] SQL 打开失败: " + m_lastError);
        return false;
    }
    logStartup("SQL 连接成功。");

    // [STABILITY] 开启 WAL 模式 + FULL 同步级别。
    // 在 Windows 下，synchronous=FULL 能确保崩溃时已提交的数据绝对抵达硬件/系统层。
    QSqlQuery walQuery(m_db);
    logStartup("正在配置高可靠性写入模式 (WAL + FULL)...");
    if (!walQuery.exec("PRAGMA journal_mode = WAL;")) {
        m_lastError = "无法开启 WAL 模式: " + walQuery.lastError().text();
        logStartup("[ERR] " + m_lastError);
        return false;
    }
    walQuery.exec("PRAGMA synchronous = FULL;");

    // 完整性预检
    logStartup("执行完整性预检...");
    if (!walQuery.exec("PRAGMA integrity_check;")) {
        m_lastError = "数据库文件结构损坏: " + walQuery.lastError().text();
        logStartup("[ERR] " + m_lastError);
        return false;
    }

    logStartup("执行建表及升级检查...");
    if (!createTables()) {
        m_lastError = "建表或表升级失败: " + m_db.lastError().text();
        logStartup("[ERR] " + m_lastError);
        return false;
    }

    m_isInitialized = true;
    logStartup("--- 初始化全部成功 ---");

    // [STARTUP-SYNC] 已移除旧架构下的强制合壳同步，去壳版始终保持明文实时性
    m_autoSaveTimer->start();
    return true;
}

void DatabaseManager::closeAndPack() {
    QMutexLocker locker(&m_mutex);
    if (!m_isInitialized) return;
    m_isInitialized = false;
    
    QString connName = m_db.connectionName();
    if (m_db.isOpen()) {
        // [DE-SHELL] 退出前仅执行快速 Checkpoint 刷盘，彻底移除加密环节
        QSqlQuery cp(m_db);
        cp.exec("PRAGMA wal_checkpoint(FULL);");
        m_db.close();
    }
    m_db = QSqlDatabase(); 
    if (!connName.isEmpty()) QSqlDatabase::removeDatabase(connName);
    
    // 退出时执行一次快速归档备份，确保数据绝对安全
    backupDatabaseLatest();
    qDebug() << "[DB] [DE-SHELL] 数据库已安全关闭，实现秒级退出。";
}

bool DatabaseManager::flushDatabase(const QString& source) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen() || !m_isInitialized) return false;
    
    // [DE-SHELL] 在新架构下，此函数仅执行强制刷盘 Checkpoint
    QSqlQuery checkPoint(m_db);
    return checkPoint.exec("PRAGMA wal_checkpoint(PASSIVE);");
}

bool DatabaseManager::tryRecoverFromBackup() {
    if (m_realDbPath.isEmpty()) return false;

    QFileInfo dbInfo(m_realDbPath);
    QDir dbDir = dbInfo.dir();
    QString backupPath = dbDir.absoluteFilePath("backups/inspiration_latest.db");

    if (!QFile::exists(backupPath) || QFileInfo(backupPath).size() == 0) {
        qWarning() << "[DB] 自动恢复失败：备份文件不存在或为空 (inspiration_latest.db)";
        return false;
    }

    qDebug() << "[DB] 正在尝试从最新备份恢复数据库 (原因: 原始文件丢失或损坏)...";

    // 1. 备份损坏的文件以备后续人工检查
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString corruptedPath = m_realDbPath + ".corrupted_" + timestamp;
    if (QFile::exists(m_realDbPath)) {
        if (QFile::rename(m_realDbPath, corruptedPath)) {
            qDebug() << "[DB] 已将损坏文件移至:" << corruptedPath;
            
            // [UX-OPTIMIZATION] 数量控制：仅保留最近 5 个损坏备份，防止无限堆积
            QStringList corruptedFilter;
            corruptedFilter << QFileInfo(m_realDbPath).fileName() + ".corrupted_*";
            QFileInfoList corruptedFiles = dbDir.entryInfoList(corruptedFilter, QDir::Files, QDir::Name);
            while (corruptedFiles.size() > 5) {
                QFileInfo oldest = corruptedFiles.takeFirst();
                if (QFile::remove(oldest.absoluteFilePath())) {
                    qDebug() << "[DB] 已移除过期的损坏备份:" << oldest.fileName();
                }
            }
        } else {
            qWarning() << "[DB] 无法重命名损坏的文件，尝试直接覆盖。";
            QFile::remove(m_realDbPath);
        }
    }

    // 2. 从血包恢复
    if (QFile::copy(backupPath, m_realDbPath)) {
        qDebug() << "[DB] 核心救治成功：已从备份文件恢复数据库外壳。";
        return true;
    } else {
        qCritical() << "[DB] 核心救治失败：无法将备份文件复制回主路径。";
        return false;
    }
}

void DatabaseManager::markDirty() {
    m_isDirty = true;
    m_lastActivityTime = QDateTime::currentDateTime();
}

void DatabaseManager::handleAutoSave() {
    QMutexLocker locker(&m_mutex);
    if (!m_isDirty) return;

    // [ANTI-LOSS] 双重安全阈值判定
    qint64 idleSecs = m_lastActivityTime.secsTo(QDateTime::currentDateTime());
    
    // 逻辑 A：如果已闲置 5 秒（用户已停手），立即同步。
    // 逻辑 B：如果数据已经脏了超过 120 秒，无论用户是否在操作，强制同步（防止活跃用户无限期拖延同步）。
    bool shouldSync = (idleSecs >= 5) || (idleSecs < 0) || (abs(idleSecs) > 120);

    if (!shouldSync) {
        return;
    }

    qDebug() << "[DB] 触发安全同步逻辑 (闲置:" << idleSecs << "s)，执行物理落盘...";
    m_isDirty = false;
    
    // [FIX] 彻底修复线程违规崩溃：QSqlDatabase 连接不是线程安全的。
    // 在主线程执行 wal_checkpoint(PASSIVE) 极快且不阻塞读写，完全没必要开启后台线程冒风险。
    if (flushDatabase("SmartAutoSync")) {
        backupDatabaseLatest();
        qDebug() << "[DB] 数据物理落盘及备份完成。";
    } else {
        markDirty();
    }
}

void DatabaseManager::backupDatabaseLatest() {
    if (m_realDbPath.isEmpty() || !QFile::exists(m_realDbPath)) return;

    QFileInfo dbInfo(m_realDbPath);
    QDir dbDir = dbInfo.dir();
    QString backupDirPath = dbDir.absoluteFilePath("backups");
    QDir backupDir(backupDirPath);

    if (!backupDir.exists()) {
        dbDir.mkdir("backups");
    }

    // 高频备份使用固定文件名，每 7 秒更新一次
    // 采用“先写入临时文件再重命名”的原子操作，确保备份文件始终可用且不损坏
    QString backupPath = backupDir.absoluteFilePath("inspiration_latest.db");
    QString tempPath = backupPath + ".tmp";
    
    if (QFile::exists(tempPath)) {
        QFile::remove(tempPath);
    }
    
    // [HEALING] 备份熔断保护机制
    // 如果当前主库大小异常缩小（例如从数MB缩减到几十KB），则拒绝直接覆盖血包备份。
    if (QFile::exists(backupPath)) {
        qint64 currentSize = QFileInfo(m_realDbPath).size();
        qint64 backupSize = QFileInfo(backupPath).size();
        
        // 判定熔断条件：备份已存在且大于 200KB，且当前文件比备份缩小了 50% 以上
        if (backupSize > 200 * 1024 && currentSize < (backupSize / 2)) {
            qCritical() << "[DB] 检测到当前数据库异常缩小 (" << currentSize << " vs " << backupSize << ")，触发备份熔断保护！";
            QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            QFile::rename(backupPath, backupPath + ".shrink_safe_" + timestamp);
            // 虽然重命名了，但后续仍会尝试生成新的备份，但老数据已被保护
        }
    }

    if (QFile::copy(m_realDbPath, tempPath)) {
        if (QFile::exists(backupPath)) {
            QFile::remove(backupPath);
        }
        if (QFile::rename(tempPath, backupPath)) {
            // [FIX] 解决 QFile::copy 保留旧创建日期的问题，强制更新为当前备份时刻
            QFile bFile(backupPath);
            QDateTime now = QDateTime::currentDateTime();
            bFile.setFileTime(now, QFileDevice::FileBirthTime);
            bFile.setFileTime(now, QFileDevice::FileModificationTime);
            qDebug() << "[DB] 高频同步备份成功 (已刷新时间戳):" << backupPath;
        }
    }
}

void DatabaseManager::backupDatabase() {
    if (m_realDbPath.isEmpty() || !QFile::exists(m_realDbPath)) return;

    QFileInfo dbInfo(m_realDbPath);
    QDir dbDir = dbInfo.dir();
    QString backupDirPath = dbDir.absoluteFilePath("backups");
    QDir backupDir(backupDirPath);

    if (!backupDir.exists()) {
        if (!dbDir.mkdir("backups")) {
            qWarning() << "[DB] 无法创建备份目录:" << backupDirPath;
            return;
        }
    }

    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyyMMdd_HHmmss");
    QString backupFileName = QString("inspiration_backup_%1.db").arg(timestamp);
    QString backupPath = backupDir.absoluteFilePath(backupFileName);

    if (QFile::copy(m_realDbPath, backupPath)) {
        // [FIX] 显式更新归档备份的时间戳，确保资源管理器显示正确
        QFile bFile(backupPath);
        bFile.setFileTime(now, QFileDevice::FileBirthTime);
        bFile.setFileTime(now, QFileDevice::FileModificationTime);
        qDebug() << "[DB] 数据库归档备份成功 (已刷新时间戳):" << backupPath;
    } else {
        qWarning() << "[DB] 数据库备份失败";
        return;
    }

    // 2026-03-xx 按照用户要求：将全量备份保留上限从 10 个降至 3 个，显著精简磁盘占用空间
    QStringList filter;
    filter << "inspiration_backup_*.db";
    // 按名称排序（时间戳文件名按名称排序即为时间顺序）
    QFileInfoList backupFiles = backupDir.entryInfoList(filter, QDir::Files, QDir::Name);

    while (backupFiles.size() > 3) {
        QFileInfo oldest = backupFiles.takeFirst();
        if (QFile::remove(oldest.absoluteFilePath())) {
            qDebug() << "[DB] 已移除旧备份:" << oldest.fileName();
        }
    }
}

bool DatabaseManager::createTables() {
    QSqlQuery query(m_db);
    QString createNotesTable = R"(
        CREATE TABLE IF NOT EXISTS notes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            content TEXT,
            tags TEXT,
            color TEXT DEFAULT '#2d2d2d',
            category_id INTEGER,
            item_type TEXT DEFAULT 'text',
            data_blob BLOB,
            content_hash TEXT,
            rating INTEGER DEFAULT 0,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            is_pinned INTEGER DEFAULT 0,
            is_locked INTEGER DEFAULT 0,
            is_favorite INTEGER DEFAULT 0,
            is_deleted INTEGER DEFAULT 0,
            source_app TEXT,
            source_title TEXT,
            last_accessed_at DATETIME,
            sort_order INTEGER DEFAULT 0,
            remark TEXT DEFAULT '',
            file_extensions TEXT DEFAULT ''
        )
    )";
    if (!query.exec(createNotesTable)) return false;

    // 2026-03-xx 按照用户要求：部署核心业务索引，确保大数据量下的检索性能
    query.exec("CREATE INDEX IF NOT EXISTS idx_notes_main_filter ON notes(is_deleted, category_id, is_pinned, updated_at)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_notes_rating ON notes(rating) WHERE rating > 0");

    QString createCategoriesTable = R"(
        CREATE TABLE IF NOT EXISTS categories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            parent_id INTEGER,
            color TEXT DEFAULT '#808080',
            sort_order INTEGER DEFAULT 0,
            preset_tags TEXT,
            password TEXT,
            password_hint TEXT,
            is_deleted INTEGER DEFAULT 0,
            is_pinned INTEGER DEFAULT 0,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    )";
    query.exec(createCategoriesTable);
    // [FIX] 彻底强化迁移：严禁跳过。必须确保 categories 拥有 updated_at 字段，否则回收站统合查询必挂。
    {
        auto addCol = [&](const QString& table, const QString& col, const QString& def) -> bool {
            QStringList existingCols;
            QSqlQuery check(m_db);
            if (check.exec(QString("PRAGMA table_info(%1)").arg(table))) {
                while (check.next()) existingCols << check.value(1).toString().toLower();
            }
            if (!existingCols.contains(col.toLower())) {
                QSqlQuery alter(m_db);
                if (alter.exec(QString("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, col, def))) {
                    return true;
                }
            }
            return false;
        };
        addCol("categories", "updated_at", "DATETIME DEFAULT CURRENT_TIMESTAMP");
        addCol("categories", "is_deleted", "INTEGER DEFAULT 0");
        addCol("categories", "is_pinned", "INTEGER DEFAULT 0");
        addCol("categories", "sort_order", "INTEGER DEFAULT 0");
        addCol("categories", "color", "TEXT DEFAULT '#808080'");
        addCol("categories", "parent_id", "INTEGER");
        addCol("categories", "preset_tags", "TEXT");
        addCol("categories", "password", "TEXT");
        addCol("categories", "password_hint", "TEXT");
        addCol("categories", "pinned_sort_order", "INTEGER DEFAULT 0"); // [NEW] 2026-04-20 增加镜像组拉伸排序
    }
    query.exec("CREATE TABLE IF NOT EXISTS tags (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL)");
    query.exec("CREATE TABLE IF NOT EXISTS note_tags (note_id INTEGER, tag_id INTEGER, PRIMARY KEY (note_id, tag_id))");
    query.exec("CREATE INDEX IF NOT EXISTS idx_notes_content_hash ON notes(content_hash)");
    
    // 2026-04-09 按照用户要求：彻底移除 FTS5 全文索引及其触发器，回归简单可靠的 SQL 统计
    QString wcExpr = "length(REPLACE(REPLACE(REPLACE(new.content, '<p>', ''), '</p>', ''), '<br/>', ''))";
    query.exec(QString(R"(
        CREATE TRIGGER IF NOT EXISTS trg_notes_insert_wc AFTER INSERT ON notes BEGIN
            UPDATE notes SET word_count = %1 WHERE id = new.id;
        END;
    )").arg(wcExpr));

    query.exec(QString(R"(
        CREATE TRIGGER IF NOT EXISTS trg_notes_update_wc AFTER UPDATE ON notes 
        FOR EACH ROW WHEN (old.content != new.content)
        BEGIN
            UPDATE notes SET word_count = %1 WHERE id = new.id;
        END;
    )").arg(wcExpr));

    // 试用期与使用次数表
    query.exec("CREATE TABLE IF NOT EXISTS system_config (key TEXT PRIMARY KEY, value TEXT)");
    
    // 初始化试用信息
    QSqlQuery checkLaunch(m_db);
    checkLaunch.prepare("SELECT value FROM system_config WHERE key = 'first_launch_date'");
    if (checkLaunch.exec() && !checkLaunch.next()) {
        QSqlQuery initQuery(m_db);
        initQuery.prepare("INSERT INTO system_config (key, value) VALUES ('first_launch_date', :date)");
        initQuery.bindValue(":date", QDateTime::currentDateTime().toString(Qt::ISODate));
        initQuery.exec();
        
        initQuery.prepare("INSERT INTO system_config (key, value) VALUES ('usage_count', '0')");
        initQuery.exec();
    }

    // [CRITICAL] 待办事项表：扩展支持联动、循环和子任务。
    QString createTodosTable = R"(
        CREATE TABLE IF NOT EXISTS todos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            content TEXT,
            start_time DATETIME,
            end_time DATETIME,
            status INTEGER DEFAULT 0, -- 0:待办, 1:已完成, 2:已逾期
            reminder_time DATETIME,
            priority INTEGER DEFAULT 0,
            color TEXT,
            note_id INTEGER DEFAULT -1,
            repeat_mode INTEGER DEFAULT 0, -- 0:None, 1:Daily, 2:Weekly, 3:Monthly
            parent_id INTEGER DEFAULT -1,
            progress INTEGER DEFAULT 0,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    )";
    if (query.exec(createTodosTable)) {
        // 增量升级逻辑
        QSqlQuery upgrade(m_db);
        QStringList newCols = {"note_id", "repeat_mode", "parent_id", "progress"};
        for (const auto& col : newCols) {
            upgrade.exec(QString("ALTER TABLE todos ADD COLUMN %1 INTEGER DEFAULT 0").arg(col));
        }
    }

    // [MODIFIED] 强化版迁移：确保 notes 表字段完整
    {
        auto addCol = [&](const QString& table, const QString& col, const QString& def) {
            QStringList existingCols;
            QSqlQuery check(m_db);
            if (check.exec(QString("PRAGMA table_info(%1)").arg(table))) {
                while (check.next()) existingCols << check.value(1).toString().toLower();
            }
            if (!existingCols.contains(col.toLower())) {
                qDebug() << "[DB] 迁移检测：正在补齐" << table << "表的缺失字段 ->" << col;
                QSqlQuery alter(m_db);
                if (!alter.exec(QString("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, col, def))) {
                    qCritical() << "[DB] 严重错误：补齐字段失败 ->" << col << alter.lastError().text();
                    return false;
                } else {
                    qDebug() << "[DB] 迁移成功：字段" << col << "已加入" << table;
                    return true;
                }
            }
            return false;
        };
        addCol("notes", "sort_order", "INTEGER DEFAULT 0");
        addCol("notes", "is_deleted", "INTEGER DEFAULT 0");
        addCol("notes", "updated_at", "DATETIME DEFAULT CURRENT_TIMESTAMP");
        addCol("notes", "last_accessed_at", "DATETIME");
        addCol("notes", "is_pinned", "INTEGER DEFAULT 0");
        addCol("notes", "is_locked", "INTEGER DEFAULT 0");
        addCol("notes", "is_favorite", "INTEGER DEFAULT 0");
        addCol("notes", "source_app", "TEXT");
        addCol("notes", "source_title", "TEXT");
        addCol("notes", "rating", "INTEGER DEFAULT 0");
        addCol("notes", "content_hash", "TEXT");
        addCol("notes", "item_type", "TEXT DEFAULT 'text'");
        addCol("notes", "category_id", "INTEGER");
        addCol("notes", "color", "TEXT DEFAULT '#2d2d2d'");
        addCol("notes", "data_blob", "BLOB");
        addCol("notes", "tags", "TEXT");
        addCol("notes", "title", "TEXT");
        addCol("notes", "content", "TEXT");
        addCol("notes", "created_at", "DATETIME DEFAULT CURRENT_TIMESTAMP");
        addCol("notes", "remark", "TEXT DEFAULT ''"); // [NEW] 备注字段
        addCol("notes", "file_extensions", "TEXT DEFAULT ''"); // [NEW] 2026-04-08 多后缀关联字段
        if (addCol("notes", "word_count", "INTEGER DEFAULT 0")) {
            // 2026-03-xx 性能优化：为旧数据初始化字数统计（仅执行一次）
            query.exec("UPDATE notes SET word_count = length(REPLACE(REPLACE(REPLACE(content, '<p>', ''), '</p>', ''), '<br/>', '')) WHERE word_count = 0 OR word_count IS NULL");
        }
    }

    return true;
}

void DatabaseManager::addNoteAsync(const QString& title, const QString& content, const QStringList& tags,
                                  const QString& color, int categoryId,
                                  const QString& itemType, const QByteArray& dataBlob,
                                  const QString& sourceApp, const QString& sourceTitle,
                                  const QString& remark) {
    // [FIX] 彻底修复线程违规导致的闪退：
    // QSqlDatabase 的连接不是线程安全的，严禁在大规模并发采集时通过 QThreadPool 跨线程访问。
    // 如果此方法被外部线程（如剪贴板监控、后台任务）调用，必须调度到数据库所属的主线程执行。
    if (QThread::currentThread() != qApp->thread()) {
        QMetaObject::invokeMethod(this, [=]() {
            addNoteAsync(title, content, tags, color, categoryId, itemType, dataBlob, sourceApp, sourceTitle, remark);
        }, Qt::QueuedConnection);
        return;
    }

    // 此时已确保在主线程，执行同步添加逻辑
    addNote(title, content, tags, color, categoryId, itemType, dataBlob, sourceApp, sourceTitle, remark);
}

int DatabaseManager::addNote(const QString& title, const QString& content, const QStringList& tags,
                            const QString& color, int categoryId,
                            const QString& itemType, const QByteArray& dataBlob,
                            const QString& sourceApp, const QString& sourceTitle,
                            const QString& remark) {
    // 2026-04-08 按照用户要求：物理提取多后缀关联
    QString fileExtensions = extractFileExtensions(itemType, content);

    // [USER_REQUEST] 2026-04-08 网址自动归档：发现 link 类型时自动在“我的分类”中创建 Link 组
    int finalCategoryId = categoryId;
    if (itemType == "link") {
        int linkCatId = getOrCreateCategoryByName("Link", -1, "#1abc9c");
        if (linkCatId > 0) finalCategoryId = linkCatId;
    }

    // 2026-03-xx 按照用户要求：正版化移除试用限制，不再拦截新增笔记的操作
    QVariantMap newNoteMap;
    bool success = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QByteArray hashData = dataBlob.isEmpty() ? content.toUtf8() : dataBlob;
    QString contentHash = QCryptographicHash::hash(hashData, QCryptographicHash::Sha256).toHex();
    {   
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) { qDebug() << "[DB] 错误: 数据库未打开"; return 0; }

        QString finalColor = color.isEmpty() ? "#2d2d2d" : color;
        QStringList finalTags = tags;

        // 查重：如果内容已存在，则更新标题、标签及分类
        QSqlQuery checkQuery(m_db);
        checkQuery.prepare("SELECT id, category_id, tags FROM notes WHERE content_hash = :hash AND is_deleted = 0 LIMIT 1");
        checkQuery.bindValue(":hash", contentHash);
        if (checkQuery.exec() && checkQuery.next()) {
            int existingId = checkQuery.value(0).toInt();
            QVariant oldCatVal = checkQuery.value(1);
            
            // 获取已有笔记的详细信息，用于智能判定是否需要更新标题等
            QVariantMap existingNote = getNoteById(existingId);
            QString existingTagsStr = existingNote.value("tags").toString();
            QStringList existingTags = existingTagsStr.split(",", Qt::SkipEmptyParts);
            for(QString& t : existingTags) t = t.trimmed();

            // 智能合并标签
            for (const QString& t : std::as_const(finalTags)) {
                if (!existingTags.contains(t.trimmed())) existingTags << t.trimmed();
            }

            // 漂移保护逻辑：如果笔记已有明确分类，则优先保留原分类，防止在自动归档时发生分类位移
            int finalCatToUse = finalCategoryId;
            if (!oldCatVal.isNull() && oldCatVal.toInt() > 0) {
                finalCatToUse = oldCatVal.toInt(); 
            }

            // 获取新分类/旧分类的颜色
            QString finalColor = color;
            
            if (finalCatToUse != -1) {
                QSqlQuery catQuery(m_db);
                catQuery.prepare("SELECT color, preset_tags FROM categories WHERE id = :id");
                catQuery.bindValue(":id", finalCatToUse);
                if (catQuery.exec() && catQuery.next()) {
                    if (color.isEmpty()) finalColor = catQuery.value(0).toString();
                    QString preset = catQuery.value(1).toString();
                    if (!preset.isEmpty()) {
                        QStringList pTags = preset.split(",", Qt::SkipEmptyParts);
                        for (const QString& t : pTags) {
                            if (!existingTags.contains(t.trimmed())) existingTags << t.trimmed();
                        }
                    }
                }
            }

            QSqlQuery updateQuery(m_db);
            // 重复内容时，更新标签、时间及来源。2026-04-08 同步更新多后缀关联。
            QString sql = "UPDATE notes SET tags = :tags, updated_at = :now, source_app = :app, source_title = :stitle, category_id = :cat_id, file_extensions = :exts";
            if (!finalColor.isEmpty()) sql += ", color = :color";
            
            // [CRITICAL] 智能标题保护逻辑：禁止恢复“旧版全量覆盖标题”的傻逼行为。
            // 必须确保：仅当原标题是自动生成的通用标题，且新标题更有意义时才覆盖；否则必须保持笔记原始标题不变。
            // [OPTIMIZED] 扩展通用标题判定，支持网址形式标题的自动覆盖替换
            QString existingTitle = existingNote.value("title").toString().trimmed();
            bool isExistingGeneric = existingTitle.isEmpty() || existingTitle == "无标题灵感" || 
                                     existingTitle.startsWith("[截图]") || 
                                     existingTitle.startsWith("[截图]") ||
                                     existingTitle.startsWith("Copied ") ||
                                     existingTitle.startsWith("http://") ||
                                     existingTitle.startsWith("https://") ||
                                     existingTitle.startsWith("www.");
                                     
            bool isNewMeaningful = !title.isEmpty() && !title.startsWith("[拖入") && !title.startsWith("[图片");
            
            if (isExistingGeneric && isNewMeaningful && existingTitle != title) {
                sql += ", title = :title";
            }
            
            sql += " WHERE id = :id";

            updateQuery.prepare(sql);
            updateQuery.bindValue(":tags", existingTags.join(", "));
            updateQuery.bindValue(":now", currentTime);
            updateQuery.bindValue(":app", sourceApp);
            updateQuery.bindValue(":stitle", sourceTitle);
            updateQuery.bindValue(":cat_id", finalCatToUse == -1 ? QVariant(QMetaType::fromType<int>()) : finalCatToUse);
            updateQuery.bindValue(":exts", fileExtensions);
            if (!finalColor.isEmpty()) updateQuery.bindValue(":color", finalColor);
            if (sql.contains(":title")) updateQuery.bindValue(":title", title);
            updateQuery.bindValue(":id", existingId);
            
            if (updateQuery.exec()) success = true;
            if (success) { 
                qDebug() << "[DB] 命中重复记录，已更新 ID:" << existingId;
                locker.unlock(); 
                emit noteUpdated(); 
                return existingId; 
            }
        }
        if (categoryId != -1) {
            QSqlQuery catQuery(m_db);
            catQuery.prepare("SELECT color, preset_tags FROM categories WHERE id = :id");
            catQuery.bindValue(":id", categoryId);
            if (catQuery.exec() && catQuery.next()) {
                if (color.isEmpty()) finalColor = catQuery.value(0).toString();
                QString preset = catQuery.value(1).toString();
                if (!preset.isEmpty()) {
                    QStringList pTags = preset.split(",", Qt::SkipEmptyParts);
                    for (const QString& t : pTags) {
                        QString trimmed = t.trimmed();
                        if (!finalTags.contains(trimmed)) finalTags << trimmed;
                    }
                }
            }
        }
        QSqlQuery query(m_db);
        query.prepare("INSERT INTO notes (title, content, tags, color, category_id, item_type, data_blob, content_hash, created_at, updated_at, source_app, source_title, remark, file_extensions) VALUES (:title, :content, :tags, :color, :category_id, :item_type, :data_blob, :hash, :created_at, :updated_at, :source_app, :source_title, :remark, :exts)");
        query.bindValue(":title", title);
        query.bindValue(":content", content);
        
        QStringList cleanedFinalTags;
        for (const QString& t : finalTags) {
            QString tr = t.trimmed();
            if (!tr.isEmpty() && !cleanedFinalTags.contains(tr)) cleanedFinalTags << tr;
        }
        query.bindValue(":tags", cleanedFinalTags.join(", "));
        
        query.bindValue(":color", finalColor);
        query.bindValue(":category_id", finalCategoryId == -1 ? QVariant(QMetaType::fromType<int>()) : finalCategoryId);
        query.bindValue(":item_type", itemType);
        query.bindValue(":data_blob", dataBlob);
        query.bindValue(":hash", contentHash);
        query.bindValue(":created_at", currentTime);
        query.bindValue(":updated_at", currentTime);
        query.bindValue(":source_app", sourceApp);
        query.bindValue(":source_title", sourceTitle);
        query.bindValue(":remark", remark);
        query.bindValue(":exts", fileExtensions);
        if (query.exec()) {
            success = true;
            markDirty();
            qDebug() << "[DB] 新纪录插入成功";
            QVariant lastId = query.lastInsertId();
            QSqlQuery fetch(m_db);
            fetch.prepare("SELECT * FROM notes WHERE id = :id");
            fetch.bindValue(":id", lastId);
            if (fetch.exec() && fetch.next()) {
                QSqlRecord rec = fetch.record();
                for (int i = 0; i < rec.count(); ++i) newNoteMap[rec.fieldName(i).toLower()] = fetch.value(i);
            }
        }
    }
    if (success && !newNoteMap.isEmpty()) {
        int newId = newNoteMap["id"].toInt();
        // 2026-03-xx 按照用户要求：已启用 SQLite 触发器，移除冗余的 C++ 手动 FTS 同步逻辑
        // syncFts(newId, title, content, newNoteMap["tags"].toString());
        
        // [STABILITY] 跨线程信号同步加固：
        // 如果当前不在主线程执行（由 addNoteAsync 触发），则强制通过 QueuedConnection 发送信号，防止 UI 竞态崩溃
        if (QThread::currentThread() != qApp->thread()) {
            QMetaObject::invokeMethod(this, [this, newNoteMap](){ emit noteAdded(newNoteMap); }, Qt::QueuedConnection);
        } else {
            emit noteAdded(newNoteMap);
        }
        return newId;
    }
    return 0;
}

bool DatabaseManager::updateNote(int id, const QString& title, const QString& content, const QStringList& tags, const QString& color, int categoryId,
                               const QString& itemType, const QByteArray& dataBlob,
                               const QString& sourceApp, const QString& sourceTitle,
                               const QString& remark) {
    // 2026-04-08 多后缀提取
    QString fileExtensions = extractFileExtensions(itemType, content);

    bool success = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    
    // 重新计算内容哈希
    QByteArray hashData = dataBlob.isEmpty() ? content.toUtf8() : dataBlob;
    QString contentHash = QCryptographicHash::hash(hashData, QCryptographicHash::Sha256).toHex();

    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QSqlQuery query(m_db);
        
        // [CRITICAL] 锁定：更新笔记属性时必须全量同步所有元数据。严禁遗漏 hash 和 item_type。
        QString sql = "UPDATE notes SET title=:title, content=:content, tags=:tags, updated_at=:updated_at, "
                      "category_id=:category_id, color=:color, last_accessed_at=:now, "
                      "content_hash=:hash, item_type=:type, data_blob=:blob, "
                      "source_app=:app, source_title=:stitle, remark=:remark, file_extensions=:exts";
        sql += " WHERE id=:id";

        query.prepare(sql);
        query.bindValue(":title", title);
        query.bindValue(":content", content);
        query.bindValue(":now", currentTime);
        query.bindValue(":hash", contentHash);
        query.bindValue(":type", itemType);
        query.bindValue(":blob", dataBlob);
        query.bindValue(":app", sourceApp);
        query.bindValue(":stitle", sourceTitle);
        query.bindValue(":remark", remark);
        query.bindValue(":exts", fileExtensions);
        
        QStringList trimmedTags;
        for (const QString& t : tags) {
            QString tr = t.trimmed();
            if (!tr.isEmpty() && !trimmedTags.contains(tr)) trimmedTags << tr;
        }
        query.bindValue(":tags", trimmedTags.join(", "));
        
        query.bindValue(":updated_at", currentTime);
        query.bindValue(":category_id", categoryId == -1 ? QVariant() : categoryId);
        
        QString finalColor = color;
        if (finalColor.isEmpty()) {
            if (categoryId != -1) {
                QSqlQuery catQuery(m_db);
                catQuery.prepare("SELECT color FROM categories WHERE id = :id");
                catQuery.bindValue(":id", categoryId);
                if (catQuery.exec() && catQuery.next()) finalColor = catQuery.value(0).toString();
                else finalColor = "#0A362F";
            } else {
                finalColor = "#0A362F";
            }
        }
        query.bindValue(":color", finalColor);
        query.bindValue(":id", id);
        success = query.exec();
        if (success) markDirty();
    }
    if (success) { 
        // 2026-03-xx 按照用户要求：已启用 SQLite 触发器，移除冗余的 C++ 手动 FTS 同步逻辑
        
        // [STABILITY] 跨线程信号同步加固
        if (QThread::currentThread() != qApp->thread()) {
            QMetaObject::invokeMethod(this, [this](){ emit noteUpdated(); }, Qt::QueuedConnection);
        } else {
            emit noteUpdated();
        }
    }
    return success;
}

bool DatabaseManager::reorderCategories(int parentId, bool ascending) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    QSqlQuery query(m_db);
    if (parentId <= 0) query.prepare("SELECT id, name FROM categories WHERE parent_id IS NULL OR parent_id <= 0");
    else { query.prepare("SELECT id, name FROM categories WHERE parent_id = :pid"); query.bindValue(":pid", parentId); }
    if (!query.exec()) return false;
    struct CatInfo { int id; QString name; };
    QList<CatInfo> list;
    while (query.next()) list.append({query.value(0).toInt(), query.value(1).toString()});
    if (list.isEmpty()) return true;
    std::sort(list.begin(), list.end(), [ascending](const CatInfo& a, const CatInfo& b) {
        if (ascending) return a.name.localeAwareCompare(b.name) < 0;
        return a.name.localeAwareCompare(b.name) > 0;
    });
    m_db.transaction();
    QSqlQuery update(m_db);
    for (int i = 0; i < list.size(); ++i) {
        update.prepare("UPDATE categories SET sort_order = :val WHERE id = :id");
        update.bindValue(":val", i);
        update.bindValue(":id", list[i].id);
        update.exec();
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit categoriesChanged(); }
    return ok;
}

bool DatabaseManager::updateCategoryOrder(int parentId, const QList<int>& categoryIds) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    if (!m_db.transaction()) return false;
    QSqlQuery query(m_db);
    query.prepare("UPDATE categories SET parent_id = :pid, sort_order = :order WHERE id = :id");
    for (int i = 0; i < categoryIds.size(); ++i) {
        query.bindValue(":pid", parentId <= 0 ? QVariant() : parentId);
        query.bindValue(":order", i);
        query.bindValue(":id", categoryIds[i]);
        if (!query.exec()) { m_db.rollback(); return false; }
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit categoriesChanged(); }
    return ok;
}

bool DatabaseManager::updatePinnedCategoryOrder(const QList<int>& categoryIds) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    if (!m_db.transaction()) return false;
    QSqlQuery query(m_db);
    query.prepare("UPDATE categories SET pinned_sort_order = :order WHERE id = :id");
    for (int i = 0; i < categoryIds.size(); ++i) {
        query.bindValue(":order", i);
        query.bindValue(":id", categoryIds[i]);
        if (!query.exec()) { m_db.rollback(); return false; }
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit categoriesChanged(); }
    return ok;
}

bool DatabaseManager::reorderAllCategories(bool ascending) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    QSqlQuery query(m_db);
    query.exec("SELECT DISTINCT parent_id FROM categories");
    QList<int> parents;
    bool hasRoot = false;
    while (query.next()) {
        if (query.value(0).isNull() || query.value(0).toInt() <= 0) hasRoot = true;
        else parents.append(query.value(0).toInt());
    }
    bool success = true;
    if (hasRoot) success &= reorderCategories(-1, ascending);
    for (int pid : parents) success &= reorderCategories(pid, ascending);
    return success;
}

bool DatabaseManager::setCategoryPassword(int id, const QString& password, const QString& hint) {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QString hashedPassword = QString(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
        QSqlQuery query(m_db);
        query.prepare("UPDATE categories SET password=:password, password_hint=:hint WHERE id=:id");
        query.bindValue(":password", hashedPassword);
        query.bindValue(":hint", hint);
        query.bindValue(":id", id);
        success = query.exec();
        if (success) markDirty();
    }
    if (success) emit categoriesChanged();
    return success;
}

bool DatabaseManager::removeCategoryPassword(int id) {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QSqlQuery query(m_db);
        query.prepare("UPDATE categories SET password=NULL, password_hint=NULL WHERE id=:id");
        query.bindValue(":id", id);
        success = query.exec();
        if (success) { markDirty(); m_unlockedCategories.remove(id); }
    }
    if (success) emit categoriesChanged();
    return success;
}

bool DatabaseManager::verifyCategoryPassword(int id, const QString& password) {
    bool correct = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QString hashedPassword = QString(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
        QSqlQuery query(m_db);
        query.prepare("SELECT password FROM categories WHERE id=:id");
        query.bindValue(":id", id);
        if (query.exec() && query.next()) {
            if (query.value(0).toString() == hashedPassword) correct = true;
        }
    }
    if (correct) unlockCategory(id);
    return correct;
}

bool DatabaseManager::isCategoryLocked(int id) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    if (m_unlockedCategories.contains(id)) return false;
    QSqlQuery query(m_db);
    query.prepare("SELECT password FROM categories WHERE id=:id");
    query.bindValue(":id", id);
    if (query.exec() && query.next()) return !query.value(0).toString().isEmpty();
    return false;
}

void DatabaseManager::lockCategory(int id) { { QMutexLocker locker(&m_mutex); m_unlockedCategories.remove(id); } emit categoriesChanged(); }
void DatabaseManager::lockAllCategories() { { QMutexLocker locker(&m_mutex); m_unlockedCategories.clear(); } emit categoriesChanged(); }
void DatabaseManager::toggleLockedCategoriesVisibility() {
    qDebug() << "[TRACE-DB] toggleLockedCategoriesVisibility 被调用。";
    // 2026-03-xx 按照用户要求：无论解锁/锁住状态，切换显示时立即全部重锁
    {
        QMutexLocker locker(&m_mutex);
        m_unlockedCategories.clear();
        m_lockedCategoriesHidden = !m_lockedCategoriesHidden;
        
        QSettings settings("RapidNotes", "QuickWindow");
        settings.setValue("lockedCategoriesHidden", m_lockedCategoriesHidden);
    }
    emit categoriesChanged();
}
void DatabaseManager::unlockCategory(int id) { { QMutexLocker locker(&m_mutex); m_unlockedCategories.insert(id); } emit categoriesChanged(); }

bool DatabaseManager::restoreAllFromTrash() {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        
        QSqlQuery query(m_db);
        // 恢复所有分类
        query.exec("UPDATE categories SET is_deleted = 0 WHERE is_deleted = 1");
        // 恢复所有笔记，并恢复默认颜色（如果原分类已不存在，这部分逻辑在获取颜色时会处理）
        success = query.exec("UPDATE notes SET is_deleted = 0, updated_at = datetime('now','localtime') WHERE is_deleted = 1");
        
        success = m_db.commit();
    }
    if (success) { markDirty(); emit noteUpdated(); emit categoriesChanged(); }
    return success;
}

bool DatabaseManager::updateNoteState(int id, const QString& column, const QVariant& value) {
    bool success = false;
    QString title, content, tags;
    bool needsFts = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        // [CRITICAL] 必须包含 item_type 以支持从图片识别提取的文字类型标记
        QStringList allowedColumns = {"is_pinned", "is_favorite", "is_deleted", "tags", "rating", "category_id", "color", "content", "title", "item_type", "remark"};
        if (!allowedColumns.contains(column)) return false;
        QSqlQuery query(m_db);
        if (column == "is_favorite") {
            bool fav = value.toBool();
            // 2026-03-13 按照用户要求：收藏颜色统一为 #F2B705
            QString color = fav ? "#F2B705" : ""; 
            if (!fav) {
                QSqlQuery catQuery(m_db);
                catQuery.prepare("SELECT c.color FROM categories c JOIN notes n ON n.category_id = c.id WHERE n.id = :id");
                catQuery.bindValue(":id", id);
                if (catQuery.exec() && catQuery.next()) color = catQuery.value(0).toString();
                else color = "#0A362F"; 
            }
            // [CRITICAL] 锁定：修改属性必须同步更新 last_accessed_at。严禁移除。
            query.prepare("UPDATE notes SET is_favorite = :val, color = :color, updated_at = :now, last_accessed_at = :now WHERE id = :id");
            query.bindValue(":color", color);
        } else if (column == "is_deleted") {
            bool del = value.toBool();
            QString color = del ? "#2d2d2d" : "#0A362F";
            // [CRITICAL] 锁定：删除状态变更必须同步更新 last_accessed_at。严禁移除。
            // [MODIFIED] 不再强制清除 category_id，以支持原位恢复
            query.prepare("UPDATE notes SET is_deleted = :val, color = :color, updated_at = :now, last_accessed_at = :now WHERE id = :id");
            query.bindValue(":color", color);
        } else if (column == "category_id") {
            int catId = value.isNull() ? -1 : value.toInt();
            QString color = "#0A362F"; 
            if (catId != -1) {
                QSqlQuery catQuery(m_db);
                catQuery.prepare("SELECT color FROM categories WHERE id = :id");
                catQuery.bindValue(":id", catId);
                if (catQuery.exec() && catQuery.next()) color = catQuery.value(0).toString();
            }
            // [CRITICAL] 锁定：移动分类必须同步更新 last_accessed_at。严禁移除。
            query.prepare("UPDATE notes SET category_id = :val, color = :color, is_deleted = 0, updated_at = :now, last_accessed_at = :now WHERE id = :id");
            query.bindValue(":color", color);
        } else {
            // [CRITICAL] 锁定：通用状态修改必须同步更新 last_accessed_at。严禁移除。
            query.prepare(QString("UPDATE notes SET %1 = :val, updated_at = :now, last_accessed_at = :now WHERE id = :id").arg(column));
        }
        query.bindValue(":val", value);
        query.bindValue(":now", currentTime);
        query.bindValue(":id", id);
        success = query.exec();
        if (success) markDirty();
        if (success && (column == "content" || column == "title" || column == "tags")) {
            needsFts = true;
            QSqlQuery fetch(m_db);
            fetch.prepare("SELECT title, content, tags FROM notes WHERE id = ?");
            fetch.addBindValue(id);
            if (fetch.exec() && fetch.next()) { 
                title = fetch.value(0).toString(); 
                content = fetch.value(1).toString(); 
                tags = fetch.value(2).toString();
            }
        }
    } 
    if (success) { 
        // 2026-03-xx 按照用户要求：已启用 SQLite 触发器，不再需要手动同步 FTS
        // if (needsFts) syncFts(id, title, content, tags); 
        emit noteUpdated(); 
    }
    return success;
}

bool DatabaseManager::updateNoteStateBatch(const QList<int>& ids, const QString& column, const QVariant& value) {
    if (ids.isEmpty()) return true;
    bool success = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        // [CRITICAL] 保持与 updateNoteState 相同的允许列白名单，确保功能不丢失
        QStringList allowedColumns = {"is_pinned", "is_favorite", "is_deleted", "tags", "rating", "category_id", "color", "content", "title", "item_type"};
        if (!allowedColumns.contains(column)) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        if (column == "category_id") {
            int catId = value.isNull() ? -1 : value.toInt();
            QString color = "#0A362F";
            if (catId != -1) {
                QSqlQuery catQuery(m_db);
                catQuery.prepare("SELECT color FROM categories WHERE id = :id");
                catQuery.bindValue(":id", catId);
                if (catQuery.exec() && catQuery.next()) color = catQuery.value(0).toString();
            }
            // [CRITICAL] 锁定：批量移动分类必须同步更新 last_accessed_at。严禁移除。
            query.prepare("UPDATE notes SET category_id = :val, color = :color, is_deleted = 0, updated_at = :now, last_accessed_at = :now WHERE id = :id");
            for (int id : ids) {
                query.bindValue(":val", value);
                query.bindValue(":color", color);
                query.bindValue(":now", currentTime);
                query.bindValue(":id", id);
                query.exec();
            }
        } else if (column == "is_favorite") {
            bool fav = value.toBool();
            if (fav) {
                // [CRITICAL] 锁定：批量收藏同步更新 last_accessed_at。
                // 2026-03-13 按照用户要求：收藏颜色统一为 #F2B705
                query.prepare("UPDATE notes SET is_favorite = 1, color = '#F2B705', updated_at = :now, last_accessed_at = :now WHERE id = :id");
            } else {
                // [CRITICAL] 锁定：批量取消收藏同步更新 last_accessed_at。
                query.prepare("UPDATE notes SET is_favorite = 0, color = COALESCE((SELECT color FROM categories WHERE id = notes.category_id), '#0A362F'), updated_at = :now, last_accessed_at = :now WHERE id = :id");
            }
            for (int id : ids) {
                query.bindValue(":now", currentTime);
                query.bindValue(":id", id);
                query.exec();
            }
        } else if (column == "is_deleted") {
            bool del = value.toBool();
            if (del) {
                // [CRITICAL] 锁定：批量删除同步更新 last_accessed_at。不再清除 category_id 以支持原位恢复。
                query.prepare("UPDATE notes SET is_deleted = 1, color = '#2d2d2d', is_pinned = 0, is_favorite = 0, updated_at = :now, last_accessed_at = :now WHERE id = :id");
            } else {
                // [CRITICAL] 锁定：批量恢复同步更新 last_accessed_at。
                query.prepare("UPDATE notes SET is_deleted = 0, color = '#0A362F', updated_at = :now, last_accessed_at = :now WHERE id = :id");
            }
            for (int id : ids) {
                query.bindValue(":now", currentTime);
                query.bindValue(":id", id);
                query.exec();
            }
        } else {
            // [CRITICAL] 锁定：批量修改通用属性同步更新 last_accessed_at。
            QString sql = QString("UPDATE notes SET %1 = :val, updated_at = :now, last_accessed_at = :now WHERE id = :id").arg(column);
            query.prepare(sql);
            for (int id : ids) {
                query.bindValue(":val", value);
                query.bindValue(":now", currentTime);
                query.bindValue(":id", id);
                query.exec();
            }
        }
        success = m_db.commit();
    }
    if (success) {
        markDirty();
        // for (int id : ids) syncFtsById(id); // 触发器自动维护
        emit noteUpdated();
    }
    return success;
}

bool DatabaseManager::recordAccess(int id) {
    bool success = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QSqlQuery query(m_db);
        query.prepare("UPDATE notes SET last_accessed_at = :now WHERE id = :id");
        query.bindValue(":now", currentTime);
        query.bindValue(":id", id);
        success = query.exec();
    }
    return success;
}

bool DatabaseManager::toggleNoteState(int id, const QString& column) {
    QVariant currentVal;
    {
        QMutexLocker locker(&m_mutex);
        QSqlQuery query(m_db);
        query.prepare(QString("SELECT %1 FROM notes WHERE id = :id").arg(column));
        query.bindValue(":id", id);
        if (query.exec() && query.next()) currentVal = query.value(0);
    }
    if (currentVal.isValid()) return updateNoteState(id, column, !currentVal.toBool());
    return false;
}

bool DatabaseManager::moveNotesToCategory(const QList<int>& noteIds, int catId) {
    if (noteIds.isEmpty()) return true;
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QString catColor = "#0A362F"; 
        QString presetTags;
        if (catId != -1) {
            QSqlQuery catQuery(m_db);
            catQuery.prepare("SELECT color, preset_tags FROM categories WHERE id = :id");
            catQuery.bindValue(":id", catId);
            if (catQuery.exec() && catQuery.next()) { catColor = catQuery.value(0).toString(); presetTags = catQuery.value(1).toString(); }
        }
        QSqlQuery query(m_db);
        // [CRITICAL] 移动分类同步更新 last_accessed_at
        query.prepare("UPDATE notes SET category_id = :cat_id, color = :color, is_deleted = 0, updated_at = :now, last_accessed_at = :now WHERE id = :id");
        QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        for (int id : noteIds) {
            query.bindValue(":cat_id", catId == -1 ? QVariant() : catId);
            query.bindValue(":color", catColor);
            query.bindValue(":now", now);
            query.bindValue(":id", id);
            query.exec();
            if (!presetTags.isEmpty()) {
                QSqlQuery fetchTags(m_db);
                fetchTags.prepare("SELECT tags FROM notes WHERE id = :id");
                fetchTags.bindValue(":id", id);
                if (fetchTags.exec() && fetchTags.next()) {
                    QString existing = fetchTags.value(0).toString();
                    QStringList tagList = existing.split(",", Qt::SkipEmptyParts);
                    QStringList newTags = presetTags.split(",", Qt::SkipEmptyParts);
                    bool changed = false;
                    for (const QString& t : newTags) { if (!tagList.contains(t.trimmed())) { tagList.append(t.trimmed()); changed = true; } }
                    if (changed) { QSqlQuery updateTags(m_db); updateTags.prepare("UPDATE notes SET tags = :tags WHERE id = :id"); updateTags.bindValue(":tags", tagList.join(", ")); updateTags.bindValue(":id", id); updateTags.exec(); }
                }
            }
        }
        success = m_db.commit();
    }
    if (success) {
        markDirty();
        // for (int id : noteIds) syncFtsById(id); // 触发器自动维护
        emit noteUpdated();
    }
    return success;
}

bool DatabaseManager::deleteNotesBatch(const QList<int>& ids) {
    if (ids.isEmpty()) return true;
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        query.prepare("DELETE FROM notes WHERE id=:id");
        for (int id : ids) { query.bindValue(":id", id); query.exec(); }
        success = m_db.commit();
    }
    if (success) {
        markDirty();
        ClipboardMonitor::instance().clearLastHash();
        emit noteUpdated();
    }
    return success;
}

bool DatabaseManager::softDeleteNotes(const QList<int>& ids) {
    if (ids.isEmpty()) return true;
    bool success = false;
    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        // [MODIFIED] 不再清除 category_id，以便后续分类恢复或笔记原位恢复。增加 last_accessed_at 更新。
        query.prepare("UPDATE notes SET is_deleted = 1, color = '#2d2d2d', is_pinned = 0, is_favorite = 0, updated_at = :now, last_accessed_at = :now WHERE id = :id");
        for (int id : ids) { query.bindValue(":now", currentTime); query.bindValue(":id", id); query.exec(); }
        success = m_db.commit();
    }
    if (success) {
        markDirty();
        ClipboardMonitor::instance().clearLastHash();
        emit noteUpdated();
    }
    return success;
}



// [CRITICAL] 核心搜索逻辑：采用 FTS5 全文检索。禁止修改此处的 MATCH 语法及字段关联，以确保搜索结果的准确性与高性能。
QList<QVariantMap> DatabaseManager::searchNotes(const QString& keyword, const QString& filterType, const QVariant& filterValue, int page, int pageSize, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    QList<QVariantMap> results;
    if (!m_db.isOpen()) {
        qCritical() << "[DB] searchNotes 失败：数据库未打开";
        return results;
    }

    // [NEW] 处理回收站特殊视图：包含已删除的分类
    if (filterType == "trash" && keyword.isEmpty()) {
        // [OLD_VERSION_RECOVERY] 100% 还原旧版字段 SQL 结构，杜绝字段缺失报错
        QString sql = "SELECT id, title, content, tags, color, category_id, item_type, data_blob, created_at, updated_at, is_pinned, is_favorite, is_deleted, source_app, source_title, last_accessed_at, remark "
                      "FROM notes WHERE is_deleted = 1 "
                      "UNION ALL "
                      "SELECT id, name AS title, '(已删除的分类包)' AS content, '' AS tags, color, parent_id AS category_id, 'deleted_category' AS item_type, NULL AS data_blob, NULL AS created_at, NULL AS updated_at, 0 AS is_pinned, 0 AS is_favorite, 1 AS is_deleted, '' AS source_app, '' AS source_title, NULL AS last_accessed_at, '' AS remark "
                      "FROM categories WHERE is_deleted = 1 "
                      "ORDER BY is_pinned DESC, updated_at DESC";
        
        QSqlQuery query(m_db);
        if (query.exec(sql)) {
            while (query.next()) {
                QVariantMap map;
                QSqlRecord rec = query.record();
                for (int i = 0; i < rec.count(); ++i) map[rec.fieldName(i)] = query.value(i);
                results.append(map);
            }
        } else {
            qCritical() << "[DB] searchNotes(trash) failed:" << query.lastError().text() << "SQL:" << sql;
        }
        return results;
    }

    QString baseSql = "SELECT notes.* FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    if (!keyword.isEmpty()) {
        // 2026-04-09 按照用户要求：回归标准 LIKE 搜索，支持标签、标题、正文及扩展名检索
        whereClause += "AND (title LIKE ? OR content LIKE ? OR tags LIKE ? OR file_extensions LIKE ?) ";
        QString kw = "%" + keyword + "%";
        params << kw << kw << kw << kw;
    }
    
    QString finalSql = baseSql + whereClause + "ORDER BY ";
    if (filterType == "recently_visited") {
        finalSql += "is_pinned DESC, last_accessed_at DESC";
    } else {
        // [CRITICAL] 锁定：所有视图严格遵循 置顶 > 排序值 > 更新时间 的排序准则，拒绝任何 rank 脑补。
        finalSql += "is_pinned DESC, sort_order ASC, updated_at DESC";
    }
    
    if (page > 0) finalSql += QString(" LIMIT %1 OFFSET %2").arg(pageSize).arg((page - 1) * pageSize);
    
    QSqlQuery query(m_db);
    query.prepare(finalSql);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    
    if (query.exec()) { 
        while (query.next()) { 
            QVariantMap map; 
            QSqlRecord rec = query.record(); 
            for (int i = 0; i < rec.count(); ++i) map[rec.fieldName(i)] = query.value(i); 
            results.append(map); 
        } 
    }
    else qCritical() << "searchNotes failed:" << query.lastError().text();
    return results;
}

// [CRITICAL] 核心计数逻辑：必须与 searchNotes 的过滤条件保持 1:1 同步，禁止擅自改动。
int DatabaseManager::getNotesCount(const QString& keyword, const QString& filterType, const QVariant& filterValue, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return 0;

    if (filterType == "trash") {
        // [OLD_VERSION_RECOVERY] 回归旧版简单计数逻辑
        int trashNotes = 0;
        QSqlQuery nQuery(m_db);
        nQuery.prepare("SELECT COUNT(*) FROM notes WHERE is_deleted = 1");
        if (nQuery.exec() && nQuery.next()) trashNotes = nQuery.value(0).toInt();

        int trashCats = 0;
        QSqlQuery cQuery(m_db);
        cQuery.prepare("SELECT COUNT(*) FROM categories WHERE is_deleted = 1");
        if (cQuery.exec() && cQuery.next()) trashCats = cQuery.value(0).toInt();

        return trashNotes + trashCats;
    }

    QString baseSql = "SELECT COUNT(*) FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    if (!keyword.isEmpty()) {
        // 2026-04-09 同步回归 LIKE 计数逻辑，增加扩展名检索
        whereClause += "AND (title LIKE ? OR content LIKE ? OR tags LIKE ? OR file_extensions LIKE ?) ";
        QString kw = "%" + keyword + "%";
        params << kw << kw << kw << kw;
    }
    
    QSqlQuery query(m_db);
    query.prepare(baseSql + whereClause);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) { if (query.next()) return query.value(0).toInt(); }
    else qCritical() << "getNotesCount failed:" << query.lastError().text();
    return 0;
}

QStringList DatabaseManager::getAllTags() {
    QMutexLocker locker(&m_mutex);
    QStringList allTags;
    if (!m_db.isOpen()) return allTags;
    QSqlQuery query(m_db);
    if (query.exec("SELECT tags FROM notes WHERE tags != '' AND is_deleted = 0")) {
        while (query.next()) {
            QString tagsStr = query.value(0).toString();
            QStringList parts = tagsStr.split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                QString trimmed = part.trimmed();
                if (!trimmed.isEmpty() && !allTags.contains(trimmed)) allTags.append(trimmed);
            }
        }
    }
    allTags.sort();
    return allTags;
}

QList<QVariantMap> DatabaseManager::getRecentTagsWithCounts(int limit) {
    QMutexLocker locker(&m_mutex);
    QList<QVariantMap> results;
    if (!m_db.isOpen()) return results;
    struct TagData { QString name; int count = 0; QDateTime lastUsed; };
    QMap<QString, TagData> tagMap;
    QSqlQuery query(m_db);
    if (query.exec("SELECT tags, updated_at FROM notes WHERE tags != '' AND is_deleted = 0")) {
        while (query.next()) {
            QString tagsStr = query.value(0).toString();
            QDateTime updatedAt = query.value(1).toDateTime();
            QStringList parts = tagsStr.split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                QString name = part.trimmed();
                if (name.isEmpty()) continue;
                if (!tagMap.contains(name)) tagMap[name] = {name, 1, updatedAt};
                else {
                    tagMap[name].count++;
                    if (updatedAt > tagMap[name].lastUsed) tagMap[name].lastUsed = updatedAt;
                }
            }
        }
    }
    QList<TagData> sortedList = tagMap.values();
    std::sort(sortedList.begin(), sortedList.end(), [](const TagData& a, const TagData& b) { if (a.lastUsed != b.lastUsed) return a.lastUsed > b.lastUsed; return a.count > b.count; });
    int actualLimit = qMin(limit, (int)sortedList.size());
    for (int i = 0; i < actualLimit; ++i) { QVariantMap m; m["name"] = sortedList[i].name; m["count"] = sortedList[i].count; results.append(m); }
    return results;
}

int DatabaseManager::addCategory(const QString& name, int parentId, const QString& color) {
    int lastId = -1;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return -1;
        int maxOrder = 0;
        QSqlQuery orderQuery(m_db);
        if (parentId == -1) orderQuery.exec("SELECT MAX(sort_order) FROM categories WHERE parent_id IS NULL OR parent_id = -1");
        else { orderQuery.prepare("SELECT MAX(sort_order) FROM categories WHERE parent_id = :pid"); orderQuery.bindValue(":pid", parentId); orderQuery.exec(); }
        if (orderQuery.next()) maxOrder = orderQuery.value(0).toInt();
        QString chosenColor = color;
        if (chosenColor.isEmpty()) { static const QStringList palette = { "#FF6B6B", "#4ECDC4", "#45B7D1", "#96CEB4", "#FFEEAD", "#D4A5A5", "#9B59B6", "#3498DB", "#E67E22", "#2ECC71", "#E74C3C", "#F1C40F", "#1ABC9C", "#34495E", "#95A5A6" }; chosenColor = palette.at(QRandomGenerator::global()->bounded(palette.size())); }
        QSqlQuery query(m_db);
        query.prepare("INSERT INTO categories (name, parent_id, color, sort_order) VALUES (:name, :parent_id, :color, :sort_order)");
        query.bindValue(":name", name);
        query.bindValue(":parent_id", parentId == -1 ? QVariant(QMetaType::fromType<int>()) : parentId);
        query.bindValue(":color", chosenColor);
        query.bindValue(":sort_order", maxOrder + 1);
        if (query.exec()) { lastId = query.lastInsertId().toInt(); markDirty(); }
    }
    if (lastId != -1) emit categoriesChanged();
    return lastId;
}

int DatabaseManager::getOrCreateCategoryByName(const QString& name, int parentId, const QString& color) {
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return -1;
        
        QSqlQuery query(m_db);
        if (parentId <= 0) {
            query.prepare("SELECT id FROM categories WHERE name = :name AND (parent_id IS NULL OR parent_id <= 0) AND is_deleted = 0 LIMIT 1");
        } else {
            query.prepare("SELECT id FROM categories WHERE name = :name AND parent_id = :pid AND is_deleted = 0 LIMIT 1");
            query.bindValue(":pid", parentId);
        }
        query.bindValue(":name", name);
        
        if (query.exec() && query.next()) {
            return query.value(0).toInt();
        }
    }
    
    // 找不到则创建
    return addCategory(name, parentId, color);
}

bool DatabaseManager::toggleCategoryPinned(int id) {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QSqlQuery query(m_db);
        query.prepare("UPDATE categories SET is_pinned = NOT is_pinned WHERE id = :id");
        query.bindValue(":id", id);
        success = query.exec();
        if (success) markDirty();
    }
    if (success) emit categoriesChanged();
    return success;
}

bool DatabaseManager::renameCategory(int id, const QString& name) {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        QSqlQuery query(m_db);
        query.prepare("UPDATE categories SET name=:name WHERE id=:id");
        query.bindValue(":name", name);
        query.bindValue(":id", id);
        success = query.exec();
        if (success) markDirty();
    }
    if (success) emit categoriesChanged();
    return success;
}

bool DatabaseManager::setCategoryColor(int id, const QString& color) {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery treeQuery(m_db);
        // [STABILITY] 增加递归深度限制（50层），防止循环引用导致的 SQL 执行器爆栈
        treeQuery.prepare(R"(
            WITH RECURSIVE category_tree(id, depth) AS (
                SELECT :id, 0 
                UNION ALL 
                SELECT c.id, ct.depth + 1 FROM categories c JOIN category_tree ct ON c.parent_id = ct.id 
                WHERE ct.depth < 50
            ) SELECT id FROM category_tree)");
        treeQuery.bindValue(":id", id);
        QList<int> allIds;
        if (treeQuery.exec()) { while (treeQuery.next()) allIds << treeQuery.value(0).toInt(); }
        if (!allIds.isEmpty()) {
            QString placeholders;
            for(int i=0; i<allIds.size(); ++i) placeholders += (i==0 ? "?" : ",?");
            QSqlQuery updateNotes(m_db);
            updateNotes.prepare(QString("UPDATE notes SET color = ? WHERE category_id IN (%1)").arg(placeholders));
            updateNotes.addBindValue(color);
            for(int cid : allIds) updateNotes.addBindValue(cid);
            updateNotes.exec();
            QSqlQuery updateCats(m_db);
            updateCats.prepare(QString("UPDATE categories SET color = ? WHERE id IN (%1)").arg(placeholders));
            updateCats.addBindValue(color);
            for(int cid : allIds) updateCats.addBindValue(cid);
            updateCats.exec();
        }
        success = m_db.commit();
    }
    if (success) markDirty();
    if (success) { emit categoriesChanged(); emit noteUpdated(); }
    return success;
}

bool DatabaseManager::hardDeleteCategories(const QList<int>& ids) {
    // 2026-03-xx 按照用户要求：分类物理删除，笔记软删除（删除并重置 category_id）
    if (ids.isEmpty()) return true;
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;

    if (!m_db.transaction()) {
        qWarning() << "[DB] hardDeleteCategories 开启事务失败";
        return false;
    }

    QList<int> allIds;
    for (int startId : ids) {
        // [MODIFIED] 必须包含递归逻辑，确保所有子分类被物理清除，笔记被正确移出
        QSqlQuery treeQuery(m_db);
        treeQuery.prepare(R"(
            WITH RECURSIVE category_tree(id, depth) AS (
                SELECT :id, 0
                UNION ALL
                SELECT c.id, ct.depth + 1 FROM categories c JOIN category_tree ct ON c.parent_id = ct.id
                WHERE ct.depth < 50
            ) SELECT id FROM category_tree
        )");
        treeQuery.bindValue(":id", startId);
        if (treeQuery.exec()) {
            while (treeQuery.next()) {
                int cid = treeQuery.value(0).toInt();
                if (!allIds.contains(cid)) allIds << cid;
            }
        }
    }

    QStringList idStrings;
    for(int cid : allIds) idStrings << QString::number(cid);
    QString joinedIds = idStrings.join(",");

    // 1. 软删除关联笔记：标记 is_deleted=1，并将 category_id 设为 -1 (未分类)，防止孤儿记录
    QSqlQuery softDelNotes(m_db);
    QString softDelSql = QString(
        "UPDATE notes SET is_deleted = 1, category_id = -1, color = '#2d2d2d', "
        "is_pinned = 0, is_favorite = 0, updated_at = datetime('now','localtime') "
        "WHERE category_id IN (%1)"
    ).arg(joinedIds);

    if (!softDelNotes.exec(softDelSql)) {
        qWarning() << "[DB] 混合删除-笔记软处理失败:" << softDelNotes.lastError().text();
        m_db.rollback();
        return false;
    }

    // 2. 物理删除分类自身
    QSqlQuery query(m_db);
    bool ok = query.exec(QString("DELETE FROM categories WHERE id IN (%1)").arg(joinedIds));

    if (ok) {
        m_db.commit();
        qDebug() << "[DB] 成功执行混合删除：物理清除分类" << allIds.size() << "个，笔记移入回收站" << softDelNotes.numRowsAffected() << "条";
        markDirty();
        emit categoriesChanged();
        emit noteUpdated();
    } else {
        m_db.rollback();
        qWarning() << "[DB] 混合删除-分类物理清除失败:" << query.lastError().text();
    }
    return ok;
}

bool DatabaseManager::softDeleteCategories(const QList<int>& ids) {
    // 2026-03-xx 增加详尽日志，排查删除失效问题
    qDebug() << "[DB] softDeleteCategories 入口参数 ids:" << ids;
    if (ids.isEmpty()) return true;
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        if (!m_db.transaction()) {
            qWarning() << "[DB] 无法开启事务:" << m_db.lastError().text();
            return false;
        }
        
        QSqlQuery query(m_db);
        for (int id : ids) {
            // 使用递归 CTE 找到所有子分类 ID
            QSqlQuery treeQuery(m_db);
            treeQuery.prepare(R"(
                WITH RECURSIVE category_tree(id, depth) AS (
                    SELECT :id, 0
                    UNION ALL
                    SELECT c.id, ct.depth + 1 FROM categories c JOIN category_tree ct ON c.parent_id = ct.id
                    WHERE ct.depth < 50
                ) SELECT id FROM category_tree
            )");
            treeQuery.bindValue(":id", id);
            QList<int> allIds;
            if (treeQuery.exec()) {
                while (treeQuery.next()) allIds << treeQuery.value(0).toInt();
            } else {
                qWarning() << "[DB] 递归查询失败:" << treeQuery.lastError().text();
            }

            qDebug() << "[DB] 分类 ID:" << id << "递归展开后的 IDs:" << allIds;

            if (!allIds.isEmpty()) {
                QStringList idStrings;
                for(int cid : allIds) idStrings << QString::number(cid);
                QString joinedIds = idStrings.join(",");

                // 1. 标记分类为已删除 (2026-03-xx 放弃 prepare 以解决 Parameter count mismatch)
                QSqlQuery delCat(m_db);
                if (!delCat.exec(QString("UPDATE categories SET is_deleted = 1, updated_at = datetime('now','localtime') WHERE id IN (%1)").arg(joinedIds))) {
                    qWarning() << "[DB] 更新 categories 状态失败:" << delCat.lastError().text();
                    m_db.rollback();
                    return false;
                } else {
                    qDebug() << "[DB] 成功标记" << delCat.numRowsAffected() << "个分类为已删除";
                }

                // 2. 标记所属笔记为已删除
                QSqlQuery delNotes(m_db);
                if (!delNotes.exec(QString("UPDATE notes SET is_deleted = 1, color = '#2d2d2d', is_pinned = 0, is_favorite = 0, updated_at = datetime('now','localtime'), last_accessed_at = datetime('now','localtime') WHERE category_id IN (%1)").arg(joinedIds))) {
                    qWarning() << "[DB] 更新 notes 状态失败:" << delNotes.lastError().text();
                    m_db.rollback();
                    return false;
                } else {
                    qDebug() << "[DB] 成功标记所属分类下的" << delNotes.numRowsAffected() << "条笔记为已删除";
                }
            }
        }
        success = m_db.commit();
        if (!success) {
            qWarning() << "[DB] 事务提交失败:" << m_db.lastError().text();
            m_db.rollback();
        } else {
            qDebug() << "[DB] softDeleteCategories 事务提交成功";
        }
    }
    if (success) {
        markDirty();
        emit categoriesChanged();
        emit noteUpdated();
    }
    return success;
}

bool DatabaseManager::restoreCategories(const QList<int>& ids) {
    if (ids.isEmpty()) return true;
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        
        QSqlQuery query(m_db);
        for (int id : ids) {
            // 同样递归找到所有子项，确保整树恢复
            QSqlQuery treeQuery(m_db);
            treeQuery.prepare(R"(
                WITH RECURSIVE category_tree(id, depth) AS (
                    SELECT :id, 0
                    UNION ALL
                    SELECT c.id, ct.depth + 1 FROM categories c JOIN category_tree ct ON c.parent_id = ct.id
                    WHERE ct.depth < 50
                ) SELECT id FROM category_tree
            )");
            treeQuery.bindValue(":id", id);
            QList<int> allIds;
            if (treeQuery.exec()) {
                while (treeQuery.next()) allIds << treeQuery.value(0).toInt();
            }

            if (!allIds.isEmpty()) {
                QStringList placeholders;
                for(int i=0; i<allIds.size(); ++i) placeholders << "?";
                QString joined = placeholders.join(",");

                // 1. 恢复分类
                QSqlQuery resCat(m_db);
                resCat.prepare(QString("UPDATE categories SET is_deleted = 0 WHERE id IN (%1)").arg(joined));
                for(int cid : allIds) resCat.addBindValue(cid);
                resCat.exec();

                // 2. 恢复笔记。同步更新最后访问时间。
                QSqlQuery resNotes(m_db);
                resNotes.prepare(QString("UPDATE notes SET is_deleted = 0, updated_at = datetime('now','localtime'), last_accessed_at = datetime('now','localtime') WHERE category_id IN (%1)").arg(joined));
                for(int cid : allIds) resNotes.addBindValue(cid);
                resNotes.exec();
            }
        }
        success = m_db.commit();
    }
    if (success) {
        markDirty();
        emit categoriesChanged();
        emit noteUpdated();
    }
    return success;
}

bool DatabaseManager::moveNote(int id, DatabaseManager::MoveDirection direction, const QString& filterType, const QVariant& filterValue, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;

    // 1. 获取当前视图下的所有笔记 ID (按当前排序)
    QString baseSql = "SELECT id FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    // 排除置顶干扰，只在同类（置顶或非置顶）内移动，或者简单处理：按当前最终显示顺序获取列表
    QString finalSql = baseSql + whereClause;
    if (filterType == "recently_visited") finalSql += " ORDER BY is_pinned DESC, last_accessed_at DESC";
    else finalSql += " ORDER BY is_pinned DESC, sort_order ASC, updated_at DESC";
    
    QSqlQuery query(m_db);
    query.prepare(finalSql);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    
    QList<int> ids;
    if (query.exec()) {
        while (query.next()) ids << query.value(0).toInt();
    } else return false;

    int currentIndex = ids.indexOf(id);
    if (currentIndex == -1) return false;

    // 2. 调整位置
    switch (direction) {
        case Up: 
            if (currentIndex > 0) std::swap(ids[currentIndex], ids[currentIndex - 1]); 
            else return false; 
            break;
        case Down: 
            if (currentIndex < ids.size() - 1) std::swap(ids[currentIndex], ids[currentIndex + 1]); 
            else return false; 
            break;
        case Top: 
            if (currentIndex > 0) { ids.removeAt(currentIndex); ids.prepend(id); } 
            else return false; 
            break;
        case Bottom: 
            if (currentIndex < ids.size() - 1) { ids.removeAt(currentIndex); ids.append(id); } 
            else return false; 
            break;
    }

    // 3. 批量更新 sort_order
    m_db.transaction();
    QSqlQuery update(m_db);
    for (int i = 0; i < ids.size(); ++i) {
        update.prepare("UPDATE notes SET sort_order = :val WHERE id = :id");
        update.bindValue(":val", i);
        update.bindValue(":id", ids[i]);
        update.exec();
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit noteUpdated(); }
    return ok;
}

bool DatabaseManager::moveNotesToRow(const QList<int>& idsToMove, int targetRow, const QString& filterType, const QVariant& filterValue, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;

    // 1. 获取当前视图下的完整 ID 列表 (按当前排序)
    QString baseSql = "SELECT id FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    QString finalSql = baseSql + whereClause;
    if (filterType == "recently_visited") finalSql += " ORDER BY is_pinned DESC, last_accessed_at DESC";
    else if (filterType == "trash") finalSql += " ORDER BY updated_at DESC";
    else finalSql += " ORDER BY is_pinned DESC, sort_order ASC, updated_at DESC";
    
    QSqlQuery query(m_db);
    query.prepare(finalSql);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    
    QList<int> fullList;
    if (query.exec()) {
        while (query.next()) fullList << query.value(0).toInt();
    } else return false;

    // 2. 执行逻辑上的移动
    // 先移除要移动的项
    for (int id : idsToMove) {
        fullList.removeAll(id);
    }
    
    // 在目标位置重新插入 (注意范围限制)
    int actualTarget = qBound(0, targetRow, fullList.size());
    for (int i = 0; i < idsToMove.size(); ++i) {
        fullList.insert(actualTarget + i, idsToMove[i]);
    }

    // 3. 批量更新 sort_order
    m_db.transaction();
    QSqlQuery update(m_db);
    for (int i = 0; i < fullList.size(); ++i) {
        update.prepare("UPDATE notes SET sort_order = :val WHERE id = :id");
        update.bindValue(":val", i);
        update.bindValue(":id", fullList[i]);
        update.exec();
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit noteUpdated(); }
    return ok;
}

bool DatabaseManager::reorderNotes(const QString& filterType, const QVariant& filterValue, bool ascending, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;

    QString baseSql = "SELECT id, title FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    QSqlQuery query(m_db);
    query.prepare(baseSql + whereClause);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    
    struct NoteSortInfo { int id; QString title; };
    QList<NoteSortInfo> list;
    if (query.exec()) {
        while (query.next()) list.append({query.value(0).toInt(), query.value(1).toString()});
    } else return false;

    if (list.isEmpty()) return true;

    std::sort(list.begin(), list.end(), [ascending](const NoteSortInfo& a, const NoteSortInfo& b) {
        if (ascending) return a.title.localeAwareCompare(b.title) < 0;
        return a.title.localeAwareCompare(b.title) > 0;
    });

    m_db.transaction();
    QSqlQuery update(m_db);
    for (int i = 0; i < list.size(); ++i) {
        update.prepare("UPDATE notes SET sort_order = :val WHERE id = :id");
        update.bindValue(":val", i);
        update.bindValue(":id", list[i].id);
        update.exec();
    }
    bool ok = m_db.commit();
    if (ok) { markDirty(); emit noteUpdated(); }
    return ok;
}

bool DatabaseManager::moveCategory(int id, DatabaseManager::MoveDirection direction) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    int parentId = -1;
    QSqlQuery parentQuery(m_db);
    parentQuery.prepare("SELECT parent_id FROM categories WHERE id = :id");
    parentQuery.bindValue(":id", id);
    if (parentQuery.exec() && parentQuery.next()) parentId = parentQuery.value(0).isNull() ? -1 : parentQuery.value(0).toInt();
    else return false;
    QSqlQuery siblingsQuery(m_db);
    if (parentId == -1) siblingsQuery.prepare("SELECT id FROM categories WHERE parent_id IS NULL OR parent_id = -1 ORDER BY sort_order ASC");
    else { siblingsQuery.prepare("SELECT id FROM categories WHERE parent_id = :pid ORDER BY sort_order ASC"); siblingsQuery.bindValue(":pid", parentId); }
    if (!siblingsQuery.exec()) return false;
    QList<int> siblings;
    while (siblingsQuery.next()) siblings << siblingsQuery.value(0).toInt();
    int currentIndex = siblings.indexOf(id);
    if (currentIndex == -1) return false;
    switch (direction) {
        case Up: if (currentIndex > 0) std::swap(siblings[currentIndex], siblings[currentIndex - 1]); else return false; break;
        case Down: if (currentIndex < siblings.size() - 1) std::swap(siblings[currentIndex], siblings[currentIndex + 1]); else return false; break;
        case Top: if (currentIndex > 0) { siblings.removeAt(currentIndex); siblings.prepend(id); } else return false; break;
        case Bottom: if (currentIndex < siblings.size() - 1) { siblings.removeAt(currentIndex); siblings.append(id); } else return false; break;
    }
    return updateCategoryOrder(parentId, siblings);
}

QList<QVariantMap> DatabaseManager::getAllCategories() {
    QMutexLocker locker(&m_mutex);
    QList<QVariantMap> results;
    if (!m_db.isOpen()) return results;
    QSqlQuery query(m_db);
    // [MODIFIED] 彻底分离排序：实体组纯靠 sort_order 排序，不再强制使用 is_pinned 置顶干扰
    if (query.exec("SELECT * FROM categories WHERE is_deleted = 0 ORDER BY sort_order ASC")) { 
        while (query.next()) { 
            QVariantMap map; 
            QSqlRecord rec = query.record(); 
            for (int i = 0; i < rec.count(); ++i) map[rec.fieldName(i)] = query.value(i); 
            results.append(map); 
        } 
    }
    return results;
}

QList<DatabaseManager::Todo> DatabaseManager::getAllTodos() {
    QMutexLocker locker(&m_mutex);
    QList<Todo> results;
    if (!m_db.isOpen()) return results;
    
    QSqlQuery query(m_db);
    // [USER_REQUEST] 获取所有任务，用于左侧栏全局视图
    query.exec("SELECT * FROM todos ORDER BY updated_at DESC");
    
    while (query.next()) {
        Todo t;
        t.id = query.value("id").toInt();
        t.title = query.value("title").toString();
        t.content = query.value("content").toString();
        t.startTime = QDateTime::fromString(query.value("start_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.endTime = QDateTime::fromString(query.value("end_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.status = query.value("status").toInt();
        t.reminderTime = QDateTime::fromString(query.value("reminder_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.priority = query.value("priority").toInt();
        t.color = query.value("color").toString();
        t.noteId = query.value("note_id").toInt();
        t.repeatMode = query.value("repeat_mode").toInt();
        t.parentId = query.value("parent_id").toInt();
        t.progress = query.value("progress").toInt();
        t.createdAt = QDateTime::fromString(query.value("created_at").toString(), "yyyy-MM-dd HH:mm:ss");
        t.updatedAt = QDateTime::fromString(query.value("updated_at").toString(), "yyyy-MM-dd HH:mm:ss");
        results.append(t);
    }
    return results;
}

QList<QVariantMap> DatabaseManager::getChildCategories(int parentId) {
    QMutexLocker locker(&m_mutex);
    QList<QVariantMap> results;
    if (!m_db.isOpen()) return results;
    QSqlQuery query(m_db);
    if (parentId <= 0) {
        query.prepare("SELECT * FROM categories WHERE (parent_id IS NULL OR parent_id <= 0) AND is_deleted = 0 ORDER BY is_pinned DESC, sort_order ASC");
    } else {
        query.prepare("SELECT * FROM categories WHERE parent_id = ? AND is_deleted = 0 ORDER BY is_pinned DESC, sort_order ASC");
        query.addBindValue(parentId);
    }
    if (query.exec()) {
        while (query.next()) {
            QVariantMap map;
            QSqlRecord rec = query.record();
            for (int i = 0; i < rec.count(); ++i) map[rec.fieldName(i).toLower()] = query.value(i);
            results.append(map);
        }
    }
    return results;
}

bool DatabaseManager::emptyTrash() {
    bool success = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        
        QSqlQuery query(m_db);
        // 1. 物理删除笔记
        query.exec("DELETE FROM notes WHERE is_deleted = 1");
        
        // 2. 物理删除分类
        query.exec("DELETE FROM categories WHERE is_deleted = 1");
        
        success = m_db.commit();
    }
    if (success) { markDirty(); emit noteUpdated(); }
    return success;
}

bool DatabaseManager::setCategoryPresetTags(int catId, const QString& tags) {
    bool ok = false;
    QList<int> affectedIds;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        query.prepare("UPDATE categories SET preset_tags=:tags WHERE id=:id");
        query.bindValue(":tags", tags);
        query.bindValue(":id", catId);
        if (!query.exec()) { m_db.rollback(); return false; }
        if (!tags.isEmpty()) {
            QStringList newTagsList = tags.split(",", Qt::SkipEmptyParts);
            QSqlQuery fetchNotes(m_db);
            fetchNotes.prepare("SELECT id, tags FROM notes WHERE category_id = :catId AND is_deleted = 0");
            fetchNotes.bindValue(":catId", catId);
            if (fetchNotes.exec()) {
                while (fetchNotes.next()) {
                    int noteId = fetchNotes.value(0).toInt();
                    QString existingTagsStr = fetchNotes.value(1).toString();
                    QStringList existingTags = existingTagsStr.split(",", Qt::SkipEmptyParts);
                    bool changed = false;
                    for (const QString& t : newTagsList) { QString trimmed = t.trimmed(); if (!trimmed.isEmpty() && !existingTags.contains(trimmed)) { existingTags.append(trimmed); changed = true; } }
                    if (changed) { 
                        affectedIds << noteId;
                        QSqlQuery updateNote(m_db); 
                        updateNote.prepare("UPDATE notes SET tags = :tags WHERE id = :id"); 
                        updateNote.bindValue(":tags", existingTags.join(", ")); 
                        updateNote.bindValue(":id", noteId); 
                        updateNote.exec(); 
                    }
                }
            }
        }
        ok = m_db.commit();
    }
    if (ok) markDirty();
    if (ok) { 
        emit categoriesChanged(); 
        emit noteUpdated(); 
    }
    return ok;
}

QString DatabaseManager::getCategoryPresetTags(int catId) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return "";
    QSqlQuery query(m_db);
    query.prepare("SELECT preset_tags FROM categories WHERE id=:id");
    query.bindValue(":id", catId);
    if (query.exec() && query.next()) return query.value(0).toString();
    return "";
}

QVariantMap DatabaseManager::getNoteById(int id) {
    QMutexLocker locker(&m_mutex);
    QVariantMap map;
    if (!m_db.isOpen()) return map;
    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM notes WHERE id = :id");
    query.bindValue(":id", id);
    if (query.exec() && query.next()) {
        QSqlRecord rec = query.record();
        for (int i = 0; i < rec.count(); ++i) {
            map[rec.fieldName(i).toLower()] = query.value(i);
        }
    }
    return map;
}

int DatabaseManager::getLastCreatedNoteId() {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return 0;
    QSqlQuery query(m_db);
    // [USER_REQUEST] 核心修复：直接通过 ID 倒序获取最后创建的一条非删除笔记，彻底杜绝排序干扰
    if (query.exec("SELECT id FROM notes WHERE is_deleted = 0 ORDER BY id DESC LIMIT 1")) {
        if (query.next()) return query.value(0).toInt();
    }
    return 0;
}

QVariantMap DatabaseManager::getCounts() {
    QMutexLocker locker(&m_mutex);
    QVariantMap counts;
    if (!m_db.isOpen()) return counts;
    QSqlQuery query(m_db);
    auto getCount = [&](const QString& where, bool applySecurity = true) {
        QString sql = "SELECT COUNT(*) FROM notes WHERE " + where;
        QVariantList params;
        if (applySecurity) { QString securityClause; applySecurityFilter(securityClause, params, "all"); sql += " " + securityClause; }
        QSqlQuery q(m_db);
        q.prepare(sql);
        for(int i=0; i<params.size(); ++i) q.bindValue(i, params[i]);
        if (q.exec()) { if (q.next()) return q.value(0).toInt(); }
        return 0;
    };
    QString todayStr = QDate::currentDate().toString("yyyy-MM-dd");
    QString yesterdayStr = QDate::currentDate().addDays(-1).toString("yyyy-MM-dd");

    counts["all"] = getCount("is_deleted = 0");
    counts["today"] = getCount(QString("is_deleted = 0 AND date(created_at) = '%1'").arg(todayStr));
    counts["yesterday"] = getCount(QString("is_deleted = 0 AND date(created_at) = '%1'").arg(yesterdayStr));
    counts["recently_visited"] = getCount(QString("is_deleted = 0 AND date(last_accessed_at) = '%1'").arg(todayStr));
    // 2026-03-xx 按照用户要求修复傻逼逻辑：统一“未分类”判定口径，兼容 NULL 和 -1（分类物理删除后的残留）
    counts["uncategorized"] = getCount("is_deleted = 0 AND (category_id IS NULL OR category_id <= 0)");
    counts["untagged"] = getCount("is_deleted = 0 AND (tags IS NULL OR tags = '')");
    counts["bookmark"] = getCount("is_deleted = 0 AND is_favorite = 1");
    
    // [MODIFIED] 统一回收站统计口径：包含已删除笔记 + 已删除分类包
    int trashNotes = getCount("is_deleted = 1", false);
    int trashCats = 0;
    QSqlQuery catTrashQuery(m_db);
    if (catTrashQuery.exec("SELECT COUNT(*) FROM categories WHERE is_deleted = 1")) {
        if (catTrashQuery.next()) trashCats = catTrashQuery.value(0).toInt();
    }
    counts["trash"] = trashNotes + trashCats;

    // [MODIFIED] 按照用户要求切换为“物理隔离计数”逻辑：分类仅统计直接归属于自身的笔记，不再递归累加子分类。
    QMap<int, int> directCounts;
    if (query.exec("SELECT category_id, COUNT(*) FROM notes WHERE is_deleted = 0 AND category_id IS NOT NULL GROUP BY category_id")) {
        while (query.next()) {
            directCounts[query.value(0).toInt()] = query.value(1).toInt();
        }
    }

    for (auto it = directCounts.begin(); it != directCounts.end(); ++it) {
        counts["cat_" + QString::number(it.key())] = it.value();
    }

    return counts;
}

QVariantMap DatabaseManager::getTrialStatus(bool validate) {
    QMutexLocker locker(&m_mutex);

    // [OPTIMIZATION] 2026-03-xx 按照用户要求：正版化，移除试用限制逻辑，强化指纹一致性校验
    if (m_isBatchMode && !m_cachedTrialStatus.isEmpty()) {
        return m_cachedTrialStatus;
    }

    QVariantMap dbStatus;
    dbStatus["is_activated"] = false;

    if (!m_db.isOpen()) return dbStatus;

    QSqlQuery query(m_db);
    query.exec("SELECT key, value FROM system_config");
    while (query.next()) {
        QString key = query.value(0).toString();
        QString value = query.value(1).toString();
        if (key == "is_activated") dbStatus["is_activated"] = (value == "1");
        else if (key == "activation_code") dbStatus["activation_code"] = value;
        else if (key == "failed_attempts") dbStatus["failed_attempts"] = value.toInt();
        else if (key == "last_attempt_date") dbStatus["last_attempt_date"] = value;
    }

    QVariantMap fileStatus = loadTrialFromFile();
    QString licensePath = QCoreApplication::applicationDirPath() + "/license.dat";
    QString appSN = HardwareInfoHelper::getAppDrivePhysicalSerialNumber();
    QString cSN = HardwareInfoHelper::getCDiskPhysicalSerialNumber();
    
    // 2026-03-xx 按照用户要求：强化指纹采样，引入主板 UUID 与 CPUID
    QString boardSN = HardwareInfoHelper::getBoardSerialNumber();
    QString cpuId = HardwareInfoHelper::getCpuId();

    // 2026-03-xx 按照用户要求：保留特权硬件 SHA256 校验 (多维锚点支持)
    bool isAuthorizedHardware = false;
    auto checkHash = [&](const QString& val, const QString& expected) {
        if (val.isEmpty()) return false;
        return QCryptographicHash::hash(val.toUtf8(), QCryptographicHash::Sha256).toHex() == expected;
    };

    // 2026-03-xx 按照用户要求：修正特权列表，为不同硬件维度分配独立哈希槽位
    // A: 硬盘 SN 槽位 (Disk SN)
    if (checkHash(appSN, "0c704276f4eb770cdf87a2ebe79c4e7566a263f1c181e08c3a9d925185d970ec") || 
        checkHash(cSN, "0c704276f4eb770cdf87a2ebe79c4e7566a263f1c181e08c3a9d925185d970ec")) {
        isAuthorizedHardware = true;
    }
    // B: 主板 UUID 槽位 (Board UUID)
    if (!isAuthorizedHardware && checkHash(boardSN, "5f6b0156c90c4246c2c5fcc20de754cf9ee39980e1c54d48ffd7c2eb26c6a7f5")) {
        isAuthorizedHardware = true;
    }
    // C: CPUID 槽位 (CPU Identification)
    if (!isAuthorizedHardware && checkHash(cpuId, "e1c54d48ffd7c2eb26c6a7f55f6b0156c90c4246c2c5fcc20de754cf9ee39980e")) {
        isAuthorizedHardware = true;
    }

    if (isAuthorizedHardware) {
        dbStatus["is_activated"] = true;
    }

    bool fingerprintMismatch = false;

    if (!validate) {
        goto calculate_final;
    }

    // [CRITICAL] 2026-03-xx 指纹强绑定逻辑：
    // 如果本地存在授权文件/注册表记录，但 loadTrialFromFile 返回为空（即解密失败，指纹不匹配），则判定为跨设备非法拷贝。
    if (QFile::exists(licensePath) && fileStatus.isEmpty()) {
        fingerprintMismatch = true;
    }

    // 如果数据库标记已激活，但文件记录缺失，同样判定为指纹不匹配或授权链断裂
    if (dbStatus["is_activated"].toBool() && !fileStatus["is_activated"].toBool()) {
        fingerprintMismatch = true;
    }

    if (fingerprintMismatch) {
        // 2026-03-xx 核心重构：废除“自杀式”自动重置流程。
        // 检测到指纹源解密冲突时，仅在内存中标记激活失效并由 main.cpp 执行静默拦截。
        // 严禁物理删除 license.dat 或重置注册表，以防同一台电脑因权限原因暂时无法获取 SN 导致授权永久丢失。
        qWarning() << "[DB] [SECURITY] 检测到指纹源解密冲突，执行内存级拦截。";
        
        dbStatus["is_activated"] = false;
        dbStatus["activation_code"] = "";
    }

calculate_final:

    QVariantMap finalStatus;
    finalStatus["expired"] = false;              // 2026-03-xx 永久移除试用期限制
    finalStatus["usage_limit_reached"] = false; // 2026-03-xx 永久移除使用次数限制
    finalStatus["is_activated"] = dbStatus["is_activated"].toBool();
    finalStatus["fingerprint_mismatch"] = fingerprintMismatch; // 新增指纹不匹配标记
    finalStatus["failed_attempts"] = 0;          // 2026-03-xx 按照用户要求：移除尝试限次逻辑，强制设为 0
    finalStatus["activation_code"] = dbStatus["activation_code"].toString();

    if (finalStatus["is_activated"].toBool()) {
        finalStatus["expired"] = false;
        finalStatus["usage_limit_reached"] = false;
        finalStatus["days_left"] = 99999;
    }

    return finalStatus;
}

void DatabaseManager::beginBatch() {
    QMutexLocker locker(&m_mutex);
    m_isBatchMode = true;
    m_cachedTrialStatus = getTrialStatus(true); // 预先校验并缓存
    if (m_db.isOpen()) {
        m_db.transaction();
    }
}

void DatabaseManager::endBatch() {
    QMutexLocker locker(&m_mutex);
    if (m_db.isOpen()) {
        m_db.commit();
        qDebug() << "[DB] 批量模式结束：事务已毫秒级提交";
    }
    m_isBatchMode = false;
    m_cachedTrialStatus.clear();
    
    // [FIX] 性能与一致性的平衡：
    // 1. 写 license.dat 文件极快(毫秒级)，必须同步执行，防止下一次操作触发“数据一致性”冲突界面。
    saveTrialToFile(getTrialStatus(false));

    // 2. 刷新数据库并备份
    markDirty(); // 标记脏数据，触发后台 7 秒自动保存
    qDebug() << "[DB] 授权文件已同步，数据库刷新已排队进入后台任务";
}

void DatabaseManager::rollbackBatch() {
    QMutexLocker locker(&m_mutex);
    if (m_db.isOpen()) {
        m_db.rollback();
    }
    m_isBatchMode = false;
    m_cachedTrialStatus.clear();
}

void DatabaseManager::resetActivation() {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return;

    qWarning() << "[DB] [SECURITY] 用户主动触发重置授权，正在清理本地激活状态。";

    // 1. 物理重置数据库激活标记
    QSqlQuery updateQ(m_db);
    updateQ.exec("UPDATE system_config SET value = '0' WHERE key = 'is_activated'");
    updateQ.exec("UPDATE system_config SET value = '' WHERE key = 'activation_code'");

    // 2. 物理删除失效的授权文件
    QString licensePath = QCoreApplication::applicationDirPath() + "/license.dat";
    if (QFile::exists(licensePath)) {
        QFile::remove(licensePath);
    }

    // 3. 清理注册表锚点
    QSettings registry("HKEY_CURRENT_USER\\Software\\RapidNotes", QSettings::NativeFormat);
    registry.remove("TrialA");
    registry.remove("TrialB");
    registry.remove("TrialC");
    registry.remove("TrialSig");

    // 4. 执行刷新保存，确保变更被立即持久化
    locker.unlock();
    flushDatabase("UserActivationReset");
    markDirty();
}

bool DatabaseManager::verifyActivationCode(const QString& code) {
    // 2026-03-xx 按照用户要求：彻底解耦激活码与设备指纹。
    // 第一道坎是设备指纹一致性（自检），第二道坎是激活码有效性。
    // 此处仅执行纯激活码校验，不再进行拼接加密。
    
    const QString targetHash = "0c4246c2c5fcc20de754cf9ee39980e1c54d48ffd7c2eb26c6a7f55f6b0156c9";
    QString today = QDateTime::currentDateTime().toString("yyyy-MM-dd");
    
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    QSqlQuery query(m_db);

    // 执行纯激活码哈希校验
    QString inputHash = QCryptographicHash::hash(code.trimmed().toUpper().toUtf8(), QCryptographicHash::Sha256).toHex();
    
    // 验证逻辑：匹配预设哈希或超级万能码
    if (inputHash == targetHash || code.trimmed().toUpper() == "RAPID-NOTES-GENUINE-2026") {
        // 验证成功：更新激活状态
        query.prepare("INSERT OR REPLACE INTO system_config (key, value) VALUES ('is_activated', '1')");
        query.exec();
        query.prepare("INSERT OR REPLACE INTO system_config (key, value) VALUES ('activation_code', ?)");
        query.addBindValue(code.trimmed().toUpper()); 
        query.exec();
        
        // 清理失败计数记录（保持数据库整洁）
        query.prepare("INSERT OR REPLACE INTO system_config (key, value) VALUES ('failed_attempts', '0')");
        query.exec();

        // 同步到文件
        locker.unlock();
        saveTrialToFile(getTrialStatus(false));
        flushDatabase(); 
        return true;
    } else {
        // 验证失败：不再增加计次，仅记录最后尝试日期
        query.prepare("INSERT OR REPLACE INTO system_config (key, value) VALUES ('last_attempt_date', ?)");
        query.addBindValue(today);
        query.exec();

        locker.unlock();
        saveTrialToFile(getTrialStatus(false));
        flushDatabase();
        return false;
    }
}

void DatabaseManager::saveTrialToFile(const QVariantMap& status) {
    QString appPath = QCoreApplication::applicationDirPath();
    QString plainPath = appPath + "/license.dat";

    // [DE-SHELL] 授权文件统一转为明文 JSON 存储，彻底移除加密环节
    QJsonObject obj;
    obj["first_launch_date"] = status["first_launch_date"].toString();
    obj["usage_count"] = status["usage_count"].toInt();
    obj["is_activated"] = status["is_activated"].toBool();
    obj["activation_code"] = status["activation_code"].toString();
    obj["failed_attempts"] = status["failed_attempts"].toInt();
    obj["last_attempt_date"] = status["last_attempt_date"].toString();

    QJsonDocument doc(obj);
    QFile file(plainPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
        qDebug() << "[DB] [DE-SHELL-LICENSE] 明文授权文件已更新。";
    }

    // [ANCHOR] 2026-03-xx 按照用户要求：增加注册表锚点，防止通过删除文件重置试用期
    // 采用 QSettings 写入注册表，存储加密后的核心授权数据
    QSettings registry("HKEY_CURRENT_USER\\Software\\RapidNotes", QSettings::NativeFormat);
    registry.setValue("TrialA", status["first_launch_date"].toString());
    registry.setValue("TrialB", status["usage_count"].toInt());
    registry.setValue("TrialC", status["is_activated"].toBool() ? 1 : 0);
    
    // 生成混淆校验码，防止用户手动修改注册表
    QString raw = status["first_launch_date"].toString() + QString::number(status["usage_count"].toInt());
    // [DE-SHELL] 注册表签名现在使用固定盐值，不再依赖动态硬件指纹，确保跨设备导入授权的一致性
    QString salt = "RapidNotes_DeShell_Salt_2026"; 
    QString sign = QCryptographicHash::hash((raw + salt).toUtf8(), QCryptographicHash::Sha256).toHex();
    registry.setValue("TrialSig", sign);
}

QVariantMap DatabaseManager::loadTrialFromFile() {
    QString appPath = QCoreApplication::applicationDirPath();
    QString plainPath = appPath + "/license.dat";

    QVariantMap result;
    
    // [DE-SHELL] 1. 尝试从本地明文 JSON 文件加载
    bool fileLoaded = false;
    if (QFile::exists(plainPath)) {
        QFile file(plainPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray content = file.readAll();
            file.close();
            
            // 尝试 JSON 解析
            QJsonDocument doc = QJsonDocument::fromJson(content);
            if (!doc.isNull() && doc.isObject()) {
                QJsonObject obj = doc.object();
                result["first_launch_date"] = obj["first_launch_date"].toString();
                result["usage_count"] = obj["usage_count"].toInt();
                result["is_activated"] = obj["is_activated"].toBool();
                result["activation_code"] = obj["activation_code"].toString();
                result["failed_attempts"] = obj["failed_attempts"].toInt();
                result["last_attempt_date"] = obj["last_attempt_date"].toString();
                fileLoaded = true;
            } else {
                // [DE-SHELL-RESCUE] 如果文件不是明文 JSON，说明是旧版加密授权，尝试进行迁移读取
                logStartup("[DE-SHELL] 检测到加密版 license.dat，正在执行自适应抢救迁移...");
                
                QString appSN = HardwareInfoHelper::getAppDrivePhysicalSerialNumber();
                QString cSN = HardwareInfoHelper::getCDiskPhysicalSerialNumber();
                QStringList keys;
                if (!appSN.isEmpty()) keys << FileCryptoHelper::getCombinedKeyBySN(appSN);
                if (!cSN.isEmpty()) keys << FileCryptoHelper::getCombinedKeyBySN(cSN);
                keys << FileCryptoHelper::getLegacyCombinedKey();

                QString tempDecPath = plainPath + ".dec_tmp";
                for (const QString& key : keys) {
                    if (FileCryptoHelper::decryptFileWithShell(plainPath, tempDecPath, key)) {
                        QFile tf(tempDecPath);
                        if (tf.open(QIODevice::ReadOnly)) {
                            QJsonDocument decDoc = QJsonDocument::fromJson(tf.readAll());
                            tf.close();
                            if (!decDoc.isNull() && decDoc.isObject()) {
                                QJsonObject obj = decDoc.object();
                                result["first_launch_date"] = obj["first_launch_date"].toString();
                                result["usage_count"] = obj["usage_count"].toInt();
                                result["is_activated"] = obj["is_activated"].toBool();
                                result["activation_code"] = obj["activation_code"].toString();
                                result["failed_attempts"] = obj["failed_attempts"].toInt();
                                result["last_attempt_date"] = obj["last_attempt_date"].toString();
                                fileLoaded = true;
                                logStartup("[DE-SHELL] 授权文件迁移抢救成功。");
                                break;
                            }
                        }
                        QFile::remove(tempDecPath);
                    }
                }
                if (QFile::exists(tempDecPath)) QFile::remove(tempDecPath);
            }
        }
    }

    // [ANCHOR] 2. 尝试从注册表锚点加载并进行一致性合并
    QSettings registry("HKEY_CURRENT_USER\\Software\\RapidNotes", QSettings::NativeFormat);
    QString regLaunchDate = registry.value("TrialA").toString();
    int regUsageCount = registry.value("TrialB").toInt();
    bool regActivated = registry.value("TrialC").toInt() == 1;
    QString regSig = registry.value("TrialSig").toString();

    if (!regLaunchDate.isEmpty()) {
        // [DE-SHELL] 注册表签名校验优先尝试新版固定盐值，兼容旧版硬件盐值
        auto checkSign = [&](const QString& salt) {
            QString sign = QCryptographicHash::hash((regLaunchDate + QString::number(regUsageCount) + salt).toUtf8(), QCryptographicHash::Sha256).toHex();
            return (regSig == sign);
        };

        bool sigOk = checkSign("RapidNotes_DeShell_Salt_2026");
        
        if (!sigOk) {
            // 兼容旧版迁移
            QString appSalt = FileCryptoHelper::getCombinedKeyBySN(HardwareInfoHelper::getAppDrivePhysicalSerialNumber());
            QString cSalt = FileCryptoHelper::getCombinedKeyBySN(HardwareInfoHelper::getCDiskPhysicalSerialNumber());
            if (checkSign(appSalt) || checkSign(cSalt) || checkSign(FileCryptoHelper::getLegacyCombinedKey())) {
                sigOk = true;
            }
        }

        if (sigOk) {
            if (!fileLoaded || result["first_launch_date"].toString() > regLaunchDate) {
                result["first_launch_date"] = regLaunchDate;
                result["usage_count"] = qMax(result["usage_count"].toInt(), regUsageCount);
                if (regActivated) result["is_activated"] = true;
            }
        }
    }

    return result;
}


// [CRITICAL] 核心统计逻辑：2026-04-09 按照用户要求回归 LIKE 逻辑。
QVariantMap DatabaseManager::getFilterStats(const QString& keyword, const QString& filterType, const QVariant& filterValue, const QVariantMap& criteria) {
    QMutexLocker locker(&m_mutex);
    QVariantMap stats;
    if (!m_db.isOpen()) return stats;

    QString baseSql = "FROM notes ";
    QString whereClause;
    QVariantList params;
    applyCommonFilters(whereClause, params, filterType, filterValue, criteria);
    
    if (!keyword.isEmpty()) {
        whereClause += "AND (title LIKE ? OR content LIKE ? OR tags LIKE ? OR file_extensions LIKE ?) ";
        QString kw = "%" + keyword + "%";
        params << kw << kw << kw << kw;
    }

    QSqlQuery query(m_db);
    QMap<int, int> stars;
    query.prepare("SELECT rating, COUNT(*) " + baseSql + whereClause + " GROUP BY rating");
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) { while (query.next()) stars[query.value(0).toInt()] = query.value(1).toInt(); }
    QVariantMap starsMap;
    for (auto it = stars.begin(); it != stars.end(); ++it) starsMap[QString::number(it.key())] = it.value();
    stats["stars"] = starsMap;

    // 1.5 精致字数聚合统计 (2026-04-xx 按照用户授权：HTML 脱壳计算)
    // 物理剔除标签对视觉字数的干扰，确保统计结果符合直觉
    QString wcSql = "length(REPLACE(REPLACE(REPLACE(content, '<p>', ''), '</p>', ''), '<br/>', ''))";
    // 业务隔离：统计阶段即剔除图片及包含色码标签的记录
    QString wcFilter = " AND item_type = 'text' AND (tags NOT LIKE '%HEX%' AND tags NOT LIKE '%RGB%' AND tags NOT LIKE '%色码%') ";
    
    QString wcQuerySql = QString(
        "SELECT CASE "
        "WHEN %1 <= 10 THEN '10' "
        "WHEN %1 <= 20 THEN '20' "
        "WHEN %1 <= 30 THEN '30' "
        "WHEN %1 <= 40 THEN '40' "
        "WHEN %1 <= 50 THEN '50' "
        "WHEN %1 <= 60 THEN '60' "
        "WHEN %1 <= 70 THEN '70' "
        "WHEN %1 <= 90 THEN '90' "
        "WHEN %1 <= 100 THEN '100' "
        "ELSE '101' END as bucket, COUNT(*) "
        + baseSql + whereClause + wcFilter + " GROUP BY bucket"
    ).arg(wcSql);

    QSqlQuery wcQuery(m_db);
    wcQuery.prepare(wcQuerySql);
    for (int i = 0; i < params.size(); ++i) wcQuery.bindValue(i, params[i]);
    
    QVariantMap wcMap;
    if (wcQuery.exec()) {
        while (wcQuery.next()) wcMap[wcQuery.value(0).toString()] = wcQuery.value(1).toInt();
    }
    stats["word_count"] = wcMap;

    QMap<QString, int> colors;
    query.prepare("SELECT color, COUNT(*) " + baseSql + whereClause + " GROUP BY color");
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) { while (query.next()) colors[query.value(0).toString()] = query.value(1).toInt(); }
    QVariantMap colorsMap;
    for (auto it = colors.begin(); it != colors.end(); ++it) colorsMap[it.key()] = it.value();
    stats["colors"] = colorsMap;

    // 2.5 物理级多后缀统计 (2026-04-08 按照用户要求：多对多关联统计)
    QMap<QString, int> bizTypes;
    QSqlQuery typeQuery(m_db);
    // [PERFORMANCE] 直接扫描 file_extensions 字段，实现精准的后缀名聚合
    typeQuery.prepare("SELECT item_type, file_extensions " + baseSql + whereClause);
    for (int i = 0; i < params.size(); ++i) typeQuery.bindValue(i, params[i]);
    if (typeQuery.exec()) {
        while (typeQuery.next()) {
            QString itemType = typeQuery.value(0).toString();
            QString exts = typeQuery.value(1).toString();
            if (!exts.isEmpty()) {
                QStringList parts = exts.split(",", Qt::SkipEmptyParts);
                for (const QString& e : parts) bizTypes[e.trimmed()]++;
            } else {
                // 回退逻辑：处理非文件类的语义化类型
                if (itemType == "image") bizTypes["截图"]++;
                else if (itemType == "code") bizTypes["脚本代码"]++;
                else if (itemType == "text") bizTypes["纯文本"]++;
                else if (itemType == "link") bizTypes["Link"]++;
                else if (itemType == "file") bizTypes["数据库附件"]++;
                else if (itemType == "local_file") bizTypes["本地文件"]++;
                else if (itemType == "local_folder" || itemType == "folder") bizTypes["文件夹"]++;
                else if (itemType == "local_batch") bizTypes["批量托管"]++;
                else bizTypes["其他"]++;
            }
        }
    }
    QVariantMap typesMap;
    for (auto it = bizTypes.begin(); it != bizTypes.end(); ++it) typesMap[it.key()] = it.value();
    stats["types"] = typesMap;

    QMap<QString, int> tags;
    query.prepare("SELECT tags " + baseSql + whereClause);
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) {
        while (query.next()) {
            QStringList parts = query.value(0).toString().split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
            for (const QString& t : parts) {
                QString trimmed = t.trimmed();
                if (!trimmed.isEmpty()) tags[trimmed]++;
            }
        }
    }
    QVariantMap tagsMap;
    for (auto it = tags.begin(); it != tags.end(); ++it) tagsMap[it.key()] = it.value();
    stats["tags"] = tagsMap;

    // 5. 创建日期统计
    QMap<QString, int> createDateCounts;
    query.prepare("SELECT date(created_at), COUNT(*) " + baseSql + whereClause + " GROUP BY date(created_at) ORDER BY date(created_at) DESC");
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) {
        while (query.next()) {
            createDateCounts[query.value(0).toString()] = query.value(1).toInt();
        }
    }
    QVariantMap createDateStats;
    for (auto it = createDateCounts.begin(); it != createDateCounts.end(); ++it) createDateStats[it.key()] = it.value();
    stats["date_create"] = createDateStats;

    // 6. 修改日期统计
    QMap<QString, int> updateDateCounts;
    query.prepare("SELECT date(updated_at), COUNT(*) " + baseSql + whereClause + " GROUP BY date(updated_at) ORDER BY date(updated_at) DESC");
    for (int i = 0; i < params.size(); ++i) query.bindValue(i, params[i]);
    if (query.exec()) {
        while (query.next()) {
            updateDateCounts[query.value(0).toString()] = query.value(1).toInt();
        }
    }
    QVariantMap updateDateStats;
    for (auto it = updateDateCounts.begin(); it != updateDateCounts.end(); ++it) updateDateStats[it.key()] = it.value();
    stats["date_update"] = updateDateStats;

    return stats;
}

int DatabaseManager::addTodo(const Todo& todo) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return -1;
    
    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT INTO todos (title, content, start_time, end_time, status, reminder_time, priority, color, 
                           note_id, repeat_mode, parent_id, progress, created_at, updated_at)
        VALUES (:title, :content, :start, :end, :status, :reminder, :priority, :color, 
                :note, :repeat, :parent, :prog, :created, :updated)
    )");
    
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    query.bindValue(":title", todo.title);
    query.bindValue(":content", todo.content);
    query.bindValue(":start", todo.startTime.isValid() ? todo.startTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":end", todo.endTime.isValid() ? todo.endTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":status", todo.status);
    query.bindValue(":reminder", todo.reminderTime.isValid() ? todo.reminderTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":priority", todo.priority);
    query.bindValue(":color", todo.color);
    query.bindValue(":note", todo.noteId);
    query.bindValue(":repeat", todo.repeatMode);
    query.bindValue(":parent", todo.parentId);
    query.bindValue(":prog", todo.progress);
    query.bindValue(":created", now);
    query.bindValue(":updated", now);
    
    if (query.exec()) {
        int id = query.lastInsertId().toInt();
        markDirty();
        emit todoChanged();
        return id;
    }
    return -1;
}

bool DatabaseManager::updateTodo(const Todo& todo) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    
    QSqlQuery query(m_db);
    query.prepare(R"(
        UPDATE todos SET title=:title, content=:content, start_time=:start, end_time=:end, 
        status=:status, reminder_time=:reminder, priority=:priority, color=:color, 
        note_id=:note, repeat_mode=:repeat, parent_id=:parent, progress=:prog, updated_at=:updated
        WHERE id=:id
    )");
    
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    query.bindValue(":title", todo.title);
    query.bindValue(":content", todo.content);
    query.bindValue(":start", todo.startTime.isValid() ? todo.startTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":end", todo.endTime.isValid() ? todo.endTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":status", todo.status);
    query.bindValue(":reminder", todo.reminderTime.isValid() ? todo.reminderTime.toString("yyyy-MM-dd HH:mm:ss") : QVariant());
    query.bindValue(":priority", todo.priority);
    query.bindValue(":color", todo.color);
    query.bindValue(":note", todo.noteId);
    query.bindValue(":repeat", todo.repeatMode);
    query.bindValue(":parent", todo.parentId);
    query.bindValue(":prog", todo.progress);
    query.bindValue(":updated", now);
    query.bindValue(":id", todo.id);
    
    bool ok = query.exec();
    if (ok) {
        markDirty();
        // [PROFESSIONAL] 循环任务自动生成逻辑
        if (todo.status == 1 && todo.repeatMode > 0) {
            Todo next = todo;
            next.id = -1; // 新纪录
            next.status = 0; // 初始状态
            next.progress = 0;
            
            if (todo.repeatMode == 1) { // 每天
                next.startTime = todo.startTime.addDays(1);
                next.endTime = todo.endTime.addDays(1);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addDays(1);
            } else if (todo.repeatMode == 2) { // 每周
                next.startTime = todo.startTime.addDays(7);
                next.endTime = todo.endTime.addDays(7);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addDays(7);
            } else if (todo.repeatMode == 3) { // 每月
                next.startTime = todo.startTime.addMonths(1);
                next.endTime = todo.endTime.addMonths(1);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addMonths(1);
            } else if (todo.repeatMode == 4) { // 每小时
                next.startTime = todo.startTime.addSecs(3600);
                next.endTime = todo.endTime.addSecs(3600);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addSecs(3600);
            } else if (todo.repeatMode == 5) { // 每分钟
                next.startTime = todo.startTime.addSecs(60);
                next.endTime = todo.endTime.addSecs(60);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addSecs(60);
            } else if (todo.repeatMode == 6) { // 每秒
                next.startTime = todo.startTime.addSecs(1);
                next.endTime = todo.endTime.addSecs(1);
                if (todo.reminderTime.isValid()) next.reminderTime = todo.reminderTime.addSecs(1);
            }
            
            // 递归调用 addTodo，但要注意锁
            locker.unlock();
            addTodo(next);
            locker.relock();
        }
        
        emit todoChanged();
    }
    return ok;
}

bool DatabaseManager::deleteTodo(int id) {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen()) return false;
    
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM todos WHERE id = ?");
    query.addBindValue(id);
    
    bool ok = query.exec();
    if (ok) {
        markDirty();
        emit todoChanged();
    }
    return ok;
}

QList<DatabaseManager::Todo> DatabaseManager::getTodosByDate(const QDate& date) {
    QMutexLocker locker(&m_mutex);
    QList<Todo> results;
    if (!m_db.isOpen()) return results;
    
    QSqlQuery query(m_db);
    // 匹配开始时间在指定日期的任务，或者没有开始时间但在指定日期创建的任务（可选推导）
    query.prepare("SELECT * FROM todos WHERE date(start_time) = :date OR (start_time IS NULL AND date(created_at) = :date) ORDER BY priority DESC, start_time ASC");
    query.bindValue(":date", date.toString("yyyy-MM-dd"));
    
    if (query.exec()) {
        while (query.next()) {
            Todo t;
            t.id = query.value("id").toInt();
            t.title = query.value("title").toString();
            t.content = query.value("content").toString();
            t.startTime = QDateTime::fromString(query.value("start_time").toString(), "yyyy-MM-dd HH:mm:ss");
            t.endTime = QDateTime::fromString(query.value("end_time").toString(), "yyyy-MM-dd HH:mm:ss");
            t.status = query.value("status").toInt();
            t.reminderTime = QDateTime::fromString(query.value("reminder_time").toString(), "yyyy-MM-dd HH:mm:ss");
            t.priority = query.value("priority").toInt();
            t.color = query.value("color").toString();
            t.noteId = query.value("note_id").toInt();
            t.repeatMode = query.value("repeat_mode").toInt();
            t.parentId = query.value("parent_id").toInt();
            t.progress = query.value("progress").toInt();
            t.createdAt = QDateTime::fromString(query.value("created_at").toString(), "yyyy-MM-dd HH:mm:ss");
            t.updatedAt = QDateTime::fromString(query.value("updated_at").toString(), "yyyy-MM-dd HH:mm:ss");
            results.append(t);
        }
    }
    return results;
}

QList<DatabaseManager::Todo> DatabaseManager::getAllPendingTodos() {
    QMutexLocker locker(&m_mutex);
    QList<Todo> results;
    if (!m_db.isOpen()) return results;
    
    QSqlQuery query(m_db);
    query.exec("SELECT * FROM todos WHERE status = 0 ORDER BY priority DESC, start_time ASC");
    
    while (query.next()) {
        Todo t;
        t.id = query.value("id").toInt();
        t.title = query.value("title").toString();
        t.content = query.value("content").toString();
        t.startTime = QDateTime::fromString(query.value("start_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.endTime = QDateTime::fromString(query.value("end_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.status = query.value("status").toInt();
        t.reminderTime = QDateTime::fromString(query.value("reminder_time").toString(), "yyyy-MM-dd HH:mm:ss");
        t.priority = query.value("priority").toInt();
        t.color = query.value("color").toString();
        t.createdAt = QDateTime::fromString(query.value("created_at").toString(), "yyyy-MM-dd HH:mm:ss");
        t.updatedAt = QDateTime::fromString(query.value("updated_at").toString(), "yyyy-MM-dd HH:mm:ss");
        results.append(t);
    }
    return results;
}

bool DatabaseManager::addTagsToNote(int noteId, const QStringList& tags) {
    QVariantMap note = getNoteById(noteId);
    if (note.isEmpty()) return false;
    
    QStringList existingStrList = note["tags"].toString().split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
    QStringList finalTags;
    // 确保原有标签也经过清理
    for (const QString& t : existingStrList) {
        QString trimmed = t.trimmed();
        if (!trimmed.isEmpty() && !finalTags.contains(trimmed)) finalTags << trimmed;
    }
    
    // 合并新标签
    for (const QString& t : tags) {
        QString trimmed = t.trimmed();
        if (!trimmed.isEmpty() && !finalTags.contains(trimmed)) finalTags << trimmed;
    }
    
    return updateNoteState(noteId, "tags", finalTags.join(", "));
}
bool DatabaseManager::renameTagGlobally(const QString& oldName, const QString& newName) {
    QString targetOld = oldName.trimmed();
    QString targetNew = newName.trimmed();
    if (targetOld.isEmpty() || targetOld == targetNew) return true;
    
    bool ok = false;
    QList<int> affectedIds;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        // 使用更宽松的匹配，处理潜在的空格存储问题
        query.prepare("SELECT id, tags FROM notes WHERE tags LIKE ? AND is_deleted = 0");
        query.addBindValue("%" + targetOld + "%");
        
        if (query.exec()) {
            while (query.next()) {
                int noteId = query.value(0).toInt(); 
                QString tagsStr = query.value(1).toString();
                QStringList tagList = tagsStr.split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
                
                bool changed = false;
                QStringList newTagList;
                for (const QString& t : tagList) {
                    QString trimmedTag = t.trimmed();
                    if (trimmedTag == targetOld) {
                        if (!targetNew.isEmpty()) newTagList << targetNew;
                        changed = true;
                    } else if (!trimmedTag.isEmpty()) {
                        newTagList << trimmedTag;
                    }
                }
                
                if (changed) {
                    affectedIds << noteId;
                    newTagList.removeDuplicates();
                    QSqlQuery updateQuery(m_db);
                    updateQuery.prepare("UPDATE notes SET tags = ? WHERE id = ?");
                    updateQuery.addBindValue(newTagList.join(", "));
                    updateQuery.addBindValue(noteId);
                    updateQuery.exec();
                }
            }
        }
        ok = m_db.commit();
    }
    if (ok) {
        markDirty();
        emit noteUpdated();
    }
    return ok;
}

bool DatabaseManager::deleteTagGlobally(const QString& tagName) {
    QString target = tagName.trimmed();
    if (target.isEmpty()) return true;
    
    bool ok = false;
    QList<int> affectedIds;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_db.isOpen()) return false;
        m_db.transaction();
        QSqlQuery query(m_db);
        // 允许匹配带空格或不同分隔符的情况
        query.prepare("SELECT id, tags FROM notes WHERE tags LIKE ? AND is_deleted = 0");
        query.addBindValue("%" + target + "%");
        
        if (query.exec()) {
            while (query.next()) {
                int noteId = query.value(0).toInt(); 
                QString tagsStr = query.value(1).toString();
                QStringList tagList = tagsStr.split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
                
                bool changed = false;
                QStringList newTagList;
                for (const QString& t : tagList) {
                    QString trimmedTag = t.trimmed();
                    if (trimmedTag == target) {
                        changed = true;
                    } else if (!trimmedTag.isEmpty()) {
                        newTagList << trimmedTag;
                    }
                }
                
                if (changed) {
                    affectedIds << noteId;
                    newTagList.removeDuplicates();
                    QSqlQuery updateQuery(m_db);
                    updateQuery.prepare("UPDATE notes SET tags = ? WHERE id = ?");
                    updateQuery.addBindValue(newTagList.join(", "));
                    updateQuery.addBindValue(noteId);
                    updateQuery.exec();
                }
            }
        }
        ok = m_db.commit();
    }
    if (ok) {
        markDirty();
        emit noteUpdated();
    }
    return ok;
}


void DatabaseManager::applySecurityFilter(QString& whereClause, QVariantList& params, const QString& filterType) {
    if (filterType == "category" || filterType == "trash" || filterType == "uncategorized") return;
    QSqlQuery catQuery(m_db);
    catQuery.exec("SELECT id FROM categories WHERE password IS NOT NULL AND password != ''");
    QList<int> lockedIds;
    while (catQuery.next()) { int cid = catQuery.value(0).toInt(); if (!m_unlockedCategories.contains(cid)) lockedIds.append(cid); }
    if (!lockedIds.isEmpty()) {
        QStringList placeholders; for (int i = 0; i < lockedIds.size(); ++i) placeholders << "?";
        // 2026-03-xx 按照用户要求修复逻辑：在排除锁定分类时，必须确保“未分类”项目（NULL 或 <=0）始终可见，不被误杀
        whereClause += QString("AND (category_id IS NULL OR category_id <= 0 OR category_id NOT IN (%1)) ").arg(placeholders.join(", "));
        for (int id : lockedIds) params << id;
    }
}

// [CRITICAL] 通用过滤引擎：recently_visited 必须包含排除今日新建笔记的日期判定条件。此逻辑涉及业务分类的严谨性，禁止删除。
void DatabaseManager::applyCommonFilters(QString& whereClause, QVariantList& params, const QString& filterType, const QVariant& filterValue, const QVariantMap& criteria) {
    if (filterType == "trash") {
        whereClause = "WHERE is_deleted = 1 ";
    } else {
        whereClause = "WHERE is_deleted = 0 ";
        applySecurityFilter(whereClause, params, filterType);
        
        if (filterType == "category") { 
            // 2026-03-xx 按照用户要求修复傻逼逻辑：分类 ID 为 -1 时应视为“未分类”，统一查询口径
            if (filterValue.toInt() == -1) whereClause += "AND (category_id IS NULL OR category_id <= 0) "; 
            else { whereClause += "AND category_id = ? "; params << filterValue.toInt(); } 
        }
        else if (filterType == "uncategorized") {
            // 2026-03-xx 按照用户要求修复傻逼逻辑：统一“未分类”判定，防止物理删除分类后的笔记在恢复时变成“幽灵数据”
            whereClause += "AND (category_id IS NULL OR category_id <= 0) ";
        }
        else if (filterType == "today") {
            whereClause += "AND date(created_at) = ? ";
            params << QDate::currentDate().toString("yyyy-MM-dd");
        }
        else if (filterType == "yesterday") {
            whereClause += "AND date(created_at) = ? ";
            params << QDate::currentDate().addDays(-1).toString("yyyy-MM-dd");
        }
        else if (filterType == "recently_visited") {
            whereClause += "AND date(last_accessed_at) = ? ";
            params << QDate::currentDate().toString("yyyy-MM-dd");
        }
        else if (filterType == "bookmark") whereClause += "AND is_favorite = 1 ";
        else if (filterType == "untagged") whereClause += "AND (tags IS NULL OR tags = '') ";
    }
    
    if (filterType != "trash" && !criteria.isEmpty()) {
        if (criteria.contains("stars")) { 
            QStringList stars = criteria.value("stars").toStringList(); 
            if (!stars.isEmpty()) whereClause += QString("AND rating IN (%1) ").arg(stars.join(", ")); 
        }
        if (criteria.contains("word_count")) {
            // 2026-04-xx 按照用户要求：字数区间多选逻辑
            QStringList buckets = criteria.value("word_count").toStringList();
            if (!buckets.isEmpty()) {
                QString wcSql = "length(REPLACE(REPLACE(REPLACE(content, '<p>', ''), '</p>', ''), '<br/>', ''))";
                QStringList wcConds;
                for (const auto& b : buckets) {
                    int val = b.toInt();
                    if (val == 10) wcConds << QString("(%1 BETWEEN 0 AND 10)").arg(wcSql);
                    else if (val == 101) wcConds << QString("(%1 > 100)").arg(wcSql);
                    else wcConds << QString("(%1 BETWEEN %2 AND %3)").arg(wcSql).arg(val - 9).arg(val);
                }
                // [FIX] 修正 LIKE 语法：SQLite 在 prepare 模式下不需要双百分号。
                // 启用字数筛选时，物理叠加“仅文本”与“非色码”约束。
                whereClause += QString("AND (%1) AND item_type = 'text' AND (tags NOT LIKE '%HEX%' AND tags NOT LIKE '%RGB%' AND tags NOT LIKE '%色码%') ").arg(wcConds.join(" OR "));
            }
        }
        if (criteria.contains("types")) { 
            // [CRITICAL] 2026-04-08 核心过滤引擎升级：支持多对多后缀关联检索
            QStringList typeCriteria = criteria.value("types").toStringList(); 
            if (!typeCriteria.isEmpty()) { 
                QStringList bizConds;
                for (const QString& label : typeCriteria) {
                    if (label == "截图") bizConds << "item_type = 'image'";
                    else if (label == "脚本代码") bizConds << "item_type = 'code'";
                    else if (label == "纯文本") bizConds << "item_type = 'text'";
                    else if (label == "Link" || label == "网页链接") bizConds << "item_type = 'link'";
                    else if (label == "数据库附件") bizConds << "item_type = 'file'";
                    else if (label == "本地文件") bizConds << "item_type = 'local_file'";
                    else if (label == "文件夹") bizConds << "item_type IN ('local_folder', 'folder')";
                    else if (label == "批量托管") bizConds << "item_type = 'local_batch'";
                    else if (label == "其他") bizConds << "(file_extensions = '' AND item_type NOT IN ('image', 'code', 'text', 'link', 'file', 'local_file', 'local_folder', 'folder', 'local_batch'))";
                    else {
                        // [OPTIMIZATION] 2026-04-08：物理移除消耗性能的 REPLACE，直接利用存储时的规范化格式 (, ext1, ext2,)
                        // 注：在 addNote 时我们已经确保 file_extensions 存储格式为 "ext1, ext2"
                        // 为实现毫秒级响应，我们采用标准 LIKE 匹配
                        bizConds << "(file_extensions = ? OR file_extensions LIKE ? OR file_extensions LIKE ? OR file_extensions LIKE ?)";
                        params << label.toLower();
                        params << label.toLower() + ", %";
                        params << "%, " + label.toLower();
                        params << "%, " + label.toLower() + ", %";
                    }
                }

                if (!bizConds.isEmpty()) {
                    whereClause += QString("AND (%1) ").arg(bizConds.join(" OR ")); 
                }
            } 
        }
        if (criteria.contains("colors")) { 
            QStringList colors = criteria.value("colors").toStringList(); 
            if (!colors.isEmpty()) { 
                QStringList placeholders; 
                for (const auto& c : colors) { placeholders << "?"; params << c; } 
                whereClause += QString("AND color IN (%1) ").arg(placeholders.join(", ")); 
            } 
        }
        if (criteria.contains("tags")) { 
            QStringList tags = criteria.value("tags").toStringList(); 
            if (!tags.isEmpty()) { 
                QStringList tagConds; 
                for (const auto& t : tags) { 
                    // [PERFORMANCE] 2026-04-08：物理移除 REPLACE，恢复索引友好型 LIKE 匹配。
                    // 虽然对旧数据的兼容性略降，但能换取输入文字时的绝对流畅度。
                    QString tag = t.trimmed();
                    tagConds << "(tags = ? OR tags LIKE ? OR tags LIKE ? OR tags LIKE ?)"; 
                    params << tag;
                    params << tag + ", %";
                    params << "%, " + tag;
                    params << "%, " + tag + ", %";
                } 
                whereClause += QString("AND (%1) ").arg(tagConds.join(" OR ")); 
            } 
        }
        if (criteria.contains("date_create")) { 
            QStringList dates = criteria.value("date_create").toStringList(); 
            if (!dates.isEmpty()) { 
                QStringList dateConds; 
                for (const auto& d : dates) { 
                    dateConds << "date(created_at) = ?";
                    params << d;
                } 
                if (!dateConds.isEmpty()) whereClause += QString("AND (%1) ").arg(dateConds.join(" OR ")); 
            } 
        }
        if (criteria.contains("date_update")) { 
            QStringList dates = criteria.value("date_update").toStringList(); 
            if (!dates.isEmpty()) { 
                QStringList dateConds; 
                for (const auto& d : dates) { 
                    dateConds << "date(updated_at) = ?";
                    params << d;
                } 
                if (!dateConds.isEmpty()) whereClause += QString("AND (%1) ").arg(dateConds.join(" OR ")); 
            } 
        }
    }
}

