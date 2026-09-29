use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
};

use serde_json::Value;

use crate::model::{ParamInfo, ScriptInfo};

/// Maps declared parameters and selected paths to the SCRIPT_SPEC argument vector.
pub fn build_arguments(
    script: &ScriptInfo,
    values: &HashMap<String, Value>,
    inputs: &[PathBuf],
) -> Result<Vec<String>, String> {
    let mut arguments = Vec::new();
    for parameter in &script.params {
        let supplied = values.get(&parameter.name);
        match parameter.kind.as_str() {
            "bool" => {
                if parse_bool(parameter, supplied)? {
                    arguments.push(format!("--{}", parameter.name.replace('_', "-")));
                }
            }
            "int" => {
                let value = parse_integer(parameter, supplied)?;
                arguments.push(format!("--{}", parameter.name.replace('_', "-")));
                arguments.push(value.to_string());
            }
            "float" => {
                let value = parse_float(parameter, supplied)?;
                arguments.push(format!("--{}", parameter.name.replace('_', "-")));
                arguments.push(value.to_string());
            }
            "choice" => {
                let value = value_text(parameter, supplied)?;
                if !parameter.choices.iter().any(|choice| choice == &value) {
                    return Err(format!(
                        "'{}' is not a valid choice for {}.",
                        value, parameter.label
                    ));
                }
                arguments.push(format!("--{}", parameter.name.replace('_', "-")));
                arguments.push(value);
            }
            "str" | "path" => {
                arguments.push(format!("--{}", parameter.name.replace('_', "-")));
                arguments.push(parameter.default.clone());
            }
            kind => return Err(format!("Unsupported parameter type '{kind}'.")),
        }
    }

    arguments.push("--".to_string());
    arguments.extend(
        inputs
            .iter()
            .map(|path| path.as_os_str().to_string_lossy().into_owned()),
    );
    Ok(arguments)
}

/// Checks selected inputs against the script's accepted kind, multiplicity, and extensions.
pub fn validate_inputs(script: &ScriptInfo, inputs: &[String]) -> Result<Vec<PathBuf>, String> {
    if !script.valid {
        return Err(format!(
            "This script declaration is invalid: {}",
            script.errors.join(" ")
        ));
    }
    if inputs.is_empty() {
        return Err("Choose at least one input file or directory.".to_string());
    }
    if !script.multi && inputs.len() != 1 {
        return Err("This script accepts one input at a time.".to_string());
    }

    inputs
        .iter()
        .map(|input| {
            let path = PathBuf::from(input);
            let metadata = fs::metadata(&path)
                .map_err(|error| format!("Cannot access '{}': {error}", path.display()))?;
            let is_directory = metadata.is_dir();
            let accepted = matches!(
                (script.accepts.as_str(), is_directory),
                ("file", false) | ("dir", true) | ("both", _)
            );
            if !accepted {
                return Err(format!(
                    "'{}' is not accepted by this script.",
                    path.display()
                ));
            }
            if !is_directory
                && !script.extensions.is_empty()
                && !has_allowed_extension(&path, &script.extensions)
            {
                return Err(format!(
                    "'{}' has an unsupported file extension.",
                    path.display()
                ));
            }
            Ok(path)
        })
        .collect()
}

fn parse_bool(parameter: &ParamInfo, value: Option<&Value>) -> Result<bool, String> {
    match value {
        Some(Value::Bool(value)) => Ok(*value),
        Some(Value::String(value)) if value.eq_ignore_ascii_case("true") => Ok(true),
        Some(Value::String(value)) if value.eq_ignore_ascii_case("false") => Ok(false),
        None if parameter.default.eq_ignore_ascii_case("true") => Ok(true),
        None if parameter.default.eq_ignore_ascii_case("false") => Ok(false),
        _ => Err(format!("Invalid boolean value for {}.", parameter.label)),
    }
}

fn parse_integer(parameter: &ParamInfo, value: Option<&Value>) -> Result<i64, String> {
    let value = value_text(parameter, value)?;
    let parsed = value
        .parse::<i64>()
        .map_err(|_| format!("{} must be an integer.", parameter.label))?;
    validate_range(parameter, parsed as f64)?;
    Ok(parsed)
}

fn parse_float(parameter: &ParamInfo, value: Option<&Value>) -> Result<f64, String> {
    let value = value_text(parameter, value)?;
    let parsed = value
        .parse::<f64>()
        .map_err(|_| format!("{} must be a number.", parameter.label))?;
    if !parsed.is_finite() {
        return Err(format!("{} must be finite.", parameter.label));
    }
    validate_range(parameter, parsed)?;
    Ok(parsed)
}

fn value_text(parameter: &ParamInfo, value: Option<&Value>) -> Result<String, String> {
    match value {
        Some(Value::String(value)) => Ok(value.clone()),
        Some(Value::Number(value)) => Ok(value.to_string()),
        None => Ok(parameter.default.clone()),
        _ => Err(format!("Invalid value for {}.", parameter.label)),
    }
}

fn validate_range(parameter: &ParamInfo, value: f64) -> Result<(), String> {
    let constraint = parameter.constraints.as_deref().unwrap_or_default();
    let Some(range) = constraint
        .split_whitespace()
        .next()
        .filter(|value| value.contains(".."))
    else {
        return Ok(());
    };
    let Some((minimum, maximum)) = range.split_once("..") else {
        return Ok(());
    };
    let minimum = minimum.parse::<f64>().ok();
    let maximum = maximum.parse::<f64>().ok();
    if minimum.is_some_and(|minimum| value < minimum)
        || maximum.is_some_and(|maximum| value > maximum)
    {
        return Err(format!("{} must be within {}.", parameter.label, range));
    }
    Ok(())
}

fn has_allowed_extension(path: &Path, extensions: &[String]) -> bool {
    let Some(extension) = path.extension().and_then(|value| value.to_str()) else {
        return false;
    };
    extensions.iter().any(|allowed| {
        allowed
            .trim_start_matches('.')
            .eq_ignore_ascii_case(extension)
    })
}

#[cfg(test)]
mod tests {
    use super::{build_arguments, validate_inputs};
    use crate::model::{ParamInfo, ScriptInfo};
    use serde_json::{json, Value};
    use std::{collections::HashMap, path::PathBuf};

    fn script() -> ScriptInfo {
        ScriptInfo {
            id: "sample.py".to_string(),
            name: "Sample".to_string(),
            group: "Image".to_string(),
            description: "Example".to_string(),
            accepts: "file".to_string(),
            extensions: vec![".png".to_string()],
            multi: true,
            requires: Vec::new(),
            destructive: None,
            host: "python".to_string(),
            blender_path: None,
            exe: None,
            params: vec![
                ParamInfo {
                    name: "max_pixels".to_string(),
                    kind: "int".to_string(),
                    default: "1024".to_string(),
                    label: "Max pixels".to_string(),
                    constraints: Some("256..4096".to_string()),
                    presets: Vec::new(),
                    choices: Vec::new(),
                },
                ParamInfo {
                    name: "enabled".to_string(),
                    kind: "bool".to_string(),
                    default: "false".to_string(),
                    label: "Enabled".to_string(),
                    constraints: None,
                    presets: Vec::new(),
                    choices: Vec::new(),
                },
            ],
            valid: true,
            errors: Vec::new(),
        }
    }

    #[test]
    fn maps_parameters_before_the_required_file_separator() {
        let mut values: HashMap<String, Value> = HashMap::new();
        values.insert("max_pixels".to_string(), json!(2048));
        values.insert("enabled".to_string(), json!(true));
        let result = build_arguments(
            &script(),
            &values,
            &[PathBuf::from("D:/pictures/-front.png")],
        );
        assert_eq!(
            result.unwrap_or_default(),
            [
                "--max-pixels",
                "2048",
                "--enabled",
                "--",
                "D:/pictures/-front.png"
            ]
        );
    }

    #[test]
    fn rejects_out_of_range_parameters() {
        let mut values: HashMap<String, Value> = HashMap::new();
        values.insert("max_pixels".to_string(), json!(9999));
        assert!(build_arguments(&script(), &values, &[PathBuf::from("sample.png")]).is_err());
    }

    #[test]
    fn requires_a_file_or_directory_input() {
        assert!(validate_inputs(&script(), &[]).is_err());
    }
}
