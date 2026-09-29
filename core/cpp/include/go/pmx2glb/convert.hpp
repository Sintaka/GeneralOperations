#pragma once

// go::pmx2glb —— PMX→GLB 转换核心接口（解析与写出的粘合层）。
//
// 为什么独立一个头：解析（pmx.cpp）与 GLB 组装（glb.cpp）由不同的实现
// 文件承担，main.cpp 与两侧测试都只面向本接口——解析器的输入是字节流，
// 转换器的输入是 PmxModel，互不 include 对方的实现文件。

#include <functional>
#include <string>

#include "go/pmx2glb/pmx.hpp"

namespace go::pmx2glb {

struct ConvertOptions {
    // MMD 单位 → glTF 米制的统一缩放（主流惯例 0.08，1 单位 ≈ 8cm）。
    // 1.0 = 保持 PMX 原始单位。作用于顶点/骨骼平移/morph 位移。
    float scale = 0.08f;
};

// 进度回调：每行调用一次（UTF-8，不含行尾换行），可空。
using ProgressFn = std::function<void(const std::string& line)>;

// 把已解析的 PMX 模型写成 GLB（glTF 2.0 二进制）。
//   pmx_dir  贴图相对路径的基准目录（pmx 所在目录），UTF-8。
//   glb_path 输出文件路径，UTF-8（内部经 fs::u8path，支持日文路径）。
// 成功返回 true；失败返回 false 且 *err（可空）给出 UTF-8 原因。
// 纹理缺失/解码失败等非致命问题不打断转换，只经 progress 提示。
bool convert_pmx_to_glb(const PmxModel& model, const std::string& pmx_dir,
                        const std::string& glb_path, const ConvertOptions& options,
                        const ProgressFn& progress, std::string* err);

}  // namespace go::pmx2glb
