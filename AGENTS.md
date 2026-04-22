Development plan prompt

## 为 Jules 生成的实现提示词

---

```
# Task: Build a High-Performance NTFS File Indexer (Everything.exe-style)

## Project Overview
Build a Windows file indexing and search system that directly reads NTFS 
metadata structures (MFT + USN Journal) to build an ultra-fast in-memory 
index, persisted via Raw Flat Binary File with mmap for near-zero load times.
The system must be written in Rust (preferred) or C++, targeting Windows x64.

---

## Core Concepts & Terminology (strictly follow these mappings)

- NTFS Volume       = The Library         (e.g., C:\, D:\)
- MFT               = The Card Cabinet    (Master File Table)
- FRN               = Book's Unique ID    (File Reference Number, uint64)
- USN Journal       = Transaction Ledger  (Update Sequence Number Journal)
- Index File (.bin) = Snapshot of Cabinet (Raw Flat Binary on disk)
- mmap              = Direct shelf access (no copy, OS-managed page faults)

---

## Phase 1: Volume Enumeration (Find all Libraries)

1. Call `GetLogicalDrives()` to list all drive letters.
2. For each drive, call `GetVolumeInformation()` to verify filesystem type.
3. **Only process NTFS volumes.** Skip FAT32, exFAT, ReFS silently.
4. For each valid NTFS volume, open a device handle:
   ```
   CreateFile(
       "\\\\.\\C:",
       GENERIC_READ,
       FILE_SHARE_READ | FILE_SHARE_WRITE,
       NULL,
       OPEN_EXISTING,
       FILE_FLAG_NO_BUFFERING,  // bypass OS cache, direct I/O
       NULL
   )
   ```
5. Requires **SeBackupPrivilege** or Administrator elevation.
   Attempt to acquire privilege via `AdjustTokenPrivileges()` at startup.
   If denied, log error and skip that volume gracefully.

---

## Phase 2: Full MFT Scan (Copy the entire Card Cabinet)

### 2a. Read MFT via FSCTL_ENUM_USN_DATA (recommended) OR DeviceIoControl
Use `FSCTL_ENUM_USN_DATA` with `MFT_ENUM_DATA_V0`:
```
MFT_ENUM_DATA_V0 med;
med.StartFileReferenceNumber = 0;
med.LowUsn = 0;
med.HighUsn = MAXLONGLONG;

DeviceIoControl(
    hVolume,
    FSCTL_ENUM_USN_DATA,
    &med, sizeof(med),
    buffer, BUFFER_SIZE,
    &bytesReturned,
    NULL
);
```
Loop until `ERROR_HANDLE_EOF`.

### 2b. Parse each USN_RECORD_V2 entry:
For each record extract:
- `FileReferenceNumber`       → FRN (unique book ID)
- `ParentFileReferenceNumber` → Parent FRN (which shelf/folder)
- `FileAttributes`            → Directory flag (`FILE_ATTRIBUTE_DIRECTORY`)
- `FileName` + `FileNameLength` → UTF-16 name (convert to UTF-8 internally)
- `TimeStamp` (from separate MFT attribute if needed)

### 2c. Store raw parsed entries in a temporary Vec/array in memory:
```rust
struct RawEntry {
    frn:        u64,
    parent_frn: u64,
    file_size:  u64,
    modified:   u64,   // FILETIME as u64
    flags:      u32,   // directory, hidden, system, etc.
    name:       String,
}
```
Do NOT build full paths yet. Just collect flat records first.

---

## Phase 3: Build In-Memory Index (Assemble the Index Tree)

### 3a. Build FRN HashMap for O(1) parent lookup:
```
HashMap<u64, usize>   // FRN → index in records array
```

### 3b. Resolve full paths:
For each entry, walk up parent_frn chain until reaching root FRN (FRN=5 
for most NTFS volumes). Cache resolved paths to avoid redundant traversal.
Handle orphaned records (parent_frn not found) by placing under 
a virtual `<ORPHAN>\` root.

### 3c. Final in-memory structure:
Two parallel flat arrays (Structure of Arrays layout for cache efficiency):

```rust
struct IndexStore {
    // Parallel arrays — index N refers to the same file across all arrays
    frns:        Vec<u64>,     // FRN of each entry
    parent_frns: Vec<u64>,     // parent FRN
    sizes:       Vec<u64>,     // file size in bytes
    timestamps:  Vec<u64>,     // last modified (FILETIME)
    flags:       Vec<u32>,     // bitmask: IS_DIR, IS_HIDDEN, IS_SYSTEM
    name_offsets:Vec<u32>,     // byte offset into string_pool
    
    // Separate string pool: all filenames concatenated with \0 separator
    string_pool: Vec<u8>,      // UTF-8 encoded, null-terminated entries
    
    // Lookup structures
    frn_to_idx:  HashMap<u64, u32>,  // FRN → array index (for USN updates)
    
    // Sorted index for binary search by filename
    sorted_idx:  Vec<u32>,     // indices sorted by lowercase filename
}
```

**Critical design rule:** 
- `string_pool` is a single contiguous byte buffer.
- `name_offsets[i]` is the byte position of file i's name in `string_pool`.
- No heap allocation per file. No `Vec<String>`. No `Box`.

---

## Phase 4: Persist to Disk (Raw Flat Binary File)

### File format specification (single file per volume, e.g. `c_drive.idx`):

```
Offset  Size   Field
──────────────────────────────────────────────────────
0x00    4B     Magic: 0x46494458 ("FIDX")
0x04    4B     Version: 1
0x08    8B     Record count (N)
0x10    8B     String pool byte size
0x18    8B     USN watermark (last processed USN)
0x20    8B     Volume serial number
0x28    4B     CRC32 of entire file (set to 0 during calculation)
0x2C    4B     Reserved (padding to 48B header)

── Record Block (starts at offset 0x30) ──────────────
  Per record, 40 bytes, tightly packed, no padding between records:
  [0]  u64  frn
  [8]  u64  parent_frn
  [16] u64  size
  [24] u64  timestamp
  [32] u32  name_offset
  [36] u32  flags

── String Pool Block (immediately after Record Block) ──
  Raw UTF-8 bytes, null-terminated filenames concatenated:
  "Documents\0Desktop\0毕业论文.docx\0..."
```

### Write procedure:
1. Allocate output buffer = header(48B) + records(N×40B) + string_pool
2. Fill sequentially (no seeks)
3. Calculate CRC32 over entire buffer, write into header offset 0x28
4. Single `WriteFile()` call (or `fwrite`) for entire buffer
5. `FlushFileBuffers()` to ensure durability

---

## Phase 5: Load from Disk via mmap (Zero-Copy Startup)

### Windows mmap procedure:
```c
HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, ...);
HANDLE hMap  = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
void*  pData = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
```

### On startup:
1. Open `*.idx` file for the volume.
2. mmap the entire file.
3. Verify magic number and version.
4. Verify CRC32 (if mismatch → discard, trigger full MFT rescan).
5. Cast pointers directly into mmap'd memory:
   ```rust
   let records: &[Record] = slice::from_raw_parts(
       (pData + HEADER_SIZE) as *const Record, 
       record_count
   );
   let pool: &[u8] = slice::from_raw_parts(
       (pData + HEADER_SIZE + record_block_size) as *const u8,
       pool_size
   );
   ```
6. Rebuild `frn_to_idx` HashMap and `sorted_idx` in memory 
   (these are not persisted, rebuilt in O(N) on load).
7. Read stored USN watermark from header.
8. Query USN Journal for all entries AFTER the watermark → apply 
   incremental patches to in-memory index.

**Do NOT copy data out of the mmap region.** 
All reads go directly through the mapped pointer.

---

## Phase 6: USN Journal Monitoring (Incremental Updates)

### Subscribe to USN Journal:
```c
READ_USN_JOURNAL_DATA_V0 rujd;
rujd.StartUsn          = last_usn_watermark;
rujd.ReasonMask        = USN_REASON_FILE_CREATE |
                         USN_REASON_FILE_DELETE |
                         USN_REASON_RENAME_NEW_NAME |
                         USN_REASON_RENAME_OLD_NAME |
                         USN_REASON_DATA_OVERWRITE;
rujd.ReturnOnlyOnClose = 0;
rujd.Timeout           = 0;
rujd.BytesToWaitFor    = 0;
rujd.UsnJournalID      = journal_id;  // from FSCTL_QUERY_USN_JOURNAL

DeviceIoControl(hVolume, FSCTL_READ_USN_JOURNAL, ...);
```

### On each USN event received, update in-memory IndexStore:
- `FILE_CREATE`      → insert new RawEntry, update frn_to_idx, resort sorted_idx
- `FILE_DELETE`      → remove entry by FRN, compact arrays or tombstone flag
- `RENAME_OLD_NAME`  → mark old name as tombstone
- `RENAME_NEW_NAME`  → update name_offset with new string in pool

### USN polling strategy:
- Run a background thread with `FSCTL_READ_USN_JOURNAL`.
- Use `WaitForSingleObject` with a 500ms timeout.
- Batch multiple USN events before applying to reduce lock contention.
- Update USN watermark in `.idx` header after each batch flush.

---

## Phase 7: Search Engine (Pure In-Memory)

### Search execution:
1. Normalize query to lowercase UTF-8.
2. Use `sorted_idx` (pre-sorted array of record indices by filename) 
   for binary search to find the starting range.
3. For substring search: linear scan over `string_pool` using 
   SIMD-accelerated `memchr` or equivalent.
4. Return matched record indices → resolve full paths on-demand.
5. Target: < 50ms for 5,000,000 file index on substring search.

### Full path resolution (lazy, on demand):
Walk `parent_frn` chain upward using `frn_to_idx` until root.
Cache results in a separate `LruCache<u64, String>` (capacity: 10,000).

---

## Phase 8: Multi-Volume Support

- One `.idx` file per NTFS volume.
- One USN monitoring thread per volume.
- Shared search layer fans out query across all volume IndexStores in parallel
  (use Rayon or thread pool).
- Results merged and sorted by relevance (filename match quality + path depth).

---

## Error Handling Requirements

| Scenario                          | Required Behavior                          |
|-----------------------------------|--------------------------------------------|
| No admin privilege                | Log warning, skip volume, continue others  |
| `.idx` CRC mismatch               | Delete corrupted file, trigger full rescan |
| USN Journal overflowed/reset      | Detect via journal ID mismatch, full rescan|
| MFT record parsing failure        | Log + skip that record, continue scanning  |
| Volume disconnected (USB removed) | Unmap mmap region, remove from active list |
| File too large for u64            | Clamp to u64::MAX                          |

---

## Non-Functional Requirements

- **Memory**: Index for 1,000,000 files must use < 200MB RAM.
- **Startup**: Full load from `.idx` via mmap < 500ms for 1M files.
- **First scan**: Full MFT scan of 1M files < 10 seconds.
- **Search latency**: Substring search over 1M files < 50ms.
- **No external dependencies** for the core indexer 
  (no SQLite, no RocksDB, no serde_json in hot path).

---

## Deliverables

1. `indexer/` — Core library crate (volume scan, MFT parsing, USN monitor)
2. `storage/` — Binary file writer/reader with mmap loader
3. `search/`  — In-memory search engine over IndexStore
4. `cli/`     — Simple CLI: `index <drive>`, `search <query>`, `watch`
5. `tests/`   — Integration tests using a mock MFT dataset

---

## Implementation Order (strict sequence)

1. Phase 1 → Volume enumeration + privilege acquisition
2. Phase 2 → MFT scan + raw record parsing
3. Phase 3 → In-memory IndexStore construction
4. Phase 4 → Binary file writer
5. Phase 5 → mmap loader + CRC verification
6. Phase 6 → USN Journal monitor (background thread)
7. Phase 7 → Search engine
8. Phase 8 → Multi-volume fan-out
```

---

这份提示词的设计原则：

- **没有模糊指令**，每个 Phase 都有具体 API 名称、数据结构、字节偏移
- **顺序严格**，Jules 不会因为不知道从哪开始而乱序实现
- **错误场景已列明**，避免 Jules 遇到边界条件时自作主张
- **比喻映射保留在最顶部**，确保整个实现过程术语一致