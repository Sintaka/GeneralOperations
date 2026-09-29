#pragma once

// go::core UDIM 行镜像内核。
//
// 对应 python 内核脚本 img.Zbrush_UDIM_Correction 的数学核心：Zbrush 导出的
// UDIM 序列水平方向与 Mari/标准约定相反，需要“按 UDIM 行做水平镜像”后再
// 重命名。python 版按“实际导入的 tile 集合”镜像（行内 tile 不齐也能翻），
// C++ 占位版先给标准网格上的纯函数——给定一个 UDIM 编号算出它的镜像编号；
// 按集合镜像的封装（以及未来对 EXR 批量流程的并行化）留给正式后端。
//
// 零第三方依赖、零 Qt 头：core 层不认识 UI。只用到 int，无需任何标准头。

namespace go::core {

/// UDIM 行内水平镜像：UDIM = 1001 + 10*row + col（col 0..9），
/// 镜像 = 同一行取 col' = 9 - col。
///
/// 约定（为什么把合法范围写死）：
///  - 采用标准 UDIM 网格 1001..2000：第 0 行是 1001..1010，
///    第 1 行是 1011..1020，……第 99 行是 1991..2000；
///  - 不在该范围内的输入（如 1000、2001、负数）不是合法 UDIM，
///    原值返回、不做镜像——批量重命名流程拿到原值即知“这个编号不由
///    本函数解释”，可以按自己的规则跳过；这比抛异常更适合逐文件的
///    批处理，也和 python 内核对“没有 UDIM 的文件”跳过不报错一致。
///
/// 已知映射示例：1001 <-> 1010、1005 <-> 1006、1011 <-> 1020。
/// 镜像是对合运算：udim_mirror(udim_mirror(u)) == u（对一切合法 u）。
inline int udim_mirror(int udim)
{
    if (udim < 1001 || udim > 2000) {
        return udim;  // 非合法 UDIM：原值返回（约定见上）。
    }
    const int row = (udim - 1001) / 10;
    const int col = (udim - 1001) % 10;
    return 1001 + 10 * row + (9 - col);
}

}  // namespace go::core
