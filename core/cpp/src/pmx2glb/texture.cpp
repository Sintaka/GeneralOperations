// go::pmx2glb —— 贴图定位/嗅探/转码实现。
#include "texture.hpp"

#include <limits>

#include "path_io.hpp"
#include "stb_image.h"
#include "stb_image_write.h"

namespace go::pmx2glb {
namespace {

bool try_resolve(const std::wstring& dir, const std::wstring& relative,
                 std::wstring* out) {
    return path_io::resolve_relative(dir, relative, out);
}

bool read_file_bytes(const std::wstring& path, std::vector<uint8_t>* out) {
    return path_io::read_bytes(path, out);
}

// stb 的 PNG 编码流式回吐到内存 vector。
void stbi_write_cb(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

bool is_png(const uint8_t* d, size_t n) {
    return n >= 8 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47 &&
           d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A;
}

bool is_jpeg(const uint8_t* d, size_t n) {
    return n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}

bool is_bmp(const uint8_t* d, size_t n) { return n >= 2 && d[0] == 'B' && d[1] == 'M'; }

// TGA 没有魔数，只做保守指纹（标准 18 字节头的类型/颜色表/位深字段落在
// 合法集合）。指纹不中就当未知格式交给 stb 试解码，宁可错杀不多拒。
bool is_likely_tga(const uint8_t* d, size_t n) {
    if (n < 18) return false;
    const int color_map_type = d[1];
    const int image_type = d[2];
    if (color_map_type != 0 && color_map_type != 1) return false;
    if (image_type != 1 && image_type != 2 && image_type != 3 && image_type != 9 &&
        image_type != 10 && image_type != 11) {
        return false;
    }
    const int depth = d[16];
    return depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
}

}  // namespace

bool sniff_has_alpha(const uint8_t* data, size_t size, bool* out_has_alpha) {
    if (!out_has_alpha) return false;
    *out_has_alpha = false;
    if (!data || size == 0) return false;

    if (is_png(data, size)) {
        // colorType 在 PNG 签名(8) + IHDR 长度(4) + "IHDR"(4) + 宽(4) + 高(4)
        // + 位深(1) 之后，即偏移 25；不足 26 字节算嗅探失败。
        if (size < 26) return false;
        const int color_type = data[25];
        *out_has_alpha = (color_type == 4 || color_type == 6);
        return true;
    }
    if (is_jpeg(data, size)) {
        *out_has_alpha = false;  // JPEG 没有 alpha 通道
        return true;
    }
    if (is_bmp(data, size)) {
        // biBitCount 是 BITMAPINFOHEADER 里偏移 28 处的 LE u16（文件头 14 +
        // 信息头前 14 字节：size4+width4+height4+planes2）；位深 32 才可能带 alpha。
        if (size < 30) return false;
        const int bpp = data[28] | (data[29] << 8);
        *out_has_alpha = (bpp == 32);
        return true;
    }
    if (is_likely_tga(data, size)) {
        *out_has_alpha = (data[16] == 32);  // 头部偏移 16 = 像素位深
        return true;
    }
    return false;
}

bool load_glb_texture(const std::string& pmx_dir, const std::string& tex_rel,
                      TextureBlob* out, std::string* err) {
    if (!out) return false;
    out->bytes.clear();
    out->mime_type.clear();
    if (tex_rel.empty()) {
        if (err) *err = "empty texture path";
        return false;
    }

    std::wstring resolved;
    const std::wstring base = utf8_to_wide(pmx_dir);
    const std::wstring rel = utf8_to_wide(tex_rel);
    if ((base.empty() && !pmx_dir.empty()) || rel.empty()) {
        if (err) *err = "texture path is not valid UTF-8: " + tex_rel;
        return false;
    }
    // 候选一：直接相对 pmx_dir；候选二：PMX 打包惯例的 textures 子目录。
    if (!try_resolve(base, rel, &resolved) &&
        !try_resolve(path_io::join(base, L"textures"), rel, &resolved)) {
        if (err) *err = "texture not found: " + tex_rel;
        return false;
    }

    std::vector<uint8_t> raw;
    if (!read_file_bytes(resolved, &raw) || raw.empty()) {
        if (err) *err = "cannot read texture file: " + tex_rel;
        return false;
    }
    if (raw.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        if (err) *err = "texture too large: " + tex_rel;
        return false;
    }

    if (is_png(raw.data(), raw.size())) {
        out->bytes = std::move(raw);
        out->mime_type = "image/png";
        sniff_has_alpha(out->bytes.data(), out->bytes.size(), &out->has_alpha);
        return true;
    }
    if (is_jpeg(raw.data(), raw.size())) {
        out->bytes = std::move(raw);
        out->mime_type = "image/jpeg";
        out->has_alpha = false;  // JPEG 没有 alpha 通道
        return true;
    }

    // 其余格式（BMP/TGA/…）交给 stb 通用解码，统一转 PNG 再内嵌：
    // GLB 只保证 image/png 与 image/jpeg 两种 mimeType 的通用支持。
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()),
                                            &w, &h, &comp, 4);
    if (!pixels) {
        if (err) {
            const char* reason = stbi_failure_reason();
            *err = std::string("texture decode failed: ") + tex_rel + " (" +
                   (reason ? reason : "unknown") + ")";
        }
        return false;
    }
    // alpha 判定取解码时的源通道数（BMP/TGA 32bpp → 4），不能看重编码
    // 结果——stb 重编码一律是 RGBA PNG，会把 24bpp 源误判成带 alpha。
    out->has_alpha = (comp == 4);
    const bool encoded =
        stbi_write_png_to_func(&stbi_write_cb, &out->bytes, w, h, 4, pixels, w * 4) != 0;
    stbi_image_free(pixels);
    if (!encoded || out->bytes.empty()) {
        out->bytes.clear();
        if (err) *err = "texture PNG encode failed: " + tex_rel;
        return false;
    }
    out->mime_type = "image/png";
    return true;
}

}  // namespace go::pmx2glb
