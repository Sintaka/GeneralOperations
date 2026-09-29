import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
const frontendDir = path.resolve(scriptsDir, "..");
const repositoryRoot = path.resolve(frontendDir, "..", "..");
const buildType = process.argv[2] === "Release" ? "Release" : "Debug";
const buildDir = path.join(repositoryRoot, "build", `tauri2-core-${buildType.toLowerCase()}`);

function run(command, args) {
  const result = spawnSync(command, args, { stdio: "inherit", windowsHide: true });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}

run("cmake", [
  "-S", repositoryRoot,
  "-B", buildDir,
  "-G", "Ninja",
  `-DCMAKE_BUILD_TYPE=${buildType}`,
  "-DGO_FRONTEND=tauri2",
  "-DCMAKE_EXE_LINKER_FLAGS=-static",
]);
run("cmake", ["--build", buildDir, "--target", "go_pmx2glb", "go_script_catalog"]);
