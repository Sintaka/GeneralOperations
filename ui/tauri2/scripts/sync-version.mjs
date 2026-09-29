import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
const frontendDir = path.resolve(scriptsDir, "..");
const repositoryRoot = path.resolve(frontendDir, "..", "..");
const cmake = await readFile(path.join(repositoryRoot, "CMakeLists.txt"), "utf8");
const match = cmake.match(/project\s*\(\s*GeneralOperations[\s\S]*?\bVERSION\s+(\d+)\.(\d+)\.(\d+)/i);

if (!match) {
  throw new Error("Could not read GeneralOperations VERSION from root CMakeLists.txt.");
}

const cargoVersion = `${match[1]}.${match[2]}.${Number(match[3])}`;
const templatePath = path.join(frontendDir, "src-tauri", "Cargo.toml.in");
const generatedPath = path.join(frontendDir, "src-tauri", "Cargo.toml");
const template = await readFile(templatePath, "utf8");

if (!template.includes("@@VERSION@@")) {
  throw new Error("Cargo.toml.in must contain the @@VERSION@@ placeholder.");
}

await writeFile(generatedPath, template.replaceAll("@@VERSION@@", cargoVersion), "utf8");
console.log(`Generated src-tauri/Cargo.toml with root version ${match[1]}.${match[2]}.${match[3]} (Cargo ${cargoVersion}).`);
