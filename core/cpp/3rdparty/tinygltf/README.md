# 3rdparty/tinygltf —— vendored 上游拷贝（单一工具链兼容适配）

- 上游：https://github.com/syoyo/tinygltf
- 版本：tag `v2.9.7`（commit `488a70a3df62a4df1a736e9e56fb8836580c4888`）
- 许可：MIT（LICENSE 原样拷贝）

## 文件

| 文件 | 用途 |
|---|---|
| `tiny_gltf.h` | glTF 2.0 读写（API + 实现，`TINYGLTF_IMPLEMENTATION`） |
| `tiny_gltf.cc` | 唯一实现 TU：定义 `TINYGLTF_IMPLEMENTATION` 与 `STB_IMAGE(_WRITE)_IMPLEMENTATION` |
| `json.hpp` | 上游随附的 nlohmann/json 拷贝（`tiny_gltf.h` 直接 include） |
| `LICENSE` | 上游 MIT 原文 |

## 选型记录（为什么是 v2.9.7 而不是 master 的 v3）

上游 master（2026-09，tag v3.0.1）已切换为纯 C 的 `tiny_gltf_v3.h/.c`
arena API（v3.0.0-alpha 体系）。C API 与 v2 完全不兼容且是新发布的 alpha，
风险大于收益；v2.9.x 是久经使用的稳定 C++ API（C++17 兼容，MSVC 无告警
问题），故取最后一个 v2 tag。注意：v2 全部是 .h/.cc，**没有 .c 源文件**，
因此本目录不需要 `enable_language(C)`（根 project() 只声明了 CXX）；若将来
换 v3（`tiny_gltf_v3.c`），必须在 core/cpp/CMakeLists.txt 补
`enable_language(C)`。

## 使用方式与适配

- 上游源码保持原样；唯一适配在 `tiny_gltf.cc`：MinGW GCC 8.1 虽声明
  C++17，其 `<filesystem>` 头自身无法编译，因此预定义 `JSON_HAS_CPP_14`，只关闭
  nlohmann/json 未使用的 `std::filesystem::path` 序列化重载。stb 的实现符号由
  同一 TU 里的 `STB_IMAGE(_WRITE)_IMPLEMENTATION` 编入（stb 头文件在 `../stb/`，
  靠 include 路径解析），整个工程只有一份实现，不会符号冲突；其余代码只取声明。
- 编译宏：`_CRT_SECURE_NO_WARNINGS`（tinygltf/stb 内部用 fopen 等 CRT
  函数，MSVC 默认会当错误报 C4996）。

## GLB 写出的日文路径决策（关键坑）

tinygltf 的 `WriteGltfSceneToFile` 内部用 `ofstream(char*)`，MSVC 下按
ANSI 代码页解释路径，日文输出路径会乱码。本次决策：**序列化到内存，再由
go_pmx2glb 自己写盘**——

1. `TinyGLTF::WriteGltfSceneToStream(&model, std::ostringstream, false,
   true)`（`writeBinary=true`）把完整 GLB（含 BIN chunk、内嵌纹理）序列化
   到内存流；
2. `go_pmx2glb` 用 Win32 宽字符文件 API 写同目录临时文件，再原子替换目标——
   UTF-8 先显式转 UTF-16，日文路径安全，写失败也不会留下半截 GLB。

不采用 tinygltf 的文件接口或 ANSI 临时路径：前者破坏日文路径，后者临时名
可能撞车；内存序列化在 v2.9.7 是现成接口。
