import { spawn, spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { prepareBuildEnvironment } from "./build-env.mjs";

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
const frontendDir = path.resolve(scriptsDir, "..");
const manifest = path.join(frontendDir, "src-tauri", "Cargo.toml");

const sync = spawnSync(process.execPath, [path.join(scriptsDir, "sync-version.mjs")], { stdio: "inherit", windowsHide: true });
if (sync.error) throw sync.error;
if (sync.status !== 0) process.exit(sync.status ?? 1);
const icon = spawnSync(process.execPath, [path.join(scriptsDir, "generate-icon.mjs")], { stdio: "inherit", windowsHide: true });
if (icon.error) throw icon.error;
if (icon.status !== 0) process.exit(icon.status ?? 1);

const env = await prepareBuildEnvironment();
const result = spawn("cargo", [process.argv[2] ?? "check", "--manifest-path", manifest, ...process.argv.slice(3)], {
  stdio: "inherit",
  env,
  windowsHide: true,
});
result.on("error", (error) => {
  console.error(error.message);
  process.exitCode = 1;
});
result.on("exit", (code) => {
  process.exitCode = code ?? 1;
});
