#[path = "../src/manifest.rs"]
mod manifest;
#[allow(dead_code)]
#[path = "../src/model.rs"]
mod model;
#[allow(dead_code)]
#[path = "../src/paths.rs"]
mod paths;

use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

static NEXT_TEST_DIR: AtomicU64 = AtomicU64::new(1);

struct TestDirectory(PathBuf);

impl TestDirectory {
    fn new() -> Self {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_nanos())
            .unwrap_or_default();
        let path = std::env::temp_dir().join(format!(
            "go-tauri-catalog-测试-{}-{unique}-{}",
            std::process::id(),
            NEXT_TEST_DIR.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).expect("temporary catalog directory should be created");
        Self(path)
    }
}

impl Drop for TestDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn shared_catalog_reads_core_scripts_and_preserves_the_tauri_schema() {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let script_root = repository_root.join("core/python/scripts");
    let scripts = manifest::load_scripts(&script_root)
        .expect("shared script catalog should read core scripts");

    assert!(
        !scripts.is_empty(),
        "the catalog must come from core/python/scripts"
    );
    assert!(
        scripts.iter().all(|script| script.valid),
        "all shipped core scripts must be valid: {:?}",
        scripts
            .iter()
            .filter(|script| !script.valid)
            .map(|script| (&script.id, &script.errors))
            .collect::<Vec<_>>()
    );

    let exr = scripts
        .iter()
        .find(|script| script.id == "Image/Format Convert/img.EXR2PNG_Large.py")
        .expect("the core EXR2PNG script should be present");
    assert!(
        exr.valid,
        "EXR2PNG declaration should be valid: {:?}",
        exr.errors
    );
    assert_eq!(exr.name, "EXR to PNG (Large / 4K)");
    assert!(exr
        .params
        .iter()
        .any(|parameter| parameter.name == "target"));

    let resize = scripts
        .iter()
        .find(|script| script.id == "Image/ReSize/img.Resize_jpg.py")
        .expect("the core resize script should be present");
    let max_pixels = resize
        .params
        .iter()
        .find(|parameter| parameter.name == "max_pixels")
        .expect("the core resize parameter should be present");
    assert_eq!(max_pixels.presets, ["512", "1024", "2048", "4096", "8192"]);

    let pmx_fbx = scripts
        .iter()
        .find(|script| script.id == "Geometry/Format Convert/geo.pmx2fbx.py")
        .expect("the core PMX-to-FBX script should be present");
    assert!(
        pmx_fbx.valid,
        "PMX-to-FBX declaration should be valid: {:?}",
        pmx_fbx.errors
    );
    assert_eq!(pmx_fbx.host, "blender");

    let pmx_glb = scripts
        .iter()
        .find(|script| script.id == "Geometry/Format Convert/geo.pmx2glb.py")
        .expect("the core PMX-to-GLB contract card should be present");
    assert_eq!(pmx_glb.host, "exe");
    assert_eq!(pmx_glb.exe.as_deref(), Some("go_pmx2glb"));

    let value = serde_json::to_value(&scripts).expect("catalog should serialize to JSON");
    let first = value[0]
        .as_object()
        .expect("script should be a JSON object");
    for key in [
        "id",
        "name",
        "group",
        "description",
        "accepts",
        "extensions",
        "multi",
        "requires",
        "destructive",
        "host",
        "blenderPath",
        "exe",
        "params",
        "valid",
        "errors",
    ] {
        assert!(first.contains_key(key), "missing script JSON field {key}");
    }
    let param = value
        .as_array()
        .and_then(|scripts| {
            scripts
                .iter()
                .find(|script| script["id"].as_str() == Some(exr.id.as_str()))
        })
        .and_then(|script| script["params"].as_array())
        .and_then(|params| params.first())
        .and_then(serde_json::Value::as_object)
        .expect("EXR2PNG parameter should be a JSON object");
    for key in [
        "name",
        "kind",
        "default",
        "label",
        "constraints",
        "presets",
        "choices",
    ] {
        assert!(
            param.contains_key(key),
            "missing parameter JSON field {key}"
        );
    }

    let decoded: Vec<model::ScriptInfo> =
        serde_json::from_value(value).expect("camelCase catalog JSON should deserialize");
    assert_eq!(decoded, scripts);
}

#[test]
fn shared_catalog_keeps_scripts_with_missing_declarations_invalid() {
    let root = TestDirectory::new();
    let nested = root.0.join("nested");
    fs::create_dir_all(&nested).expect("nested script directory should be created");
    fs::write(nested.join("missing.py"), "print('no declaration')\n")
        .expect("temporary script should be written");
    fs::write(
        nested.join("incomplete.py"),
        "\"\"\"\n@name Incomplete\n@desc Missing required fields\n\"\"\"\n",
    )
    .expect("incomplete script declaration should be written");

    let scripts = manifest::load_scripts_fresh(&root.0)
        .expect("shared catalog should return invalid script entries instead of hiding them");
    assert_eq!(scripts.len(), 2);
    assert!(scripts
        .iter()
        .all(|script| !script.valid && !script.errors.is_empty()));
}
