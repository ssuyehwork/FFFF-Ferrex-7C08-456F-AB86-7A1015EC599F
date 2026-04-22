#include "UsnWatcher.h"
#include "MftReader.h"
#include "../db/Database.h"
#include "../db/ItemRepo.h"
#include "../db/FolderRepo.h"
#include <winioctl.h>
#include <vector>
#include <QSqlQuery>
#include <QVariant>
#include <QString>

namespace ArcMeta {

UsnWatcher::UsnWatcher(const std::wstring& volume) : m_volume(volume) {}

UsnWatcher::~UsnWatcher() {
    stop();
}

void UsnWatcher::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&UsnWatcher::watcherThread, this);
}

void UsnWatcher::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void UsnWatcher::watcherThread() {
    std::wstring volPath = L"\\\\.\\" + m_volume;
    HANDLE hVol = CreateFileW(volPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) return;

    USN_JOURNAL_DATA journalData;
    DWORD cb;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &journalData, sizeof(journalData), &cb, NULL)) {
        CloseHandle(hVol);
        return;
    }

    // 1. 离线变更追平逻辑：从数据库读取 NextUsn (原生 QtSql 实现)
    QString key = "usn_state_" + QString::fromStdWString(m_volume);
    QSqlQuery query;
    query.prepare("SELECT value FROM sync_state WHERE key = ?");
    query.addBindValue(key);
    
    if (query.exec() && query.next()) {
        m_lastUsn = query.value(0).toString().toLongLong();
    } else {
        m_lastUsn = journalData.NextUsn;
    }

    READ_USN_JOURNAL_DATA readData;
    readData.StartUsn = m_lastUsn;
    readData.ReasonMask = 0xFFFFFFFF;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = journalData.UsnJournalID;

    const int BUF_SIZE = 64 * 1024;
    std::vector<BYTE> buffer(BUF_SIZE);

    while (m_running) {
        if (DeviceIoControl(hVol, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData), buffer.data(), BUF_SIZE, &cb, NULL)) {
            if (cb <= sizeof(USN)) {
                // 无新记录，轮询休眠 (文档红线：200ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            BYTE* pData = buffer.data() + sizeof(USN);
            while (pData < buffer.data() + cb) {
                USN_RECORD_V2* pRecord = (USN_RECORD_V2*)pData;
                handleRecord(pRecord);
                pData += pRecord->RecordLength;
            }
            
            // 更新起始 USN 为本次读取的最后一条
            readData.StartUsn = *(USN*)buffer.data();
        } else {
            break;
        }
    }

    CloseHandle(hVol);
}

/**
 * @brief 处理具体变更事件
 * 逻辑：CREATE -> 插入索引；DELETE -> 级联删除；RENAME -> 原子迁移
 */
void UsnWatcher::handleRecord(USN_RECORD_V2* pRecord) {
    DWORD reason = pRecord->Reason;
    std::wstring fileName(pRecord->FileName, pRecord->FileNameLength / sizeof(wchar_t));
    std::wstring frnStr = QString::number(pRecord->FileReferenceNumber, 16).prepend("0x").toStdWString();

    if (reason & USN_REASON_FILE_CREATE) {
        FileEntry entry;
        entry.volume = m_volume;
        entry.frn = pRecord->FileReferenceNumber;
        entry.parentFrn = pRecord->ParentFileReferenceNumber;
        entry.attributes = pRecord->FileAttributes;
        entry.name = fileName;
        MftReader::instance().updateEntry(entry);
    }

    if (reason & USN_REASON_FILE_DELETE) {
        ItemRepo::markAsDeleted(m_volume, frnStr);
        MftReader::instance().removeEntry(m_volume, pRecord->FileReferenceNumber);
        // 2026-06-xx 物理改进：使用 getPathFast 废除递归解析
        std::wstring fullPath = MftReader::instance().getPathFast(m_volume, pRecord->FileReferenceNumber);
        if (!fullPath.empty()) {
            FolderRepo::remove(m_volume, fullPath);
        }
    }

    if (reason & USN_REASON_RENAME_NEW_NAME) {
        // 1. 获取新路径及新父目录路径
        // 2026-06-xx 物理改进：使用 getPathFast 废除递归解析
        std::wstring newPath = MftReader::instance().getPathFast(m_volume, pRecord->FileReferenceNumber);
        std::wstring newParentPath = MftReader::instance().getPathFast(m_volume, pRecord->ParentFileReferenceNumber);
        std::wstring oldPath = ItemRepo::getPathByFrn(m_volume, frnStr);

        if (!newPath.empty() && oldPath != newPath) {
            // 2026-06-xx 由于彻底移除 .am_meta.json，此处不再进行跨目录的 JSON 物理迁移逻辑
            // 数据库已按 volume/frn 作为主键，路径更新不会影响元数据关联

            // 更新数据库路径与内存索引
            ItemRepo::updatePath(m_volume, frnStr, newPath, newParentPath);

            // 更新内存 MFT 索引 (同步维护反向索引)
            FileEntry entry;
            entry.volume = m_volume;
            entry.frn = pRecord->FileReferenceNumber;
            entry.parentFrn = pRecord->ParentFileReferenceNumber;
            entry.attributes = pRecord->FileAttributes;
            entry.name = newPath.substr(newPath.find_last_of(L"\\/") + 1);
            MftReader::instance().updateEntry(entry);
        }
    }
}

} // namespace ArcMeta
