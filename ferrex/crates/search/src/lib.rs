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

    pub fn search(&self, query: &str) -> Vec<usize> {
        if query.is_empty() { return Vec::new(); }
        let query_bytes = query.to_lowercase().as_bytes().to_vec();
        let finder = memmem::Finder::new(&query_bytes);

        (0..self.records.len())
            .into_par_iter()
            .filter(|&idx| {
                let rec = &self.records[idx];
                let offset = rec.name_offset as usize;
                let name_slice = &self.string_pool[offset..];
                let end = name_slice.iter().position(|&b| b == 0).unwrap_or(name_slice.len());
                let name = String::from_utf8_lossy(&name_slice[..end]).to_lowercase();
                finder.find(name.as_bytes()).is_some()
            })
            .collect()
    }
}
