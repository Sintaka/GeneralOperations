# core/cpp —— 零 Qt 的 C++ 内核与独立后端

## 边界

`core/cpp` 属于内核层：不依赖 Qt 或 `ui/`，可独立构建和测试。第三方仅以源码
vendored 在 `3rdparty/`，版本与许可证随目录保存；当前为 tinygltf v2.9.7（MIT）
和 stb image/image_write（public domain / MIT）。

## 目标

| CMake target | 用途 |
|---|---|
| `go_core_cpp` | 既有无依赖纯函数头：RGBA 水平翻转、UDIM 镜像 |
| `go_core_cpp_tests` | 对应 CTest `go_core_cpp` |
| `go_script_manifest_core` | 共享 Python 模块 docstring 声明解析与 JSON 序列化库，零 Qt/Python 运行依赖 |
| `go_script_catalog` | 同库 CLI：`go_script_catalog --root <scripts-dir>` 输出 UTF-8 JSON 清单，供 Tauri2 调用 |
| `go_script_manifest_tests` | 共享解析器及 CLI 的回归测试 |
| `go_pmx2glb_core` | PMX 2.0/2.1 解析、贴图处理、GLB 组装静态库 |
| `go_pmx2glb` | 独立后端：`go_pmx2glb --scale 0.08 -- <file.pmx>` |
| `go_pmx2glb_tests` | 对应 CTest `go_pmx2glb`，含合成 PMX 与 GLB 回读 |

`go_pmx2glb` 支持 UTF-8/UTF-16LE PMX、BDEF1/2/4、SDEF/QDEF、材质切片、
骨架与顶点 morph；输出坐标由 PMX 左手系转 glTF 右手系，贴图嵌入 GLB。
默认 scale 为 `0.08`，输出到输入同目录同名 `.glb`。非顶点 morph 与 morph 后的
显示枠、刚体、关节、软体段不参与几何转换。

Windows 路径统一由 UTF-8 显式转 UTF-16 后使用 Win32 宽字符 API，日文输入、贴图
与输出路径不经过本地代码页。发布时前端将可执行文件装配到
`tools/go_pmx2glb/go_pmx2glb.exe`；脚本契约见 `docs/SCRIPT_SPEC.md`。

## 构建与测试

从仓库根执行：

```text
cmake --preset qt5-mingw-debug
cmake --build --preset qt5-mingw-debug
ctest --test-dir build/qt5-mingw-debug --output-on-failure
```

测试完全由内存合成 fixture 驱动，覆盖 UTF-8/UTF-16、1/2/4 字节顶点索引、五种
蒙皮、骨骼可选字段、截断输入、材质、内嵌 PNG、稀疏 morph、Unicode 文件 IO 与
CLI 端到端。仓库未附带第三方真实 PMX 模型，避免测试资产和授权不清。
