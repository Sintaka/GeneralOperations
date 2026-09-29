use std::{
    fs::{self, OpenOptions},
    io::{self, Write},
    path::Path,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use serde::{Deserialize, Serialize};

use crate::{model::ScriptInfo, paths};

const CACHE_VERSION: u32 = 3;
const CACHE_FILE_NAME: &str = "script-catalog-cache.json";
static NEXT_TEMP_FILE: AtomicU64 = AtomicU64::new(1);

/// Reads the shared script catalog CLI's JSON result for every Python script under `root`.
pub fn load_scripts(root: &Path) -> io::Result<Vec<ScriptInfo>> {
    let (executable_base, catalog) = catalog_location()?;
    load_scripts_cached(root, &executable_base, &catalog, run_catalog)
}

/// Refreshes the catalog before executing a script, without trusting the UI cache.
pub fn load_scripts_fresh(root: &Path) -> io::Result<Vec<ScriptInfo>> {
    let (_, catalog) = catalog_location()?;
    run_catalog(&catalog, root)
}

fn catalog_location() -> io::Result<(std::path::PathBuf, std::path::PathBuf)> {
    let executable = std::env::current_exe().map_err(|error| {
        io::Error::other(format!("Cannot locate the Tauri executable: {error}"))
    })?;
    let executable_base = executable.parent().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "Cannot determine the Tauri executable directory.",
        )
    })?;
    let catalog = paths::resolve_script_catalog(
        executable_base,
        &paths::repository_root(),
        cfg!(debug_assertions),
    )
    .map_err(|error| io::Error::new(io::ErrorKind::NotFound, error))?;
    Ok((executable_base.to_path_buf(), catalog))
}

fn run_catalog(catalog: &Path, root: &Path) -> io::Result<Vec<ScriptInfo>> {
    let output = Command::new(catalog)
        .arg("--root")
        .arg(root)
        .output()
        .map_err(|error| {
            io::Error::other(format!(
                "Cannot run shared script catalog {}: {error}",
                catalog.display()
            ))
        })?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let detail = stderr.trim();
        let message = if detail.is_empty() {
            format!("Shared script catalog exited with {}.", output.status)
        } else {
            format!(
                "Shared script catalog exited with {}: {detail}",
                output.status
            )
        };
        return Err(io::Error::other(message));
    }

    let json = String::from_utf8(output.stdout).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Shared script catalog stdout is not UTF-8: {error}"),
        )
    })?;
    serde_json::from_str(&json).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Shared script catalog returned invalid JSON: {error}"),
        )
    })
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct ModifiedTime {
    seconds: i64,
    nanoseconds: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct FileStamp {
    path: String,
    size: u64,
    modified: ModifiedTime,
    content_hash: String,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct ScriptFileStamp {
    relative_path: String,
    size: u64,
    modified: ModifiedTime,
    content_hash: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct CacheSnapshot {
    scripts_root: String,
    catalog: FileStamp,
    files: Vec<ScriptFileStamp>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct CatalogCache {
    version: u32,
    scripts_root: String,
    catalog: FileStamp,
    files: Vec<ScriptFileStamp>,
    scripts: Vec<ScriptInfo>,
}

impl CatalogCache {
    fn matches(&self, snapshot: &CacheSnapshot) -> bool {
        self.version == CACHE_VERSION
            && self.scripts_root == snapshot.scripts_root
            && self.catalog == snapshot.catalog
            && self.files == snapshot.files
    }
}

fn load_scripts_cached<F>(
    root: &Path,
    executable_base: &Path,
    catalog: &Path,
    mut run: F,
) -> io::Result<Vec<ScriptInfo>>
where
    F: FnMut(&Path, &Path) -> io::Result<Vec<ScriptInfo>>,
{
    let cache_path = executable_base.join(CACHE_FILE_NAME);
    let before = cache_snapshot(root, catalog).ok();
    if let Some(snapshot) = before.as_ref() {
        if let Some(cache) = read_cache(&cache_path) {
            if cache.matches(snapshot) {
                return Ok(cache.scripts);
            }
        }
    }

    let scripts = run(catalog, root)?;
    if let Ok(snapshot) = cache_snapshot(root, catalog) {
        if before.as_ref().is_none_or(|previous| previous == &snapshot) {
            let cache = CatalogCache {
                version: CACHE_VERSION,
                scripts_root: snapshot.scripts_root,
                catalog: snapshot.catalog,
                files: snapshot.files,
                scripts: scripts.clone(),
            };
            let _ = write_cache_atomically(&cache_path, &cache);
        }
    }
    Ok(scripts)
}

fn cache_snapshot(root: &Path, catalog: &Path) -> io::Result<CacheSnapshot> {
    let canonical_root = fs::canonicalize(root)?;
    let scripts_root = path_string(&canonical_root)?;
    let catalog_path = fs::canonicalize(catalog)?;
    let catalog_stamp = file_stamp(&catalog_path, path_string(&catalog_path)?)?;
    let mut files = Vec::new();
    collect_script_files(&canonical_root, &canonical_root, &mut files)?;
    files.sort_by(|left, right| left.relative_path.cmp(&right.relative_path));
    Ok(CacheSnapshot {
        scripts_root,
        catalog: catalog_stamp,
        files,
    })
}

fn collect_script_files(
    root: &Path,
    directory: &Path,
    files: &mut Vec<ScriptFileStamp>,
) -> io::Result<()> {
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        if kind.is_symlink() {
            continue;
        }
        let path = entry.path();
        if kind.is_dir() {
            collect_script_files(root, &path, files)?;
        } else if kind.is_file()
            && path
                .extension()
                .and_then(|extension| extension.to_str())
                .map(|extension| extension.eq_ignore_ascii_case("py"))
                .unwrap_or(false)
        {
            let relative = path
                .strip_prefix(root)
                .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error.to_string()))?;
            let relative_path = path_string(relative)?.replace('\\', "/");
            let metadata = fs::metadata(&path)?;
            files.push(ScriptFileStamp {
                relative_path,
                size: metadata.len(),
                modified: modified_time(metadata.modified()?)?,
                content_hash: content_hash(&fs::read(&path)?),
            });
        }
    }
    Ok(())
}

// A stable content fingerprint catches edits that preserve both size and mtime.
fn content_hash(bytes: &[u8]) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")
}

fn file_stamp(path: &Path, stored_path: String) -> io::Result<FileStamp> {
    let metadata = fs::metadata(path)?;
    Ok(FileStamp {
        path: stored_path,
        size: metadata.len(),
        modified: modified_time(metadata.modified()?)?,
        content_hash: content_hash(&fs::read(path)?),
    })
}

fn modified_time(time: SystemTime) -> io::Result<ModifiedTime> {
    let (seconds, nanoseconds) = match time.duration_since(UNIX_EPOCH) {
        Ok(duration) => (
            i64::try_from(duration.as_secs())
                .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error.to_string()))?,
            duration.subsec_nanos(),
        ),
        Err(error) => {
            let duration = error.duration();
            let seconds = i64::try_from(duration.as_secs())
                .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error.to_string()))?;
            if duration.subsec_nanos() == 0 {
                (-seconds, 0)
            } else {
                (
                    seconds
                        .checked_neg()
                        .and_then(|value| value.checked_sub(1))
                        .ok_or_else(|| {
                            io::Error::new(
                                io::ErrorKind::InvalidData,
                                "File timestamp is out of range.",
                            )
                        })?,
                    1_000_000_000 - duration.subsec_nanos(),
                )
            }
        }
    };
    Ok(ModifiedTime {
        seconds,
        nanoseconds,
    })
}

fn path_string(path: &Path) -> io::Result<String> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Path is not valid UTF-8."))
}

fn read_cache(path: &Path) -> Option<CatalogCache> {
    let bytes = fs::read(path).ok()?;
    let cache: CatalogCache = serde_json::from_slice(&bytes).ok()?;
    (cache.version == CACHE_VERSION).then_some(cache)
}

fn write_cache_atomically(path: &Path, cache: &CatalogCache) -> io::Result<()> {
    let parent = path.parent().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "Cache path has no parent directory.",
        )
    })?;
    let contents = serde_json::to_vec(cache)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error.to_string()))?;
    for _ in 0..8 {
        let sequence = NEXT_TEMP_FILE.fetch_add(1, Ordering::Relaxed);
        let temp_path = parent.join(format!(
            ".{CACHE_FILE_NAME}.{}.{}.tmp",
            std::process::id(),
            sequence
        ));
        let mut file = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp_path)
        {
            Ok(file) => file,
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error),
        };
        let write_result = file.write_all(&contents).and_then(|()| file.sync_all());
        drop(file);
        if let Err(error) = write_result {
            let _ = fs::remove_file(&temp_path);
            return Err(error);
        }
        if let Err(error) = fs::rename(&temp_path, path) {
            let _ = fs::remove_file(&temp_path);
            return Err(error);
        }
        return Ok(());
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "Could not allocate a temporary catalog cache file.",
    ))
}

#[cfg(test)]
mod tests {
    use super::{load_scripts_cached, read_cache, CACHE_FILE_NAME};
    use crate::model::ScriptInfo;
    use std::{
        cell::Cell,
        fs, io,
        path::{Path, PathBuf},
        sync::atomic::{AtomicU64, Ordering},
    };

    static NEXT_TEST_DIR: AtomicU64 = AtomicU64::new(1);

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "general-operations-catalog-cache-{}-{}",
                std::process::id(),
                NEXT_TEST_DIR.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&path).expect("test directory should be created");
            Self(path)
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    struct Fixture {
        base: TestDirectory,
        app: PathBuf,
        root: PathBuf,
        catalog: PathBuf,
    }

    impl Fixture {
        fn new() -> Self {
            let base = TestDirectory::new();
            let app = base.0.join("app");
            let root = base.0.join("scripts");
            let catalog = app.join("tools/go_script_catalog/go_script_catalog.exe");
            fs::create_dir_all(catalog.parent().unwrap()).expect("catalog directory should exist");
            fs::create_dir_all(&root).expect("script directory should exist");
            fs::write(&catalog, "catalog-v1").expect("catalog marker should exist");
            fs::write(root.join("one.py"), "print(1)").expect("script should exist");
            Self {
                base,
                app,
                root,
                catalog,
            }
        }

        fn cache_path(&self) -> PathBuf {
            self.app.join(CACHE_FILE_NAME)
        }
    }

    fn script_info(id: &str) -> ScriptInfo {
        ScriptInfo {
            id: id.to_owned(),
            name: id.to_owned(),
            group: String::new(),
            description: String::new(),
            accepts: String::new(),
            extensions: Vec::new(),
            multi: false,
            requires: Vec::new(),
            destructive: None,
            host: "python".to_owned(),
            blender_path: None,
            exe: None,
            params: Vec::new(),
            valid: true,
            errors: Vec::new(),
        }
    }

    fn load_with<F>(fixture: &Fixture, run: F) -> Vec<ScriptInfo>
    where
        F: FnMut(&Path, &Path) -> io::Result<Vec<ScriptInfo>>,
    {
        load_scripts_cached(&fixture.root, &fixture.app, &fixture.catalog, run)
            .expect("catalog load should succeed")
    }

    #[test]
    fn first_load_writes_cache_and_hit_skips_catalog_process() {
        let fixture = Fixture::new();
        let calls = Cell::new(0);
        let first = load_scripts_cached(
            &fixture.root,
            &fixture.app,
            &fixture.catalog,
            |_: &Path, _: &Path| {
                calls.set(calls.get() + 1);
                Ok(vec![script_info("first")])
            },
        )
        .expect("initial catalog load should succeed");
        assert_eq!(first[0].id, "first");
        let second = load_scripts_cached(
            &fixture.root,
            &fixture.app,
            &fixture.catalog,
            |_: &Path, _: &Path| panic!("cache hit must not launch the catalog process"),
        )
        .expect("cached catalog load should succeed");
        assert_eq!(second, first);
        assert_eq!(calls.get(), 1);

        let cache = read_cache(&fixture.cache_path()).expect("version 3 cache should be written");
        assert_eq!(cache.version, 3);
        assert_eq!(cache.files.len(), 1);
        assert_eq!(cache.files[0].relative_path, "one.py");
        assert_eq!(
            cache.catalog.path,
            fixture.catalog.canonicalize().unwrap().to_str().unwrap()
        );
        assert_eq!(
            cache.scripts_root,
            fixture.root.canonicalize().unwrap().to_str().unwrap()
        );
    }

    #[test]
    fn additions_renames_edits_and_deletions_invalidate_cache() {
        let fixture = Fixture::new();
        let calls = Cell::new(0);
        let mut run = |_: &Path, _: &Path| {
            calls.set(calls.get() + 1);
            Ok(vec![script_info(&format!("catalog-{}", calls.get()))])
        };
        assert_eq!(load_with(&fixture, &mut run)[0].id, "catalog-1");
        assert_eq!(
            load_with(&fixture, |_: &Path, _: &Path| panic!(
                "unchanged snapshot should hit cache"
            ))[0]
                .id,
            "catalog-1"
        );
        fs::write(fixture.root.join("one.py"), "print('longer')").unwrap();
        assert_eq!(load_with(&fixture, &mut run)[0].id, "catalog-2");
        fs::rename(fixture.root.join("one.py"), fixture.root.join("renamed.py")).unwrap();
        assert_eq!(load_with(&fixture, &mut run)[0].id, "catalog-3");
        fs::create_dir(fixture.root.join("nested")).unwrap();
        fs::write(fixture.root.join("nested/extra.PY"), "print(2)").unwrap();
        assert_eq!(load_with(&fixture, &mut run)[0].id, "catalog-4");
        fs::remove_file(fixture.root.join("renamed.py")).unwrap();
        assert_eq!(load_with(&fixture, &mut run)[0].id, "catalog-5");
        assert_eq!(calls.get(), 5);
    }

    #[test]
    fn same_size_edit_with_restored_mtime_invalidates_cache() {
        let fixture = Fixture::new();
        let script = fixture.root.join("one.py");
        let original_modified = fs::metadata(&script).unwrap().modified().unwrap();
        assert_eq!(
            load_with(&fixture, |_: &Path, _: &Path| Ok(vec![script_info("old")]))[0].id,
            "old"
        );

        fs::write(&script, "print(2)").unwrap();
        fs::OpenOptions::new()
            .write(true)
            .open(&script)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(original_modified))
            .unwrap();
        assert_eq!(
            fs::metadata(&script).unwrap().modified().unwrap(),
            original_modified
        );

        assert_eq!(
            load_with(&fixture, |_: &Path, _: &Path| Ok(vec![script_info("new")]))[0].id,
            "new"
        );
        assert_eq!(
            load_with(&fixture, |_: &Path, _: &Path| panic!(
                "updated cache should hit"
            ))[0]
                .id,
            "new"
        );
    }

    #[test]
    fn damaged_cache_different_root_and_catalog_move_or_change_refresh() {
        let fixture = Fixture::new();
        fs::write(fixture.cache_path(), "{ damaged").unwrap();
        let calls = Cell::new(0);
        let mut run = |_: &Path, _: &Path| {
            calls.set(calls.get() + 1);
            Ok(vec![script_info(&format!("catalog-{}", calls.get()))])
        };
        let mut load = |root: &Path, app: &Path, catalog: &Path| {
            load_scripts_cached(root, app, catalog, &mut run).expect("catalog load should succeed")
        };

        assert_eq!(
            load(&fixture.root, &fixture.app, &fixture.catalog)[0].id,
            "catalog-1"
        );
        let other_root = fixture.base.0.join("other-scripts");
        fs::create_dir(&other_root).unwrap();
        fs::write(other_root.join("other.py"), "print(2)").unwrap();
        assert_eq!(
            load(&other_root, &fixture.app, &fixture.catalog)[0].id,
            "catalog-2"
        );

        let moved_app = fixture.base.0.join("moved-app");
        fs::rename(&fixture.app, &moved_app).unwrap();
        let moved_catalog = moved_app.join("tools/go_script_catalog/go_script_catalog.exe");
        assert_eq!(
            load(&other_root, &moved_app, &moved_catalog)[0].id,
            "catalog-3"
        );
        fs::write(&moved_catalog, "catalog-v2-with-different-size").unwrap();
        assert_eq!(
            load(&other_root, &moved_app, &moved_catalog)[0].id,
            "catalog-4"
        );
        assert_eq!(
            load_scripts_cached(
                &other_root,
                &moved_app,
                &moved_catalog,
                |_: &Path, _: &Path| {
                    panic!("updated cache should hit after replacing an existing cache file")
                }
            )
            .expect("updated cache should be readable")[0]
                .id,
            "catalog-4"
        );
        let previous_modified = fs::metadata(&moved_catalog).unwrap().modified().unwrap();
        fs::write(&moved_catalog, "catalog-v3-with-different-size").unwrap();
        fs::OpenOptions::new()
            .write(true)
            .open(&moved_catalog)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(previous_modified))
            .unwrap();
        assert_eq!(
            fs::metadata(&moved_catalog).unwrap().modified().unwrap(),
            previous_modified
        );
        assert_eq!(
            load(&other_root, &moved_app, &moved_catalog)[0].id,
            "catalog-5"
        );
        assert_eq!(calls.get(), 5);
    }

    #[test]
    fn cache_write_failure_does_not_break_catalog_load() {
        let fixture = Fixture::new();
        fs::create_dir(fixture.cache_path()).expect("cache path blocker should be created");
        let calls = Cell::new(0);
        let scripts = load_scripts_cached(
            &fixture.root,
            &fixture.app,
            &fixture.catalog,
            |_: &Path, _: &Path| {
                calls.set(calls.get() + 1);
                Ok(vec![script_info("loaded")])
            },
        )
        .expect("cache write failure must not break script display");
        assert_eq!(scripts[0].id, "loaded");
        assert_eq!(calls.get(), 1);
        assert!(fixture.cache_path().is_dir());
    }

    #[test]
    fn missing_root_still_calls_catalog_and_preserves_its_error() {
        let fixture = Fixture::new();
        let missing_root = fixture.base.0.join("missing");
        let error = load_scripts_cached(
            &missing_root,
            &fixture.app,
            &fixture.catalog,
            |_: &Path, root: &Path| {
                assert_eq!(root, missing_root.as_path());
                Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "catalog root error",
                ))
            },
        )
        .expect_err("catalog error should be returned");
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert_eq!(error.to_string(), "catalog root error");
    }
}
