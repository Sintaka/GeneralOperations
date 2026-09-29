// go_pmx2glb deterministic parser and GLB tests (no external fixture/framework).
#include <go/pmx2glb/convert.hpp>
#include <go/pmx2glb/pmx.hpp>

#include "glb.hpp"
#include "path_io.hpp"
#include "tiny_gltf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
const char* current_case = "setup";
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s:%d case=%s check=%s\n", __FILE__, __LINE__, current_case, #x); } } while (0)

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }
    void u16(uint16_t v) { u8(v & 255); u8(v >> 8); }
    void i32(int32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint32_t>(v) >> (i * 8)); }
    void f32(float v) { uint32_t n; std::memcpy(&n, &v, 4); i32(static_cast<int32_t>(n)); }
    void raw(const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); bytes.insert(bytes.end(), b, b + n); }
    void zeros(size_t n) { bytes.insert(bytes.end(), n, 0); }
    void index(int32_t v, uint8_t size, bool sign = true) {
        if (size == 1) sign ? i8(static_cast<int8_t>(v)) : u8(static_cast<uint8_t>(v));
        else if (size == 2) u16(static_cast<uint16_t>(v));
        else i32(v);
    }
};

void text(Writer& w, uint8_t encoding, const char* value) {
    const size_t n = std::strlen(value);
    w.i32(static_cast<int32_t>(n * (encoding == 0 ? 2 : 1)));
    for (size_t i = 0; i < n; ++i) { w.u8(static_cast<uint8_t>(value[i])); if (encoding == 0) w.u8(0); }
}

void model_name(Writer& w, uint8_t encoding) {
    static const char utf8[] = "\xE3\x83\xA2\xE3\x83\x87\xE3\x83\xAB";
    if (encoding == 1) {
        w.i32(static_cast<int32_t>(sizeof(utf8) - 1)); w.raw(utf8, sizeof(utf8) - 1);
    } else {
        const uint16_t utf16[] = {0x30E2, 0x30C7, 0x30EB};
        w.i32(static_cast<int32_t>(sizeof(utf16)));
        for (uint16_t ch : utf16) w.u16(ch);
    }
}

void vertex(Writer& w, float x, float y, float z, uint8_t type) {
    w.f32(x); w.f32(y); w.f32(z);
    w.f32(0); w.f32(0); w.f32(1);
    w.f32(x); w.f32(y);
    w.u8(type);
    if (type == 0) {
        w.index(0, 1);
    } else if (type == 1 || type == 3) {
        w.index(0, 1); w.index(1, 1); w.f32(0.25f);
        if (type == 3) w.zeros(36);
    } else {
        for (int i = 0; i < 4; ++i) w.index(i, 1);
        w.f32(0.1f); w.f32(0.2f); w.f32(0.3f); w.f32(0.4f);
    }
    w.f32(1.0f);
}

void material(Writer& w, uint8_t encoding, const char* name,
              int8_t texture_index, uint8_t flags, int face_count) {
    text(w, encoding, name); text(w, encoding, "material-en");
    w.f32(0.5f); w.f32(0.6f); w.f32(0.7f); w.f32(1.0f);
    w.zeros(12 + 4 + 12); w.u8(flags); w.zeros(16); w.f32(1.0f);
    w.index(texture_index, 1); w.index(-1, 1); w.u8(0);
    w.i8(1); w.u8(0); text(w, encoding, "memo"); w.i32(face_count);
}

void bone_header(Writer& w, uint8_t encoding, const char* name,
                 float x, float y, float z, int8_t parent, uint16_t flags) {
    text(w, encoding, name); text(w, encoding, "bone-en");
    w.f32(x); w.f32(y); w.f32(z); w.index(parent, 1); w.i32(0); w.u16(flags);
}

std::vector<uint8_t> fixture(uint8_t encoding, uint8_t vertex_index_size = 2) {
    Writer w;
    w.raw("PMX ", 4); w.f32(2.1f); w.u8(8);
    w.u8(encoding); w.u8(0); w.u8(vertex_index_size); w.u8(1); w.u8(1); w.u8(1); w.u8(1); w.u8(1);
    model_name(w, encoding); text(w, encoding, "model-en");
    text(w, encoding, "comment"); text(w, encoding, "comment-en");

    w.i32(5);
    vertex(w, 1, 2, 3, 0); vertex(w, -1, 0, 2, 1); vertex(w, 0, 1, 0, 2);
    vertex(w, 2, 0, 1, 3); vertex(w, 0, -1, -2, 4);
    w.i32(6); for (uint16_t i : {0, 1, 2, 2, 3, 4}) w.index(i, vertex_index_size, false);
    w.i32(1); text(w, encoding, "TeX.PNG");
    w.i32(2); material(w, encoding, "mat-a", 0, 1, 3); material(w, encoding, "mat-b", -1, 0, 3);

    w.i32(4);
    bone_header(w, encoding, "root", 0, 0, 0, -1, 0); w.zeros(12);
    bone_header(w, encoding, "child", 0, 1, 0, 0, 0x0001); w.index(0, 1);
    const uint16_t optional = 0x0001 | 0x0100 | 0x0400 | 0x0800 | 0x2000;
    bone_header(w, encoding, "options", 1, 1, 0, 1, optional); w.index(0, 1);
    w.index(1, 1); w.f32(0.5f); w.zeros(12 + 24); w.i32(7);
    bone_header(w, encoding, "ik", 1, 2, 0, 2, 0x0020); w.zeros(12);
    w.index(0, 1); w.i32(4); w.f32(1.0f); w.i32(1); w.index(1, 1); w.u8(1); w.zeros(24);

    w.i32(3);
    text(w, encoding, "wink"); text(w, encoding, "wink-en"); w.u8(3); w.u8(1); w.i32(2);
    w.index(3, vertex_index_size, false); w.f32(1); w.f32(2); w.f32(3);
    w.index(1, vertex_index_size, false); w.f32(0.5f); w.f32(0); w.f32(-1);
    text(w, encoding, "group"); text(w, encoding, "group-en"); w.u8(0); w.u8(0); w.i32(1);
    w.index(0, 1); w.f32(0.5f);
    text(w, encoding, "uv"); text(w, encoding, "uv-en"); w.u8(0); w.u8(3); w.i32(1);
    w.index(0, vertex_index_size, false); w.zeros(16);
    w.zeros(16);  // Legal unparsed PMX tail.
    return w.bytes;
}

bool close_enough(double a, double b) { return std::fabs(a - b) < 0.00001; }

template <typename T>
T item(const tinygltf::Model& model, int accessor_index, size_t index) {
    const tinygltf::Accessor& accessor = model.accessors.at(static_cast<size_t>(accessor_index));
    const tinygltf::BufferView& view = model.bufferViews.at(static_cast<size_t>(accessor.bufferView));
    const size_t offset = view.byteOffset + accessor.byteOffset + index * sizeof(T);
    T value{};
    std::memcpy(&value, model.buffers.at(static_cast<size_t>(view.buffer)).data.data() + offset,
                sizeof(T));
    return value;
}

void test_parse(uint8_t encoding) {
    current_case = encoding == 0 ? "parse_utf16" : "parse_utf8";
    const std::vector<uint8_t> bytes = fixture(encoding);
    go::pmx2glb::PmxModel model;
    std::string error;
    CHECK(go::pmx2glb::parse_pmx(bytes.data(), bytes.size(), &model, &error));
    CHECK(model.version == 2.1f);
    CHECK(model.modelName == "\xE3\x83\xA2\xE3\x83\x87\xE3\x83\xAB");
    CHECK(model.vertices.size() == 5); CHECK(model.indices.size() == 6);
    CHECK(model.materials.size() == 2); CHECK(model.bones.size() == 4);
    CHECK(model.vertexMorphs.size() == 1); CHECK(model.skippedMorphCount == 2);
    CHECK(model.vertices[3].weightType == go::pmx2glb::PmxWeightType::SDEF);
    CHECK(model.vertices[4].weightType == go::pmx2glb::PmxWeightType::QDEF);
    CHECK(model.materials[0].doubleSided); CHECK(model.materials[0].textureIndex == 0);
    CHECK(model.bones[3].parent == 2); CHECK(model.vertexMorphs[0].vertexIndices[0] == 3);
}

void test_truncation() {
    current_case = "all_truncated_prefixes";
    const std::vector<uint8_t> bytes = fixture(1);
    for (size_t n = 0; n < bytes.size() - 16; ++n) {
        go::pmx2glb::PmxModel model; std::string error;
        CHECK(!go::pmx2glb::parse_pmx(bytes.data(), n, &model, &error));
    }
    std::vector<uint8_t> bad = bytes; bad[4] = 0; bad[5] = 0; bad[6] = 0; bad[7] = 0;
    go::pmx2glb::PmxModel model; std::string error;
    CHECK(!go::pmx2glb::parse_pmx(bad.data(), bad.size(), &model, &error));
    CHECK(!go::pmx2glb::parse_pmx(nullptr, 0, &model, &error));
    bad = fixture(0); bad[18] = 7; bad[19] = 0; bad[20] = 0; bad[21] = 0;
    CHECK(!go::pmx2glb::parse_pmx(bad.data(), bad.size(), &model, &error));
}

void test_glb() {
    current_case = "glb_readback";
    const std::vector<uint8_t> source = fixture(1);
    go::pmx2glb::PmxModel pmx; std::string error;
    CHECK(go::pmx2glb::parse_pmx(source.data(), source.size(), &pmx, &error));
    std::vector<std::string> progress;
    std::vector<uint8_t> glb;
    CHECK(go::pmx2glb::build_glb_bytes(pmx, "Z:/missing", 0.08f,
        [&](const std::string& line) { progress.push_back(line); }, &glb, &error));
    CHECK(glb.size() > 20); CHECK(std::memcmp(glb.data(), "glTF", 4) == 0);
    CHECK(!progress.empty());

    tinygltf::TinyGLTF loader; tinygltf::Model model; std::string warning;
    loader.SetImagesAsIs(true);
    CHECK(loader.LoadBinaryFromMemory(&model, &error, &warning, glb.data(),
                                     static_cast<unsigned int>(glb.size())));
    CHECK(model.meshes.size() == 1); CHECK(model.meshes[0].primitives.size() == 2);
    CHECK(model.materials.size() == 2); CHECK(model.nodes.size() == 5);
    CHECK(model.skins.size() == 1); CHECK(model.skins[0].joints.size() == 4);
    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    const int positions = primitive.attributes.at("POSITION");
    CHECK(close_enough(item<float>(model, positions, 0), 0.08));
    CHECK(close_enough(item<float>(model, positions, 1), 0.16));
    CHECK(close_enough(item<float>(model, positions, 2), -0.24));
    CHECK(item<uint32_t>(model, primitive.indices, 0) == 0);
    CHECK(item<uint32_t>(model, primitive.indices, 1) == 2);
    CHECK(item<uint32_t>(model, primitive.indices, 2) == 1);
    const int weights = primitive.attributes.at("WEIGHTS_0");
    CHECK(close_enough(item<float>(model, weights, 4), 0.25));
    CHECK(close_enough(item<float>(model, weights, 5), 0.75));
    CHECK(primitive.targets.size() == 1);
    const tinygltf::Accessor& morph = model.accessors[primitive.targets[0].at("POSITION")];
    CHECK(morph.sparse.isSparse); CHECK(morph.sparse.count == 2);
    CHECK(model.meshes[0].extras.IsObject());
    CHECK(model.meshes[0].extras.Get("targetNames").IsArray());
    CHECK(model.meshes[0].extras.Get("targetNames").ArrayLen() == 1);
    CHECK(model.nodes[1].translation.size() == 3);
    CHECK(close_enough(model.nodes[1].translation[1], 0.08));
    CHECK(model.materials[0].doubleSided); CHECK(model.materials[0].alphaMode != "BLEND");
}

void test_embedded_texture() {
    current_case = "embedded_texture";
    static const uint8_t png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
        0,0,0,1,0,0,0,1,8,4,0,0,0,0xB5,0x1C,0x0C,2,0,0,0,0x0B,0x49,0x44,0x41,
        0x54,0x78,0xDA,0x63,0x64,0xF8,0x0F,0,1,5,1,1,0x27,0x18,0xE3,0x66,0,0,0,0,
        0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
    const std::wstring dir = L"pmx2glb-texture-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = go::pmx2glb::path_io::join(dir, L"tex.png");
    CHECK(go::pmx2glb::path_io::write_bytes(file, std::vector<uint8_t>(png, png + sizeof(png))));
    const std::vector<uint8_t> source = fixture(1);
    go::pmx2glb::PmxModel pmx; std::string error;
    CHECK(go::pmx2glb::parse_pmx(source.data(), source.size(), &pmx, &error));
    std::vector<uint8_t> glb;
    CHECK(go::pmx2glb::build_glb_bytes(pmx, "pmx2glb-texture-test", 1.0f, {}, &glb, &error));
    tinygltf::TinyGLTF loader; tinygltf::Model model; std::string warning;
    loader.SetImagesAsIs(true);
    CHECK(loader.LoadBinaryFromMemory(&model, &error, &warning, glb.data(),
                                      static_cast<unsigned int>(glb.size())));
    CHECK(model.images.size() == 1); CHECK(model.textures.size() == 1);
    CHECK(model.images[0].mimeType == "image/png"); CHECK(model.images[0].bufferView >= 0);
    CHECK(model.materials[0].pbrMetallicRoughness.baseColorTexture.index == 0);
    CHECK(model.materials[0].alphaMode == "BLEND");
    DeleteFileW(file.c_str()); RemoveDirectoryW(dir.c_str());
}

DWORD run_cli(const std::wstring& arguments) {
    std::wstring exe = go::pmx2glb::utf8_to_wide(GO_PMX2GLB_EXE_PATH);
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> writable(command.begin(), command.end()); writable.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), writable.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return static_cast<DWORD>(-1);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return code;
}

void test_cli() {
    current_case = "cli_unicode_end_to_end";
    const std::wstring input = L"\x65E5\x672C-cli.PMX";
    const std::wstring output = go::pmx2glb::path_io::replace_extension(input, L".glb");
    CHECK(go::pmx2glb::path_io::write_bytes(input, fixture(1)));
    CHECK(run_cli(L"--scale 0.08 -- \"" + input + L"\"") == 0);
    CHECK(go::pmx2glb::path_io::exists(output, true));
    CHECK(run_cli(L"--scale nope -- \"" + input + L"\"") == 2);
    DeleteFileW(input.c_str()); DeleteFileW(output.c_str());
}

void test_unicode_write() {
    current_case = "unicode_output_path";
    const std::string path = "\xE6\x97\xA5\xE6\x9C\xAC-test.glb";
    std::vector<uint8_t> data = {'g', 'l', 'T', 'F'}; std::string error;
    CHECK(go::pmx2glb::write_glb_file(data, path, &error));
    const std::wstring wide = go::pmx2glb::utf8_to_wide(path);
    CHECK(go::pmx2glb::path_io::exists(wide, true));
    DeleteFileW(wide.c_str());
}

}  // namespace

int main() {
    test_parse(0); test_parse(1);
    for (uint8_t width : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
        current_case = "vertex_index_width";
        const std::vector<uint8_t> bytes = fixture(1, width);
        go::pmx2glb::PmxModel model; std::string error;
        CHECK(go::pmx2glb::parse_pmx(bytes.data(), bytes.size(), &model, &error));
        CHECK(model.indices.size() == 6 && model.indices[5] == 4);
    }
    test_truncation(); test_glb(); test_embedded_texture(); test_cli(); test_unicode_write();
    if (failures == 0) std::puts("PASS go_pmx2glb_tests");
    return failures == 0 ? 0 : 1;
}
