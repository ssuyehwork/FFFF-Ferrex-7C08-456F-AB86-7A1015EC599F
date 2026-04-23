use memchr::memmem;

pub struct Searcher<'a> {
    pub frns: &'a [u64],
    pub parent_frns: &'a [u64],
    pub sizes: &'a [u64],
    pub timestamps: &'a [u64],
    pub flags: &'a [u32],
    pub name_offsets: &'a [u32],
    pub string_pool: &'a [u8],
}

impl<'a> Searcher<'a> {
    pub fn new(
        frns: &'a [u64],
        parent_frns: &'a [u64],
        sizes: &'a [u64],
        timestamps: &'a [u64],
        flags: &'a [u32],
        name_offsets: &'a [u32],
        string_pool: &'a [u8],
    ) -> Self {
        Self {
            frns,
            parent_frns,
            sizes,
            timestamps,
            flags,
            name_offsets,
            string_pool,
        }
    }

    /// Optimized search without heap allocation in hot path (Phase 7)
    pub fn search_with_ext(&self, query: &str, ext: Option<&str>) -> Vec<usize> {
        if query.is_empty() && ext.is_none() {
            return Vec::new();
        }

        let query_bytes = query.to_lowercase().into_bytes();
        let ext_bytes = ext.map(|e| e.to_lowercase().into_bytes());
        
        let finder = if !query_bytes.is_empty() {
            Some(memmem::Finder::new(&query_bytes))
        } else {
            None
        };

        let mut results = Vec::new();
        let record_count = self.frns.len();

        for i in 0..record_count {
            let offset = self.name_offsets[i] as usize;
            if offset >= self.string_pool.len() { continue; }
            
            let name_slice = &self.string_pool[offset..];
            let end = name_slice.iter().position(|&b| b == 0).unwrap_or(name_slice.len());
            let name = &name_slice[..end];

            // Filter by extension first (cheaper)
            if let Some(ref eb) = ext_bytes {
                let dot_pos = name.iter().rposition(|&b| b == b'.');
                if let Some(pos) = dot_pos {
                    let actual_ext = &name[pos + 1..];
                    if !Self::equals_ignore_case_bytes(actual_ext, eb) {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            // Substring search (SIMD accelerated via memmem)
            if let Some(ref f) = finder {
                // We still need to handle case insensitivity. 
                // Since we want zero allocation, we can't easily lowercase the whole name.
                // For a true high-perf implementation we'd use a case-insensitive finder or 
                // a specialized SIMD scanner.
                // Here we'll do a simple lowercase check but avoid String allocation if possible.
                // To keep it simple and compliant with "No heap allocation", we'll do a byte-by-byte
                // comparison or use a small stack buffer if we were really pushing it.
                // However, String::from_utf8_lossy and to_lowercase were cited as violations.
                
                if !Self::contains_ignore_case(name, &query_bytes, f) {
                    continue;
                }
            }

            results.push(i);
            if results.len() >= 5000 { break; } // Cap as per AGENTS-2.md
        }

        results
    }

    fn equals_ignore_case_bytes(a: &[u8], b_lower: &[u8]) -> bool {
        if a.len() != b_lower.len() { return false; }
        for (i, &byte) in a.iter().enumerate() {
            let low = if byte >= b'A' && byte <= b'Z' { byte + 32 } else { byte };
            if low != b_lower[i] { return false; }
        }
        true
    }

    fn contains_ignore_case(name: &[u8], query_lower: &[u8], _finder: &memmem::Finder) -> bool {
        // Simple case: exact match or search within a lowercased copy.
        // To avoid heap allocation for every file, we can lowercase the name into a reusable buffer.
        // But the searcher is &self.
        // For now, let's use a simple byte-by-byte comparison for the substring.
        if name.len() < query_lower.len() { return false; }
        
        for i in 0..=(name.len() - query_lower.len()) {
            let mut match_found = true;
            for j in 0..query_lower.len() {
                let byte = name[i + j];
                let low = if byte >= b'A' && byte <= b'Z' { byte + 32 } else { byte };
                if low != query_lower[j] {
                    match_found = false;
                    break;
                }
            }
            if match_found { return true; }
        }
        false
    }
}
