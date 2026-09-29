import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { confirm, open } from "@tauri-apps/plugin-dialog";
import { escapeHtml, renderHostTag } from "./html_safety";
import "./styles.css";
import type { ParameterValue, ParamInfo, RunFinished, RunOutput, ScriptInfo } from "./types";

interface ViewState {
  scripts: ScriptInfo[];
  selectedId: string | null;
  search: string;
  inputs: string[];
  parameters: Record<string, ParameterValue>;
  activeRunId: string | null;
  startingRun: boolean;
  pendingEvents: Array<{ kind: "output"; value: RunOutput } | { kind: "finished"; value: RunFinished }>;
  status: string;
  logCount: number;
}

const state: ViewState = {
  scripts: [], selectedId: null, search: "", inputs: [], parameters: {},
  activeRunId: null, startingRun: false, pendingEvents: [], status: "正在读取脚本清单…", logCount: 0,
};

const root = document.querySelector<HTMLDivElement>("#app");
if (!root) throw new Error("App root is missing.");

root.innerHTML = `
  <div class="app-frame">
    <header class="window-titlebar" aria-label="窗口标题栏">
      <div class="window-titlebar-brand" data-tauri-drag-region>
        <span class="window-titlebar-mark" aria-hidden="true">GO</span>
        <span class="window-titlebar-name">General Operations</span>
        <span class="window-titlebar-divider" aria-hidden="true">/</span>
        <span class="window-titlebar-caption">工作台</span>
      </div>
      <div class="window-titlebar-drag" data-tauri-drag-region aria-hidden="true"></div>
      <div class="window-controls" role="group" aria-label="窗口控制">
        <button id="window-minimize" class="window-control" type="button" title="最小化窗口" aria-label="最小化窗口">
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M4 11.5h8" /></svg>
        </button>
        <button id="window-maximize" class="window-control" type="button" title="最大化或还原窗口" aria-label="最大化或还原窗口">
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><rect x="4.25" y="4.25" width="7.5" height="7.5" rx="1" /><path d="M6 2.75h6.25c.55 0 1 .45 1 1V10" /></svg>
        </button>
        <button id="window-close" class="window-control window-control-close" type="button" title="关闭窗口" aria-label="关闭窗口">
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="m4.5 4.5 7 7m0-7-7 7" /></svg>
        </button>
      </div>
    </header>
    <div class="shell">
    <aside class="sidebar">
      <div class="brand"><span class="brand-mark">GO</span><div><strong>General Operations</strong><small>工作台</small></div></div>
      <label class="search"><span aria-hidden="true">⌕</span><input id="search" type="search" placeholder="搜索脚本" autocomplete="off"></label>
      <div class="nav-caption"><span>脚本库</span><span id="script-count" class="count-pill">0</span></div>
      <nav id="script-list" class="script-list" aria-label="脚本列表"></nav>
      <div class="sidebar-footer"><span class="status-dot"></span><span id="catalog-status">加载中</span></div>
    </aside>
    <main class="main-panel">
      <header class="topbar"><div><div class="eyebrow">LOCAL TOOLKIT</div><h1>脚本工作台</h1></div><div class="topbar-note"><span class="runtime-icon">⌘</span><span>本地运行 · 文件不会离开此设备</span></div></header>
      <div id="detail" class="detail"></div>
    </main>
    <aside class="activity-panel">
      <div class="activity-header"><div><div class="eyebrow">ACTIVITY</div><h2>运行记录</h2></div><button id="clear-log" class="icon-button" title="清空记录" aria-label="清空记录">⌫</button></div>
      <div id="run-status" class="run-status"><span class="status-dot idle"></span><span>等待运行</span></div>
      <div id="log" class="log" role="log" aria-live="polite"><div class="log-empty">运行输出会显示在这里</div></div>
      <div class="activity-footer">stdout 和 stderr 实时回显</div>
    </aside>
    </div>
  </div>`;

const scriptList = document.querySelector<HTMLElement>("#script-list")!;
const detail = document.querySelector<HTMLElement>("#detail")!;
const log = document.querySelector<HTMLElement>("#log")!;
const statusLine = document.querySelector<HTMLElement>("#run-status")!;

const appWindow = getCurrentWindow();
root.querySelector<HTMLButtonElement>("#window-minimize")!.addEventListener("click", () => {
  void runWindowAction("最小化", () => appWindow.minimize());
});
root.querySelector<HTMLButtonElement>("#window-maximize")!.addEventListener("click", () => {
  void runWindowAction("最大化或还原", () => appWindow.toggleMaximize());
});
root.querySelector<HTMLButtonElement>("#window-close")!.addEventListener("click", () => {
  void runWindowAction("关闭", () => appWindow.close());
});

document.querySelector<HTMLInputElement>("#search")!.addEventListener("input", (event) => {
  state.search = (event.currentTarget as HTMLInputElement).value.trim().toLocaleLowerCase();
  renderScripts();
});
document.querySelector<HTMLButtonElement>("#clear-log")!.addEventListener("click", clearLog);
scriptList.addEventListener("click", onScriptClick);
detail.addEventListener("click", onDetailClick);
detail.addEventListener("input", onParameterChange);
detail.addEventListener("change", onParameterChange);

void initialize();

async function initialize(): Promise<void> {
  try {
    const scriptListRequest = Promise.resolve()
      .then(() => invoke<ScriptInfo[]>("list_scripts"))
      .catch((error: unknown) => {
        throw new Error(`脚本清单读取失败：${errorMessage(error)}`);
      });
    const runOutputListener = registerStartupHandler("run-output 事件监听", () =>
      listen<RunOutput>("run-output", ({ payload }) => {
        if (payload.runId === state.activeRunId) appendLog(payload.stream, payload.line);
        else if (state.startingRun) state.pendingEvents.push({ kind: "output", value: payload });
      }),
    );
    const runFinishedListener = registerStartupHandler("run-finished 事件监听", () =>
      listen<RunFinished>("run-finished", ({ payload }) => {
        if (payload.runId === state.activeRunId) handleFinished(payload);
        else if (state.startingRun) state.pendingEvents.push({ kind: "finished", value: payload });
      }),
    );
    const dragDropListener = registerStartupHandler("拖放事件监听", () =>
      getCurrentWebview().onDragDropEvent((event) => {
        if (event.payload.type === "over") {
          document.body.classList.add("drag-over");
        } else {
          document.body.classList.remove("drag-over");
          if (event.payload.type === "drop") acceptInputs(event.payload.paths);
        }
      }),
    );

    const [scripts] = await Promise.all([
      scriptListRequest,
      runOutputListener,
      runFinishedListener,
      dragDropListener,
    ]);
    state.scripts = scripts;
    const firstRunnable = state.scripts.find((script) => script.valid);
    state.selectedId = firstRunnable?.id ?? state.scripts[0]?.id ?? null;
    state.status = `${state.scripts.length} 个脚本已载入`;
    if (state.selectedId) setParameterDefaults(selectedScript());
    renderAll();
    const invalidCount = state.scripts.filter((script) => !script.valid).length;
    setCatalogStatus(invalidCount ? `${invalidCount} 个声明需要修复` : "脚本目录已同步", invalidCount > 0);
  } catch (error) {
    state.status = errorMessage(error);
    setCatalogStatus("初始化失败", true);
    setRunStatus("error", "初始化失败");
    renderDetail();
    showBanner(state.status);
  }
}

function registerStartupHandler<T>(name: string, register: () => Promise<T>): Promise<T> {
  return Promise.resolve().then(register).catch((error: unknown) => {
    throw new Error(`${name}注册失败：${errorMessage(error)}`);
  });
}

async function runWindowAction(action: string, run: () => Promise<void>): Promise<void> {
  try {
    await run();
  } catch (error) {
    showBanner(`窗口${action}失败：${errorMessage(error)}`);
  }
}

function selectedScript(): ScriptInfo | undefined {
  return state.scripts.find((script) => script.id === state.selectedId);
}

function renderAll(): void {
  renderScripts();
  renderDetail();
  document.querySelector<HTMLElement>("#script-count")!.textContent = String(state.scripts.length);
}

function renderScripts(): void {
  const visible = state.scripts.filter((script) => {
    const haystack = `${script.name} ${script.group} ${script.description}`.toLocaleLowerCase();
    return haystack.includes(state.search);
  });
  const groups = new Map<string, ScriptInfo[]>();
  for (const script of visible) groups.set(script.group, [...(groups.get(script.group) ?? []), script]);
  scriptList.innerHTML = [...groups.entries()].sort(([a], [b]) => a.localeCompare(b, "zh-CN"))
    .map(([group, scripts]) => `
      <section class="script-group">
        <div class="group-title"><span class="folder-glyph">▸</span>${escapeHtml(group)}</div>
        ${scripts.map((script) => `
          <button class="script-row ${script.id === state.selectedId ? "active" : ""} ${script.valid ? "" : "invalid"}" data-script-id="${escapeHtml(script.id)}" type="button">
            <span class="script-symbol">${script.valid ? hostGlyph(script.host) : "!"}</span>
            <span class="script-row-copy"><span class="script-name">${escapeHtml(script.name)}</span><span class="script-host">${escapeHtml(hostLabel(script))}</span></span>
            ${script.id === state.selectedId ? `<span class="active-chevron">›</span>` : ""}
          </button>`).join("")}
      </section>`).join("") || `<div class="empty-list">${state.scripts.length ? "没有匹配的脚本" : "还没有可用脚本"}</div>`;
}

function renderDetail(): void {
  const script = selectedScript();
  if (!script) {
    detail.innerHTML = `<section class="welcome-card"><div class="welcome-orbit">✦</div><h2>${escapeHtml(state.status)}</h2><p>请检查脚本目录或选择一个脚本。</p></section>`;
    return;
  }

  const inputButtons = selectionButtons(script);
  const runDisabled = !script.valid || !state.inputs.length || state.startingRun || Boolean(state.activeRunId);
  detail.innerHTML = `
    <section class="script-heading">
      <div class="breadcrumbs"><span>脚本库</span><span>/</span><span>${escapeHtml(script.group)}</span></div>
      <div class="title-line"><div><div class="title-tags">${renderHostTag(script.host, hostGlyph(script.host), hostLabel(script))}${script.multi ? `<span class="soft-tag">支持批量</span>` : `<span class="soft-tag">单项处理</span>`}${script.destructive ? `<span class="warning-tag">需要确认</span>` : ""}</div><h2>${escapeHtml(script.name)}</h2></div></div>
      <p class="script-description">${escapeHtml(script.description || "此脚本没有提供描述。")}</p>
      ${script.requires.length ? `<div class="requires"><span>Python 依赖</span>${script.requires.map((item) => `<span class="dependency-chip">${escapeHtml(item)}</span>`).join("")}</div>` : ""}
      ${script.valid ? "" : `<div class="error-banner"><strong>脚本声明有误</strong><span>${escapeHtml(script.errors.join(" "))}</span></div>`}
    </section>
    <section class="work-card">
      <div class="section-heading"><div><span class="section-number">01</span><div><h3>输入内容</h3><p>${script.accepts === "file" ? "添加要处理的文件" : script.accepts === "dir" ? "添加要处理的文件夹" : "添加文件或文件夹"}</p></div></div><span class="selection-count">${state.inputs.length} 项</span></div>
      <div class="drop-zone ${state.inputs.length ? "has-inputs" : ""}" id="drop-zone">
        <div class="drop-icon">⇧</div><div class="drop-copy"><strong>拖放到这里</strong><span>或从资源管理器拖入文件${script.accepts === "file" ? "" : "或文件夹"}</span></div>
        <div class="pick-actions">${inputButtons}</div>
      </div>
      ${renderInputList()}
    </section>
    <section class="work-card params-card">
      <div class="section-heading"><div><span class="section-number">02</span><div><h3>运行参数</h3><p>参数值将传递给脚本宿主</p></div></div></div>
      ${script.params.length ? `<div class="param-grid">${script.params.map(renderParameter).join("")}</div>` : `<div class="no-params">此脚本使用默认参数。</div>`}
    </section>
    <div class="run-actions">
      <div class="run-hint"><span class="hint-dot"></span>${escapeHtml(state.status)}</div>
      <div class="run-buttons">${state.activeRunId ? `<button class="stop-button" type="button" data-action="stop">停止运行</button>` : ""}<button class="run-button" type="button" data-action="run" ${runDisabled ? "disabled" : ""}><span>▶</span>运行脚本</button></div>
    </div>`;
}

function selectionButtons(script: ScriptInfo): string {
  const buttons: string[] = [];
  if (script.accepts !== "dir") buttons.push(`<button type="button" class="pick-button" data-action="pick-files">选择文件</button>`);
  if (script.accepts !== "file") buttons.push(`<button type="button" class="pick-button secondary" data-action="pick-dirs">选择文件夹</button>`);
  return buttons.join("");
}

function renderInputList(): string {
  if (!state.inputs.length) return "";
  return `<div class="input-list">${state.inputs.map((item, index) => `
    <div class="input-item"><span class="file-glyph">${item.endsWith("\\") ? "▰" : "▧"}</span><span class="input-path" title="${escapeHtml(item)}">${escapeHtml(item)}</span><button type="button" class="remove-input" data-action="remove-input" data-index="${index}" aria-label="移除">×</button></div>`).join("")}</div>`;
}

function renderParameter(parameter: ParamInfo): string {
  const current = state.parameters[parameter.name] ?? parameter.default;
  const id = `param-${parameter.name}`;
  const control = parameter.kind === "bool"
    ? `<label class="toggle-control"><input id="${id}" type="checkbox" data-param="${escapeHtml(parameter.name)}" ${current === true || current === "true" ? "checked" : ""}><span class="toggle-track"></span><span>${current === true || current === "true" ? "开启" : "关闭"}</span></label>`
    : parameter.kind === "choice"
      ? `<select id="${id}" data-param="${escapeHtml(parameter.name)}">${parameter.choices.map((choice) => `<option value="${escapeHtml(choice)}" ${String(current) === choice ? "selected" : ""}>${escapeHtml(choice)}</option>`).join("")}</select>`
      : `<input id="${id}" type="${parameter.kind === "int" || parameter.kind === "float" ? "number" : "text"}" ${numberConstraints(parameter)} data-param="${escapeHtml(parameter.name)}" value="${escapeHtml(String(current))}" ${parameter.kind === "str" || parameter.kind === "path" ? "readonly" : ""} ${parameter.kind === "float" ? "step=any" : ""}>`;
  const presets = parameter.presets.length ? `<div class="presets">${parameter.presets.map((preset) => `<button type="button" data-action="preset" data-param="${escapeHtml(parameter.name)}" data-value="${escapeHtml(preset)}" class="preset-button ${String(current) === preset ? "selected" : ""}">${escapeHtml(preset)}</button>`).join("")}</div>` : "";
  return `<label class="param-field" for="${id}"><span class="param-label">${escapeHtml(parameter.label)}</span><span class="param-control">${control}</span>${presets}${parameter.constraints ? `<span class="param-constraint">${escapeHtml(parameter.constraints)}</span>` : ""}</label>`;
}

function numberConstraints(parameter: ParamInfo): string {
  const range = parameter.constraints?.match(/(-?\d+(?:\.\d+)?)\.\.(-?\d+(?:\.\d+)?)/);
  return range ? `min="${range[1]}" max="${range[2]}"` : "";
}

async function onDetailClick(event: MouseEvent): Promise<void> {
  const button = (event.target as HTMLElement).closest<HTMLButtonElement>("button[data-action]");
  if (!button) return;
  const action = button.dataset.action;
  if (action === "pick-files") await pickInputs(false);
  else if (action === "pick-dirs") await pickInputs(true);
  else if (action === "remove-input") {
    state.inputs.splice(Number(button.dataset.index), 1);
    renderDetail();
  } else if (action === "preset") {
    const paramName = button.dataset.param;
    const paramValue = button.dataset.value;
    if (paramName && paramValue !== undefined) {
      state.parameters[paramName] = paramValue;
      renderDetail();
    }
  } else if (action === "run") await runSelected();
  else if (action === "stop") await stopActiveRun();
}

function onParameterChange(event: Event): void {
  const control = event.target as HTMLInputElement | HTMLSelectElement;
  const name = control.dataset.param;
  if (!name) return;
  state.parameters[name] = control instanceof HTMLInputElement && control.type === "checkbox" ? control.checked : control.value;
}

async function pickInputs(directories: boolean): Promise<void> {
  const script = selectedScript();
  if (!script) return;
  try {
    const extensions = script.extensions.map((value) => value.replace(/^\./, ""));
    const result = await open({
      multiple: script.multi,
      directory: directories,
      ...(directories || !extensions.length ? {} : { filters: [{ name: "支持的文件", extensions }] }),
    });
    if (!result) return;
    acceptInputs(Array.isArray(result) ? result : [result]);
  } catch (error) {
    showBanner(errorMessage(error));
  }
}

function acceptInputs(paths: string[]): void {
  const script = selectedScript();
  if (!script) return;
  state.inputs = script.multi ? [...new Set([...state.inputs, ...paths])] : paths.slice(0, 1);
  state.status = `${state.inputs.length} 项已选择`;
  renderDetail();
}

async function runSelected(): Promise<void> {
  const script = selectedScript();
  if (!script || !script.valid || !state.inputs.length || state.activeRunId || state.startingRun) return;

  state.startingRun = true;
  state.pendingEvents = [];
  state.status = script.destructive ? "等待确认…" : "正在启动…";
  renderDetail();

  try {
    const confirmed = script.destructive
      ? await confirm(`${script.destructive}\n\n此操作可能覆盖或删除文件。`, {
        title: `确认运行：${script.name}`,
        kind: "warning",
        okLabel: "继续运行",
        cancelLabel: "取消",
      })
      : false;
    if (script.destructive && !confirmed) {
      state.startingRun = false;
      state.pendingEvents = [];
      state.status = "等待运行";
      renderDetail();
      return;
    }

    state.status = "正在启动…";
    renderDetail();
    const runId = await invoke<string>("run_script", {
      scriptId: script.id,
      inputs: state.inputs,
      parameters: state.parameters,
      confirmed,
    });
    state.activeRunId = runId;
    state.startingRun = false;
    state.status = "正在运行";
    setRunStatus("running", "正在运行");
    appendLog("system", `开始运行 ${script.name}`);
    for (const event of state.pendingEvents.splice(0)) {
      if (event.value.runId !== runId) continue;
      if (event.kind === "output") appendLog(event.value.stream, event.value.line);
      else handleFinished(event.value);
    }
    renderDetail();
  } catch (error) {
    state.startingRun = false;
    state.pendingEvents = [];
    state.status = "启动失败";
    setRunStatus("error", "启动失败");
    appendLog("stderr", errorMessage(error));
    renderDetail();
  }
}

function handleFinished(payload: RunFinished): void {
  state.activeRunId = null;
  state.startingRun = false;
  state.status = payload.cancelled ? "已取消" : payload.success ? "运行完成" : "运行失败";
  const statusKind = payload.cancelled ? "cancelled" : payload.success ? "success" : "error";
  setRunStatus(statusKind, state.status === "运行失败" ? failureText(payload) : state.status);
  if (payload.cancelled) appendLog("system", "任务已取消");
  if (payload.error) appendLog("stderr", payload.error);
  renderDetail();
}

async function stopActiveRun(): Promise<void> {
  const runId = state.activeRunId;
  if (!runId) return;
  state.status = "正在停止…";
  setRunStatus("running", "正在停止…");
  renderDetail();
  try {
    await invoke("stop_script", { runId });
  } catch (error) {
    if (state.activeRunId === runId) {
      state.status = "正在运行";
      setRunStatus("running", "正在运行");
      appendLog("stderr", errorMessage(error));
      showBanner(errorMessage(error));
      renderDetail();
    }
  }
}

function onScriptClick(event: MouseEvent): void {
  const button = (event.target as HTMLElement).closest<HTMLButtonElement>("button[data-script-id]");
  const id = button?.dataset.scriptId;
  if (!id) return;
  state.selectedId = id;
  state.inputs = [];
  state.status = "等待运行";
  setParameterDefaults(selectedScript());
  renderAll();
}

function setParameterDefaults(script: ScriptInfo | undefined): void {
  state.parameters = Object.fromEntries((script?.params ?? []).map((parameter) => [
    parameter.name,
    parameter.kind === "bool" ? parameter.default.toLowerCase() === "true" : parameter.default,
  ]));
}

function appendLog(stream: string, message: string): void {
  const empty = log.querySelector(".log-empty");
  empty?.remove();
  const line = document.createElement("div");
  line.className = `log-line ${stream}`;
  const label = document.createElement("span");
  label.className = "log-stream";
  label.textContent = stream === "system" ? "GO" : stream === "stderr" ? "ERR" : "OUT";
  const body = document.createElement("span");
  body.className = "log-message";
  body.textContent = message;
  line.append(label, body);
  log.append(line);
  state.logCount += 1;
  while (state.logCount > 500 && log.firstElementChild) {
    log.firstElementChild.remove();
    state.logCount -= 1;
  }
  log.scrollTop = log.scrollHeight;
}

function clearLog(): void {
  log.innerHTML = `<div class="log-empty">运行输出会显示在这里</div>`;
  state.logCount = 0;
}

function setRunStatus(kind: "idle" | "running" | "success" | "error" | "cancelled", text: string): void {
  statusLine.innerHTML = `<span class="status-dot ${kind}"></span><span>${escapeHtml(text)}</span>`;
}

function setCatalogStatus(text: string, error = false): void {
  const status = document.querySelector<HTMLElement>("#catalog-status")!;
  status.textContent = text;
  status.parentElement?.classList.toggle("error", error);
}

function showBanner(message: string): void {
  appendLog("stderr", message);
}

function failureText(result: RunFinished): string {
  return result.error ?? `运行失败，退出码 ${result.exitCode ?? "未知"}`;
}

function hostLabel(script: ScriptInfo): string {
  if (script.host === "exe") return script.exe ?? "独立后端";
  if (script.host === "blender") return "Blender";
  if (script.host === "python") return "Python";
  return `未知宿主：${script.host}`;
}

function hostGlyph(host: string): string {
  return host === "exe" ? "◇" : host === "blender" ? "◈" : "⌘";
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
