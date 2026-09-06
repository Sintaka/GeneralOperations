// go_core_cpp 自测可执行。
//
// 刻意不引任何测试框架：这个 exe 的职责是“证明 core/cpp 槽位存在且可用”，
// 依赖越少越不容易腐烂。它进默认构建（理由见 CMakeLists.txt 注释），
// 每次构建都会编译并可通过 ctest 运行。
//
// 约定：所有控制台输出用 ASCII——Windows 控制台代码页（如 GBK/CP936）与
// UTF-8 源码不一致时中文会打成乱码，失败信息必须永远可读；中文只出现在注释。

#include <go/core/flip.hpp>
#include <go/core/udim.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;
const char* g_case = "(setup)";

/// 简单断言宏：失败打印 文件:行 + 当前用例名 + 断言原文，累计失败数，不中断。
/// 不中断是为了一次跑完看到全部失败点；整体成败由 main 的返回值表达
/// （有失败 return 1，ctest 即判红）。
#define GO_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d  case=%s  check=%s\n",                      \
                        __FILE__, __LINE__, g_case, #cond);                     \
        }                                                                       \
    } while (0)

constexpr std::ptrdiff_t kBytesPerPixel = 4;
constexpr std::uint8_t kPadding = 0xEE;  // 行尾填充哨兵：翻转后必须原样不动

/// 造测试图：R 编码列号（翻转后第 x 列应为原第 width-1-x 列），
/// G 编码行号（翻转后每行的 G 必须不变——检查“首尾行不被误翻”：
/// 行序不被打乱、行与行经 stride 不互相污染），B/A 固定标记防分量错位。
/// 乘系数（7/11）让相邻行列取值不同，错位/串行一眼可辨。
void fill_gradient(std::uint8_t* buf, int width, int height, std::ptrdiff_t stride)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* p = buf + y * stride + x * kBytesPerPixel;
            p[0] = static_cast<std::uint8_t>(x * 7 + 1);
            p[1] = static_cast<std::uint8_t>(y * 11 + 2);
            p[2] = 0x5A;
            p[3] = 0xFF;
        }
    }
}

/// 逐像素核对翻转后的期望图（含第 0 行与最后一行）。发现第一个坏像素即报
/// 详细位置并停止本图检查，避免刷屏。
void expect_flipped(const std::uint8_t* buf, int width, int height, std::ptrdiff_t stride)
{
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = buf + y * stride;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + x * kBytesPerPixel;
            const std::uint8_t want_r = static_cast<std::uint8_t>((width - 1 - x) * 7 + 1);
            const std::uint8_t want_g = static_cast<std::uint8_t>(y * 11 + 2);
            if (p[0] != want_r || p[1] != want_g || p[2] != 0x5A || p[3] != 0xFF) {
                ++g_failures;
                std::printf("FAIL case=%s  pixel mismatch at (x=%d,y=%d): "
                            "got R=%d G=%d B=%d A=%d, want R=%d G=%d\n",
                            g_case, x, y, p[0], p[1], p[2], p[3], want_r, want_g);
                return;
            }
        }
    }
}

/// 核对每行行尾填充字节（[width*4, stride) 区间）未被触碰。
/// stride > width*4 时这是“翻转只动了像素、没越界串行”的直接证据。
void expect_padding_untouched(const std::uint8_t* buf, int width, int height,
                              std::ptrdiff_t stride)
{
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = buf + y * stride;
        for (std::ptrdiff_t off = width * kBytesPerPixel; off < stride; ++off) {
            if (row[off] != kPadding) {
                ++g_failures;
                std::printf("FAIL case=%s  padding changed at (row=%d,offset=%d): got %d\n",
                            g_case, y, static_cast<int>(off), row[off]);
                return;
            }
        }
    }
}

void test_flip_even_width()
{
    g_case = "flip_even_width";
    const int width = 4;
    const int height = 3;
    const std::ptrdiff_t stride = width * kBytesPerPixel;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, kPadding);
    fill_gradient(buf.data(), width, height, stride);

    go::core::flip_horizontal_rgba8(buf.data(), width, height, stride);

    // 偶数宽：所有列两两对换，无不动列。
    expect_flipped(buf.data(), width, height, stride);
}

void test_flip_odd_width()
{
    g_case = "flip_odd_width";
    const int width = 5;
    const int height = 2;
    const std::ptrdiff_t stride = width * kBytesPerPixel;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, kPadding);
    fill_gradient(buf.data(), width, height, stride);

    go::core::flip_horizontal_rgba8(buf.data(), width, height, stride);

    // 奇数宽：中列（x=2）与自己对换，值必须保持 x*7+1。
    const std::uint8_t* mid = buf.data() + kBytesPerPixel * 2;
    GO_CHECK(mid[0] == 2 * 7 + 1 && mid[1] == 2 && mid[2] == 0x5A && mid[3] == 0xFF);
    expect_flipped(buf.data(), width, height, stride);
}

void test_flip_single_pixel_width()
{
    g_case = "flip_single_pixel_width";
    // 宽度为 1：翻与不翻等价，且必须不出错、不动值。
    const int width = 1;
    const int height = 3;
    const std::ptrdiff_t stride = width * kBytesPerPixel;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, kPadding);
    fill_gradient(buf.data(), width, height, stride);

    go::core::flip_horizontal_rgba8(buf.data(), width, height, stride);

    expect_flipped(buf.data(), width, height, stride);
}

void test_flip_with_stride_padding()
{
    g_case = "flip_with_stride_padding";
    // stride > width*4（行尾多 5 字节，取奇数防止“恰好按 4 字节对齐侥幸对”的
    // 错误实现通过）。翻转必须只动像素：行尾填充原样，且首行仍是首行、
    // 末行仍是末行（G 编码行号恒定），即多行缓冲的首尾行不被误翻。
    const int width = 4;
    const int height = 3;
    const std::ptrdiff_t stride = width * kBytesPerPixel + 5;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, kPadding);
    fill_gradient(buf.data(), width, height, stride);

    go::core::flip_horizontal_rgba8(buf.data(), width, height, stride);

    expect_flipped(buf.data(), width, height, stride);
    expect_padding_untouched(buf.data(), width, height, stride);
    // 首行/末行的行号标记仍在原行位置（行序未被交换，只有水平镜像）。
    GO_CHECK(buf.data()[1] == 0 * 11 + 2);                       // 第 0 行的 G
    GO_CHECK((buf.data() + static_cast<std::size_t>(stride) * (height - 1))[1]
             == static_cast<std::uint8_t>((height - 1) * 11 + 2));  // 末行的 G
}

void test_flip_rejects_insufficient_stride()
{
    g_case = "flip_rejects_insufficient_stride";
    // 契约：stride < width*4 属于语义无法定义的输入，必须拒绝且不动缓冲。
    const int width = 4;
    const int height = 2;
    const std::ptrdiff_t stride = width * kBytesPerPixel - 1;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, 0xAA);

    go::core::flip_horizontal_rgba8(buf.data(), width, height, stride);

    bool untouched = true;
    for (const std::uint8_t b : buf) {
        untouched = untouched && (b == 0xAA);
    }
    GO_CHECK(untouched);
}

void test_flip_degenerate_inputs()
{
    g_case = "flip_degenerate_inputs";
    // 空指针 / 零尺寸 / 负 stride：契约规定“无害返回”。不崩就是第一层断言，
    // 再用一次真实调用确认函数状态未受影响。
    go::core::flip_horizontal_rgba8(nullptr, 4, 4, 16);
    std::uint8_t one[4] = {1, 2, 3, 4};
    go::core::flip_horizontal_rgba8(one, 0, 4, 0);
    go::core::flip_horizontal_rgba8(one, 4, 0, 16);
    go::core::flip_horizontal_rgba8(one, 4, 4, 15);  // stride 不足：拒绝
    GO_CHECK(one[0] == 1 && one[1] == 2 && one[2] == 3 && one[3] == 4);

    // 随后正常翻转仍工作（1 像素宽，翻与不翻等价）。
    go::core::flip_horizontal_rgba8(one, 1, 1, 4);
    GO_CHECK(one[0] == 1 && one[1] == 2 && one[2] == 3 && one[3] == 4);
}

void test_udim_mirror_known_values()
{
    g_case = "udim_mirror_known_values";
    GO_CHECK(go::core::udim_mirror(1001) == 1010);  // 第 0 行首 <-> 第 0 行尾
    GO_CHECK(go::core::udim_mirror(1010) == 1001);
    GO_CHECK(go::core::udim_mirror(1005) == 1006);  // 行内中间对
    GO_CHECK(go::core::udim_mirror(1006) == 1005);
    GO_CHECK(go::core::udim_mirror(1011) == 1020);  // 第 1 行首 <-> 第 1 行尾
    GO_CHECK(go::core::udim_mirror(1020) == 1011);
    GO_CHECK(go::core::udim_mirror(1050) == 1041);  // 第 4 行：行尾列 <-> 行首列
    GO_CHECK(go::core::udim_mirror(1500) == 1491);  // 第 49 行
    GO_CHECK(go::core::udim_mirror(1991) == 2000);  // 最后一行（第 99 行）首尾
    GO_CHECK(go::core::udim_mirror(2000) == 1991);
}

void test_udim_mirror_involution()
{
    g_case = "udim_mirror_involution";
    // 对合性质：镜像两次必须回到原值。全网格 1000 个合法编号遍历一遍，
    // 比罗列样本更能防边界错（例如 2000 的镜像）。
    bool ok = true;
    for (int u = 1001; u <= 2000; ++u) {
        ok = ok && (go::core::udim_mirror(go::core::udim_mirror(u)) == u);
    }
    GO_CHECK(ok);
}

void test_udim_mirror_invalid_passthrough()
{
    g_case = "udim_mirror_invalid_passthrough";
    // 合法范围 1001..2000 之外原值返回（约定见 udim.hpp）。
    GO_CHECK(go::core::udim_mirror(1000) == 1000);  // 恰低于下界
    GO_CHECK(go::core::udim_mirror(2001) == 2001);  // 恰高于上界
    GO_CHECK(go::core::udim_mirror(999) == 999);
    GO_CHECK(go::core::udim_mirror(0) == 0);
    GO_CHECK(go::core::udim_mirror(-1) == -1);
    GO_CHECK(go::core::udim_mirror(3000) == 3000);
}

}  // namespace

int main()
{
    test_flip_even_width();
    test_flip_odd_width();
    test_flip_single_pixel_width();
    test_flip_with_stride_padding();
    test_flip_rejects_insufficient_stride();
    test_flip_degenerate_inputs();
    test_udim_mirror_known_values();
    test_udim_mirror_involution();
    test_udim_mirror_invalid_passthrough();

    if (g_failures != 0) {
        std::printf("go_core_cpp_tests: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("go_core_cpp_tests: all ok\n");
    return 0;
}
