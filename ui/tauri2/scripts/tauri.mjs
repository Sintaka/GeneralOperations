import { spawn } from "node:child_process";
import path from "node:path";
import { frontendDir, prepareBuildEnvironment } from "./build-env.mjs";

const [subcommand, ...args] = process.argv.slice(2);
if (!subcommand) throw new Error("Tauri 子命令缺失");

const cli = path.join(frontendDir, "node_modules", "@tauri-apps", "cli", "tauri.js");
const env = await prepareBuildEnvironment();
const result = spawn(process.execPath, [cli, subcommand, ...args], {
  cwd: frontendDir,
  env,
  stdio: "inherit",
  windowsHide: true,
});
result.on("error", (error) => {
  console.error(error.message);
  process.exitCode = 1;
});
result.on("exit", (code) => {
  process.exitCode = code ?? 1;
});
