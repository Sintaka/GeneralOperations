#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod arguments;
mod commands;
mod manifest;
mod model;
mod paths;
mod runner;
mod state;

use tauri::Manager;

fn main() {
    let result = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let repository_root = paths::repository_root();
            let executable = std::env::current_exe()?;
            let scripts_dir =
                paths::resolve_scripts_dir(&executable, cfg!(debug_assertions), &repository_root);
            app.manage(state::AppState::new(repository_root, scripts_dir));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::list_scripts,
            commands::run_script,
            commands::stop_script
        ])
        .run(tauri::generate_context!());

    if let Err(error) = result {
        eprintln!("GeneralOperations Tauri failed: {error}");
        std::process::exit(1);
    }
}
