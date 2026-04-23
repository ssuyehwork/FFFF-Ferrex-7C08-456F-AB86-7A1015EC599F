use clap::{Parser, Subcommand};
use indexer::{acquire_privileges, get_ntfs_volumes, get_volume_serial, MftScanner};
use storage::{save_index_soa, LoadedIndex};
use search::{Searcher, get_name_from_pool};
use std::collections::HashMap;
use std::path::Path;

#[derive(Parser)]
#[command(name = "ferrex")]
#[command(about = "Ferrex - High Performance NTFS Indexer", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Force full MFT scan and save index (.idx)
    Index {
        /// Drive to index (e.g., C). If omitted, all NTFS drives are indexed.
        drive: Option<String>,
    },
    /// Load index via mmap and search for substring
    Search {
        /// Search query
        query: String,
    },
    /// Monitor USN journal for incremental updates
    Watch,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    match &cli.command {
        Commands::Index { drive } => {
            acquire_privileges()?;
            let volumes = if let Some(d) = drive {
                vec![format!("{}:", d.to_uppercase().replace(":", ""))]
            } else {
                get_ntfs_volumes()
            };

            for vol in volumes {
                println!("Scanning volume {}...", vol);
                let scanner = MftScanner::new(&vol)?;
                let (entries, usn) = scanner.scan()?;
                println!("Found {} entries", entries.len());

                let mut frns = Vec::new();
                let mut parents = Vec::new();
                let mut sizes = Vec::new();
                let mut times = Vec::new();
                let mut offsets = Vec::new();
                let mut flags = Vec::new();
                let mut pool = Vec::new();

                for e in entries {
                    frns.push(e.frn);
                    parents.push(e.parent_frn);
                    sizes.push(e.file_size);
                    times.push(e.modified);
                    offsets.push(pool.len() as u32);
                    flags.push(e.flags);
                    pool.extend_from_slice(e.name.as_bytes());
                    pool.push(0);
                }

                let serial = get_volume_serial(&vol);
                let idx_path = format!("{}_drive.idx", vol.chars().next().unwrap().to_lowercase());
                save_index_soa(&idx_path, &frns, &parents, &sizes, &times, &offsets, &flags, &pool, usn, serial)?;
                println!("Successfully saved index to {}", idx_path);
            }
        }
        Commands::Search { query } => {
            let drives = vec!['c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'];
            let mut loaded_stores = Vec::new();

            for d in drives {
                let idx_path = format!("{}_drive.idx", d);
                if Path::new(&idx_path).exists() {
                    if let Ok(loaded) = LoadedIndex::load(&idx_path) {
                        println!("Loaded index: {} ({} records)", idx_path, loaded.record_count);
                        loaded_stores.push((d.to_uppercase().to_string(), loaded));
                    }
                }
            }

            if loaded_stores.is_empty() {
                println!("Error: No index files found. Please run 'ferrex index' first.");
                return Ok(());
            }

            for (drive_letter, store) in loaded_stores {
                let frns = store.frns();
                let parents = store.parent_frns();
                let offsets = store.name_offsets();
                let pool = store.get_string_pool();
                
                let mut frn_to_idx = HashMap::with_capacity(store.record_count);
                for (i, &frn) in frns.iter().enumerate() {
                    frn_to_idx.insert(frn, i);
                }

                let mut sorted_idx: Vec<u32> = (0..store.record_count as u32).collect();
                sorted_idx.sort_by_key(|&i| {
                    get_name_from_pool(pool, offsets[i as usize] as usize).to_lowercase()
                });

                let searcher = Searcher::new(frns, offsets, pool, &sorted_idx);
                let matches = searcher.search(query);

                for idx in matches {
                    let full_path = resolve_full_path(&drive_letter, idx, frns, parents, offsets, pool, &frn_to_idx);
                    println!("{}", full_path);
                }
            }
        }
        Commands::Watch => {
            println!("USN Watcher mode not fully implemented in this CLI version. Use GUI or check back later.");
        }
    }

    Ok(())
}

fn resolve_full_path(
    drive: &str,
    idx: usize,
    frns: &[u64],
    parents: &[u64],
    offsets: &[u32],
    pool: &[u8],
    frn_to_idx: &HashMap<u64, usize>
) -> String {
    let mut path_parts = Vec::new();
    let mut current_idx = Some(idx);

    while let Some(idx) = current_idx {
        let name = get_name_from_pool(pool, offsets[idx] as usize);
        path_parts.push(name);

        let p_frn = parents[idx];
        let c_frn = frns[idx];

        if c_frn == 5 || p_frn == 0 {
            break;
        }

        current_idx = frn_to_idx.get(&p_frn).copied();
    }

    path_parts.reverse();
    format!("{}:\\{}", drive, path_parts.join("\\"))
}
