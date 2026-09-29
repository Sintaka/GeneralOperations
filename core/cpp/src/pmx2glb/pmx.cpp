// PMX 2.0/2.1 二进制解析。结构与字段顺序的依据见 pmx.hpp 顶部说明；
// 各段布局按规范逐字段落定，并用 .sandbox/vendor 下的 saba / MikuMikuFormats
// 两份独立实现交叉核对过（仅核对布局，未拷贝代码）。

#include <go/pmx2glb/pmx.hpp>

#include "utf.hpp"

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace go::pmx2glb {

namespace {

// 为什么不用 istream：PMX 全是小端定长字段，直接在内存上 memcpy 最简单，
// 也天然把"解析"做成纯函数（测试可以直接喂合成的字节流，无需落盘）。
// 目标平台 Windows x86/x64 是小端，PMX 也是小端，逐字段 memcpy 即正确。
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : cur_(data), end_(data + size) {}

    bool bad() const { return bad_; }
    size_t remaining() const { return bad_ ? 0 : static_cast<size_t>(end_ - cur_); }

    bool read(void* dst, size_t bytes)
    {
        if (bad_ || bytes > remaining()) {
            bad_ = true;
            return false;
        }
        if (bytes != 0) {
            std::memcpy(dst, cur_, bytes);
        }
        cur_ += bytes;
        return true;
    }

    // 跳过 bytes 字节（跳过同样要做边界检查，坏文件必须立刻失败）。
    bool skip(size_t bytes)
    {
        if (bad_ || bytes > remaining()) {
            bad_ = true;
            return false;
        }
        cur_ += bytes;
        return true;
    }

    template <typename T>
    bool read_pod(T* v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "memcpy-able only");
        return read(v, sizeof(T));
    }

    bool read_u8(uint8_t* v) { return read_pod(v); }
    bool read_i8(int8_t* v) { return read_pod(v); }
    bool read_u16(uint16_t* v) { return read_pod(v); }
    bool read_i32(int32_t* v) { return read_pod(v); }
    bool read_f32(float* v) { return read_pod(v); }

    // 有符号索引（骨骼/材质/纹理/morph/刚体索引）：1/2 字节按补码解释，
    // 0xFF/0xFFFF 自然得到 -1（规范用 -1 表示"无"）。
    bool read_signed_index(int32_t* v, uint8_t size)
    {
        switch (size) {
        case 1: {
            int8_t raw;
            if (!read_pod(&raw)) return false;
            *v = raw;
            return true;
        }
        case 2: {
            int16_t raw;
            if (!read_pod(&raw)) return false;
            *v = raw;
            return true;
        }
        case 4:
            return read_pod(v);
        default:
            return false;  // 入口已校验合法值，这里兜底
        }
    }

    // 无符号索引（顶点索引）：大模型顶点数可超过 32767，按有符号读 2 字节
    // 会溢出成负数（上游 MikuMikuFormats 就栽在这里），规范明确顶点索引无符号。
    bool read_unsigned_index(uint32_t* v, uint8_t size)
    {
        switch (size) {
        case 1: {
            uint8_t raw;
            if (!read_pod(&raw)) return false;
            *v = raw;
            return true;
        }
        case 2: {
            uint16_t raw;
            if (!read_pod(&raw)) return false;
            *v = raw;
            return true;
        }
        case 4:
            return read_pod(v);
        default:
            return false;
        }
    }

private:
    const uint8_t* cur_;
    const uint8_t* end_;
    bool bad_ = false;
};

bool fail(std::string* err, const char* message)
{
    if (err != nullptr) {
        *err = message;
    }
    return false;
}

bool fail_at(std::string* err, const char* what, int32_t index)
{
    if (err != nullptr) {
        *err = std::string(what) + " at index " + std::to_string(index);
    }
    return false;
}

// 读取"数量"字段并做边界校验：数量非负，且 count * min_bytes_per_item 不超过
// 剩余字节数。为什么要求调用方给出每种元素的最小字节数：调用方在逐元素读取
// 之前就要 resize 容器，若只校验 count <= remaining，一个坏文件可以把数量声称
// 到与剩余字节数一样大，在第一个元素读取失败之前先撑出远超输入规模的内存
// （每个元素的内存足迹是其最小磁盘字节数的固定小倍数，见各调用点的实参）。
bool read_count(Reader& r, int32_t* count, uint64_t min_bytes_per_item)
{
    if (min_bytes_per_item == 0) {
        min_bytes_per_item = 1;  // 防御性钳制：除零未定义
    }
    if (!r.read_i32(count)) {
        return false;
    }
    if (*count < 0 || static_cast<uint64_t>(*count) > r.remaining() / min_bytes_per_item) {
        return false;
    }
    return true;
}

bool read_string(Reader& r, uint8_t encode, std::string* out)
{
    int32_t len = 0;
    if (!r.read_i32(&len)) {
        return false;
    }
    if (len < 0 || static_cast<uint64_t>(len) > r.remaining() ||
        (encode == 0 && (len & 1) != 0)) {
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(len));
    if (len > 0 && !r.read(buf.data(), static_cast<size_t>(len))) {
        return false;
    }
    if (encode == 0) {
        *out = utf16le_to_utf8(buf.data(), buf.size());
        return true;
    }
    if (encode == 1) {
        out->assign(buf.data(), buf.size());
        return true;
    }
    return false;  // 规范只定义 0/1（见 pmx.hpp 顶部说明），不猜
}

// 头部 8 字节全局设置（见 pmx.hpp：0=UTF16LE/1=UTF8、附加 UV 数、
// 六类索引各自 1/2/4 字节）。
struct Globals {
    uint8_t encode = 0;
    uint8_t addUVCount = 0;
    uint8_t vertexIndexSize = 4;
    uint8_t textureIndexSize = 1;
    uint8_t materialIndexSize = 1;
    uint8_t boneIndexSize = 2;
    uint8_t morphIndexSize = 2;
    uint8_t rigidbodyIndexSize = 1;
};

bool read_vertex(Reader& r, const Globals& g, PmxVertex* v, std::string* err, int32_t index)
{
    if (!r.read(v->position, 12) || !r.read(v->normal, 12) || !r.read(v->uv, 8)) {
        return fail_at(err, "truncated vertex", index);
    }
    // 附加 UV：每套 4 个 float，本转换器不消费，跳过保位。
    if (!r.skip(static_cast<size_t>(g.addUVCount) * 16)) {
        return fail_at(err, "truncated vertex (addUV)", index);
    }
    uint8_t weightType = 0;
    if (!r.read_u8(&weightType)) {
        return fail_at(err, "truncated vertex", index);
    }
    if (weightType > 4) {
        return fail_at(err, "unsupported vertex weight type", index);
    }
    v->weightType = static_cast<PmxWeightType>(weightType);
    const uint8_t bs = g.boneIndexSize;
    switch (v->weightType) {
    case PmxWeightType::BDEF1:
        if (!r.read_signed_index(&v->boneIndices[0], bs)) {
            return fail_at(err, "truncated vertex (BDEF1)", index);
        }
        v->boneWeights[0] = 1.0f;
        break;
    case PmxWeightType::BDEF2:
    case PmxWeightType::SDEF:  // SDEF 按 BDEF2 语义取 bone1/bone2/weight1（约定），C/R0/R1 丢弃
        if (!r.read_signed_index(&v->boneIndices[0], bs) ||
            !r.read_signed_index(&v->boneIndices[1], bs) ||
            !r.read_f32(&v->boneWeights[0])) {
            return fail_at(err, "truncated vertex (BDEF2/SDEF)", index);
        }
        // weight2 隐含 = 1 - weight1（规范如此），读取端不落盘，转换端现算。
        if (v->weightType == PmxWeightType::SDEF && !r.skip(36)) {  // C/R0/R1 各 3f
            return fail_at(err, "truncated vertex (SDEF C/R)", index);
        }
        break;
    case PmxWeightType::BDEF4:
    case PmxWeightType::QDEF:  // QDEF 与 BDEF4 同构（四骨骼四权重），差异只在语义
        for (int i = 0; i < 4; ++i) {
            if (!r.read_signed_index(&v->boneIndices[i], bs)) {
                return fail_at(err, "truncated vertex (BDEF4/QDEF bones)", index);
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (!r.read_f32(&v->boneWeights[i])) {
                return fail_at(err, "truncated vertex (BDEF4/QDEF weights)", index);
            }
        }
        break;
    }
    if (!r.skip(4)) {  // 边缘放大率，不消费
        return fail_at(err, "truncated vertex (edge)", index);
    }
    return true;
}

bool read_material(Reader& r, const Globals& g, PmxMaterial* m, std::string* err, int32_t index)
{
    std::string discard;
    if (!read_string(r, g.encode, &m->name) || !read_string(r, g.encode, &discard)) {
        return fail_at(err, "truncated material (name)", index);
    }
    if (!r.read(m->diffuse, 16) || !r.skip(12) ||   // specular 3f
        !r.skip(4) ||                                // specularPower 1f
        !r.skip(12)) {                               // ambient 3f
        return fail_at(err, "truncated material (colors)", index);
    }
    uint8_t flag = 0;
    if (!r.read_u8(&flag) || !r.skip(16) || !r.skip(4)) {  // flag / edgeColor 4f / edgeSize
        return fail_at(err, "truncated material (flags)", index);
    }
    m->doubleSided = (flag & 0x01) != 0;  // bit0 = 両面表示（规范唯一在意的位）
    int32_t sphereIndexDiscard = -1;
    if (!r.read_signed_index(&m->textureIndex, g.textureIndexSize) ||
        !r.read_signed_index(&sphereIndexDiscard, g.textureIndexSize) ||  // sphere 引用，丢弃
        !r.skip(1)) {  // sphereMode i8，不消费
        return fail_at(err, "truncated material (texture refs)", index);
    }
    int8_t toonFlag = 0;
    if (!r.read_i8(&toonFlag)) {
        return fail_at(err, "truncated material (toon flag)", index);
    }
    if (toonFlag == 1) {
        // 共享 toon：1 字节编号（toon01..toon10），不消费不落盘。
        if (!r.skip(1)) {
            return fail_at(err, "truncated material (shared toon)", index);
        }
    } else {
        // 非共享（0，以及规范外的杂值也按此处理）：一个纹理索引尺寸的
        // toon 贴图引用，同样丢弃。取 0/1/杂值时游标都必须停在 memo 之前。
        int32_t toonRefDiscard = -1;
        if (!r.read_signed_index(&toonRefDiscard, g.textureIndexSize)) {
            return fail_at(err, "truncated material (toon ref)", index);
        }
    }
    if (!read_string(r, g.encode, &discard)) {  // memo 注释，不消费
        return fail_at(err, "truncated material (memo)", index);
    }
    if (!r.read_i32(&m->faceVertexCount) || m->faceVertexCount < 0) {
        return fail_at(err, "bad material face count", index);
    }
    return true;
}

// 骨骼 flag 位：只关心决定后续字段布局的位。
// 注意字段处理顺序不是按位值升序，而是规范的固定顺序：
// 接続先(0x0001) → 付与(0x0100|0x0200) → 軸固定(0x0400) → 局部軸(0x0800)
// → 外部親(0x2000) → IK(0x0020)；IK 的位值最小却在最后，按升序写会读错位。
constexpr uint16_t kBoneConnectionIsIndex = 0x0001;  // 1=接続先为骨骼索引 / 0=坐标偏移
constexpr uint16_t kBoneAppendRotate = 0x0100;
constexpr uint16_t kBoneAppendTranslate = 0x0200;
constexpr uint16_t kBoneFixedAxis = 0x0400;
constexpr uint16_t kBoneLocalAxes = 0x0800;
constexpr uint16_t kBoneExternalParent = 0x2000;
constexpr uint16_t kBoneIK = 0x0020;

bool read_bone(Reader& r, const Globals& g, PmxBone* b, std::string* err, int32_t index)
{
    std::string discard;
    if (!read_string(r, g.encode, &b->name) || !read_string(r, g.encode, &discard)) {
        return fail_at(err, "truncated bone (name)", index);
    }
    // PMX 存的骨骼位置是 rest 姿态的绝对坐标（非父相对），直接落盘。
    if (!r.read(b->position, 12)) {
        return fail_at(err, "truncated bone (position)", index);
    }
    if (!r.read_signed_index(&b->parent, g.boneIndexSize)) {
        return fail_at(err, "truncated bone (parent)", index);
    }
    if (!r.skip(4)) {  // 表示階層 level，不消费
        return fail_at(err, "truncated bone (level)", index);
    }
    uint16_t flag = 0;
    if (!r.read_u16(&flag)) {
        return fail_at(err, "truncated bone (flag)", index);
    }
    if ((flag & kBoneConnectionIsIndex) != 0) {
        if (!r.skip(g.boneIndexSize)) {  // 接続先骨骼索引，不消费
            return fail_at(err, "truncated bone (connection)", index);
        }
    } else if (!r.skip(12)) {  // 接続先坐标偏移 3f
        return fail_at(err, "truncated bone (offset)", index);
    }
    if ((flag & (kBoneAppendRotate | kBoneAppendTranslate)) != 0) {
        if (!r.skip(static_cast<size_t>(g.boneIndexSize) + 4)) {  // 付与親索引 + 付与率 f
            return fail_at(err, "truncated bone (append)", index);
        }
    }
    if ((flag & kBoneFixedAxis) != 0 && !r.skip(12)) {  // 軸固定方向 3f
        return fail_at(err, "truncated bone (fixed axis)", index);
    }
    if ((flag & kBoneLocalAxes) != 0 && !r.skip(24)) {  // 局部X/Z軸方向 3f + 3f
        return fail_at(err, "truncated bone (local axes)", index);
    }
    if ((flag & kBoneExternalParent) != 0 && !r.skip(4)) {  // 外部親変形キー i32
        return fail_at(err, "truncated bone (external parent)", index);
    }
    if ((flag & kBoneIK) != 0) {
        // IK 头：ターゲット索引 + ループ回数 i32 + 制限角度 f。
        if (!r.skip(static_cast<size_t>(g.boneIndexSize) + 4 + 4)) {
            return fail_at(err, "truncated bone (ik head)", index);
        }
        int32_t linkCount = 0;
        if (!read_count(r, &linkCount, static_cast<uint64_t>(g.boneIndexSize) + 1)) {
            return fail_at(err, "truncated bone (ik links)", index);
        }
        for (int32_t i = 0; i < linkCount; ++i) {
            // リンク：骨骼索引 + 角度制限 u8；制限非 0 时再跟上限/下限各 3f。
            // 制限取 0/1 之外的杂值也按非 0 处理（与 saba 同款宽松读法），
            // 保证任意取值下游标都停在下一个リンク之前。
            if (!r.skip(static_cast<size_t>(g.boneIndexSize))) {
                return fail_at(err, "truncated bone (ik link)", index);
            }
            uint8_t limit = 0;
            if (!r.read_u8(&limit)) {
                return fail_at(err, "truncated bone (ik limit flag)", index);
            }
            if (limit != 0 && !r.skip(24)) {
                return fail_at(err, "truncated bone (ik limit angles)", index);
            }
        }
    }
    return true;
}

// morph 类型常量（规范 0..10；Flip/Impulse 是 2.1 增补，跳过逻辑同样覆盖，
// 否则 2.1 文件的后续 morph 会错位）。只有 Vertex 被本转换器消费。
constexpr uint8_t kMorphGroup = 0;
constexpr uint8_t kMorphVertex = 1;
constexpr uint8_t kMorphBone = 2;
constexpr uint8_t kMorphUV = 3;
constexpr uint8_t kMorphAddUV1 = 4;
constexpr uint8_t kMorphAddUV2 = 5;
constexpr uint8_t kMorphAddUV3 = 6;
constexpr uint8_t kMorphAddUV4 = 7;
constexpr uint8_t kMorphMaterial = 8;
constexpr uint8_t kMorphFlip = 9;
constexpr uint8_t kMorphImpulse = 10;

bool read_morph(Reader& r, const Globals& g, PmxModel* out, std::string* err, int32_t index)
{
    std::string name;
    std::string discard;
    if (!read_string(r, g.encode, &name) || !read_string(r, g.encode, &discard)) {
        return fail_at(err, "truncated morph (name)", index);
    }
    uint8_t category = 0;
    uint8_t type = 0;
    if (!r.read_u8(&category) || !r.read_u8(&type)) {  // 分类（眉/目/口等）仅 UI 用，不消费
        return fail_at(err, "truncated morph (type)", index);
    }
    // 各类型单个 offset 的最小字节数（索引按最小 1 字节算），供 read_count
    // 做 count 粗校验；同时这张表就是类型合法性校验（default 分支报错不猜）。
    uint64_t offsetMin = 1;
    switch (type) {
    case kMorphGroup:    offsetMin = static_cast<uint64_t>(g.morphIndexSize) + 4; break;
    case kMorphVertex:   offsetMin = static_cast<uint64_t>(g.vertexIndexSize) + 12; break;
    case kMorphBone:     offsetMin = static_cast<uint64_t>(g.boneIndexSize) + 28; break;
    case kMorphUV:
    case kMorphAddUV1:
    case kMorphAddUV2:
    case kMorphAddUV3:
    case kMorphAddUV4:   offsetMin = static_cast<uint64_t>(g.vertexIndexSize) + 16; break;
    case kMorphMaterial: offsetMin = static_cast<uint64_t>(g.materialIndexSize) + 113; break;
    case kMorphFlip:     offsetMin = static_cast<uint64_t>(g.morphIndexSize) + 4; break;
    case kMorphImpulse:  offsetMin = static_cast<uint64_t>(g.rigidbodyIndexSize) + 25; break;
    default:
        return fail_at(err, "unsupported morph type", index);
    }
    int32_t offsetCount = 0;
    if (!read_count(r, &offsetCount, offsetMin)) {
        return fail_at(err, "truncated morph (offset count)", index);
    }

    if (type == kMorphVertex) {
        PmxVertexMorph& vm = out->vertexMorphs.emplace_back();
        vm.name = std::move(name);
        vm.vertexIndices.resize(static_cast<size_t>(offsetCount));
        vm.offsets.resize(static_cast<size_t>(offsetCount) * 3);
        for (int32_t i = 0; i < offsetCount; ++i) {
            // 顶点 morph offset：顶点索引（无符号）+ 位置偏移 3f。
            uint32_t vi = 0;
            float off[3] = {0, 0, 0};
            if (!r.read_unsigned_index(&vi, g.vertexIndexSize) || vi > INT32_MAX ||
                !r.read(off, 12)) {
                return fail_at(err, "truncated morph (vertex offset)", index);
            }
            const size_t u = static_cast<size_t>(i);
            vm.vertexIndices[u] = static_cast<int32_t>(vi);
            vm.offsets[u * 3 + 0] = off[0];
            vm.offsets[u * 3 + 1] = off[1];
            vm.offsets[u * 3 + 2] = off[2];
        }
        return true;
    }

    // 非 vertex 类型逐 offset 跳过对应字段——转换器不消费这些内容，
    // 但游标必须按类型走准，否则同一文件里后续 morph 会整体错位。
    for (int32_t i = 0; i < offsetCount; ++i) {
        bool ok = false;
        switch (type) {
        case kMorphGroup:  // morph 索引 + 影响率 f
        case kMorphFlip:   // morph 索引 + 翻转值 f
            ok = r.skip(static_cast<size_t>(g.morphIndexSize) + 4);
            break;
        case kMorphBone:  // 骨骼索引 + 平移 3f + 旋转 4f
            ok = r.skip(static_cast<size_t>(g.boneIndexSize) + 28);
            break;
        case kMorphUV:  // 顶点索引 + UV 偏移 4f（附加 UV morph 同构）
        case kMorphAddUV1:
        case kMorphAddUV2:
        case kMorphAddUV3:
        case kMorphAddUV4:
            ok = r.skip(static_cast<size_t>(g.vertexIndexSize) + 16);
            break;
        case kMorphMaterial:  // 材质索引 + 类型 u8 + 9 组颜色共 28 个 f
            ok = r.skip(static_cast<size_t>(g.materialIndexSize) + 1 + 28 * 4);
            break;
        case kMorphImpulse:  // 刚体索引 + 局部标记 u8 + 速度 3f + 角速度 3f
            ok = r.skip(static_cast<size_t>(g.rigidbodyIndexSize) + 1 + 24);
            break;
        default:
            return fail_at(err, "unsupported morph type", index);  // 不可达（上方已校验）
        }
        if (!ok) {
            return fail_at(err, "truncated morph (offset)", index);
        }
    }
    ++out->skippedMorphCount;
    return true;
}

}  // namespace

bool parse_pmx(const uint8_t* data, size_t size, PmxModel* out, std::string* err)
{
    if (out == nullptr) {
        return fail(err, "out is null");
    }
    if (data == nullptr) {
        return fail(err, size == 0 ? "not a PMX file (empty input)" : "data is null");
    }
    *out = PmxModel{};  // 从干净状态开始，同一 out 反复解析不残留上次内容
    Reader r(data, size);

    char magic[4] = {};
    if (!r.read(magic, 4) || std::memcmp(magic, "PMX ", 4) != 0) {
        return fail(err, "not a PMX file (bad magic)");
    }
    float version = 0.0f;
    if (!r.read_f32(&version)) {
        return fail(err, "truncated header (version)");
    }
    if (version != 2.0f && version != 2.1f) {
        const std::string msg = "unsupported PMX version: " + std::to_string(version);
        return fail(err, msg.c_str());
    }
    uint8_t globalsCount = 0;
    if (!r.read_u8(&globalsCount) || globalsCount != 8) {
        return fail(err, "unsupported PMX globals count (expect 8)");
    }
    Globals g;
    if (!r.read_u8(&g.encode) || !r.read_u8(&g.addUVCount) ||
        !r.read_u8(&g.vertexIndexSize) || !r.read_u8(&g.textureIndexSize) ||
        !r.read_u8(&g.materialIndexSize) || !r.read_u8(&g.boneIndexSize) ||
        !r.read_u8(&g.morphIndexSize) || !r.read_u8(&g.rigidbodyIndexSize)) {
        return fail(err, "truncated header (globals)");
    }
    if (g.encode > 1) {
        return fail(err, "unsupported string encoding (only 0=UTF-16LE / 1=UTF-8)");
    }
    if (g.addUVCount > 4) {
        return fail(err, "addUV count out of range (0-4)");
    }
    const auto is_index_size = [](uint8_t s) { return s == 1 || s == 2 || s == 4; };
    if (!is_index_size(g.vertexIndexSize) || !is_index_size(g.textureIndexSize) ||
        !is_index_size(g.materialIndexSize) || !is_index_size(g.boneIndexSize) ||
        !is_index_size(g.morphIndexSize) || !is_index_size(g.rigidbodyIndexSize)) {
        return fail(err, "unsupported index size (only 1/2/4 bytes)");
    }
    out->version = version;

    // 模型信息 4 串：只留模型名，英文名/注释/英文注释丢弃。
    std::string discard;
    if (!read_string(r, g.encode, &out->modelName) || !read_string(r, g.encode, &discard) ||
        !read_string(r, g.encode, &discard) || !read_string(r, g.encode, &discard)) {
        return fail(err, "truncated model info");
    }

    // 顶点：最小元素 38 字节 = pos 12 + normal 12 + uv 8 + 类型 1 + 边缘 4
    // + 骨骼索引最小 1。
    int32_t vertexCount = 0;
    if (!read_count(r, &vertexCount, 38)) {
        return fail(err, "bad vertex count");
    }
    out->vertices.resize(static_cast<size_t>(vertexCount));
    for (int32_t i = 0; i < vertexCount; ++i) {
        if (!read_vertex(r, g, &out->vertices[static_cast<size_t>(i)], err, i)) {
            return false;  // err 已由 read_vertex 填好
        }
    }

    // 顶点索引：PMX 原始顺序整段落盘，材质切片由 faceVertexCount 约定。
    int32_t indexCount = 0;
    if (!read_count(r, &indexCount, g.vertexIndexSize)) {
        return fail(err, "bad index count");
    }
    out->indices.resize(static_cast<size_t>(indexCount));
    for (int32_t i = 0; i < indexCount; ++i) {
        if (!r.read_unsigned_index(&out->indices[static_cast<size_t>(i)], g.vertexIndexSize)) {
            return fail(err, "truncated index section");
        }
    }

    // 纹理路径：最小元素 4 字节（串长 i32 本身）。
    int32_t textureCount = 0;
    if (!read_count(r, &textureCount, 4)) {
        return fail(err, "bad texture count");
    }
    out->textures.resize(static_cast<size_t>(textureCount));
    for (int32_t i = 0; i < textureCount; ++i) {
        if (!read_string(r, g.encode, &out->textures[static_cast<size_t>(i)])) {
            return fail_at(err, "truncated texture name", i);
        }
    }

    // 材质：最小元素 86 字节（2 串 8 + 颜色 44 + flag 1 + edgeColor 16 +
    // edgeSize 4 + 纹理引用最小 2 + sphereMode 1 + toon 最小 2 + memo 4 + 切片数 4）。
    int32_t materialCount = 0;
    if (!read_count(r, &materialCount, 86)) {
        return fail(err, "bad material count");
    }
    out->materials.resize(static_cast<size_t>(materialCount));
    for (int32_t i = 0; i < materialCount; ++i) {
        if (!read_material(r, g, &out->materials[static_cast<size_t>(i)], err, i)) {
            return false;
        }
    }

    // 骨骼：最小元素 28 字节（2 串 8 + 位置 12 + 親索引 1 + level 4 + flag 2
    // + 接続先最小 1）。
    int32_t boneCount = 0;
    if (!read_count(r, &boneCount, 28)) {
        return fail(err, "bad bone count");
    }
    out->bones.resize(static_cast<size_t>(boneCount));
    for (int32_t i = 0; i < boneCount; ++i) {
        if (!read_bone(r, g, &out->bones[static_cast<size_t>(i)], err, i)) {
            return false;
        }
    }

    // morph：结构最小元素 14 字节（头部 2 串 8 + category 1 + type 1 + count 4）。
    // 为什么不把最小 offset 也算进去：morph 循环不做整段预分配（逐 morph
    // emplace，vertex morph 的 offset 数另有独立的 read_count 校验兜底），
    // 这里的 min 只是粗校验；0 个 offset 的 morph 合法且恰好 14 字节，
    // 把最小 offset 计入会把"morph 段收尾即文件尾"的合法流误拒。
    int32_t morphCount = 0;
    if (!read_count(r, &morphCount, 14)) {
        return fail(err, "bad morph count");
    }
    for (int32_t i = 0; i < morphCount; ++i) {
        if (!read_morph(r, g, out, err, i)) {
            return false;
        }
    }

    // 读到 morph 段结束即停：2.0 剩余的表示枠/剛体/ジョイント与 2.1 的软体
    // 段本转换器都不消费，文件尾有残留字节属正常，不校验。
    return true;
}

}  // namespace go::pmx2glb
