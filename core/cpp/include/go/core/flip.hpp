#pragma once

// go::core 水平翻转内核（RGBA8、原位）。
//
// 对应 python 内核脚本 img.FlipImage_Horizontal 的未来 C++ 化：python 版以
// 文件为单位读写整图，C++ 版下沉为对内存缓冲的纯函数——这样行翻转才能被
// 拆成互不依赖的分片，交给未来的并行后端（这正是 core/cpp 槽位存在的意义，
// 见本目录 README.md）。
//
// 自包含、零第三方依赖、零 Qt 头：core 层不认识 UI。

#include <cstddef>
#include <cstdint>

namespace go::core {

/// 行主序 RGBA8 缓冲的水平翻转（原位、逐行独立）。
///
/// 契约：
///  - pixels 指向缓冲首字节（第 0 行第 0 列像素的 R 分量）；
///  - width/height 单位是像素（不是字节）；
///  - stride_bytes 是每行字节数，允许行尾填充（stride_bytes > width*4）；
///
/// 边界（为什么是“拒绝”而不是“处理”）：stride_bytes < width*4 意味着
/// 一行放不下 width 个像素、行与行互相重叠覆盖，任何翻转语义都无从定义。
/// 这里选择显式拒绝——直接返回、不动缓冲、不报错——而不是猜一个含义；
/// 报告参数错误是调用方（未来的前端壳）的职责，core 不抛异常、不碰 UI。
/// pixels 为空或 width/height <= 0 同样按“无事可做”返回。
///
/// 为什么逐行独立翻转：水平镜像不改变行序——第 0 行翻转后仍是第 0 行。
/// 因此每行可独立处理，未来按行分片并行天然安全，无需任何锁。
inline void flip_horizontal_rgba8(std::uint8_t* pixels, int width, int height,
                                  std::ptrdiff_t stride_bytes)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;  // 空输入：无事可做（理由见上方契约说明）。
    }
    const std::ptrdiff_t min_stride = static_cast<std::ptrdiff_t>(width) * 4;
    if (stride_bytes < min_stride) {
        return;  // stride 不足：语义无法定义，显式拒绝（理由见上方契约说明）。
    }

    // 只交换轴对称像素对（x 与 width-1-x）。奇数宽时中列与自己对称，
    // 循环上界 width/2 自然把它排除在外，不用特判。
    //
    // 为什么按 4 个分量字节交换、而不是按 32 位整数交换：pixels 不保证
    // 4 字节对齐，按整数读写会有未定义行为风险。占位期以正确、清晰为先，
    // 向量化（SIMD/并行）留给正式后端做。
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = pixels + static_cast<std::ptrdiff_t>(y) * stride_bytes;
        for (int x = 0; x < width / 2; ++x) {
            std::uint8_t* left = row + static_cast<std::ptrdiff_t>(x) * 4;
            std::uint8_t* right = row + static_cast<std::ptrdiff_t>(width - 1 - x) * 4;
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t tmp = left[c];
                left[c] = right[c];
                right[c] = tmp;
            }
        }
    }
}

}  // namespace go::core
