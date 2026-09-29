use std::{
    env,
    path::{Path, PathBuf},
};

/// Returns the repository root from this crate's fixed location under `ui/tauri2`.
pub fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..")
}

/// Locates the bundled script directory, with a repository fallback for Debug builds only.
pub fn resolve_scripts_dir(
    executable: &Path,
    debug_build: bool,
    repository_root: &Path,
) -> Result<PathBuf, String> {
    if let Some(base_dir) = executable.parent() {
        let bundled = base_dir.join("scripts");
        if bundled.is_dir() {
            return bundled.canonicalize().map_err(|error| {
                format!(
                    "Cannot open bundled scripts directory {}: {error}",
                    bundled.display()
                )
            });
        }
    }

    if debug_build {
        let development = repository_root.join("core/python/scripts");
        if development.is_dir() {
            return development.canonicalize().map_err(|error| {
                format!(
                    "Cannot open development scripts directory {}: {error}",
                    development.display()
                )
            });
        }
    }

    let packaged_hint = executable
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join("scripts");
    Err(format!(
        "Scripts directory not found at {}. Copy core/python/scripts to the executable's sibling scripts/ directory.",
        packaged_hint.display()
    ))
}

/// Returns the directory containing the current application executable.
pub fn executable_base(executable: &Path) -> Result<&Path, String> {
    executable
        .parent()
        .ok_or_else(|| "Cannot determine the application executable directory.".to_string())
}

/// Locates the shared script catalog executable used by both frontends.
/// Release builds only accept the bundled copy; Debug builds can use the CMake output.
pub fn resolve_script_catalog(
    executable_base: &Path,
    repository_root: &Path,
    debug_build: bool,
) -> Result<PathBuf, String> {
    let packaged = executable_base
        .join("tools")
        .join("go_script_catalog")
        .join("go_script_catalog.exe");
    if packaged.is_file() {
        return Ok(packaged);
    }

    if debug_build {
        let development =
            repository_root.join("build/tauri2-core-debug/core/cpp/go_script_catalog.exe");
        if development.is_file() {
            return Ok(development);
        }
    }

    Err(format!(
        "Shared script catalog was not found at {}{}.",
        packaged.display(),
        if debug_build {
            format!(
                " or {}",
                repository_root
                    .join("build/tauri2-core-debug/core/cpp/go_script_catalog.exe")
                    .display()
            )
        } else {
            String::new()
        }
    ))
}

/// Locates the shared Python wrapper beside the app or in the Debug source tree.
pub fn resolve_python_launcher(
    executable_base: &Path,
    repository_root: &Path,
    debug_build: bool,
) -> Result<PathBuf, String> {
    let packaged = executable_base.join("python-launcher.ps1");
    if packaged.is_file() {
        return Ok(packaged);
    }

    let development = repository_root.join("core/python/runtime/python-launcher.ps1");
    if debug_build && development.is_file() {
        return Ok(development);
    }

    Err(format!(
        "Python launcher was not found at {}{}.",
        packaged.display(),
        if debug_build {
            format!(" or {}", development.display())
        } else {
            String::new()
        }
    ))
}

/// Resolves the Blender path declared by the script contract.
pub fn resolve_blender(blender_path: &str) -> Result<PathBuf, String> {
    let path = PathBuf::from(blender_path);
    if path.is_file() {
        Ok(path)
    } else {
        Err(format!(
            "Blender executable does not exist: {}",
            path.display()
        ))
    }
}

/// Resolves an `@host exe` payload without searching PATH in Release builds.
pub fn resolve_backend(
    executable_base: &Path,
    repository_root: &Path,
    debug_build: bool,
    exe_name: &str,
) -> Result<PathBuf, String> {
    let packaged = executable_base
        .join("tools")
        .join(exe_name)
        .join(format!("{exe_name}.exe"));
    if packaged.is_file() {
        return Ok(packaged);
    }

    if debug_build {
        if let Some(directory) = env::var_os("GO_DEV_BACKEND_DIR") {
            let candidate = PathBuf::from(directory).join(format!("{exe_name}.exe"));
            if candidate.is_file() {
                return Ok(candidate);
            }
        }

        let build_dir = repository_root.join("build/tauri2-core-debug/core/cpp");
        let candidate = build_dir.join(format!("{exe_name}.exe"));
        if candidate.is_file() {
            return Ok(candidate);
        }
    }

    Err(format!(
        "Backend '{exe_name}' was not found at {}. Rebuild/package the go_pmx2glb backend.",
        packaged.display()
    ))
}

#[cfg(test)]
mod tests {
    use super::{resolve_python_launcher, resolve_script_catalog};
    use std::{
        fs,
        path::PathBuf,
        sync::atomic::{AtomicU64, Ordering},
    };

    static NEXT_TEST_DIR: AtomicU64 = AtomicU64::new(1);

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "general-operations-paths-{}-{}",
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

    #[test]
    fn packaged_launcher_is_preferred_for_debug_builds() {
        let root = TestDirectory::new();
        let executable_base = root.0.join("app");
        let repository_root = root.0.join("repo");
        fs::create_dir_all(&executable_base).expect("app directory should be created");
        fs::create_dir_all(repository_root.join("core/python/runtime"))
            .expect("runtime directory should be created");
        let packaged = executable_base.join("python-launcher.ps1");
        let development = repository_root.join("core/python/runtime/python-launcher.ps1");
        fs::write(&packaged, "").expect("packaged launcher should be written");
        fs::write(&development, "").expect("development launcher should be written");

        let resolved = resolve_python_launcher(&executable_base, &repository_root, true);

        assert_eq!(resolved, Ok(packaged));
    }

    #[test]
    fn debug_build_can_fall_back_to_the_source_launcher() {
        let root = TestDirectory::new();
        let executable_base = root.0.join("app");
        let repository_root = root.0.join("repo");
        fs::create_dir_all(&executable_base).expect("app directory should be created");
        let development = repository_root.join("core/python/runtime/python-launcher.ps1");
        fs::create_dir_all(
            development
                .parent()
                .expect("development launcher should have a parent"),
        )
        .expect("runtime directory should be created");
        fs::write(&development, "").expect("development launcher should be written");

        let resolved = resolve_python_launcher(&executable_base, &repository_root, true);

        assert_eq!(resolved, Ok(development));
        assert!(resolve_python_launcher(&executable_base, &repository_root, false).is_err());
    }

    #[test]
    fn packaged_script_catalog_is_preferred_for_debug_builds() {
        let root = TestDirectory::new();
        let executable_base = root.0.join("app");
        let repository_root = root.0.join("repo");
        let packaged = executable_base.join("tools/go_script_catalog/go_script_catalog.exe");
        let development =
            repository_root.join("build/tauri2-core-debug/core/cpp/go_script_catalog.exe");
        fs::create_dir_all(packaged.parent().expect("packaged path has a parent"))
            .expect("packaged tool directory should be created");
        fs::create_dir_all(development.parent().expect("development path has a parent"))
            .expect("development tool directory should be created");
        fs::write(&packaged, "").expect("packaged catalog should be written");
        fs::write(&development, "").expect("development catalog should be written");

        assert_eq!(
            resolve_script_catalog(&executable_base, &repository_root, true),
            Ok(packaged)
        );
    }

    #[test]
    fn only_debug_builds_can_fall_back_to_the_cmake_catalog() {
        let root = TestDirectory::new();
        let executable_base = root.0.join("app");
        let repository_root = root.0.join("repo");
        let development =
            repository_root.join("build/tauri2-core-debug/core/cpp/go_script_catalog.exe");
        fs::create_dir_all(development.parent().expect("development path has a parent"))
            .expect("development tool directory should be created");
        fs::write(&development, "").expect("development catalog should be written");

        assert_eq!(
            resolve_script_catalog(&executable_base, &repository_root, true),
            Ok(development)
        );
        assert!(resolve_script_catalog(&executable_base, &repository_root, false).is_err());
    }
}
