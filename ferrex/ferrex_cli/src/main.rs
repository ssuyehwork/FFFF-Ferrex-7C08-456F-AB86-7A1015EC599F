use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner, RawEntry};
use storage::{save_index, FileRecord};
use std::collections::HashMap;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Ferrex - High Performance NTFS Indexer");

    acquire_privileges()?;
    let volumes = get_ntfs_volumes();
    println!("Found NTFS volumes: {:?}", volumes);

    for vol in volumes {
        println!("Scanning volume {}...", vol);
        let scanner = MftScanner::new(&vol)?;
        let raw_entries = scanner.scan()?;
        println!("Found {} entries", raw_entries.len());

        // Process into IndexStore format
        let mut string_pool = Vec::new();
        let mut records = Vec::with_capacity(raw_entries.len());

        for entry in raw_entries {
            let name_offset = string_pool.len() as u32;
            string_pool.extend_from_slice(entry.name.as_bytes());
            string_pool.push(0); // Null terminator

            records.push(FileRecord {
                frn: entry.frn,
                parent_frn: entry.parent_frn,
                size: entry.file_size,
                timestamp: entry.modified,
                name_offset,
                flags: entry.flags,
            });
        }

        let idx_path = format!("{}_drive.idx", vol.chars().next().unwrap().to_lowercase());
        save_index(&idx_path, &records, &string_pool, 0, 0)?;
        println!("Saved index to {}", idx_path);
    }

    Ok(())
}
