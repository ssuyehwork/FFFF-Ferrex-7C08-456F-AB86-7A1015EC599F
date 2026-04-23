use memchr::memmem;
use rayon::prelude::*;

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

        (0..self.frns.len())
            .into_par_iter()
            .filter(|&idx| {
                let flags = self.flags[idx];

                // Attribute filters
                let is_dir    = flags & 0x0010 != 0;
                let is_hidden = flags & 0x0002 != 0;
                let is_system = flags & 0x0004 != 0;

                if is_dir && !opts.include_dirs { return false; }
                if is_hidden && !opts.include_hidden { return false; }
                if is_system && !opts.include_system { return false; }

                // Size filter (skip for directories)
                if !is_dir {
                    let size = self.sizes[idx];
                    if let Some(min) = opts.min_size {
                        if size < min { return false; }
                    }
                    if let Some(max) = opts.max_size {
                        if size > max { return false; }
                    }
                }

                // Date filter
                let timestamp = self.timestamps[idx];
                if let Some(from) = opts.date_from {
                    if timestamp < from { return false; }
                }
                if let Some(to) = opts.date_to {
                    if timestamp > to { return false; }
                }

                // Get name from pool
                let offset = self.name_offsets[idx] as usize;
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
            let offset = self.name_offsets[idx as usize] as usize;
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

    /// Backwards compatibility for search_with_ext
    pub fn search_with_ext(&self, query: &str, ext: Option<&str>) -> Vec<usize> {
        let opts = SearchOptions {
            query,
            ext_filter: ext,
            ..Default::default()
        };
        self.search(&opts)
    }
}
