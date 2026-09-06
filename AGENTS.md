# AGENTS.md — AI 协作约定（monorepo 根）

## 每轮改动的收尾流程（硬性，逐条执行）

1. **清理**：删掉本轮产生的临时文件和诊断日志（`dropdebug.log` 之类用完即删）。
2. **静默检查**：`python tools/check_repo.py`
   —— 干净时零输出、退出码 0；有问题才逐条打印并退出非 0，修复后再继续。
   这类检查一律写成脚本跑（新增检查项直接往里加），**禁止靠模型逐字核对**。
3. **版本 bump**：根 `CMakeLists.txt` 里 `project(... VERSION 0.2.xxx)` 的 patch
   位 +1（三位数，如 0.2.000 → 0.2.001）。版本号全仓库只此一处，发行 zip
   文件名会带上它；子项目不得声明自己的版本。
4. **提交一次**：`git add -A` 后提交，标题 `v<版本>: <本轮一句话摘要>`，
   细节写正文。

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
- **发行打包**：见 docs/RELEASE.md（output/x64-<前端>/<配置>/ 自包含
  装配 + 内嵌 Python + zip 命名规则）。
- **脚本契约**：见 docs/SCRIPT_SPEC.md（脚本 docstring 头声明块，脚本实体在
  `core/python/scripts/`，前端与打包管线共同消费，启动器只读不解析代码）。
