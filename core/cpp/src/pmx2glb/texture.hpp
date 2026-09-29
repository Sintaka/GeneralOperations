#pragma once

// go::pmx2glb —— PMX 贴图 → 可嵌入 GLB 的字节（内部接口，glb.cpp 与测试消费）。
//
// 职责边界：这里只做“文件定位 + 格式嗅探 + 解码转码”，不认识 glTF 结构；
// bufferView/材质组装在 glb.cpp。stb 的实现符号（STB_IMAGE(_WRITE)_
// IMPLEMENTATION）由 3rdparty/tinygltf/tiny_gltf.cc 唯一持有（全仓库只有
// 一份 stb 实现，见该目录 README 的决策记录），本文件只 include stb 头拿声明，
// 再定义一遍实现宏就是链接期符号冲突。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go::pmx2glb {

// 一张可直接嵌入 GLB 的贴图：字节 + mimeType + alpha 判定。
// has_alpha 的语义是“通道层面带 alpha”（对应材质 alphaMode=BLEND 的判定）：
//   - PNG：IHDR colorType 4/6；JPEG：恒 false；
//   - 解码转码路径（BMP/TGA/…）：解码时的源通道数（32bpp → true）。
//     必须用源通道数而不是重编码 PNG 的 IHDR——stb 统一按 RGBA 重编码，
//     24bpp BMP 也会变成 colorType 6，按后者判定会把不透明贴图误判成 BLEND。
struct TextureBlob {
    std::vector<uint8_t> bytes;
    std::string mime_type;
    bool has_alpha = false;
};

// 文件头嗅探：图片是否携带 alpha 通道（只读头部几十字节，不解码）。
// PNG 看 IHDR 的 colorType（4=灰+α / 6=RGBA）；BMP/TGA 看位深是否 32；
// JPEG 无 alpha。识别不了的格式返回 false（嗅探失败≠无 alpha），
// 调用方应退回只看材质 diffuse 的判定。
bool sniff_has_alpha(const uint8_t* data, size_t size, bool* out_has_alpha);

// 定位并加载一张 PMX 纹理，产出可嵌入 GLB 的字节。
//   pmx_dir  PMX 文件所在目录（UTF-8），贴图相对路径的基准。
//   tex_rel  PMX textures 段里的相对路径（UTF-8，分隔符 / 或 \ 都可能出现）。
// 解析顺序：先 <pmx_dir>/<tex_rel>，再 <pmx_dir>/textures/<tex_rel>；两处
// 都先按原样试，失败再做 Windows 习惯的大小写不敏感重定位（逐级目录遍历
// 匹配——MMD 模型包里 Textures/TEX_A.PNG 与配置里 textures/tex_a.png
// 混用是常态，严格匹配会大面积丢贴图）。
// PNG/JPEG 原始字节直接透传（GLB 规范原生支持，重编码白损画质和时间）；
// BMP/TGA 等其它 stb 认识的格式解码后统一转成 PNG 内嵌。
// 失败返回 false（文件缺失/读不出/解码失败），原因写 *err（UTF-8，可空）；
// 非致命，调用方决定该材质退化为纯色还是中止转换。
bool load_glb_texture(const std::string& pmx_dir, const std::string& tex_rel,
                      TextureBlob* out, std::string* err);

}  // namespace go::pmx2glb
