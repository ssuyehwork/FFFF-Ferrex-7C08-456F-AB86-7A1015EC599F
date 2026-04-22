use windows::{
    core::*,
    Win32::Foundation::*,
    Win32::Storage::FileSystem::*,
    Win32::System::Ioctl::*,
    Win32::Security::*,
    Win32::System::Threading::*,
    Win32::System::IO::DeviceIoControl,
};

pub fn acquire_privileges() -> Result<()> {
    unsafe {
        let mut h_token = HANDLE::default();
        OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &mut h_token,
        )?;

        let mut tkp = TOKEN_PRIVILEGES::default();
        tkp.PrivilegeCount = 1;

        let mut luid = LUID::default();
        LookupPrivilegeValueW(None, w!("SeBackupPrivilege"), &mut luid)?;
        tkp.Privileges[0].Luid = luid;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        AdjustTokenPrivileges(
            h_token,
            false,
            Some(&tkp),
            0,
            None,
            None,
        )?;

        Ok(())
    }
}

pub fn get_ntfs_volumes() -> Vec<String> {
    let mut volumes = Vec::new();
    unsafe {
        let drives = GetLogicalDrives();
        for i in 0..26 {
            if (drives >> i) & 1 == 1 {
                let drive_letter = format!("{}:\\", (b'A' + i) as char);
                let mut fs_name = [0u16; 256];
                if GetVolumeInformationW(
                    PCWSTR(HSTRING::from(&drive_letter).as_ptr()),
                    None,
                    None,
                    None,
                    None,
                    Some(&mut fs_name),
                ).is_ok() {
                    let fs_name_str = String::from_utf16_lossy(&fs_name);
                    if fs_name_str.contains("NTFS") {
                        volumes.push(format!("{}:", (b'A' + i) as char));
                    }
                }
            }
        }
    }
    volumes
}

#[derive(Debug, Clone)]
pub struct RawEntry {
    pub frn: u64,
    pub parent_frn: u64,
    pub file_size: u64,
    pub modified: u64,
    pub flags: u32,
    pub name: String,
}

pub struct MftScanner {
    volume_handle: HANDLE,
}

impl MftScanner {
    pub fn new(volume: &str) -> Result<Self> {
        let path = format!("\\\\.\\{}", volume);
        unsafe {
            let handle = CreateFileW(
                &HSTRING::from(&path),
                GENERIC_READ.0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING,
                None,
            )?;
            Ok(Self {
                volume_handle: handle,
            })
        }
    }

    pub fn scan(&self) -> Result<Vec<RawEntry>> {
        let mut entries = Vec::new();
        unsafe {
            let mut bytes_returned = 0;
            let mut journal_data = USN_JOURNAL_DATA_V0::default();

            DeviceIoControl(
                self.volume_handle,
                FSCTL_QUERY_USN_JOURNAL,
                None,
                0,
                Some(&mut journal_data as *mut _ as *mut _),
                std::mem::size_of::<USN_JOURNAL_DATA_V0>() as u32,
                Some(&mut bytes_returned),
                None,
            )?;

            let mut med = MFT_ENUM_DATA_V0 {
                StartFileReferenceNumber: 0,
                LowUsn: 0,
                HighUsn: journal_data.NextUsn,
            };

            let mut buffer = [0u8; 65536];
            loop {
                let res = DeviceIoControl(
                    self.volume_handle,
                    FSCTL_ENUM_USN_DATA,
                    Some(&med as *const _ as *const _),
                    std::mem::size_of::<MFT_ENUM_DATA_V0>() as u32,
                    Some(buffer.as_mut_ptr() as *mut _),
                    buffer.len() as u32,
                    Some(&mut bytes_returned),
                    None,
                );

                if let Err(e) = res {
                    if e.code() == ERROR_HANDLE_EOF.into() {
                        break;
                    }
                    return Err(e);
                }

                if bytes_returned < 8 {
                    break;
                }

                let next_frn = u64::from_le_bytes(buffer[0..8].try_into().unwrap());
                let mut offset = 8;
                while offset < bytes_returned as usize {
                    let record = &*(buffer.as_ptr().add(offset) as *const USN_RECORD_V2);

                    let name_ptr = buffer.as_ptr().add(offset + record.FileNameOffset as usize) as *const u16;
                    let name_len = record.FileNameLength as usize / 2;
                    let name_slice = std::slice::from_raw_parts(name_ptr, name_len);
                    let name = String::from_utf16_lossy(name_slice);

                    entries.push(RawEntry {
                        frn: record.FileReferenceNumber,
                        parent_frn: record.ParentFileReferenceNumber,
                        file_size: 0,
                        modified: record.TimeStamp as u64,
                        flags: record.FileAttributes,
                        name,
                    });

                    offset += record.RecordLength as usize;
                }

                if next_frn == 0 { break; }
                med.StartFileReferenceNumber = next_frn;
            }
        }
        Ok(entries)
    }
}

impl Drop for MftScanner {
    fn drop(&mut self) {
        unsafe { let _ = CloseHandle(self.volume_handle); }
    }
}
