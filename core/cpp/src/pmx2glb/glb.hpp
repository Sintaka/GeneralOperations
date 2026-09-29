#pragma once

// go::pmx2glb —— GLB 组装/写出的内部接口（公开契约只有 convert.hpp）。
//
// convert_pmx_to_glb 拆成“组装序列化”（build_glb_bytes）与“落盘”
// （write_glb_file）两段：分开是因为日文路径落盘有一整类独立的坑
// （tinygltf 的文件接口只收 ANSI char 路径，见 3rdparty/tinygltf/README.md
// 的决策记录），单独成层才好替换、好测。

#include <cstdint>
#include <string>
#include <vector>

#include "go/pmx2glb/convert.hpp"

namespace go::pmx2glb {

// 把已解析模型组装并序列化成完整 GLB 字节流（纹理内嵌进 bufferView，
// 二进制进 BIN chunk）。经 progress 打 [2/6]..[5/6] 阶段行（每行一条，
// 无换行符；贴图加载失败等非致命警告也走 progress）。
// 失败返回 false 并把 UTF-8 原因写 *err（可空）。
bool build_glb_bytes(const PmxModel& model, const std::string& pmx_dir, float scale,
                     const ProgressFn& progress, std::vector<uint8_t>* out_glb,
                     std::string* err);

// GLB 字节原子落盘。路径按 UTF-8 解释并转 Win32 宽字符，日文路径安全；
// 先写同目录 .tmp 再替换目标，失败不会留下半截 GLB。输出目录不存在视为失败。
bool write_glb_file(const std::vector<uint8_t>& glb_bytes,
                    const std::string& glb_path_utf8, std::string* err);

}  // namespace go::pmx2glb
