# 备份备注

**备份时间**：2026-04-17 20:30:03  
**备份目录**：Buk_20260417_203003  

---

针对 ScanDialog 进行深度架构优化：
- 在 ScanDialog.cpp 中优化了 scanDriveEntries 函数，去除了昂贵的 OpenFileById/GetFileSizeEx 调用。
- 在 ScanTableModel 中引入了 QFutureWatcher 和 QtConcurrent 实现后台筛选，确保搜索时 UI 流畅。
- 增加了 m_pathCache 以避免重复路径计算。
- 改进了统计逻辑，利用扫描时的闲置 CPU 时间提前计算后缀分布。
- 严格遵循宪法，增加了线程安全锁及析构等待逻辑。

预留版 Everything.exe
