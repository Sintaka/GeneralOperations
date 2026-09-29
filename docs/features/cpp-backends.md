# C++ 内核、共享声明解析与 PMX→GLB 后端

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-28，CMake target、公开接口、脚本声明解析与 PMX→GLB 命令行入口已对照源码核对。该目录不依赖 Qt；根 CMake 无条件加入此内核。`go_core_cpp` 提供纯函数头，`go_script_manifest_core` 为两个前端提供同一套声明规则，`go_pmx2glb` 是可由脚本 `@host exe` 声明调用的独立程序。

## 入口与关键实现

- [core/cpp/CMakeLists.txt](../../core/cpp/CMakeLists.txt)：定义纯函数内核、共享声明解析库/CLI、PMX→GLB 后端及测试目标。
- [catalog.hpp](../../core/cpp/include/go/manifest/catalog.hpp)：模块 docstring 声明解析和 JSON catalog 的公开接口；Qt5 直接链接，Tauri2 调用由同库构建的 CLI。
- [flip.hpp](../../core/cpp/include/go/core/flip.hpp)：RGBA8 缓冲逐行水平翻转纯函数。
- [udim.hpp](../../core/cpp/include/go/core/udim.hpp)：标准 UDIM 网格编号行内镜像纯函数。
- [convert.hpp](../../core/cpp/include/go/pmx2glb/convert.hpp)：解析后的 PMX 模型到 GLB 的公开转换接口。
- [pmx2glb/main.cpp](../../core/cpp/src/pmx2glb/main.cpp)：`go_pmx2glb` CLI，读取 `--scale` 和 `--` 后的 PMX 文件列表。
- [core/cpp/README.md](../../core/cpp/README.md)：目标职责、输入支持范围和构建入口。
- [Qt5 启动器专题](qt5-launcher.md)：`@host exe` 清单的解析、后端程序定位与进程启动。

## 调用关系

两个前端先消费共享清单：Qt5 静态链接解析库，Tauri2 读取 `go_script_catalog --root <scripts-dir>` 的 JSON。`geo.pmx2glb.py` 以 `@host exe` / `@exe go_pmx2glb` 声明宿主程序；运行器定位构建或发行包中的后端可执行文件后传入 CLI 参数和文件列表。脚本契约见 [Python 脚本专题](python-script-contract.md)。

## 验证入口

- [go_core_cpp_tests.cpp](../../core/cpp/tests/go_core_cpp_tests.cpp)：纯函数内核测试源码。
- [go_pmx_parser_tests.cpp](../../core/cpp/tests/go_pmx_parser_tests.cpp)：PMX 解析和转换后端测试源码；CMake 注册为 `go_pmx2glb`。
