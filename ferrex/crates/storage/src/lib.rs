use std::fs::File;
use std::io::Write;
use memmap2::Mmap;
use crc32fast::Hasher;
use std::collections::HashMap;

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;
pub const RECORD_SIZE: usize = 40;

/// Structure of Arrays (SoA) layout for in-memory IndexStore
pub struct IndexStore {
    pub frns: Vec<u64>,
    pub parent_frns: Vec<u64>,
    pub sizes: Vec<u64>,
    pub timestamps: Vec<u64>,
    pub flags: Vec<u32>,
    pub name_offsets: Vec<u32>,
    pub string_pool: Vec<u8>,
    pub usn_watermark: u64,
    pub volume_serial: u64,

    // Lookup structures (Rebuilt on load, not persisted)
    pub frn_to_idx: HashMap<u64, u32>,
    pub sorted_idx: Vec<u32>,
}

impl IndexStore {
    pub fn new() -> Self {
        Self {
            frns: Vec::new(),
            parent_frns: Vec::new(),
            sizes: Vec::new(),
            timestamps: Vec::new(),
            flags: Vec::new(),
            name_offsets: Vec::new(),
            string_pool: Vec::new(),
            usn_watermark: 0,
            volume_serial: 0,
            frn_to_idx: HashMap::new(),
            sorted_idx: Vec::new(),
        }
    }

    pub fn rebuild_lookups(&mut self) {
        self.frn_to_idx.clear();
        for (i, &frn) in self.frns.iter().enumerate() {
            self.frn_to_idx.insert(frn, i as u32);
        }

        // Phase 3c: Sorted index for binary search
        let mut idx: Vec<u32> = (0..self.frns.len() as u32).collect();
        idx.sort_by_cached_key(|&i| {
            let offset = self.name_offsets[i as usize] as usize;
            let slice = &self.string_pool[offset..];
            let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
            String::from_utf8_lossy(&slice[..end]).to_lowercase()
        });
        self.sorted_idx = idx;
    }

    pub fn get_path(&self, index: usize) -> String {
        let mut path_parts = Vec::new();
        let mut current_frn = self.parent_frns[index];

        // Phase 3b: Resolve full paths by walking up parent_frn chain
        // Root FRN is usually 5 on NTFS
        while current_frn != 0 && current_frn != 5 {
            if let Some(&idx) = self.frn_to_idx.get(&current_frn) {
                let offset = self.name_offsets[idx as usize] as usize;
                let slice = &self.string_pool[offset..];
                let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
                path_parts.push(String::from_utf8_lossy(&slice[..end]).to_string());
                current_frn = self.parent_frns[idx as usize];
            } else {
                path_parts.push("<ORPHAN>".to_string());
                break;
            }
        }

        path_parts.reverse();
        path_parts.join("\\")
    }
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone)]
pub struct FileRecord {
    pub frn: u64,
    pub parent_frn: u64,
    pub size: u64,
    pub timestamp: u64,
    pub name_offset: u32,
    pub flags: u32,
}

pub fn save_index(
    path: &str,
    store: &IndexStore,
) -> std::io::Result<()> {
    let record_count = store.frns.len();
    let string_pool_size = store.string_pool.len();
    
    let total_size = HEADER_SIZE + (record_count * RECORD_SIZE) + string_pool_size;
    let mut buffer = Vec::with_capacity(total_size);
    
    buffer.extend_from_slice(MAGIC);
    buffer.extend_from_slice(&VERSION.to_le_bytes());
    buffer.extend_from_slice(&(record_count as u64).to_le_bytes());
    buffer.extend_from_slice(&(string_pool_size as u64).to_le_bytes());
    buffer.extend_from_slice(&store.usn_watermark.to_le_bytes());
    buffer.extend_from_slice(&store.volume_serial.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());
    
    for i in 0..record_count {
        buffer.extend_from_slice(&store.frns[i].to_le_bytes());
        buffer.extend_from_slice(&store.parent_frns[i].to_le_bytes());
        buffer.extend_from_slice(&store.sizes[i].to_le_bytes());
        buffer.extend_from_slice(&store.timestamps[i].to_le_bytes());
        buffer.extend_from_slice(&store.name_offsets[i].to_le_bytes());
        buffer.extend_from_slice(&store.flags[i].to_le_bytes());
    }
    
    buffer.extend_from_slice(&store.string_pool);
    
    let mut hasher = Hasher::new();
    hasher.update(&buffer);
    let checksum = hasher.finalize();
    buffer[0x28..0x2C].copy_from_slice(&checksum.to_le_bytes());
    
    let mut file = File::create(path)?;
    file.write_all(&buffer)?;
    file.sync_all()?;
    Ok(())
}

pub struct MappedIndex {
    pub mmap: Mmap,
    pub record_count: usize,
    pub string_pool_offset: usize,
    pub usn_watermark: u64,
    pub volume_serial: u64,
}

impl MappedIndex {
    pub fn load(path: &str) -> std::io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        
        if mmap.len() < HEADER_SIZE {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "File too small"));
        }
        
        if &mmap[0..4] != MAGIC {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "Invalid magic"));
        }
        
        let stored_crc = u32::from_le_bytes(mmap[0x28..0x2C].try_into().unwrap());
        let mut hasher = Hasher::new();
        hasher.update(&mmap[0..0x28]);
        hasher.update(&[0u8; 4]);
        hasher.update(&mmap[0x2C..]);
        let calculated_crc = hasher.finalize();
        
        if stored_crc != calculated_crc {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "CRC mismatch"));
        }
        
        let record_count = u64::from_le_bytes(mmap[8..16].try_into().unwrap()) as usize;
        let usn_watermark = u64::from_le_bytes(mmap[0x18..0x20].try_into().unwrap());
        let volume_serial = u64::from_le_bytes(mmap[0x20..0x28].try_into().unwrap());
        
        Ok(Self {
            mmap,
            record_count,
            string_pool_offset: HEADER_SIZE + (record_count * RECORD_SIZE),
            usn_watermark,
            volume_serial,
        })
    }
}
