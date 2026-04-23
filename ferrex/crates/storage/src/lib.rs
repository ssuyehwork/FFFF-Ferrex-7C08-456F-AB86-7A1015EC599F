use std::fs::File;
use std::io::{Write, Result, Error, ErrorKind};
use memmap2::Mmap;
use crc32fast::Hasher;

pub const MAGIC: &[u8; 4] = b"FIDX";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 48;

/// Phase 3c & 4: Structure of Arrays (SoA) layout.
/// Data is packed as: [Header] [FRNs] [ParentFRNs] [Sizes] [Timestamps] [NameOffsets] [Flags] [StringPool]
pub struct LoadedIndex {
    pub mmap: Mmap,
    pub record_count: usize,
    pub volume_serial: u64,
    pub usn_watermark: u64,
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

        let record_count = u64::from_le_bytes(mmap[8..16].try_into().unwrap()) as usize;
        let usn_watermark = u64::from_le_bytes(mmap[0x18..0x20].try_into().unwrap());
        let volume_serial = u64::from_le_bytes(mmap[0x20..0x28].try_into().unwrap());
        
        // Verify CRC
        let stored_crc = u32::from_le_bytes(mmap[0x28..0x2C].try_into().unwrap());
        let mut hasher = Hasher::new();
        hasher.update(&mmap[0..0x28]);
        hasher.update(&[0u8; 4]);
        hasher.update(&mmap[0x2C..]);
        if stored_crc != hasher.finalize() {
            return Err(Error::new(ErrorKind::InvalidData, "CRC mismatch"));
        }
        
        Ok(Self {
            mmap,
            record_count,
            volume_serial,
            usn_watermark,
        })
    }

    // SoA Slice Accessors
    pub fn frns(&self) -> &[u64] { self.get_slice(0, 8) }
    pub fn parent_frns(&self) -> &[u64] { self.get_slice(8, 8) }
    pub fn sizes(&self) -> &[u64] { self.get_slice(16, 8) }
    pub fn timestamps(&self) -> &[u64] { self.get_slice(24, 8) }
    pub fn name_offsets(&self) -> &[u32] { self.get_slice(32, 4) }
    pub fn flags(&self) -> &[u32] { self.get_slice(36, 4) }

    fn get_slice<T>(&self, offset_in_record: usize, _size_of_t: usize) -> &[T] {
        let base = HEADER_SIZE + (self.record_count * offset_in_record);
        unsafe {
            std::slice::from_raw_parts(
                self.mmap.as_ptr().add(base) as *const T,
                self.record_count
            )
        }
    }

    pub fn get_string_pool(&self) -> &[u8] {
        let pool_offset = HEADER_SIZE + (self.record_count * 40); // 40 = 8+8+8+8+4+4
        &self.mmap[pool_offset..]
    }
}

pub fn save_index_soa(
    path: &str,
    frns: &[u64],
    parents: &[u64],
    sizes: &[u64],
    times: &[u64],
    offsets: &[u32],
    flags: &[u32],
    pool: &[u8],
    usn: u64,
    serial: u64,
) -> Result<()> {
    let count = frns.len();
    let total_size = HEADER_SIZE + (count * 40) + pool.len();
    let mut buffer = Vec::with_capacity(total_size);

    // Header
    buffer.extend_from_slice(MAGIC);
    buffer.extend_from_slice(&1u32.to_le_bytes()); // Version
    buffer.extend_from_slice(&(count as u64).to_le_bytes());
    buffer.extend_from_slice(&(pool.len() as u64).to_le_bytes());
    buffer.extend_from_slice(&usn.to_le_bytes());
    buffer.extend_from_slice(&serial.to_le_bytes());
    buffer.extend_from_slice(&[0u8; 8]); // CRC + Reserved

    // Write fields in SoA order
    for f in frns { buffer.extend_from_slice(&f.to_le_bytes()); }
    for p in parents { buffer.extend_from_slice(&p.to_le_bytes()); }
    for s in sizes { buffer.extend_from_slice(&s.to_le_bytes()); }
    for t in times { buffer.extend_from_slice(&t.to_le_bytes()); }
    for o in offsets { buffer.extend_from_slice(&o.to_le_bytes()); }
    for fl in flags { buffer.extend_from_slice(&fl.to_le_bytes()); }

    buffer.extend_from_slice(pool);

    // CRC
    let mut hasher = Hasher::new();
    hasher.update(&buffer);
    let crc = hasher.finalize();
    buffer[0x28..0x2C].copy_from_slice(&crc.to_le_bytes());

    let mut file = File::create(path)?;
    file.write_all(&buffer)?;
    file.sync_all()?;
    Ok(())
}
