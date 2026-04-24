#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>

namespace ArcMeta {

/**
 * @brief USN Journal 实时监听引擎
 * 每个卷启动独立线程，处理文件的增删改移
 */
class UsnWatcher {
public:
    explicit UsnWatcher(const std::wstring& volume);
    ~UsnWatcher();

    /**
     * @brief 启动监听线程
     */
    void start();

    /**
     * @brief 停止监听线程
     */
    void stop();

private:
    void watcherThread();
    
    /**
     * @brief 处理离线变更追平
     */
    void catchUpOfflineChanges(HANDLE hVol, USN_JOURNAL_DATA& journalData);

    /**
     * @brief 解析并分发单个记录
     */
    void handleRecord(USN_RECORD_V2* pRecord);

    std::wstring m_volume;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    
    USN m_lastUsn = 0;
};

} // namespace ArcMeta
