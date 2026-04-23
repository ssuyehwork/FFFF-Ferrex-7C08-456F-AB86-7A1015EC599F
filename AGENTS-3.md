全选了，好。我来生成一份完整的、分层的实现提示词交给 Jules。

---

```
# Task: Ferrex — Full Feature Implementation (Phase 2)

## Context
Ferrex is a high-performance NTFS file indexer built in Rust using eframe/egui.
The core MFT scanning, Raw Flat Binary storage, and basic search are already implemented.
This document defines ALL remaining features to be implemented across every crate.
Jules must implement every item in this document. No stubs, no TODO placeholders.

---

## Part 1: Search Crate — crates/search/src/lib.rs

### 1.1 Extend Searcher with all filter types

Replace the existing `Searcher` with this expanded version:

```rust
use memchr::memmem;
use rayon::prelude::*;
use std::collections::HashMap;

pub struct SearchOptions<'a> {
    pub query:        &'a str,           // substring or regex pattern
    pub use_regex:    bool,
    pub ext_filter:   Option<&'a str>,   // e.g. "rs" — no dot
    pub min_size:     Option<u64>,       // bytes
    pub max_size:     Option<u64>,       // bytes
    pub date_from:    Option<u64>,       // FILETIME u64
    pub date_to:      Option<u64>,       // FILETIME u64
    pub include_dirs: bool,              // false = files only
    pub include_hidden:  bool,
    pub include_system:  bool,
    pub drives:       &'a [String],      // active drive letters to search
}

impl Default for SearchOptions<'_> {
    fn default() -> Self {
        Self {
            query: "",
            use_regex: false,
            ext_filter: None,
            min_size: None,
            max_size: None,
            date_from: None,
            date_to: None,
            include_dirs: true,
            include_hidden: false,
            include_system: false,
            drives: &[],
        }
    }
}

pub struct Searcher<'a> {
    records:     &'a [storage::FileRecord],
    string_pool: &'a [u8],
}

impl<'a> Searcher<'a> {
    pub fn new(records: &'a [storage::FileRecord], string_pool: &'a [u8]) -> Self {
        Self { records, string_pool }
    }

    pub fn search(&self, opts: &SearchOptions) -> Vec<usize> {
        if opts.query.is_empty() && opts.ext_filter.is_none()
            && opts.min_size.is_none() && opts.max_size.is_none()
            && opts.date_from.is_none() && opts.date_to.is_none()
        {
            return Vec::new();
        }

        // Compile regex once outside the parallel loop
        let regex_pattern: Option<regex::Regex> = if opts.use_regex && !opts.query.is_empty() {
            regex::Regex::new(&format!("(?i){}", opts.query)).ok()
        } else {
            None
        };

        let query_lower = opts.query.to_lowercase();
        let finder = if !opts.use_regex && !query_lower.is_empty() {
            Some(memmem::Finder::new(query_lower.as_bytes()))
        } else {
            None
        };

        let ext = opts.ext_filter.map(|e| e.to_lowercase());

        (0..self.records.len())
            .into_par_iter()
            .filter(|&idx| {
                let rec = &self.records[idx];

                // Attribute filters
                let is_dir    = rec.flags & 0x0010 != 0;
                let is_hidden = rec.flags & 0x0002 != 0;
                let is_system = rec.flags & 0x0004 != 0;

                if is_dir && !opts.include_dirs { return false; }
                if is_hidden && !opts.include_hidden { return false; }
                if is_system && !opts.include_system { return false; }

                // Size filter (skip for directories)
                if !is_dir {
                    if let Some(min) = opts.min_size {
                        if rec.size < min { return false; }
                    }
                    if let Some(max) = opts.max_size {
                        if rec.size > max { return false; }
                    }
                }

                // Date filter
                if let Some(from) = opts.date_from {
                    if rec.timestamp < from { return false; }
                }
                if let Some(to) = opts.date_to {
                    if rec.timestamp > to { return false; }
                }

                // Get name from pool
                let offset = rec.name_offset as usize;
                if offset >= self.string_pool.len() { return false; }
                let name_bytes = &self.string_pool[offset..];
                let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
                let name_bytes = &name_bytes[..end];
                let name = String::from_utf8_lossy(name_bytes);

                // Extension filter
                if let Some(ref ext_filter) = ext {
                    let name_ext = name.rsplit('.').next().unwrap_or("").to_lowercase();
                    if name_ext != *ext_filter { return false; }
                }

                // Name query
                if !opts.query.is_empty() {
                    let name_lower = name.to_lowercase();
                    if let Some(ref re) = regex_pattern {
                        if !re.is_match(&name_lower) { return false; }
                    } else if let Some(ref f) = finder {
                        if f.find(name_lower.as_bytes()).is_none() { return false; }
                    }
                }

                true
            })
            .collect()
    }

    /// Binary search for exact prefix match on sorted_idx
    /// sorted_idx must be pre-built: records sorted by lowercase name
    pub fn search_prefix<'b>(
        &self,
        prefix: &str,
        sorted_idx: &'b [u32],
    ) -> &'b [u32] {
        if prefix.is_empty() { return sorted_idx; }
        let prefix_lower = prefix.to_lowercase();

        let get_name = |idx: u32| -> String {
            let rec = &self.records[idx as usize];
            let offset = rec.name_offset as usize;
            let pool = &self.string_pool[offset..];
            let end = pool.iter().position(|&b| b == 0).unwrap_or(pool.len());
            String::from_utf8_lossy(&pool[..end]).to_lowercase()
        };

        let lo = sorted_idx.partition_point(|&idx| get_name(idx) < prefix_lower);
        let hi = sorted_idx.partition_point(|&idx| {
            let n = get_name(idx);
            n < prefix_lower || n.starts_with(&prefix_lower)
        });

        &sorted_idx[lo..hi]
    }
}
```

Add to `Cargo.toml` of `crates/search`:
```toml
regex = "1.10"
```

---

## Part 2: Storage Crate — crates/storage/src/lib.rs

### 2.1 Add sorted_idx builder

```rust
impl LoadedIndex {
    /// Build a sorted index array (indices into records, sorted by lowercase filename).
    /// Call once after loading. Store result in FerrexApp.
    pub fn build_sorted_idx(&self) -> Vec<u32> {
        let records = self.get_records();
        let pool    = self.get_string_pool();

        let mut idx: Vec<u32> = (0..records.len() as u32).collect();
        idx.sort_unstable_by(|&a, &b| {
            let name_a = pool_get_name_lower(pool, records[a as usize].name_offset as usize);
            let name_b = pool_get_name_lower(pool, records[b as usize].name_offset as usize);
            name_a.cmp(&name_b)
        });
        idx
    }

    /// Build FRN → record index map for path resolution.
    pub fn build_frn_map(&self) -> std::collections::HashMap<u64, u32> {
        let records = self.get_records();
        let mut map = std::collections::HashMap::with_capacity(records.len());
        for (i, rec) in records.iter().enumerate() {
            map.insert(rec.frn, i as u32);
        }
        map
    }
}

pub fn pool_get_name(pool: &[u8], offset: usize) -> String {
    if offset >= pool.len() { return String::new(); }
    let slice = &pool[offset..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).to_string()
}

pub fn pool_get_name_lower(pool: &[u8], offset: usize) -> String {
    pool_get_name(pool, offset).to_lowercase()
}

/// Resolve full path from FRN chain using a pre-built FRN map.
/// Uses an LRU cache to avoid redundant traversals.
pub fn resolve_path(
    drive:   &str,
    idx:     u32,
    records: &[FileRecord],
    pool:    &[u8],
    frn_map: &std::collections::HashMap<u64, u32>,
    lru:     &mut lru::LruCache<u64, String>,
) -> String {
    let rec = &records[idx as usize];

    // Check LRU for this FRN
    if let Some(cached) = lru.get(&rec.frn) {
        return cached.clone();
    }

    let mut parts: Vec<String> = Vec::new();
    let mut current = idx;

    loop {
        let r = &records[current as usize];
        parts.push(pool_get_name(pool, r.name_offset as usize));

        // Root FRN on NTFS is typically 5
        if r.frn == 5 || r.parent_frn == 0 || r.frn == r.parent_frn {
            break;
        }

        match frn_map.get(&r.parent_frn) {
            Some(&parent_idx) => current = parent_idx,
            None => break,
        }
    }

    parts.reverse();
    let path = format!("{}:\\{}", drive, parts.join("\\"));

    // Cache it
    lru.put(rec.frn, path.clone());
    path
}
```

Add to `Cargo.toml` of `crates/storage`:
```toml
lru = "0.12"
```

---

## Part 3: Indexer Crate — crates/indexer/src/lib.rs

### 3.1 USN Journal background watcher thread

Add a public function that spawns a background thread and returns a channel:

```rust
use std::sync::mpsc::{self, Receiver};

pub enum UsnEvent {
    Created { frn: u64, parent_frn: u64, name: String, flags: u32 },
    Deleted { frn: u64 },
    Renamed { old_frn: u64, new_name: String, new_parent_frn: u64 },
    Modified { frn: u64 },
}

/// Spawn a background USN monitoring thread for a volume.
/// Returns a Receiver that yields UsnEvent values.
/// The thread exits when the Receiver is dropped.
pub fn spawn_usn_watcher(volume: &str) -> Result<Receiver<UsnEvent>> {
    let volume = volume.to_string();
    let (tx, rx) = mpsc::channel();

    std::thread::Builder::new()
        .name(format!("usn-watcher-{}", volume))
        .spawn(move || {
            let path = format!("\\\\.\\{}", volume);
            unsafe {
                let handle = match CreateFileW(
                    &HSTRING::from(path),
                    GENERIC_READ.0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    None,
                    OPEN_EXISTING,
                    FILE_FLAGS_AND_ATTRIBUTES::default(),
                    None,
                ) {
                    Ok(h) => h,
                    Err(_) => return,
                };

                let mut bytes_returned = 0u32;
                let mut journal_data = USN_JOURNAL_DATA_V0::default();

                if DeviceIoControl(
                    handle,
                    FSCTL_QUERY_USN_JOURNAL,
                    None, 0,
                    Some(&mut journal_data as *mut _ as *mut _),
                    std::mem::size_of::<USN_JOURNAL_DATA_V0>() as u32,
                    Some(&mut bytes_returned),
                    None,
                ).is_err() {
                    let _ = CloseHandle(handle);
                    return;
                }

                let mut next_usn = journal_data.NextUsn;
                let journal_id  = journal_data.UsnJournalID;

                let mut buffer = vec![0u8; 65536];

                loop {
                    let rujd = READ_USN_JOURNAL_DATA_V0 {
                        StartUsn: next_usn,
                        ReasonMask: USN_REASON_FILE_CREATE
                            | USN_REASON_FILE_DELETE
                            | USN_REASON_RENAME_NEW_NAME
                            | USN_REASON_RENAME_OLD_NAME
                            | USN_REASON_DATA_OVERWRITE,
                        ReturnOnlyOnClose: 0,
                        Timeout: 1,        // 1 second wait
                        BytesToWaitFor: 1,
                        UsnJournalID: journal_id,
                    };

                    let res = DeviceIoControl(
                        handle,
                        FSCTL_READ_USN_JOURNAL,
                        Some(&rujd as *const _ as *const _),
                        std::mem::size_of::<READ_USN_JOURNAL_DATA_V0>() as u32,
                        Some(buffer.as_mut_ptr() as *mut _),
                        buffer.len() as u32,
                        Some(&mut bytes_returned),
                        None,
                    );

                    if res.is_err() {
                        std::thread::sleep(std::time::Duration::from_millis(500));
                        continue;
                    }

                    if bytes_returned >= 8 {
                        next_usn = i64::from_le_bytes(buffer[0..8].try_into().unwrap());
                        let mut offset = 8usize;
                        while offset + std::mem::size_of::<USN_RECORD_V2>() <= bytes_returned as usize {
                            let record = &*(buffer.as_ptr().add(offset) as *const USN_RECORD_V2);

                            let name_ptr = buffer.as_ptr().add(offset + record.FileNameOffset as usize) as *const u16;
                            let name_len = record.FileNameLength as usize / 2;
                            let name_slice = std::slice::from_raw_parts(name_ptr, name_len);
                            let name = String::from_utf16_lossy(name_slice);

                            let event = if record.Reason & USN_REASON_FILE_DELETE != 0 {
                                UsnEvent::Deleted { frn: record.FileReferenceNumber }
                            } else if record.Reason & USN_REASON_RENAME_NEW_NAME != 0 {
                                UsnEvent::Renamed {
                                    old_frn: record.FileReferenceNumber,
                                    new_name: name,
                                    new_parent_frn: record.ParentFileReferenceNumber,
                                }
                            } else if record.Reason & USN_REASON_FILE_CREATE != 0 {
                                UsnEvent::Created {
                                    frn: record.FileReferenceNumber,
                                    parent_frn: record.ParentFileReferenceNumber,
                                    name,
                                    flags: record.FileAttributes,
                                }
                            } else {
                                UsnEvent::Modified { frn: record.FileReferenceNumber }
                            };

                            if tx.send(event).is_err() {
                                let _ = CloseHandle(handle);
                                return; // Receiver dropped — exit cleanly
                            }

                            if record.RecordLength == 0 { break; }
                            offset += record.RecordLength as usize;
                        }
                    }
                }
            }
        })?;

    Ok(rx)
}
```

Add to `Cargo.toml` of `crates/indexer`:
```toml
[dependencies]
# existing windows deps...
```

### 3.2 Exclude path support

```rust
pub fn get_ntfs_volumes_filtered(exclude_prefixes: &[String]) -> Vec<String> {
    get_ntfs_volumes()
        .into_iter()
        .filter(|v| !exclude_prefixes.iter().any(|ex| v.starts_with(ex)))
        .collect()
}
```

---

## Part 4: ferrex_gui — Full GUI Implementation

### 4.1 Application State (complete)

```rust
use std::sync::{Arc, mpsc::Receiver};
use std::collections::{HashMap, HashSet};
use lru::LruCache;
use indexer::UsnEvent;

struct VolumeStore {
    drive:       String,
    index:       Arc<LoadedIndex>,
    sorted_idx:  Vec<u32>,
    frn_map:     HashMap<u64, u32>,
    path_cache:  LruCache<u64, String>,  // capacity: 10_000
    usn_rx:      Option<Receiver<UsnEvent>>,
    record_count:usize,
}

struct FerrexApp {
    // Volumes
    stores:         Vec<VolumeStore>,
    active_drives:  HashSet<String>,

    // Search state
    query:          String,
    ext_filter:     String,
    use_regex:      bool,
    min_size_str:   String,   // raw user input, parse on search
    max_size_str:   String,
    date_from_str:  String,
    date_to_str:    String,
    show_hidden:    bool,
    show_system:    bool,
    dirs_only:      bool,

    // Results
    results:        Vec<SearchResult>,
    selected_rows:  HashSet<usize>,   // for multi-select
    last_search_ms: f64,

    // Sort state
    sort_col:       SortColumn,
    sort_asc:       bool,

    // Search history
    search_history: Vec<String>,
    show_history:   bool,

    // Hover preview
    hovered_row:    Option<usize>,
    preview_pos:    Option<egui::Pos2>,

    // Icons (pre-warmed cache)
    icons:          IconCache,

    // Status / progress
    status_text:    String,
    is_scanning:    bool,
    scan_progress:  f32,        // 0.0 → 1.0
    scan_rx:        Option<std::sync::mpsc::Receiver<ScanProgress>>,

    // Hardware lock
    hardware_ok:    bool,

    // Exclude paths config
    exclude_paths:  Vec<String>,
}

#[derive(Clone)]
struct SearchResult {
    drive:     String,
    store_idx: usize,
    rec_idx:   u32,
    name:      String,
    full_path: String,
    size:      u64,
    timestamp: u64,
    is_dir:    bool,
}

#[derive(PartialEq, Clone, Copy)]
enum SortColumn { Name, Path, Size, Date }

enum ScanProgress {
    Progress { done: usize, total: usize },
    Done { idx_path: String },
    Error(String),
}
```

---

### 4.2 Startup sequence

On `FerrexApp::new()`:
1. Check hardware → set `hardware_ok`
2. If `hardware_ok`:
   a. For each NTFS volume, try to load `{drive}_drive.idx` via `LoadedIndex::load()`
   b. If loaded: build `sorted_idx`, `frn_map`, `path_cache(10_000)`
   c. Spawn USN watcher via `indexer::spawn_usn_watcher()` — store `Receiver<UsnEvent>`
   d. If `.idx` not found: spawn background scan thread (see 4.3)
3. Register global hotkey (see 4.6)
4. Set `active_drives` to all loaded drives

---

### 4.3 Background scan with progress

When no `.idx` exists for a volume, spawn a thread:

```rust
fn spawn_scan(vol: String, tx: std::sync::mpsc::Sender<ScanProgress>) {
    std::thread::spawn(move || {
        let scanner = match indexer::MftScanner::new(&vol) {
            Ok(s) => s,
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); return; }
        };

        // First pass: count (send Progress(0, 0) to signal start)
        let _ = tx.send(ScanProgress::Progress { done: 0, total: 0 });

        match scanner.scan() {
            Ok(entries) => {
                let total = entries.len();
                let mut pool = Vec::new();
                let mut records = Vec::with_capacity(total);
                for (i, entry) in entries.into_iter().enumerate() {
                    let name_offset = pool.len() as u32;
                    pool.extend_from_slice(entry.name.as_bytes());
                    pool.push(0);
                    records.push(storage::FileRecord {
                        frn: entry.frn,
                        parent_frn: entry.parent_frn,
                        size: entry.file_size,
                        timestamp: entry.modified,
                        name_offset,
                        flags: entry.flags,
                    });
                    // Send progress every 50k entries
                    if i % 50_000 == 0 {
                        let _ = tx.send(ScanProgress::Progress { done: i, total });
                    }
                }
                let idx_path = format!("{}_drive.idx",
                    vol.chars().next().unwrap().to_lowercase());
                let _ = storage::save_index(&idx_path, &records, &pool, 0, 0);
                let _ = tx.send(ScanProgress::Done { idx_path });
            }
            Err(e) => { let _ = tx.send(ScanProgress::Error(e.to_string())); }
        }
    });
}
```

In `update()`, poll `scan_rx` each frame:

```rust
if let Some(ref rx) = self.scan_rx {
    while let Ok(msg) = rx.try_recv() {
        match msg {
            ScanProgress::Progress { done, total } => {
                self.scan_progress = if total == 0 { 0.0 }
                    else { done as f32 / total as f32 };
                self.status_text = format!("正在扫描 {}/{}", done, total);
            }
            ScanProgress::Done { idx_path } => {
                // Reload the newly created .idx
                self.is_scanning = false;
                self.scan_rx = None;
                // ... load and add VolumeStore
            }
            ScanProgress::Error(e) => {
                self.status_text = format!("扫描错误: {}", e);
                self.is_scanning = false;
                self.scan_rx = None;
            }
        }
    }
}
```

Show progress bar in the search bar area when `is_scanning == true`:
```rust
if self.is_scanning {
    ui.add(egui::ProgressBar::new(self.scan_progress)
        .text(egui::RichText::new(&self.status_text)
            .font(egui::FontId::new(11.0, egui::FontFamily::Name("mono".into())))
            .color(ACCENT))
        .fill(ACCENT)
        .desired_height(4.0)
        .rounding(egui::Rounding::ZERO));
}
```

---

### 4.4 USN event processing (each frame)

```rust
fn process_usn_events(&mut self) {
    for store in &mut self.stores {
        if let Some(ref rx) = store.usn_rx {
            while let Ok(event) = rx.try_recv() {
                match event {
                    UsnEvent::Created { frn, parent_frn, name, flags } => {
                        // Append new record, update frn_map, sorted_idx
                        // (simplified: re-sort sorted_idx after batch)
                        let records = Arc::make_mut(&mut store.index);
                        // ... append to mutable copy or rebuild
                    }
                    UsnEvent::Deleted { frn } => {
                        // Tombstone: mark record flags |= 0x80000000 (custom deleted bit)
                        // Remove from frn_map and sorted_idx
                    }
                    UsnEvent::Renamed { old_frn, new_name, new_parent_frn } => {
                        // Update name in string pool, invalidate path_cache entry
                        store.path_cache.pop(&old_frn);
                    }
                    UsnEvent::Modified { frn } => {
                        store.path_cache.pop(&frn);
                    }
                }
            }
        }
    }
}
```

Call `process_usn_events()` at the top of every `update()` call.

---

### 4.5 Search execution

```rust
fn run_search(&mut self) {
    let t0 = std::time::Instant::now();

    let opts = search::SearchOptions {
        query:          &self.query,
        use_regex:      self.use_regex,
        ext_filter:     if self.ext_filter.is_empty() { None } else { Some(&self.ext_filter) },
        min_size:       parse_size(&self.min_size_str),
        max_size:       parse_size(&self.max_size_str),
        date_from:      parse_date_to_filetime(&self.date_from_str),
        date_to:        parse_date_to_filetime(&self.date_to_str),
        include_dirs:   !self.dirs_only || true,
        include_hidden: self.show_hidden,
        include_system: self.show_system,
        drives:         &[],  // filtering done below per-store
    };

    self.results = self.stores
        .iter_mut()
        .enumerate()
        .filter(|(_, s)| self.active_drives.contains(&s.drive))
        .flat_map(|(store_idx, store)| {
            let records = store.index.get_records();
            let pool    = store.index.get_string_pool();
            let searcher = search::Searcher::new(records, pool);
            let matches = searcher.search(&opts);

            matches.into_iter().map(move |rec_idx| {
                let rec  = &records[rec_idx];
                let name = storage::pool_get_name(pool, rec.name_offset as usize);
                let full_path = storage::resolve_path(
                    &store.drive, rec_idx as u32, records, pool,
                    &store.frn_map, &mut store.path_cache,
                );
                SearchResult {
                    drive: store.drive.clone(),
                    store_idx,
                    rec_idx: rec_idx as u32,
                    name,
                    full_path,
                    size: rec.size,
                    timestamp: rec.timestamp,
                    is_dir: rec.flags & 0x10 != 0,
                }
            }).collect::<Vec<_>>()
        })
        .take(5000)
        .collect();

    // Apply sort
    self.apply_sort();

    self.last_search_ms = t0.elapsed().as_secs_f64() * 1000.0;

    // Record history (deduplicated, max 20)
    if !self.query.is_empty() {
        self.search_history.retain(|h| h != &self.query);
        self.search_history.insert(0, self.query.clone());
        self.search_history.truncate(20);
    }
}

fn apply_sort(&mut self) {
    let asc = self.sort_asc;
    match self.sort_col {
        SortColumn::Name => self.results.sort_by(|a, b| {
            let c = a.name.to_lowercase().cmp(&b.name.to_lowercase());
            if asc { c } else { c.reverse() }
        }),
        SortColumn::Path => self.results.sort_by(|a, b| {
            let c = a.full_path.to_lowercase().cmp(&b.full_path.to_lowercase());
            if asc { c } else { c.reverse() }
        }),
        SortColumn::Size => self.results.sort_by(|a, b| {
            let c = a.size.cmp(&b.size);
            if asc { c } else { c.reverse() }
        }),
        SortColumn::Date => self.results.sort_by(|a, b| {
            let c = a.timestamp.cmp(&b.timestamp);
            if asc { c } else { c.reverse() }
        }),
    }
}
```

---

### 4.6 Global hotkey (Windows)

Use `windows` crate `RegisterHotKey` / `UnregisterHotKey`:

```rust
// In new(): register Alt+Space as global hotkey
unsafe {
    windows::Win32::UI::Input::KeyboardAndMouse::RegisterHotKey(
        HWND(0),
        1001,
        windows::Win32::UI::Input::KeyboardAndMouse::MOD_ALT,
        0x20, // VK_SPACE
    );
}

// In update(): check for hotkey message via PeekMessage
unsafe {
    let mut msg = windows::Win32::UI::WindowsAndMessaging::MSG::default();
    while windows::Win32::UI::WindowsAndMessaging::PeekMessageW(
        &mut msg, HWND(0), 0x0312, 0x0312,  // WM_HOTKEY
        windows::Win32::UI::WindowsAndMessaging::PM_REMOVE
    ).as_bool() {
        if msg.wParam.0 == 1001 {
            // Toggle window visibility — request focus
            ctx.send_viewport_cmd(egui::ViewportCommand::Focus);
            ctx.send_viewport_cmd(egui::ViewportCommand::Visible(true));
        }
    }
}
```

Unregister on drop:
```rust
impl Drop for FerrexApp {
    fn drop(&mut self) {
        unsafe {
            windows::Win32::UI::Input::KeyboardAndMouse::UnregisterHotKey(HWND(0), 1001);
        }
    }
}
```

---

### 4.7 System tray

Use `tray-icon` crate (add to Cargo.toml):
```toml
tray-icon = "0.18"
```

```rust
// In new():
let tray = tray_icon::TrayIconBuilder::new()
    .with_tooltip("Ferrex")
    .with_icon(load_tray_icon()) // 32x32 RGBA icon
    .with_menu(Box::new(tray_icon::menu::Menu::new()))
    .build()
    .ok();
self.tray = tray;

// Override close behavior — hide instead of quit:
ctx.send_viewport_cmd(egui::ViewportCommand::Decorations(true));
// In update(), intercept close event:
if ctx.input(|i| i.viewport().close_requested()) {
    ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
    ctx.send_viewport_cmd(egui::ViewportCommand::Visible(false));
}
```

---

### 4.8 Right-click context menu

When a result row is right-clicked, show a popup menu:

```rust
let row_response = ui.allocate_rect(row_rect, egui::Sense::click());

if row_response.secondary_clicked() {
    self.context_menu_row = Some(idx);
}

if let Some(ctx_idx) = self.context_menu_row {
    let result = &self.results[ctx_idx];
    egui::popup::popup_below_widget(ui, egui::Id::new("ctx_menu"), &row_response, |ui| {
        ui.set_min_width(180.0);

        if menu_item(ui, "打开文件") {
            open_file(&result.full_path);
            self.context_menu_row = None;
        }
        if menu_item(ui, "在资源管理器中定位") {
            reveal_in_explorer(&result.full_path);
            self.context_menu_row = None;
        }
        if menu_item(ui, "复制路径") {
            ui.output_mut(|o| o.copied_text = result.full_path.clone());
            self.context_menu_row = None;
        }
        if menu_item(ui, "复制文件名") {
            ui.output_mut(|o| o.copied_text = result.name.clone());
            self.context_menu_row = None;
        }
        ui.separator();
        if menu_item(ui, "属性") {
            open_properties(&result.full_path);
            self.context_menu_row = None;
        }
    });
}
```

System call helpers:
```rust
fn open_file(path: &str) {
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "", path])
        .spawn();
}

fn reveal_in_explorer(path: &str) {
    let _ = std::process::Command::new("explorer")
        .args(["/select,", path])
        .spawn();
}

fn open_properties(path: &str) {
    unsafe {
        use windows::Win32::UI::Shell::*;
        use windows::core::HSTRING;
        let info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            lpVerb: windows::core::PCWSTR(HSTRING::from("properties").as_ptr()),
            lpFile: windows::core::PCWSTR(HSTRING::from(path).as_ptr()),
            nShow: 1,
            fMask: 0x0000000C, // SEE_MASK_INVOKEIDLIST
            ..Default::default()
        };
        let _ = ShellExecuteExW(&mut { info });
    }
}
```

---

### 4.9 Hover preview panel

When `hovered_row` is `Some(idx)`, draw a floating panel at `preview_pos`:

```rust
if let Some(row_idx) = self.hovered_row {
    if let Some(pos) = self.preview_pos {
        let result = &self.results[row_idx];
        egui::Area::new(egui::Id::new("preview_panel"))
            .fixed_pos(pos + egui::vec2(12.0, 0.0))
            .order(egui::Order::Tooltip)
            .show(ctx, |ui| {
                egui::Frame::none()
                    .fill(PANEL)
                    .stroke(egui::Stroke::new(1.0, BORDER2))
                    .inner_margin(egui::Margin::same(12.0))
                    .show(ui, |ui| {
                        ui.set_min_width(260.0);
                        ui.set_max_width(400.0);

                        // Name
                        ui.label(egui::RichText::new(&result.name)
                            .font(egui::FontId::new(13.0, egui::FontFamily::Name("mono".into())))
                            .color(ACCENT));

                        ui.add_space(6.0);

                        preview_row(ui, "路径", &result.full_path);
                        preview_row(ui, "大小", &format_size(result.size));
                        preview_row(ui, "修改时间", &format_timestamp(result.timestamp));
                        preview_row(ui, "类型",
                            if result.is_dir { "目录" } else { "文件" });
                        preview_row(ui, "盘符", &result.drive);
                    });
            });
    }
}

fn preview_row(ui: &mut egui::Ui, label: &str, value: &str) {
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new(label)
            .font(egui::FontId::new(10.0, egui::FontFamily::Name("cond".into())))
            .color(TEXT3));
        ui.add_space(8.0);
        ui.label(egui::RichText::new(value)
            .font(egui::FontId::new(11.0, egui::FontFamily::Name("mono".into())))
            .color(TEXT2));
    });
}
```

---

### 4.10 Advanced search filter panel

Below the search bar, add a collapsible filter strip (default: collapsed).
Toggle with a small button labelled "过滤器" on the right of the search bar.

When expanded, show one horizontal strip containing:
- `[正则]` toggle button — sets `use_regex`
- `大小 ≥` text input (20px wide) + `≤` text input — set `min_size_str` / `max_size_str`
- `日期 从` date input + `至` date input — set `date_from_str` / `date_to_str`
- `[隐藏]` toggle — `show_hidden`
- `[系统]` toggle — `show_system`
- `[仅目录]` toggle — `dirs_only`

All toggles use the same styled button: inactive = BORDER2 border / TEXT3 text,
active = ACCENT border / ACCENT text.

---

### 4.11 Multi-select behavior

Track `selected_rows: HashSet<usize>` (indices into `self.results`).

- Single click on row: clear selection, select that row
- Shift+click: range-select from last selected to current
- Ctrl+click: toggle individual row in selection

Selected rows: background = `Color32::from_rgba_unmultiplied(255,140,0,25)`,
left border = ACCENT.

When 2+ rows selected, status bar shows:
`已选 N 项 | 合计大小: X MB | [复制路径] [导出]`

Export to CSV:
```rust
fn export_selected_csv(&self) {
    let path = rfd::FileDialog::new()
        .set_file_name("ferrex_export.csv")
        .add_filter("CSV", &["csv"])
        .save_file();
    if let Some(p) = path {
        let mut content = String::from("名称,路径,大小(字节),修改时间,类型\n");
        for &idx in &self.selected_rows {
            let r = &self.results[idx];
            content.push_str(&format!("{},{},{},{},{}\n",
                r.name, r.full_path, r.size,
                format_timestamp(r.timestamp),
                if r.is_dir { "目录" } else { "文件" }));
        }
        let _ = std::fs::write(p, content);
    }
}
```

Add to `Cargo.toml`:
```toml
rfd = "0.14"
```

---

### 4.12 Memory / CPU stats in status bar

Use `sysinfo` (already in Cargo.toml). Sample every 2 seconds:

```rust
// In state:
sysinfo:       sysinfo::System,
last_sys_poll: std::time::Instant,
mem_usage_mb:  f32,
cpu_usage:     f32,

// In update():
if self.last_sys_poll.elapsed().as_secs() >= 2 {
    self.sysinfo.refresh_memory();
    self.sysinfo.refresh_cpu_usage();
    self.mem_usage_mb = self.sysinfo.used_memory() as f32 / 1_048_576.0;
    self.cpu_usage    = self.sysinfo.global_cpu_usage();
    self.last_sys_poll = std::time::Instant::now();
}
```

Display in status bar right section:
```
MEM 142MB  CPU 0.3%
```

---

### 4.13 Startup load on Windows startup (optional, user-controlled)

Add a checkbox in a settings panel: "开机自动启动 Ferrex"

```rust
fn set_startup(enabled: bool) {
    use windows::Win32::System::Registry::*;
    unsafe {
        let key_path = w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        let mut h_key = HKEY::default();
        let _ = RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_SET_VALUE, &mut h_key);
        if enabled {
            let exe = std::env::current_exe().unwrap_or_default();
            let val = HSTRING::from(exe.to_string_lossy().as_ref());
            let bytes = val.as_wide();
            let _ = RegSetValueExW(
                h_key, w!("Ferrex"), 0, REG_SZ,
                Some(std::slice::from_raw_parts(bytes.as_ptr() as *const u8, bytes.len()*2))
            );
        } else {
            let _ = RegDeleteValueW(h_key, w!("Ferrex"));
        }
        let _ = RegCloseKey(h_key);
    }
}
```

---

## Part 5: Cargo.toml additions summary

### ferrex_gui/Cargo.toml — add:
```toml
tray-icon = "0.18"
rfd       = "0.14"
regex     = "1.10"

[dependencies.windows]
version = "0.52"
features = [
  "Win32_Foundation",
  "Win32_UI_Input_KeyboardAndMouse",
  "Win32_UI_WindowsAndMessaging",
  "Win32_UI_Shell",
  "Win32_System_Registry",
]
```

### crates/search/Cargo.toml — add:
```toml
regex = "1.10"
```

### crates/storage/Cargo.toml — add:
```toml
lru = "0.12"
```

---

## Part 6: Implementation Order (strict)

1. `storage`: add `build_sorted_idx`, `build_frn_map`, `resolve_path`, add `lru` dep
2. `indexer`: add `spawn_usn_watcher`, `UsnEvent` enum
3. `search`: extend `Searcher` with `SearchOptions`, regex support
4. `ferrex_gui`:
   a. State struct + `VolumeStore`
   b. Startup sequence (load all volumes)
   c. Background scan + progress bar
   d. USN event processing loop
   e. Search execution with all filters
   f. Sort (click column header)
   g. Search history dropdown
   h. Multi-select (shift/ctrl click)
   i. Right-click context menu
   j. Hover preview panel
   k. Advanced filter strip
   l. CSV export
   m. Global hotkey
   n. System tray + hide-on-close
   o. Memory/CPU stats in status bar
   p. Startup registry entry toggle

---

## Non-Negotiable Requirements

- `verify_hardware_wmi` must NOT unconditionally return `false`
- NO emojis anywhere — SVG icons from `IconCache` only
- All fonts via `"cond"` and `"mono"` named families
- ALL colors from the defined palette — no random hex values
- NO `.unwrap()` in production paths — use `?` or `.ok()`
- Results list renders maximum 200 rows at a time (slice first 200 from `self.results`)
- `self.results` capacity is capped at 5000 total matches
- All blocking operations (scan, file I/O) run on background threads — never block `update()`
```