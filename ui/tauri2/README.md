# ui/tauri2 —— Tauri 2 + WebView2 前端（占位）

当前状态：**仅保留位置，尚未实现。** `-DGO_FRONTEND=tauri2` configure 时根
`CMakeLists.txt` 直接 FATAL_ERROR——tauri2 不走 CMake 构建（npm + cargo +
tauri cli），本目录刻意不放 `CMakeLists.txt`，根分支没有可 add_subdirectory
的东西，拦截就发生在根。当前可用前端：qt5。

## 规划要点（接入时展开为正式 README）

- **WebView2 免分发浏览器内核，打包显著变轻。** Windows 11 自带 Evergreen
  WebView2 运行时（Windows 10 装一次系统级运行时即可），前端不随包分发浏览器
  内核与 UI 框架运行时——qt5 发行包里 Qt dll + QML 模块那一大坨在 tauri2 形态下
  不存在，发行体积的大头只剩内嵌 Python。代价是界面要用 web 技术栈另写，观感
  与 qt5 版不必逐像素一致——它是"另一套前端"，不是 qt5 的移植（对照
  `ui/qt6.8lts/README.md` 的"演进而非重写"，两条路线的取舍不同）。

- **Rust 后端经 Tauri command 调 core。** Tauri 的 `src-tauri`（Rust）用
  command 把能力暴露给 web 前端；对 core 的消费与 qt5 的 `QProcess` 托管同构：
  Rust 侧起 Python 子进程跑脚本、逐行收 stdout/stderr、按退出码判成败，脚本
  目录解析链（exe 同级 `scripts/` → 开发期路径 → 明确报"目录不存在"）沿用同一
  契约。依赖方向铁律不变：Rust/JS 只碰脚本文件与 docstring 契约，core 对
  前端零感知。

- **脚本清单契约复用：docstring 头离线导出 JSON。** web 前端更没有理由解析
  Python——`core/python/scripts` 各脚本的 docstring 契约头（规范见
  `docs/SCRIPT_SPEC.md`）由一个离线导出器汇总成 JSON（`@name` / `@group` /
  `@desc` / `@accepts` / `@ext` / `@multi` / `@requires` / `@destructive` /
  `@param` 全量字段），前端只消费这份 JSON。与 qt5 前端"只读 docstring、不解析
  代码"是同一条契约的两种消费形态；JSON 字段与 `docs/SCRIPT_SPEC.md` 一一
  对应，不另发明第二套清单格式。

- **不走 CMake 构建，根 CMake 只拦不建。** 前端 npm、Rust 走 cargo、壳用
  tauri cli，根 `CMakePresets.json` 不为它加 preset；`.vscode/tasks.json`
  接入时用 npm/cargo 命令实现同等的配置 / 构建 / 运行任务组。发行装配命名
  仍遵守根契约（装配落 `output/x64-tauri2/<配置>/`，zip 为
  `GeneralOperations-<版本>_x64-tauri2-<配置>.zip`，即
  `GO_BUNDLE_DIR` / `GO_ZIP_FILE` 的 tauri2 取值），装配工具从 `bundle.cmake`
  换成 tauri 侧脚本——"产物命名是根单点"的语义不变，只是实现换了。

- **.gitignore 已预埋。** `node_modules/`（npm 依赖）、`dist/`（前端构建
  产物）、`target/`（Rust 构建产物）三条已写进根 `.gitignore`（附注释），
  接入当天不会误提交。

接入步骤对照 `docs/ARCHITECTURE.md`「新增一个前端」的五件套：本 README 是
第 1 件；根 `CMakeLists.txt` 分支已就位（当前 FATAL_ERROR，接入时按上面
「不走 CMake 构建」一条从"拦截"改为"指路"）；任务组与文档索引届时补齐
（preset 组按同一节说明对不走 CMake 的前端不适用）。
