use std::fs::File;
use std::io::Write;
use memmap2::Mmap;
use crc32fast::Hasher;

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;
pub const RECORD_SIZE: usize = 40;

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
    records: &[FileRecord],
    string_pool: &[u8],
    usn_watermark: u64,
    volume_serial: u64,
) -> std::io::Result<()> {
    let record_count = records.len() as u64;
    let string_pool_size = string_pool.len() as u64;

    let total_size = HEADER_SIZE + (records.len() * RECORD_SIZE) + string_pool.len();
    let mut buffer = Vec::with_capacity(total_size);

    buffer.extend_from_slice(MAGIC);
    buffer.extend_from_slice(&VERSION.to_le_bytes());
    buffer.extend_from_slice(&record_count.to_le_bytes());
    buffer.extend_from_slice(&string_pool_size.to_le_bytes());
    buffer.extend_from_slice(&usn_watermark.to_le_bytes());
    buffer.extend_from_slice(&volume_serial.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());
    buffer.extend_from_slice(&0u32.to_le_bytes());

    for rec in records {
        let bytes: [u8; RECORD_SIZE] = unsafe { std::mem::transmute(*rec) };
        buffer.extend_from_slice(&bytes);
    }
    buffer.extend_from_slice(string_pool);

    let mut hasher = Hasher::new();
    hasher.update(&buffer);
    let checksum = hasher.finalize();
    buffer[0x28..0x2C].copy_from_slice(&checksum.to_le_bytes());

    let mut file = File::create(path)?;
    file.write_all(&buffer)?;
    file.sync_all()?;
    Ok(())
}

pub struct LoadedIndex {
    pub mmap: Mmap,
    pub record_count: usize,
    pub string_pool_offset: usize,
}

impl LoadedIndex {
    pub fn load(path: &str) -> std::io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        if mmap.len() < HEADER_SIZE || &mmap[0..4] != MAGIC {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "Invalid index file"));
        }
        let record_count = u64::from_le_bytes(mmap[8..16].try_into().unwrap()) as usize;
        Ok(Self {
            mmap,
            record_count,
            string_pool_offset: HEADER_SIZE + (record_count * RECORD_SIZE),
        })
    }

    pub fn get_records(&self) -> &[FileRecord] {
        unsafe { std::slice::from_raw_parts(self.mmap.as_ptr().add(HEADER_SIZE) as *const FileRecord, self.record_count) }
    }

    pub fn get_string_pool(&self) -> &[u8] {
        &self.mmap[self.string_pool_offset..]
    }
}
