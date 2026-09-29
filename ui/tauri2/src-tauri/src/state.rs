use std::{collections::HashMap, path::PathBuf, sync::Arc};

use tokio::sync::Mutex;

/// Shared desktop state for script discovery and active child processes.
pub struct AppState {
    pub repository_root: PathBuf,
    pub scripts_dir: Result<PathBuf, String>,
    pub running: Arc<Mutex<HashMap<String, RunningProcess>>>,
}

/// Tracks a host process and whether the user requested its cancellation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RunningProcess {
    pid: u32,
    cancel_requested: bool,
}

impl RunningProcess {
    /// Creates a process record before the monitor task starts.
    pub fn new(pid: u32) -> Self {
        Self {
            pid,
            cancel_requested: false,
        }
    }

    /// Returns the operating-system process identifier.
    pub fn pid(&self) -> u32 {
        self.pid
    }

    /// Marks that the user requested cancellation.
    pub fn request_cancel(&mut self) {
        self.cancel_requested = true;
    }

    /// Clears the cancellation request after the operating system rejects it.
    pub fn clear_cancel_request(&mut self) {
        self.cancel_requested = false;
    }

    /// Reports whether a cancellation request is pending.
    pub fn cancellation_requested(&self) -> bool {
        self.cancel_requested
    }
}

impl AppState {
    /// Creates the state used by the IPC command handlers.
    pub fn new(repository_root: PathBuf, scripts_dir: Result<PathBuf, String>) -> Self {
        Self {
            repository_root,
            scripts_dir,
            running: Arc::new(Mutex::new(HashMap::new())),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::RunningProcess;

    #[test]
    fn cancellation_is_reported_only_after_a_stop_request() {
        let mut process = RunningProcess::new(42);

        assert_eq!(process.pid(), 42);
        assert!(!process.cancellation_requested());
        process.request_cancel();
        assert!(process.cancellation_requested());
        process.clear_cancel_request();
        assert!(!process.cancellation_requested());
    }
}
