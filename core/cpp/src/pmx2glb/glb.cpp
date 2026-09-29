// go::pmx2glb —— PmxModel → glTF 2.0 二进制组装与 UTF-8 安全落盘。
#include "glb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "path_io.hpp"
#include "texture.hpp"
#include "tiny_gltf.h"

namespace go::pmx2glb {
namespace {

void set_err(std::string* err, const std::string& message) {
    if (err) *err = message;
}

void report(const ProgressFn& progress, const std::string& line) {
    if (progress) progress(line);
}

void transform_position(const float src[3], float scale, float dst[3]) {
    dst[0] = src[0] * scale;
    dst[1] = src[1] * scale;
    dst[2] = -src[2] * scale;
}

struct Builder {
    tinygltf::Model model;
    std::vector<unsigned char> bytes;

    int add_view(const void* source, size_t size, int target = 0) {
        while ((bytes.size() & 3u) != 0u) bytes.push_back(0);
        const size_t offset = bytes.size();
        if (size != 0) {
            const auto* first = static_cast<const unsigned char*>(source);
            bytes.insert(bytes.end(), first, first + size);
        }
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = size;
        view.target = target;
        model.bufferViews.push_back(std::move(view));
        return static_cast<int>(model.bufferViews.size() - 1);
    }

    int add_accessor(int view, size_t offset, size_t count, int component, int type) {
        tinygltf::Accessor accessor;
        accessor.bufferView = view;
        accessor.byteOffset = offset;
        accessor.count = count;
        accessor.componentType = component;
        accessor.type = type;
        model.accessors.push_back(std::move(accessor));
        return static_cast<int>(model.accessors.size() - 1);
    }
};

void resolve_skinning(const PmxVertex& vertex, size_t bone_count,
                      uint16_t joints[4], float weights[4]) {
    for (int i = 0; i < 4; ++i) {
        joints[i] = 0;
        weights[i] = 0.0f;
    }

    const auto joint = [bone_count](int32_t index) -> uint16_t {
        return index >= 0 && static_cast<size_t>(index) < bone_count && index <= 65535
            ? static_cast<uint16_t>(index) : 0;
    };
    switch (vertex.weightType) {
    case PmxWeightType::BDEF1:
        joints[0] = joint(vertex.boneIndices[0]);
        weights[0] = 1.0f;
        break;
    case PmxWeightType::BDEF2:
    case PmxWeightType::SDEF:
        joints[0] = joint(vertex.boneIndices[0]);
        joints[1] = joint(vertex.boneIndices[1]);
        weights[0] = vertex.boneWeights[0];
        weights[1] = 1.0f - weights[0];
        break;
    case PmxWeightType::BDEF4:
    case PmxWeightType::QDEF:
        for (int i = 0; i < 4; ++i) {
            joints[i] = joint(vertex.boneIndices[i]);
            weights[i] = vertex.boneWeights[i];
        }
        break;
    }

    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(weights[i]) || weights[i] < 0.0f) weights[i] = 0.0f;
        sum += weights[i];
    }
    if (sum <= 0.0f) {
        weights[0] = 1.0f;
        weights[1] = weights[2] = weights[3] = 0.0f;
    } else {
        for (int i = 0; i < 4; ++i) weights[i] /= sum;
    }
}

bool validate_model(const PmxModel& pmx, std::string* err) {
    if (pmx.vertices.empty()) {
        set_err(err, "PMX model has no vertices");
        return false;
    }
    if (pmx.indices.empty() || pmx.indices.size() % 3 != 0) {
        set_err(err, "PMX index count is empty or not divisible by 3");
        return false;
    }
    for (uint32_t index : pmx.indices) {
        if (index >= pmx.vertices.size()) {
            set_err(err, "PMX index references a missing vertex");
            return false;
        }
    }
    uint64_t material_indices = 0;
    for (const PmxMaterial& material : pmx.materials) {
        if (material.faceVertexCount < 0 || material.faceVertexCount % 3 != 0) {
            set_err(err, "PMX material face count is invalid");
            return false;
        }
        material_indices += static_cast<uint64_t>(material.faceVertexCount);
    }
    if (!pmx.materials.empty() && material_indices != pmx.indices.size()) {
        set_err(err, "PMX material face counts do not cover the index table exactly");
        return false;
    }
    if (pmx.bones.size() > 65535) {
        set_err(err, "PMX has more than 65535 bones; glTF JOINTS_0 cannot represent it");
        return false;
    }
    std::vector<uint8_t> bone_state(pmx.bones.size(), 0);
    for (size_t i = 0; i < pmx.bones.size(); ++i) {
        const int32_t parent = pmx.bones[i].parent;
        if (parent < -1 || parent >= static_cast<int32_t>(pmx.bones.size()) ||
            parent == static_cast<int32_t>(i)) {
            set_err(err, "PMX bone parent index is invalid");
            return false;
        }
        std::vector<size_t> trail;
        int32_t current = static_cast<int32_t>(i);
        while (current >= 0 && bone_state[static_cast<size_t>(current)] != 2) {
            const size_t node = static_cast<size_t>(current);
            if (bone_state[node] == 1) {
                set_err(err, "PMX bone hierarchy contains a cycle");
                return false;
            }
            bone_state[node] = 1;
            trail.push_back(node);
            current = pmx.bones[node].parent;
        }
        for (size_t node : trail) bone_state[node] = 2;
    }
    for (const PmxVertexMorph& morph : pmx.vertexMorphs) {
        if (morph.offsets.size() != morph.vertexIndices.size() * 3) {
            set_err(err, "PMX vertex morph data is inconsistent");
            return false;
        }
        for (int32_t index : morph.vertexIndices) {
            if (index < 0 || static_cast<size_t>(index) >= pmx.vertices.size()) {
                set_err(err, "PMX vertex morph references a missing vertex");
                return false;
            }
        }
    }
    return true;
}

struct GeometryAccessors {
    int positions = -1;
    int normals = -1;
    int uv = -1;
    int joints = -1;
    int weights = -1;
};

GeometryAccessors build_vertex_accessors(const PmxModel& pmx, float scale, Builder* builder) {
    const size_t count = pmx.vertices.size();
    std::vector<float> positions(count * 3);
    std::vector<float> normals(count * 3);
    std::vector<float> uv(count * 2);
    std::vector<double> minimum(3, std::numeric_limits<double>::max());
    std::vector<double> maximum(3, -std::numeric_limits<double>::max());

    for (size_t i = 0; i < count; ++i) {
        const PmxVertex& vertex = pmx.vertices[i];
        transform_position(vertex.position, scale, &positions[i * 3]);
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], static_cast<double>(positions[i * 3 + axis]));
            maximum[axis] = std::max(maximum[axis], static_cast<double>(positions[i * 3 + axis]));
        }
        float transformed[3];
        transform_position(vertex.normal, 1.0f, transformed);
        const float length = std::sqrt(transformed[0] * transformed[0] +
                                       transformed[1] * transformed[1] +
                                       transformed[2] * transformed[2]);
        if (length > 0.0f && std::isfinite(length)) {
            for (int axis = 0; axis < 3; ++axis)
                normals[i * 3 + axis] = transformed[axis] / length;
        } else {
            normals[i * 3 + 2] = 1.0f;
        }
        uv[i * 2] = vertex.uv[0];
        uv[i * 2 + 1] = vertex.uv[1];
    }

    GeometryAccessors result;
    result.positions = builder->add_accessor(
        builder->add_view(positions.data(), positions.size() * sizeof(float),
                          TINYGLTF_TARGET_ARRAY_BUFFER),
        0, count, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3);
    builder->model.accessors[result.positions].minValues = std::move(minimum);
    builder->model.accessors[result.positions].maxValues = std::move(maximum);
    result.normals = builder->add_accessor(
        builder->add_view(normals.data(), normals.size() * sizeof(float),
                          TINYGLTF_TARGET_ARRAY_BUFFER),
        0, count, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3);
    result.uv = builder->add_accessor(
        builder->add_view(uv.data(), uv.size() * sizeof(float),
                          TINYGLTF_TARGET_ARRAY_BUFFER),
        0, count, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2);

    if (!pmx.bones.empty()) {
        std::vector<uint16_t> joints(count * 4);
        std::vector<float> weights(count * 4);
        for (size_t i = 0; i < count; ++i)
            resolve_skinning(pmx.vertices[i], pmx.bones.size(), &joints[i * 4], &weights[i * 4]);
        result.joints = builder->add_accessor(
            builder->add_view(joints.data(), joints.size() * sizeof(uint16_t),
                              TINYGLTF_TARGET_ARRAY_BUFFER),
            0, count, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4);
        result.weights = builder->add_accessor(
            builder->add_view(weights.data(), weights.size() * sizeof(float),
                              TINYGLTF_TARGET_ARRAY_BUFFER),
            0, count, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4);
    }
    return result;
}

std::vector<int> build_morph_accessors(const PmxModel& pmx, float scale, Builder* builder) {
    std::vector<int> result;
    result.reserve(pmx.vertexMorphs.size());
    for (const PmxVertexMorph& morph : pmx.vertexMorphs) {
        // glTF sparse indices must be strictly increasing. PMX does not promise order,
        // so sort by vertex and merge duplicate offsets before writing.
        struct Entry { uint32_t index; float value[3]; };
        std::vector<Entry> entries;
        entries.reserve(morph.vertexIndices.size());
        for (size_t i = 0; i < morph.vertexIndices.size(); ++i) {
            float transformed[3];
            transform_position(&morph.offsets[i * 3], scale, transformed);
            const bool zero = transformed[0] == 0.0f && transformed[1] == 0.0f &&
                              transformed[2] == 0.0f;
            if (!zero) entries.push_back({static_cast<uint32_t>(morph.vertexIndices[i]),
                                          {transformed[0], transformed[1], transformed[2]}});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.index < b.index;
        });
        std::vector<uint32_t> indices;
        std::vector<float> values;
        for (const Entry& entry : entries) {
            if (!indices.empty() && indices.back() == entry.index) {
                for (int axis = 0; axis < 3; ++axis)
                    values[values.size() - 3 + static_cast<size_t>(axis)] += entry.value[axis];
            } else {
                indices.push_back(entry.index);
                values.insert(values.end(), entry.value, entry.value + 3);
            }
        }

        tinygltf::Accessor accessor;
        accessor.name = morph.name;
        accessor.bufferView = -1;
        accessor.count = pmx.vertices.size();
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.type = TINYGLTF_TYPE_VEC3;
        if (!indices.empty()) {
            accessor.sparse.isSparse = true;
            accessor.sparse.count = static_cast<int>(indices.size());
            accessor.sparse.indices.bufferView = builder->add_view(
                indices.data(), indices.size() * sizeof(uint32_t));
            accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
            accessor.sparse.values.bufferView = builder->add_view(
                values.data(), values.size() * sizeof(float));
        }
        builder->model.accessors.push_back(std::move(accessor));
        result.push_back(static_cast<int>(builder->model.accessors.size() - 1));
    }
    return result;
}

void build_materials(const PmxModel& pmx, const std::string& pmx_dir, Builder* builder,
                     const ProgressFn& progress) {
    for (const PmxMaterial& source : pmx.materials) {
        tinygltf::Material material;
        material.name = source.name;
        material.doubleSided = source.doubleSided;
        material.pbrMetallicRoughness.baseColorFactor = {
            source.diffuse[0], source.diffuse[1], source.diffuse[2], source.diffuse[3]};
        material.pbrMetallicRoughness.metallicFactor = 0.0;
        material.pbrMetallicRoughness.roughnessFactor = 1.0;
        bool texture_alpha = false;
        if (source.textureIndex >= 0 &&
            static_cast<size_t>(source.textureIndex) < pmx.textures.size()) {
            TextureBlob blob;
            std::string warning;
            const std::string& relative = pmx.textures[static_cast<size_t>(source.textureIndex)];
            if (load_glb_texture(pmx_dir, relative, &blob, &warning)) {
                tinygltf::Image image;
                image.name = relative;
                image.mimeType = blob.mime_type;
                image.bufferView = builder->add_view(blob.bytes.data(), blob.bytes.size());
                builder->model.images.push_back(std::move(image));
                tinygltf::Texture texture;
                texture.source = static_cast<int>(builder->model.images.size() - 1);
                builder->model.textures.push_back(std::move(texture));
                material.pbrMetallicRoughness.baseColorTexture.index =
                    static_cast<int>(builder->model.textures.size() - 1);
                texture_alpha = blob.has_alpha;
            } else {
                report(progress, "  warning: " + warning + "; material uses diffuse color");
            }
        }
        if (source.diffuse[3] < 0.999f || texture_alpha) material.alphaMode = "BLEND";
        builder->model.materials.push_back(std::move(material));
    }
}

void assign_common_attributes(tinygltf::Primitive* primitive,
                              const GeometryAccessors& geometry) {
    primitive->attributes["POSITION"] = geometry.positions;
    primitive->attributes["NORMAL"] = geometry.normals;
    primitive->attributes["TEXCOORD_0"] = geometry.uv;
    if (geometry.joints >= 0) {
        primitive->attributes["JOINTS_0"] = geometry.joints;
        primitive->attributes["WEIGHTS_0"] = geometry.weights;
    }
}

bool build_primitives(const PmxModel& pmx, const GeometryAccessors& geometry,
                      const std::vector<int>& morphs, Builder* builder,
                      tinygltf::Mesh* mesh, std::string* err) {
    std::vector<uint32_t> reversed(pmx.indices.size());
    for (size_t i = 0; i < pmx.indices.size(); i += 3) {
        reversed[i] = pmx.indices[i];
        reversed[i + 1] = pmx.indices[i + 2];
        reversed[i + 2] = pmx.indices[i + 1];
    }
    const int index_view = builder->add_view(reversed.data(), reversed.size() * sizeof(uint32_t),
                                             TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    auto append = [&](size_t first, size_t count, int material) {
        if (count == 0) return;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.material = material;
        primitive.indices = builder->add_accessor(index_view, first * sizeof(uint32_t), count,
                                                   TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                   TINYGLTF_TYPE_SCALAR);
        assign_common_attributes(&primitive, geometry);
        for (int accessor : morphs) primitive.targets.push_back({{"POSITION", accessor}});
        mesh->primitives.push_back(std::move(primitive));
    };

    if (pmx.materials.empty()) {
        append(0, reversed.size(), -1);
    } else {
        size_t first = 0;
        for (size_t i = 0; i < pmx.materials.size(); ++i) {
            const size_t count = static_cast<size_t>(pmx.materials[i].faceVertexCount);
            if (first + count > reversed.size()) {
                set_err(err, "PMX material face range exceeds the index table");
                return false;
            }
            append(first, count, static_cast<int>(i));
            first += count;
        }
    }
    return true;
}

int build_skin(const PmxModel& pmx, float scale, Builder* builder,
               std::vector<int>* root_bones) {
    if (pmx.bones.empty()) return -1;

    std::vector<float> inverse_bind(pmx.bones.size() * 16, 0.0f);
    tinygltf::Skin skin;
    skin.name = pmx.modelName + " Armature";
    const int bone_node_base = static_cast<int>(builder->model.nodes.size());
    skin.joints.reserve(pmx.bones.size());

    for (size_t i = 0; i < pmx.bones.size(); ++i) {
        const PmxBone& bone = pmx.bones[i];
        tinygltf::Node node;
        node.name = bone.name;
        float world[3];
        transform_position(bone.position, scale, world);
        float parent_world[3] = {0.0f, 0.0f, 0.0f};
        if (bone.parent >= 0)
            transform_position(pmx.bones[static_cast<size_t>(bone.parent)].position,
                               scale, parent_world);
        node.translation = {world[0] - parent_world[0], world[1] - parent_world[1],
                            world[2] - parent_world[2]};
        builder->model.nodes.push_back(std::move(node));
        skin.joints.push_back(bone_node_base + static_cast<int>(i));

        float* matrix = &inverse_bind[i * 16];
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        matrix[12] = -world[0];
        matrix[13] = -world[1];
        matrix[14] = -world[2];
    }

    for (size_t i = 0; i < pmx.bones.size(); ++i) {
        const int parent = pmx.bones[i].parent;
        if (parent >= 0) {
            builder->model.nodes[static_cast<size_t>(bone_node_base + parent)].children.push_back(
                bone_node_base + static_cast<int>(i));
        } else {
            root_bones->push_back(bone_node_base + static_cast<int>(i));
        }
    }
    skin.skeleton = root_bones->empty() ? bone_node_base : root_bones->front();
    skin.inverseBindMatrices = builder->add_accessor(
        builder->add_view(inverse_bind.data(), inverse_bind.size() * sizeof(float)),
        0, pmx.bones.size(), TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_MAT4);
    builder->model.skins.push_back(std::move(skin));
    return static_cast<int>(builder->model.skins.size() - 1);
}

bool serialize(Builder* builder, std::vector<uint8_t>* output, std::string* err) {
    tinygltf::Buffer buffer;
    buffer.data = std::move(builder->bytes);
    builder->model.buffers.push_back(std::move(buffer));

    tinygltf::TinyGLTF writer;
    std::ostringstream stream(std::ios::binary);
    if (!writer.WriteGltfSceneToStream(&builder->model, stream, false, true)) {
        set_err(err, "tinygltf failed to serialize GLB");
        return false;
    }
    const std::string bytes = stream.str();
    if (bytes.empty()) {
        set_err(err, "tinygltf produced an empty GLB");
        return false;
    }
    output->assign(bytes.begin(), bytes.end());
    return true;
}

}  // namespace

bool build_glb_bytes(const PmxModel& pmx, const std::string& pmx_dir, float scale,
                     const ProgressFn& progress, std::vector<uint8_t>* out_glb,
                     std::string* err) {
    if (!out_glb) {
        set_err(err, "output byte vector is null");
        return false;
    }
    out_glb->clear();
    if (!std::isfinite(scale) || scale <= 0.0f) {
        set_err(err, "scale must be a finite positive number");
        return false;
    }
    if (!validate_model(pmx, err)) return false;

    Builder builder;
    builder.model.asset.version = "2.0";
    builder.model.asset.generator = "GeneralOperations go_pmx2glb";

    report(progress, "[2/6] Build mesh attributes ...");
    const GeometryAccessors geometry = build_vertex_accessors(pmx, scale, &builder);
    const std::vector<int> morphs = build_morph_accessors(pmx, scale, &builder);

    report(progress, "[3/6] Embed textures and materials ...");
    build_materials(pmx, pmx_dir, &builder, progress);

    tinygltf::Mesh mesh;
    mesh.name = pmx.modelName;
    mesh.weights.assign(morphs.size(), 0.0);
    if (!pmx.vertexMorphs.empty()) {
        tinygltf::Value::Array target_names;
        for (const PmxVertexMorph& morph : pmx.vertexMorphs)
            target_names.emplace_back(morph.name);
        mesh.extras = tinygltf::Value(tinygltf::Value::Object{
            {"targetNames", tinygltf::Value(std::move(target_names))}});
    }
    if (!build_primitives(pmx, geometry, morphs, &builder, &mesh, err)) return false;
    builder.model.meshes.push_back(std::move(mesh));

    report(progress, "[4/6] Build skeleton and morph targets ...");
    std::vector<int> root_bones;
    const int skin = build_skin(pmx, scale, &builder, &root_bones);
    tinygltf::Node mesh_node;
    mesh_node.name = pmx.modelName;
    mesh_node.mesh = 0;
    mesh_node.skin = skin;
    const int mesh_node_index = static_cast<int>(builder.model.nodes.size());
    builder.model.nodes.push_back(std::move(mesh_node));

    tinygltf::Scene scene;
    scene.name = pmx.modelName;
    scene.nodes.push_back(mesh_node_index);
    scene.nodes.insert(scene.nodes.end(), root_bones.begin(), root_bones.end());
    builder.model.scenes.push_back(std::move(scene));
    builder.model.defaultScene = 0;

    if (pmx.skippedMorphCount > 0)
        report(progress, "  skipped " + std::to_string(pmx.skippedMorphCount) +
                         " non-vertex morph(s)");
    report(progress, "[5/6] Serialize GLB ...");
    return serialize(&builder, out_glb, err);
}

bool write_glb_file(const std::vector<uint8_t>& bytes, const std::string& path,
                    std::string* err) {
    if (bytes.empty()) {
        set_err(err, "refusing to write an empty GLB");
        return false;
    }
    const std::wstring destination = utf8_to_wide(path);
    if (destination.empty() && !path.empty()) {
        set_err(err, "output path is not valid UTF-8: " + path);
        return false;
    }
    const std::wstring temporary = destination + L".tmp";
    DeleteFileW(temporary.c_str());
    if (!path_io::write_bytes(temporary, bytes)) {
        DeleteFileW(temporary.c_str());
        set_err(err, "cannot write temporary output file: " + path);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        set_err(err, "cannot replace output file: " + path);
        return false;
    }
    return true;
}

bool convert_pmx_to_glb(const PmxModel& pmx, const std::string& pmx_dir,
                        const std::string& glb_path, const ConvertOptions& options,
                        const ProgressFn& progress, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!build_glb_bytes(pmx, pmx_dir, options.scale, progress, &bytes, err)) return false;
    report(progress, "[6/6] Write output ...");
    return write_glb_file(bytes, glb_path, err);
}

}  // namespace go::pmx2glb
