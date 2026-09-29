#pragma once

// go::pmx2glb —— PMX 2.0/2.1 二进制解析（纯内存，不做任何文件 IO）。
//
// 为什么自写解析器而不用现成库（选型过程见本目录 README 与
// core/cpp/3rdparty 下的 vendor 决策）：
//   - 首选的 benikabocha/saba PMXFile 依赖闭包失控：牵出 spdlog（日志库）、
//     glm（图形数学库）、saba::File（fopen + ANSI 路径，日文路径直接坏）、
//     SjisToUnicode（9300 行码表），远超 10 文件预算；且其 QDEF 分支存在
//     越界 bug（按 m_boneWeights[3]/[4] 读取）。
//   - oguna/MMDFormats 是 C# 实现，同样不可直接内嵌。
// 故按规范自写。格式经三份独立实现交叉核实（mmd_tools、saba、
// MikuMikuFormats），PMX 2.1 与 2.0 在 morph 段之前结构完全一致（2.1 只在
// 文件尾部多一个软体段），而本转换器只消费 morph 之前的数据，因此读到
// morph 段结束即停——一个解析器同时覆盖两个版本，也不需要软体段的大小表。
//
// 编码字段说明（重要，与直觉不同）：PMX 规范的字符串编码字节只有两个值——
// 0=UTF-16LE、1=UTF-8。Shift_JIS 是 PMD 格式的事，PMX 没有；解析到其它值
// 一律报错，不猜。

#include <cstdint>
#include <string>
#include <vector>

namespace go::pmx2glb {

// PMX 顶点蒙皮类型（规范固定值）。
enum class PmxWeightType : uint8_t {
    BDEF1 = 0,
    BDEF2 = 1,
    BDEF4 = 2,
    SDEF = 3,
    QDEF = 4,
};

struct PmxVertex {
    float position[3] = {0, 0, 0};
    float normal[3] = {0, 0, 1};
    float uv[2] = {0, 0};
    PmxWeightType weightType = PmxWeightType::BDEF1;
    int32_t boneIndices[4] = {-1, -1, -1, -1};
    float boneWeights[4] = {0, 0, 0, 0};
};

struct PmxMaterial {
    std::string name;  // UTF-8
    float diffuse[4] = {0, 0, 0, 1};
    int32_t textureIndex = -1;      // 指向 textures 下标；-1 = 无贴图
    bool doubleSided = false;       // 材质 flag bit0（両面表示）
    int32_t faceVertexCount = 0;    // 本材质覆盖的顶点索引数（3 的倍数）
};

struct PmxBone {
    std::string name;  // UTF-8
    // rest 姿态的绝对位置：PMX 存的是绝对值，不是父相对偏移，
    // 因此骨骼世界位置无需逐级累乘。
    float position[3] = {0, 0, 0};
    int32_t parent = -1;  // -1 = 根骨骼
};

struct PmxVertexMorph {
    std::string name;                // UTF-8
    std::vector<int32_t> vertexIndices;
    std::vector<float> offsets;      // 每顶点 3 个 float，与 vertexIndices 对齐
};

struct PmxModel {
    float version = 0.0f;  // 2.0 或 2.1
    std::string modelName;  // UTF-8
    std::vector<PmxVertex> vertices;
    std::vector<uint32_t> indices;  // 三角形顶点索引，PMX 原始顺序
    std::vector<std::string> textures;  // 相对 pmx 目录的路径，分隔符保留原样
    std::vector<PmxMaterial> materials;
    std::vector<PmxBone> bones;
    std::vector<PmxVertexMorph> vertexMorphs;
    int skippedMorphCount = 0;  // 非顶点 morph（group/bone/uv/material 等）数量，仅供跳过提示
};

// 解析 data 指向的 PMX 字节流（size 字节）。
// 成功返回 true 并填充 out；失败返回 false，原因写入 *err（UTF-8，可空指针）。
bool parse_pmx(const uint8_t* data, size_t size, PmxModel* out, std::string* err);

}  // namespace go::pmx2glb
