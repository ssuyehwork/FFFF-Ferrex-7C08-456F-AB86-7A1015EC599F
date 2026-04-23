use std::fs::File;
use std::io::{Write, Result, Error, ErrorKind};
use memmap2::Mmap;
use crc32fast::Hasher;

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;
pub const RECORD_SIZE: usize = 40;

/// SoA Layout Record as defined in AGENTS.md Phase 4
/// Total 40 bytes per record
#[repr(C, packed)]
#[derive(Debug, Copy, Clone, Default)]
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
) -> Result<()> {
    let record_count = records.len() as u64;
    let string_pool_size = string_pool.len() as u64;
    
    let total_size = HEADER_SIZE + (records.len() * RECORD_SIZE) + string_pool.len();
    let mut buffer = Vec::with_capacity(total_size);
    
    // Header (Phase 4)
    buffer.extend_from_slice(MAGIC); // 0x00
    buffer.extend_from_slice(&VERSION.to_le_bytes()); // 0x04
    buffer.extend_from_slice(&record_count.to_le_bytes()); // 0x08
    buffer.extend_from_slice(&string_pool_size.to_le_bytes()); // 0x10
    buffer.extend_from_slice(&usn_watermark.to_le_bytes()); // 0x18
    buffer.extend_from_slice(&volume_serial.to_le_bytes()); // 0x20
    buffer.extend_from_slice(&0u32.to_le_bytes()); // 0x28 CRC placeholder
    buffer.extend_from_slice(&0u32.to_le_bytes()); // 0x2C Reserved
    
    // Record Block (Starts at 0x30)
    for rec in records {
        let bytes: [u8; RECORD_SIZE] = unsafe { std::mem::transmute(*rec) };
        buffer.extend_from_slice(&bytes);
    }
    
    // String Pool Block
    buffer.extend_from_slice(string_pool);
    
    // Calculate CRC32
    let mut hasher = Hasher::new();
    hasher.update(&buffer);
    let checksum = hasher.finalize();
    
    // Write CRC to buffer at 0x28
    let crc_bytes = checksum.to_le_bytes();
    buffer[0x28..0x2C].copy_from_slice(&crc_bytes);
    
    let mut file = File::create(path)?;
    file.write_all(&buffer)?;
    file.sync_all()?;
    
    Ok(())
}

pub struct LoadedIndex {
    pub mmap: Mmap,
    pub record_count: usize,
    pub string_pool_offset: usize,
    pub usn_watermark: u64,
    pub volume_serial: u64,
}

impl LoadedIndex {
    pub fn load(path: &str) -> Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        
        if mmap.len() < HEADER_SIZE {
            return Err(Error::new(ErrorKind::InvalidData, "File too small"));
        }
        
        if &mmap[0..4] != MAGIC {
            return Err(Error::new(ErrorKind::InvalidData, "Invalid magic"));
        }

        let version = u32::from_le_bytes(mmap[0x04..0x08].try_into().unwrap());
        if version != VERSION {
            return Err(Error::new(ErrorKind::InvalidData, "Unsupported version"));
        }
        
        let stored_crc = u32::from_le_bytes(mmap[0x28..0x2C].try_into().unwrap());
        let mut hasher = Hasher::new();
        hasher.update(&mmap[0..0x28]);
        hasher.update(&[0u8; 4]); // CRC field as 0
        hasher.update(&mmap[0x2C..]);
        let calculated_crc = hasher.finalize();
        
        if stored_crc != calculated_crc {
            return Err(Error::new(ErrorKind::InvalidData, "CRC mismatch"));
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
    
    pub fn get_records(&self) -> &[FileRecord] {
        unsafe {
            std::slice::from_raw_parts(
                self.mmap.as_ptr().add(HEADER_SIZE) as *const FileRecord,
                self.record_count
            )
        }
    }
    
    pub fn get_string_pool(&self) -> &[u8] {
        &self.mmap[self.string_pool_offset..]
    }
}
