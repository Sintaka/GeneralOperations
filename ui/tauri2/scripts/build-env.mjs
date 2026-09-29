import { mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
export const frontendDir = path.resolve(scriptsDir, "..");
const repositoryRoot = path.resolve(frontendDir, "..", "..");
const buildRoot = path.join(repositoryRoot, "build");

export const buildPaths = {
  cargoHome: path.join(buildRoot, "tauri2-cargo-home"),
  cargoTarget: path.join(buildRoot, "tauri2-cargo-target"),
  npmCache: path.join(buildRoot, "tauri2-npm-cache"),
};

export async function prepareBuildEnvironment() {
  await Promise.all(Object.values(buildPaths).map((directory) => mkdir(directory, { recursive: true })));
  return {
    ...process.env,
    CARGO_HOME: buildPaths.cargoHome,
    CARGO_TARGET_DIR: buildPaths.cargoTarget,
    npm_config_cache: buildPaths.npmCache,
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await prepareBuildEnvironment();
  console.log(JSON.stringify(buildPaths, null, 2));
}
