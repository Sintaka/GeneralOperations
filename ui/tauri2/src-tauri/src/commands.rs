use std::collections::HashMap;

use serde_json::Value;
use tauri::{AppHandle, State};

use crate::{manifest, model::ScriptInfo, runner, state::AppState};

/// Returns the current scripts and metadata read from the core script directory.
#[tauri::command]
pub fn list_scripts(state: State<'_, AppState>) -> Result<Vec<ScriptInfo>, String> {
    let scripts_dir = state.scripts_dir.as_ref().map_err(Clone::clone)?;
    manifest::load_scripts(scripts_dir)
        .map_err(|error| format!("Cannot read script catalog: {error}"))
}

/// Starts a script host process after validating the selected inputs and confirmation.
#[tauri::command]
pub async fn run_script(
    app: AppHandle,
    state: State<'_, AppState>,
    script_id: String,
    inputs: Vec<String>,
    parameters: HashMap<String, Value>,
    confirmed: bool,
) -> Result<String, String> {
    runner::start(app, &state, script_id, inputs, parameters, confirmed).await
}

/// Cancels a previously started run.
#[tauri::command]
pub async fn stop_script(state: State<'_, AppState>, run_id: String) -> Result<(), String> {
    runner::stop(&state, &run_id).await
}
