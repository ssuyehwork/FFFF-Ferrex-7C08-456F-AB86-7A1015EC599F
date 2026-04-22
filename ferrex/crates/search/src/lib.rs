use memchr::memmem;
use rayon::prelude::*;

pub struct Searcher<'a> {
    records: &'a [storage::FileRecord],
    string_pool: &'a [u8],
}

impl<'a> Searcher<'a> {
    pub fn new(records: &'a [storage::FileRecord], string_pool: &'a [u8]) -> Self {
        Self { records, string_pool }
    }

    pub fn search(&self, query: &str, ext: &str) -> Vec<usize> {
        if query.is_empty() && ext.is_empty() { return Vec::new(); }
        
        let query_lower = query.to_lowercase();
        let query_bytes = query_lower.as_bytes();
        let finder = if !query.is_empty() { Some(memmem::Finder::new(query_bytes)) } else { None };

        let ext_lower = ext.to_lowercase().trim_start_matches('.').to_string();
        let ext_dot = format!(".{}", ext_lower);

        (0..self.records.len())
            .into_par_iter()
            .filter(|&idx| {
                let rec = &self.records[idx];
                let offset = rec.name_offset as usize;
                let name_slice = &self.string_pool[offset..];
                let end = name_slice.iter().position(|&b| b == 0).unwrap_or(name_slice.len());
                let name_bytes = &name_slice[..end];
                let name_str_lossy = String::from_utf8_lossy(name_bytes);
                let name_lower = name_str_lossy.to_lowercase();

                // Extension check
                if !ext_lower.is_empty() {
                    if !name_lower.ends_with(&ext_dot) {
                        return false;
                    }
                }

                // Query check
                if let Some(ref f) = finder {
                    if f.find(name_lower.as_bytes()).is_none() {
                        return false;
                    }
                }
                
                true
            })
            .collect()
    }
}
