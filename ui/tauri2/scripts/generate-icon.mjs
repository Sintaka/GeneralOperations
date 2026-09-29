import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const appDir = path.resolve(scriptDir, "..");
const iconDir = path.join(appDir, "src-tauri", "icons");
const size = 32;
const rgba = Buffer.alloc(size * size * 4);

for (let bmpY = 0; bmpY < size; bmpY += 1) {
  const y = size - 1 - bmpY;
  for (let x = 0; x < size; x += 1) {
    const inset = 6;
    const cx = x < inset ? inset : x >= size - inset ? size - inset - 1 : x;
    const cy = y < inset ? inset : y >= size - inset ? size - inset - 1 : y;
    const rounded = (x - cx) ** 2 + (y - cy) ** 2 <= inset ** 2;
    if (!rounded) continue;

    const distance = Math.hypot(x - 15.5, y - 15.5);
    let color = [25, 37, 39, 255];
    if (distance >= 8.5 && distance <= 12) color = [142, 227, 194, 255];
    if (distance < 5.3) color = [40, 77, 66, 255];
    if (x >= 20 && x <= 24 && y >= 8 && y <= 12 && distance < 12) color = [229, 188, 120, 255];

    const index = (bmpY * size + x) * 4;
    rgba[index] = color[2];
    rgba[index + 1] = color[1];
    rgba[index + 2] = color[0];
    rgba[index + 3] = color[3];
  }
}

const header = Buffer.alloc(40);
header.writeUInt32LE(40, 0);
header.writeInt32LE(size, 4);
header.writeInt32LE(size * 2, 8);
header.writeUInt16LE(1, 12);
header.writeUInt16LE(32, 14);
header.writeUInt32LE(rgba.length + size * 4, 20);
const mask = Buffer.alloc(size * 4);
const image = Buffer.concat([header, rgba, mask]);
const iconHeader = Buffer.alloc(6);
iconHeader.writeUInt16LE(0, 0);
iconHeader.writeUInt16LE(1, 2);
iconHeader.writeUInt16LE(1, 4);
const directory = Buffer.alloc(16);
directory[0] = size;
directory[1] = size;
directory.writeUInt16LE(1, 4);
directory.writeUInt16LE(32, 6);
directory.writeUInt32LE(image.length, 8);
directory.writeUInt32LE(iconHeader.length + directory.length, 12);

await mkdir(iconDir, { recursive: true });
await writeFile(path.join(iconDir, "icon.ico"), Buffer.concat([iconHeader, directory, image]));
