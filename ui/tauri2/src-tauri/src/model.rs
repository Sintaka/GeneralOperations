use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ScriptInfo {
    pub id: String,
    pub name: String,
    pub group: String,
    pub description: String,
    pub accepts: String,
    pub extensions: Vec<String>,
    pub multi: bool,
    pub requires: Vec<String>,
    pub destructive: Option<String>,
    pub host: String,
    pub blender_path: Option<String>,
    pub exe: Option<String>,
    pub params: Vec<ParamInfo>,
    pub valid: bool,
    pub errors: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ParamInfo {
    pub name: String,
    pub kind: String,
    pub default: String,
    pub label: String,
    pub constraints: Option<String>,
    pub presets: Vec<String>,
    pub choices: Vec<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RunOutput {
    pub run_id: String,
    pub stream: String,
    pub line: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RunFinished {
    pub run_id: String,
    pub success: bool,
    pub cancelled: bool,
    pub exit_code: Option<i32>,
    pub error: Option<String>,
}
