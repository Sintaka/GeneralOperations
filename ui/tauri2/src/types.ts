export interface ParamInfo {
  name: string;
  kind: "int" | "float" | "bool" | "choice" | "str" | "path";
  default: string;
  label: string;
  constraints: string | null;
  presets: string[];
  choices: string[];
}

export interface ScriptInfo {
  id: string;
  name: string;
  group: string;
  description: string;
  accepts: "file" | "dir" | "both";
  extensions: string[];
  multi: boolean;
  requires: string[];
  destructive: string | null;
  host: string;
  blenderPath: string | null;
  exe: string | null;
  params: ParamInfo[];
  valid: boolean;
  errors: string[];
}

export interface RunOutput {
  runId: string;
  stream: "stdout" | "stderr";
  line: string;
}

export interface RunFinished {
  runId: string;
  success: boolean;
  cancelled: boolean;
  exitCode: number | null;
  error: string | null;
}

export type ParameterValue = string | boolean;
