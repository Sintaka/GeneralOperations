# 3rdparty/stb —— vendored 上游拷贝（未改动源码）

- 上游：https://github.com/nothings/stb
- 版本：master `2c980bb59875b0d32144a71867fbdebb2f77cd20`（stb_image.h /
  stb_image_write.h 多年未变，取 master 即可）
- 许可：公有域 / MIT 双许可（LICENSE 原样拷贝）

## 文件

| 文件 | 用途 |
|---|---|
| `stb_image.h` | 解码 BMP/TGA/SPH/SPA 等非 PNG/JPEG 贴图（`stbi_load_from_memory`、`stbi_info_from_memory`） |
| `stb_image_write.h` | 重编码为 PNG（`stbi_write_png_with_func`，回调写到内存，不落盘） |
| `LICENSE` | 上游原文 |

## 使用方式与适配

- **零源码适配**。两个头的 `*_IMPLEMENTATION` 由 `../tinygltf/tiny_gltf.cc`
  定义（上游默认如此），本工程因此只有一份 stb 实现；go_pmx2glb 的其余
  编译单元只 include 头文件拿声明。
- PMX 贴图处理管线：PNG/JPEG 原始字节直接内嵌（不重编码，保真）；其余
  格式解码为 RGBA8 后重编码 PNG 内嵌。alpha 通道判定用
  `stbi_info_from_memory` 的 comp（2/4 通道视为含 alpha；已知局限：带
  tRNS 的调色板 PNG 会报 3 通道，按不含 alpha 处理）。
