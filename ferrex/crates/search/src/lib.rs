use memchr::memmem;
use rayon::prelude::*;
use storage::FileRecord;
use std::collections::HashMap;
use std::sync::RwLock;
use lru::LruCache;
use std::num::NonZeroUsize;

pub struct Searcher<'a> {
    records: &'a [FileRecord],
    string_pool: &'a [u8],
    sorted_idx: &'a [u32],
}

pub struct PathResolver<'a> {
    records: &'a [FileRecord],
    string_pool: &'a [u8],
    frn_to_idx: HashMap<u64, usize>,
    cache: RwLock<LruCache<u64, String>>,
}

impl<'a> PathResolver<'a> {
    pub fn new(records: &'a [FileRecord], string_pool: &'a [u8]) -> Self {
        let mut frn_to_idx = HashMap::with_capacity(records.len());
        for (i, rec) in records.iter().enumerate() {
            frn_to_idx.insert(rec.frn, i);
        }
        Self {
            records,
            string_pool,
            frn_to_idx,
            cache: RwLock::new(LruCache::new(NonZeroUsize::new(10000).unwrap())),
        }
    }

    pub fn resolve(&self, frn: u64) -> String {
        if let Some(path) = self.cache.read().unwrap().peek(&frn) {
            return path.clone();
        }

        let mut current_frn = frn;
        let mut parts = Vec::new();

        // Root FRN is usually 5 on NTFS
        while current_frn != 5 && current_frn != 0 {
            if let Some(&idx) = self.frn_to_idx.get(&current_frn) {
                let rec = &self.records[idx];
                let name = get_name_from_pool(self.string_pool, rec.name_offset as usize);
                parts.push(name);
                current_frn = rec.parent_frn;
            } else {
                parts.push("<ORPHAN>".to_string());
                break;
            }
        }

        parts.reverse();
        let path = parts.join("\\");
        self.cache.write().unwrap().put(frn, path.clone());
        path
    }
}

impl<'a> Searcher<'a> {
    pub fn new(records: &'a [FileRecord], string_pool: &'a [u8], sorted_idx: &'a [u32]) -> Self {
        Self { records, string_pool, sorted_idx }
    }

    /// Binary search for prefix matching + linear scan for substring (Phase 7)
    pub fn search(&self, query: &str) -> Vec<usize> {
        self.search_with_ext(query, None)
    }

    /// Search with extension filter (Phase 7)
    pub fn search_with_ext(&self, query: &str, ext: Option<&str>) -> Vec<usize> {
        if query.is_empty() && ext.is_none() { return Vec::new(); }
        
        let query_lower = query.to_lowercase();
        let query_bytes = query_lower.as_bytes();
        let ext_lower = ext.map(|e| e.trim_start_matches('.').to_lowercase());

        // Optimization: If query looks like a prefix, find starting range via Binary Search (O(log N))
        let start_range = if !query_bytes.is_empty() {
            self.sorted_idx.partition_point(|&idx| {
                let name = get_name_from_pool(self.string_pool, self.records[idx as usize].name_offset as usize);
                name.to_lowercase() < query_lower
            })
        } else {
            0
        };

        let finder = if !query_bytes.is_empty() { Some(memmem::Finder::new(query_bytes)) } else { None };

        // Process from start_range to end
        self.sorted_idx[start_range..].into_par_iter()
            .filter_map(|&idx_u32| {
                let idx = idx_u32 as usize;
                let rec = &self.records[idx];
                let name_offset = rec.name_offset as usize;
                let name_slice = &self.string_pool[name_offset..];
                let end = name_slice.iter().position(|&b| b == 0).unwrap_or(name_slice.len());
                let name_str = String::from_utf8_lossy(&name_slice[..end]);
                let name_lower = name_str.to_lowercase();

                // If we were prefix searching and the prefix no longer matches,
                // and we are NOT doing a general substring search, we could stop.
                // But for Everything-style, usually it's substring.
                // However, we MUST check extension filter here.
                if let Some(ref e) = ext_lower {
                    if !name_lower.ends_with(e) {
                        return None;
                    }
                }

                if let Some(ref f) = finder {
                    if f.find(name_lower.as_bytes()).is_none() {
                        return None;
                    }
                }

                Some(idx)
            })
            .collect()
    }
}

pub fn get_name_from_pool(pool: &[u8], offset: usize) -> String {
    if offset >= pool.len() { return String::new(); }
    let slice = &pool[offset..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).to_string()
}

#[cfg(test)]
mod tests;
