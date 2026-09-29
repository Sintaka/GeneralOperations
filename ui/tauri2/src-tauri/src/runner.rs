use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    process::Stdio,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use serde_json::Value;
use tauri::{AppHandle, Emitter};
use tokio::{
    io::{AsyncBufReadExt, AsyncRead, AsyncWriteExt, BufReader},
    process::Command,
};

use crate::{
    arguments::{build_arguments, validate_inputs},
    manifest::load_scripts_fresh,
    model::{RunFinished, RunOutput, ScriptInfo},
    paths,
    state::{AppState, RunningProcess},
};

static NEXT_RUN_ID: AtomicU64 = AtomicU64::new(1);

struct ResolvedProgram {
    program: PathBuf,
    prefix_args: Vec<String>,
    host_args: Vec<String>,
    python_host: bool,
}

/// Validates a run request, starts its host process, and streams output through Tauri events.
pub async fn start(
    app: AppHandle,
    state: &AppState,
    script_id: String,
    input_names: Vec<String>,
    parameter_values: HashMap<String, Value>,
    confirmed: bool,
) -> Result<String, String> {
    let scripts_dir = state.scripts_dir.as_ref().map_err(Clone::clone)?;
    let scripts = load_scripts_fresh(scripts_dir)
        .map_err(|error| format!("Cannot read scripts directory: {error}"))?;
    let requirements = collect_script_requirements(&scripts);
    let script = scripts
        .into_iter()
        .find(|script| script.id == script_id)
        .ok_or_else(|| format!("Script not found in the current catalog: {script_id}"))?;
    if !script.valid {
        return Err(format!(
            "This script declaration is invalid: {}",
            script.errors.join(" ")
        ));
    }
    if script.destructive.is_some() && !confirmed {
        return Err("This operation requires confirmation before it can run.".to_string());
    }

    let inputs = validate_inputs(&script, &input_names)?;
    let arguments = build_arguments(&script, &parameter_values, &inputs)?;
    let script_path = scripts_dir.join(&script.id);
    let executable = std::env::current_exe()
        .map_err(|error| format!("Cannot locate the application: {error}"))?;
    let executable_base = paths::executable_base(&executable)?;
    let resolved = resolve_program(
        &script,
        &script_path,
        executable_base,
        &state.repository_root,
        cfg!(debug_assertions),
    )?;
    let python_payload = if resolved.python_host {
        Some(build_python_payload(
            &script_path,
            &arguments,
            scripts_dir,
            &requirements,
        )?)
    } else {
        None
    };

    let mut args = resolved.prefix_args.clone();
    args.extend(resolved.host_args.clone());
    if !resolved.python_host {
        args.extend(arguments);
    }

    let mut command = Command::new(&resolved.program);
    command
        .args(&args)
        .stdin(if python_payload.is_some() {
            Stdio::piped()
        } else {
            Stdio::null()
        })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    if let Some(directory) = input_working_directory(inputs.first()) {
        command.current_dir(directory);
    }
    if resolved.python_host {
        command
            .env("PYTHONUTF8", "1")
            .env("PYTHONIOENCODING", "utf-8")
            .env_remove("PYTHONPATH");
        #[cfg(windows)]
        command.creation_flags(0x08000000);
    }

    let mut child = command
        .spawn()
        .map_err(|error| format!("Could not start {}: {error}", resolved.program.display()))?;
    if let Some(payload) = python_payload {
        let Some(mut stdin) = child.stdin.take() else {
            let _ = child.kill().await;
            return Err("Could not open Python launcher stdin.".to_string());
        };
        if let Err(error) = stdin.write_all(&payload).await {
            let _ = child.kill().await;
            return Err(format!("Could not send the Python launch request: {error}"));
        }
        drop(stdin);
    }
    let pid = child
        .id()
        .ok_or_else(|| "Started process has no Windows process id.".to_string())?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "Could not capture child stdout.".to_string())?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| "Could not capture child stderr.".to_string())?;
    let run_id = new_run_id();
    let returned_run_id = run_id.clone();
    let running = state.running.clone();
    running
        .lock()
        .await
        .insert(run_id.clone(), RunningProcess::new(pid));

    tauri::async_runtime::spawn(async move {
        let stdout_task = tauri::async_runtime::spawn(forward_lines(
            app.clone(),
            run_id.clone(),
            "stdout",
            stdout,
        ));
        let stderr_task = tauri::async_runtime::spawn(forward_lines(
            app.clone(),
            run_id.clone(),
            "stderr",
            stderr,
        ));
        let process_result = child.wait().await;
        let _ = stdout_task.await;
        let _ = stderr_task.await;
        let cancelled = running
            .lock()
            .await
            .remove(&run_id)
            .is_some_and(|process| process.cancellation_requested());

        let finished = match process_result {
            Ok(status) => RunFinished {
                run_id,
                success: status.success() && !cancelled,
                cancelled,
                exit_code: status.code(),
                error: None,
            },
            Err(error) => RunFinished {
                run_id,
                success: false,
                cancelled,
                exit_code: None,
                error: Some(format!(
                    "Failed while waiting for the child process: {error}"
                )),
            },
        };
        let _ = app.emit("run-finished", finished);
    });

    Ok(returned_run_id)
}

/// Stops a running host process and its descendants on Windows.
pub async fn stop(state: &AppState, run_id: &str) -> Result<(), String> {
    let pid = {
        let mut running = state.running.lock().await;
        let Some(process) = running.get_mut(run_id) else {
            return Ok(());
        };
        process.request_cancel();
        process.pid()
    };

    #[cfg(windows)]
    let mut command = {
        let mut command = Command::new("taskkill");
        command
            .arg("/PID")
            .arg(pid.to_string())
            .arg("/T")
            .arg("/F")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(0x08000000);
        command
    };
    #[cfg(not(windows))]
    let mut command = {
        let mut command = Command::new("kill");
        command.arg("-TERM").arg(pid.to_string());
        command
    };

    let status = match command.status().await {
        Ok(status) => status,
        Err(error) => {
            clear_cancel_request(state, run_id).await;
            return Err(format!("Could not cancel process {pid}: {error}"));
        }
    };
    if status.success() || !state.running.lock().await.contains_key(run_id) {
        Ok(())
    } else {
        clear_cancel_request(state, run_id).await;
        Err(format!("Windows could not terminate process tree {pid}."))
    }
}

async fn clear_cancel_request(state: &AppState, run_id: &str) {
    if let Some(process) = state.running.lock().await.get_mut(run_id) {
        process.clear_cancel_request();
    }
}

fn resolve_program(
    script: &ScriptInfo,
    script_path: &Path,
    executable_base: &Path,
    repository_root: &Path,
    debug_build: bool,
) -> Result<ResolvedProgram, String> {
    match script.host.as_str() {
        "python" => {
            let launcher =
                paths::resolve_python_launcher(executable_base, repository_root, debug_build)?;
            let (program, args) = python_launcher_invocation(&launcher);
            Ok(ResolvedProgram {
                program,
                prefix_args: args,
                host_args: Vec::new(),
                python_host: true,
            })
        }
        "blender" => {
            let path = script
                .blender_path
                .as_deref()
                .ok_or_else(|| "Blender script has no @blender path.".to_string())?;
            let program = paths::resolve_blender(path)?;
            let args = vec![
                "--background".to_string(),
                "--python".to_string(),
                script_path.as_os_str().to_string_lossy().into_owned(),
                "--".to_string(),
            ];
            Ok(ResolvedProgram {
                program,
                prefix_args: Vec::new(),
                host_args: args,
                python_host: false,
            })
        }
        "exe" => {
            let name = script
                .exe
                .as_deref()
                .ok_or_else(|| "Executable script has no @exe name.".to_string())?;
            let program =
                paths::resolve_backend(executable_base, repository_root, debug_build, name)?;
            Ok(ResolvedProgram {
                program,
                prefix_args: Vec::new(),
                host_args: Vec::new(),
                python_host: false,
            })
        }
        host => Err(format!("Unsupported script host '{host}'.")),
    }
}

fn python_launcher_invocation(launcher: &Path) -> (PathBuf, Vec<String>) {
    (
        PathBuf::from("powershell.exe"),
        vec![
            "-NoProfile".to_string(),
            "-NonInteractive".to_string(),
            "-ExecutionPolicy".to_string(),
            "Bypass".to_string(),
            "-File".to_string(),
            launcher.as_os_str().to_string_lossy().into_owned(),
        ],
    )
}

fn build_python_payload(
    script_path: &Path,
    arguments: &[String],
    scripts_dir: &Path,
    requirements: &[String],
) -> Result<Vec<u8>, String> {
    if !script_path.is_absolute() {
        return Err(format!(
            "Python script path must be absolute: {}",
            script_path.display()
        ));
    }

    serde_json::to_vec(&serde_json::json!({
        "script": script_path.to_string_lossy().into_owned(),
        "args": arguments,
        "scriptsDir": scripts_dir.to_string_lossy().into_owned(),
        "requirements": requirements,
    }))
    .map_err(|error| format!("Could not encode the Python launch request: {error}"))
}

fn collect_script_requirements(scripts: &[ScriptInfo]) -> Vec<String> {
    scripts
        .iter()
        .flat_map(|script| script.requires.iter().cloned())
        .collect()
}

fn input_working_directory(input: Option<&PathBuf>) -> Option<&Path> {
    let input = input?;
    if input.is_dir() {
        Some(input.as_path())
    } else {
        input.parent()
    }
}

fn new_run_id() -> String {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or_default();
    let sequence = NEXT_RUN_ID.fetch_add(1, Ordering::Relaxed);
    format!("run-{now}-{sequence}")
}

async fn forward_lines<R>(app: AppHandle, run_id: String, stream: &str, reader: R)
where
    R: AsyncRead + Unpin + Send + 'static,
{
    let mut lines = BufReader::new(reader).lines();
    loop {
        match lines.next_line().await {
            Ok(Some(line)) => {
                let output = RunOutput {
                    run_id: run_id.clone(),
                    stream: stream.to_string(),
                    line,
                };
                let _ = app.emit("run-output", output);
            }
            Ok(None) => break,
            Err(error) => {
                let output = RunOutput {
                    run_id: run_id.clone(),
                    stream: "stderr".to_string(),
                    line: format!("Could not read {stream}: {error}"),
                };
                let _ = app.emit("run-output", output);
                break;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{build_python_payload, collect_script_requirements, python_launcher_invocation};
    use std::path::{Path, PathBuf};

    use crate::model::ScriptInfo;

    #[test]
    fn invokes_the_wrapper_with_the_frozen_powershell_flags() {
        let (program, arguments) =
            python_launcher_invocation(Path::new("C:/app/python-launcher.ps1"));

        assert_eq!(program, PathBuf::from("powershell.exe"));
        assert_eq!(
            arguments,
            [
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "C:/app/python-launcher.ps1"
            ]
        );
    }

    #[test]
    fn python_payload_preserves_raw_catalog_requirements_and_arguments() {
        let script_path = Path::new("C:/repo/core/python/scripts/sample.py");
        let scripts_dir = Path::new("C:/repo/core/python/scripts");
        let arguments = vec![
            "--quality".to_string(),
            "85".to_string(),
            "--".to_string(),
            "C:/pictures/-front.png".to_string(),
        ];
        let scripts = vec![
            script_with_requirements(&["Pillow", "OpenImageIO==3.1.14.0?", "图像依赖>=1.0?"]),
            script_with_requirements(&[
                "Pillow",
                "pillow",
                "图像依赖>=1.0?",
                "opencv-python-headless==4.11.0.86?",
            ]),
        ];
        let requirements = collect_script_requirements(&scripts);

        let payload = build_python_payload(script_path, &arguments, scripts_dir, &requirements)
            .expect("absolute Python launch requests should encode");
        let parsed: serde_json::Value =
            serde_json::from_slice(&payload).expect("payload should be valid JSON");

        assert_eq!(parsed["script"], "C:/repo/core/python/scripts/sample.py");
        assert_eq!(parsed["args"], serde_json::json!(arguments));
        assert_eq!(parsed["scriptsDir"], "C:/repo/core/python/scripts");
        assert_eq!(
            parsed["requirements"],
            serde_json::json!([
                "Pillow",
                "OpenImageIO==3.1.14.0?",
                "图像依赖>=1.0?",
                "Pillow",
                "pillow",
                "图像依赖>=1.0?",
                "opencv-python-headless==4.11.0.86?"
            ])
        );
    }

    #[test]
    fn python_payload_rejects_relative_script_paths() {
        assert!(build_python_payload(
            Path::new("scripts/sample.py"),
            &[],
            Path::new("scripts"),
            &[],
        )
        .is_err());
    }

    fn script_with_requirements(requirements: &[&str]) -> ScriptInfo {
        ScriptInfo {
            id: "sample.py".to_string(),
            name: "Sample".to_string(),
            group: "Tests".to_string(),
            description: String::new(),
            accepts: "file".to_string(),
            extensions: Vec::new(),
            multi: false,
            requires: requirements
                .iter()
                .map(|value| (*value).to_string())
                .collect(),
            destructive: None,
            host: "python".to_string(),
            blender_path: None,
            exe: None,
            params: Vec::new(),
            valid: true,
            errors: Vec::new(),
        }
    }
}
