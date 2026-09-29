# AGENTS.md — AI 协作约定（monorepo 根）

## 新会话必读与角色

每次在本项目开始开发新会话，主脑先读取并应用以下三个 skill，再查项目代码；
不能只凭名称或旧会话记忆代替阅读：

1. `orchestrating-subagents`：`C:/Users/Administrator/.agents/skills/orchestrating-subagents/SKILL.md`
2. `feature-map`：`C:/Users/Administrator/.agents/skills/feature-map/SKILL.md`
3. `symptom-map`：`C:/Users/Administrator/.agents/skills/symptom-map/SKILL.md`

主脑负责目标、架构、接口、跨任务裁决、最终验证和提交；子智能体接任务时必须
明确自称“子智能体”，知道自己的任务边界、写入文件集合与验收标准，不得把
局部结论当成全项目决策。优先派发 **Luna 6 Max**；同一子任务失败超过一次，
或任务引导修改三次后仍不达标，可切换 **Sol 6 high**。派发与升级由主脑判断，
同一个文件同时只能有一个写入者；子智能体不再派生子智能体。

涉及 Tauri 2、Rust 后端、IPC 或桌面壳时，另读并应用
`C:/Users/Administrator/.codex/skills/tauri-dev/SKILL.md`；其中针对其他项目的
架构样例只是参考，本项目的 `docs/ARCHITECTURE.md` 和源码契约优先。

功能定位先查 `docs/feature-map.yaml`，未命中再查源码并按已核实事实补录。
用户可见故障先查 `docs/symptom-map.yaml` 的取证分流；仅在得到可复用、已验证
的诊断方法时更新症状条目。索引不在时继续工作，不把缺项当作功能不存在。

## 分支与验收

本地 `codex/dev` 用于准备待验收改动，不在该分支提交，也不推送该分支。
用户验收后才把已核实的改动提交到 `main`；远端只推送 `main`。
改写远端历史（强制推送、删除标签）须在实际目标提交与受影响的远端引用均
已列清、用户验收后执行，不能把普通开发提交自动当成历史改写授权。

## 验收后的收尾流程（硬性，逐条执行）

1. **清理**：删掉本轮产生的临时文件和诊断日志（`dropdebug.log` 之类用完即删）。
2. **静默检查**：`python tools/check_repo.py`
   —— 干净时零输出、退出码 0；有问题才逐条打印并退出非 0，修复后再继续。
   这类检查一律写成脚本跑（新增检查项直接往里加），**禁止靠模型逐字核对**。
3. **版本 bump**：根 `CMakeLists.txt` 里 `project(... VERSION 0.2.xxx)` 的 patch
   位 +1（三位数，如 0.2.000 → 0.2.001）。版本号全仓库只此一处，发行 zip
   文件名会带上它；子项目不得声明自己的版本。
4. **验收后在 main 提交一次**：`git add -A` 后提交，标题
   `v<版本>: <本轮一句话摘要>`，细节写正文。验收前不暂存、不提交、不推送。

## 其它既定惯例

- **踩坑记录**：只记"以后还会撞上、不记就找不回来"的坑 —— 框架/平台的
  反直觉行为，且知识无法从代码注释里恢复。修完即完事的普通 bug 不写文档，
  结论写进代码注释即可；失去价值的旧记录直接删。达到标准的才在
  `docs/pitfalls/` 写最简 md（现象/根因/解决/验证）并在该目录 README.md
  索引加一行。
- **架构分层（monorepo）**：`ui/` 只放前端，`core/` 只放后端，依赖方向 ui → core
  单向 —— ui 不 include/import core 的内部实现，core 不依赖任何 Qt；
  `core/python/scripts/` 下的脚本禁止 Qt 绑定（`tools/check_repo.py` 与打包期
  双重拦截）。边界靠目录 + 依赖方向 + 根 CMake 单点维持，不靠仓库物理隔离，
  详见 docs/ARCHITECTURE.md。
- **新前端/后端接入**：步骤与契约见 docs/ARCHITECTURE.md
  （「新增一个前端」「新增一个后端」两节），先读再动手。
- **脚本声明单一解析器**：`core/cpp` 的 `go_script_manifest_core` 是
  docstring 声明提取、字段校验和 JSON 序列化的唯一实现。Qt 前端链接公开库，
  Tauri 等独立工具链调用同库 CLI；前端只适配模型，不复制解析规则。
- **发行打包**：见 docs/RELEASE.md（output/x64-<前端>/<配置>/ 便携
  装配 + 目标机首次准备 Python 运行环境 + zip 命名规则）。
- **脚本契约**：见 docs/SCRIPT_SPEC.md（脚本 docstring 头声明块，脚本实体在
  `core/python/scripts/`，前端与打包管线共同消费，启动器只读不解析代码）。
