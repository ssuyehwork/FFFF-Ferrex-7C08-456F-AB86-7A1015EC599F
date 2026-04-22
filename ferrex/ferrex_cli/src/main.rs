use clap::{Parser, Subcommand};
use indexer::{acquire_privileges, get_ntfs_volumes, MftScanner};
use storage::{save_index, LoadedIndex, FileRecord};
use search::Searcher;
use std::collections::HashMap;
use std::path::Path;

#[derive(Parser)]
#[command(name = "ferrex")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    Index { drive: Option<String> },
    Search { query: String },
    Watch,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    match &cli.command {
        Commands::Index { drive } => {
            acquire_privileges()?;
            let volumes = if let Some(d) = drive { vec![format!("{}:", d.to_uppercase())] } else { get_ntfs_volumes() };
            for vol in volumes {
                println!("Scanning {}...", vol);
                let scanner = MftScanner::new(&vol)?;
                let raw_entries = scanner.scan()?;
                let mut string_pool = Vec::new();
                let mut records = Vec::with_capacity(raw_entries.len());
                for entry in raw_entries {
                    let name_offset = string_pool.len() as u32;
                    string_pool.extend_from_slice(entry.name.as_bytes());
                    string_pool.push(0);
                    records.push(FileRecord { frn: entry.frn, parent_frn: entry.parent_frn, size: entry.file_size, timestamp: entry.modified, name_offset, flags: entry.flags });
                }
                let idx_path = format!("{}_drive.idx", vol.chars().next().unwrap().to_lowercase());
                save_index(&idx_path, &records, &string_pool, 0, 0)?;
            }
        }
        Commands::Search { query } => {
            let drives = b'c'..=b'z';
            for d in drives {
                let idx_path = format!("{}_drive.idx", d as char);
                if Path::new(&idx_path).exists() {
                    if let Ok(store) = LoadedIndex::load(&idx_path) {
                        let records = store.get_records();
                        let pool = store.get_string_pool();
                        let mut frn_to_idx = HashMap::new();
                        for (i, rec) in records.iter().enumerate() { frn_to_idx.insert(rec.frn, i); }
                        let matches = Searcher::new(records, pool).search(query);
                        for &idx in &matches {
                            println!("{}", resolve_full_path(&(d as char).to_string().to_uppercase(), idx, records, pool, &frn_to_idx));
                        }
                    }
                }
            }
        }
        _ => {}
    }
    Ok(())
}

fn resolve_full_path(drive: &str, idx: usize, records: &[FileRecord], pool: &[u8], frn_to_idx: &HashMap<u64, usize>) -> String {
    let mut parts = Vec::new();
    let mut curr = Some(idx);
    while let Some(i) = curr {
        let rec = &records[i];
        let name = String::from_utf8_lossy(&pool[rec.name_offset as usize..]).split('\0').next().unwrap().to_string();
        parts.push(name);
        if rec.frn == 5 || rec.parent_frn == 0 { break; }
        curr = frn_to_idx.get(&rec.parent_frn).copied();
    }
    parts.reverse();
    format!("{}:\\{}", drive, parts.join("\\"))
}
