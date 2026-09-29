import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
const sourcePath = path.resolve(scriptsDir, "../src/html_safety.ts");
const source = await readFile(sourcePath, "utf8");
const { outputText, diagnostics } = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
  reportDiagnostics: true,
});
assert.equal(diagnostics?.length ?? 0, 0, "HTML safety helper should transpile cleanly");

const helperUrl = `data:text/javascript;base64,${Buffer.from(outputText).toString("base64")}`;
const { renderHostTag } = await import(helperUrl);
const attack = `python\" onmouseover=\"alert(1)'><img src=x onerror=alert(2)>`;
const rendered = renderHostTag(attack, "!", attack);

assert.equal(rendered, '<span class="host-tag unknown">! python&quot; onmouseover=&quot;alert(1)&#39;&gt;&lt;img src=x onerror=alert(2)&gt;</span>');
assert.ok(!rendered.includes('class="host-tag python" onmouseover'));

const mainSource = await readFile(path.resolve(scriptsDir, "../src/main.ts"), "utf8");
assert.match(mainSource, /renderHostTag\(script\.host, hostGlyph\(script\.host\), hostLabel\(script\)\)/);
