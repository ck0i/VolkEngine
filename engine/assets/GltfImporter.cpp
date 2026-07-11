#include "assets/GltfImporter.hpp"
#include "assets/SceneImporter.hpp"

#include "core/FileSystem.hpp"

#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace ve {
namespace {

constexpr std::uint32_t kInvalidNode = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kMaximumArtifactElements = 64U * 1024U * 1024U;

struct CgltfDeleter { void operator()(cgltf_data* data) const noexcept { cgltf_free(data); } };
using CgltfData = std::unique_ptr<cgltf_data, CgltfDeleter>;

void checkResult(const cgltf_result result, const char* operation) {
    if (result != cgltf_result_success) throw std::runtime_error(std::string(operation) + " failed with cgltf result " + std::to_string(static_cast<int>(result)));
}

const cgltf_accessor* attribute(const cgltf_primitive& primitive, const cgltf_attribute_type type, const int index = 0) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == index) return primitive.attributes[i].data;
    }
    return nullptr;
}

void requireFinite(const float* values, const std::size_t count, const char* semantic) {
    for (std::size_t index = 0; index < count; ++index) if (!std::isfinite(values[index])) throw std::runtime_error(std::string("glTF ") + semantic + " contains NaN or infinity");
}

Vec3 readVec3(const cgltf_accessor* accessor, const cgltf_size index, const char* semantic) {
    std::array<float, 3> values{};
    if (!cgltf_accessor_read_float(accessor, index, values.data(), values.size())) throw std::runtime_error(std::string("Failed to read glTF ") + semantic + " accessor");
    requireFinite(values.data(), values.size(), semantic);
    return {values[0], values[1], values[2]};
}

Vec2 readVec2(const cgltf_accessor* accessor, const cgltf_size index, const char* semantic) {
    std::array<float, 2> values{};
    if (!cgltf_accessor_read_float(accessor, index, values.data(), values.size())) throw std::runtime_error(std::string("Failed to read glTF ") + semantic + " accessor");
    requireFinite(values.data(), values.size(), semantic);
    return {values[0], values[1]};
}

Vec4 readVec4(const cgltf_accessor* accessor, const cgltf_size index, const char* semantic) {
    std::array<float, 4> values{};
    if (!cgltf_accessor_read_float(accessor, index, values.data(), values.size())) throw std::runtime_error(std::string("Failed to read glTF ") + semantic + " accessor");
    requireFinite(values.data(), values.size(), semantic);
    return {values[0], values[1], values[2], values[3]};
}
AnimationTarget animationTarget(const cgltf_animation_path_type path) {
    switch (path) {
    case cgltf_animation_path_type_translation: return AnimationTarget::Translation;
    case cgltf_animation_path_type_rotation: return AnimationTarget::Rotation;
    case cgltf_animation_path_type_scale: return AnimationTarget::Scale;
    case cgltf_animation_path_type_weights: return AnimationTarget::Weights;
    default: throw std::runtime_error("glTF animation channel has an invalid target path");
    }
}

AnimationInterpolation animationInterpolation(const cgltf_interpolation_type interpolation) {
    switch (interpolation) {
    case cgltf_interpolation_type_linear: return AnimationInterpolation::Linear;
    case cgltf_interpolation_type_step: return AnimationInterpolation::Step;
    case cgltf_interpolation_type_cubic_spline: return AnimationInterpolation::CubicSpline;
    default: throw std::runtime_error("glTF animation sampler has an invalid interpolation");
    }
}

void validateAnimationChannel(const ImportedAnimationChannel& channel, const std::size_t nodeCount) {
    if (channel.targetNode >= nodeCount || channel.keyframeCount == 0U ||
        !std::isfinite(channel.startTime) || !std::isfinite(channel.endTime) ||
        channel.startTime < 0.0f || channel.endTime < channel.startTime ||
        channel.target > AnimationTarget::Weights ||
        channel.interpolation > AnimationInterpolation::CubicSpline) {
        throw std::runtime_error("Animation channel metadata is invalid");
    }
}


void generateNormals(MeshData& mesh) {
    std::vector<Vec3> sums(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3U) {
        const std::uint32_t a = mesh.indices[index];
        const std::uint32_t b = mesh.indices[index + 1U];
        const std::uint32_t c = mesh.indices[index + 2U];
        const Vec3 face = cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                mesh.vertices[c].position - mesh.vertices[a].position);
        sums[a] = sums[a] + face; sums[b] = sums[b] + face; sums[c] = sums[c] + face;
    }
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        mesh.vertices[index].normal = normalize(sums[index]);
        if (length(mesh.vertices[index].normal) <= 0.000001f) mesh.vertices[index].normal = {0.0f, 1.0f, 0.0f};
    }
}

Vec3 fallbackTangent(const Vec3 normal) {
    const Vec3 axis = std::fabs(normal.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    return normalize(axis - normal * dot(axis, normal));
}

void generateTangents(MeshData& mesh) {
    std::vector<Vec3> tangents(mesh.vertices.size());
    std::vector<Vec3> bitangents(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3U) {
        const std::array ids{mesh.indices[index], mesh.indices[index + 1U], mesh.indices[index + 2U]};
        const Vertex& a = mesh.vertices[ids[0]]; const Vertex& b = mesh.vertices[ids[1]]; const Vertex& c = mesh.vertices[ids[2]];
        const Vec3 edge1 = b.position - a.position; const Vec3 edge2 = c.position - a.position;
        const Vec2 uv1{b.uv.x - a.uv.x, b.uv.y - a.uv.y}; const Vec2 uv2{c.uv.x - a.uv.x, c.uv.y - a.uv.y};
        const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
        if (std::fabs(determinant) <= 0.0000001f) continue;
        const float inverse = 1.0f / determinant;
        const Vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * inverse;
        const Vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * inverse;
        for (const std::uint32_t id : ids) { tangents[id] = tangents[id] + tangent; bitangents[id] = bitangents[id] + bitangent; }
    }
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        const Vec3 normal = mesh.vertices[index].normal;
        Vec3 tangent = normalize(tangents[index] - normal * dot(normal, tangents[index]));
        if (length(tangent) <= 0.000001f) tangent = fallbackTangent(normal);
        mesh.vertices[index].tangent = {tangent.x, tangent.y, tangent.z,
            dot(cross(normal, tangent), bitangents[index]) < 0.0f ? -1.0f : 1.0f};
    }
}

MeshBounds calculateBounds(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) return {};
    Vec3 minimum = vertices.front().position;
    Vec3 maximum = minimum;
    for (const Vertex& vertex : vertices) {
        minimum.x = std::min(minimum.x, vertex.position.x); minimum.y = std::min(minimum.y, vertex.position.y); minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x); maximum.y = std::max(maximum.y, vertex.position.y); maximum.z = std::max(maximum.z, vertex.position.z);
    }
    const Vec3 center = (minimum + maximum) * 0.5f;
    float radius = 0.0f;
    for (const Vertex& vertex : vertices) radius = std::max(radius, length(vertex.position - center));
    if (!finite(center) || !std::isfinite(radius)) throw std::runtime_error("glTF mesh bounds are non-finite");
    return {center, radius, true};
}

std::filesystem::path texturePath(const cgltf_texture_view& view) {
    if (view.texture == nullptr || view.texture->image == nullptr || view.texture->image->uri == nullptr) return {};
    const std::string_view uri{view.texture->image->uri};
    if (uri.starts_with("data:")) return {};
    const std::filesystem::path path{uri};
    if (path.is_absolute()) throw std::runtime_error("glTF texture URI must be relative");
    for (const std::filesystem::path& component : path) {
        if (component == "..") throw std::runtime_error("glTF texture URI escapes the source directory");
    }
    return path.lexically_normal();
}

void addTexture(ImportedMaterial& material, const cgltf_data& data,
                const cgltf_texture_view& view, const TextureRole role,
                const TextureColorSpace colorSpace, const AssetId sceneId) {
    if (view.texture == nullptr) return;
    const std::size_t textureIndex =
        static_cast<std::size_t>(view.texture - data.textures);
    ImportedTextureReference reference;
    reference.id = AssetId::derive(
        sceneId, "texture/" + std::to_string(textureIndex) + "/role/" +
                     std::to_string(static_cast<int>(role)));
    reference.sourcePath = texturePath(view);
    reference.role = role;
    reference.colorSpace = colorSpace;
    reference.texcoord = static_cast<std::uint32_t>(view.texcoord);
    reference.scale = view.scale;
    if (view.has_transform) {
        reference.offset = {view.transform.offset[0], view.transform.offset[1]};
        reference.textureScale = {view.transform.scale[0], view.transform.scale[1]};
        reference.rotation = view.transform.rotation;
        reference.texcoord = static_cast<std::uint32_t>(view.transform.texcoord);
    }
    material.textures.push_back(std::move(reference));
}

class BinaryWriter {
public:
    template <typename T> void pod(const T& value) { const auto bytes = std::as_bytes(std::span{&value, 1U}); output.insert(output.end(), bytes.begin(), bytes.end()); }
    void string(const std::string_view value) { if (value.size() > (1U << 20U)) throw std::runtime_error("Artifact string exceeds limit"); const auto size = static_cast<std::uint32_t>(value.size()); pod(size); const auto bytes = std::as_bytes(std::span{value.data(), value.size()}); output.insert(output.end(), bytes.begin(), bytes.end()); }
    std::vector<std::byte> output;
};

class BinaryReader {
public:
    explicit BinaryReader(const std::span<const std::byte> input) : input_(input) {}
    template <typename T> T pod() { require(sizeof(T)); T value; std::memcpy(&value, input_.data() + offset_, sizeof(T)); offset_ += sizeof(T); return value; }
    std::string string() { const auto size = pod<std::uint32_t>(); if (size > (1U << 20U)) throw std::runtime_error("Artifact string exceeds limit"); require(size); std::string value(reinterpret_cast<const char*>(input_.data() + offset_), size); offset_ += size; return value; }
    void require(const std::size_t count) const { if (count > input_.size() - offset_) throw std::runtime_error("Artifact is truncated"); }
    [[nodiscard]] bool done() const noexcept { return offset_ == input_.size(); }
private: std::span<const std::byte> input_; std::size_t offset_ = 0;
};

constexpr std::uint32_t kMeshMagic = 0x314d4556U;
constexpr std::uint32_t kMaterialMagic = 0x31544156U;
constexpr std::uint32_t kSceneMagic = 0x31435356U;
void writeId(BinaryWriter& writer, const AssetId id) { writer.pod(id.high); writer.pod(id.low); }
AssetId readId(BinaryReader& reader) { return {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()}; }
void writeBounds(BinaryWriter& writer, const MeshBounds& bounds) {
    writer.pod(bounds.center.x);
    writer.pod(bounds.center.y);
    writer.pod(bounds.center.z);
    writer.pod(bounds.radius);
    writer.pod(static_cast<std::uint8_t>(bounds.valid));
}
MeshBounds readBounds(BinaryReader& reader) {
    MeshBounds bounds;
    bounds.center.x = reader.pod<float>();
    bounds.center.y = reader.pod<float>();
    bounds.center.z = reader.pod<float>();
    bounds.radius = reader.pod<float>();
    bounds.valid = reader.pod<std::uint8_t>() != 0U;
    if (!finite(bounds.center) || !std::isfinite(bounds.radius) || bounds.radius < 0.0f) {
        throw std::runtime_error("Artifact mesh bounds are invalid");
    }
    return bounds;
}

} // namespace

ImportedGltfScene importGltfScene(const std::filesystem::path& path, const AssetId sceneId,
                                  const GltfImportOptions& options) {
    if (!sceneId.valid()) throw std::invalid_argument("glTF scene AssetId is invalid");
    if (path.extension() != ".gltf" && path.extension() != ".glb") throw std::runtime_error("glTF importer accepts only .gltf and .glb files");
    const std::vector<std::byte> source = readBinaryFile(path, options.maximumSourceBytes);
    cgltf_options parseOptions{};
    cgltf_data* rawData = nullptr;
    checkResult(cgltf_parse(&parseOptions, source.data(), source.size(), &rawData), "cgltf_parse");
    CgltfData data{rawData};
    checkResult(cgltf_load_buffers(&parseOptions, data.get(), path.string().c_str()), "cgltf_load_buffers");
    checkResult(cgltf_validate(data.get()), "cgltf_validate");

    constexpr std::array<std::string_view, 1> supportedRequired{"KHR_texture_transform"};
    for (cgltf_size index = 0; index < data->extensions_required_count; ++index) {
        const std::string_view extension{data->extensions_required[index]};
        if (std::ranges::find(supportedRequired, extension) == supportedRequired.end()) throw std::runtime_error("Unsupported required glTF extension: " + std::string(extension));
    }

    ImportedGltfScene result;
    result.sceneId = sceneId;
    result.sourcePath = path.filename();
    for (cgltf_size index = 0; index < data->extensions_used_count; ++index) {
        const std::string_view extension{data->extensions_used[index]};
        if (std::ranges::find(supportedRequired, extension) == supportedRequired.end()) result.optionalFeatureDiagnostics.push_back("Unsupported optional glTF extension: " + std::string(extension));
    }

    result.materials.reserve(data->materials_count);
    for (cgltf_size materialIndex = 0; materialIndex < data->materials_count; ++materialIndex) {
        const cgltf_material& sourceMaterial = data->materials[materialIndex];
        if (!sourceMaterial.has_pbr_metallic_roughness) throw std::runtime_error("glTF material does not use metallic-roughness PBR");
        if (sourceMaterial.alpha_mode == cgltf_alpha_mode_blend) throw std::runtime_error("glTF alpha-blended materials are not supported");
        ImportedMaterial material;
        material.id = AssetId::derive(sceneId, "material/" + std::to_string(materialIndex));
        material.name = sourceMaterial.name != nullptr ? sourceMaterial.name : "Material " + std::to_string(materialIndex);
        const auto& pbr = sourceMaterial.pbr_metallic_roughness;
        material.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
        material.metallicFactor = pbr.metallic_factor; material.roughnessFactor = pbr.roughness_factor;
        material.emissiveFactor = {sourceMaterial.emissive_factor[0], sourceMaterial.emissive_factor[1], sourceMaterial.emissive_factor[2]};
        material.alphaMode = sourceMaterial.alpha_mode == cgltf_alpha_mode_mask ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque;
        material.alphaCutoff = sourceMaterial.alpha_cutoff; material.doubleSided = sourceMaterial.double_sided != 0;
        requireFinite(&material.baseColorFactor.x, 4U, "material base color"); requireFinite(&material.emissiveFactor.x, 3U, "material emissive factor");
        if (!std::isfinite(material.metallicFactor) || !std::isfinite(material.roughnessFactor) || !std::isfinite(material.alphaCutoff)) throw std::runtime_error("glTF material contains a non-finite factor");
        addTexture(material, *data, pbr.base_color_texture, TextureRole::BaseColor,
                   TextureColorSpace::Srgb, sceneId);
        addTexture(material, *data, pbr.metallic_roughness_texture,
                   TextureRole::MetallicRoughness, TextureColorSpace::Linear, sceneId);
        addTexture(material, *data, sourceMaterial.normal_texture, TextureRole::Normal,
                   TextureColorSpace::Linear, sceneId);
        addTexture(material, *data, sourceMaterial.occlusion_texture, TextureRole::Occlusion,
                   TextureColorSpace::Linear, sceneId);
        addTexture(material, *data, sourceMaterial.emissive_texture, TextureRole::Emissive,
                   TextureColorSpace::Srgb, sceneId);
        result.materials.push_back(std::move(material));
    }

    std::vector<std::vector<AssetId>> meshPrimitiveIds(data->meshes_count);
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex) {
        const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
        meshPrimitiveIds[meshIndex].reserve(sourceMesh.primitives_count);
        for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex) {
            const cgltf_primitive& primitive = sourceMesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles) throw std::runtime_error("glTF primitive mode is not TRIANGLES");
            const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
            if (positions == nullptr || positions->type != cgltf_type_vec3 || positions->count == 0U) throw std::runtime_error("glTF primitive POSITION accessor is missing or invalid");
            const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
            const cgltf_accessor* tangents = attribute(primitive, cgltf_attribute_type_tangent);
            const cgltf_accessor* texcoords = attribute(primitive, cgltf_attribute_type_texcoord);
            if (positions->count > options.maximumVertices) throw std::runtime_error("glTF primitive exceeds vertex limit");
            if (normals != nullptr && (normals->type != cgltf_type_vec3 || normals->count != positions->count)) throw std::runtime_error("glTF NORMAL accessor does not match POSITION");
            if (tangents != nullptr && (tangents->type != cgltf_type_vec4 || tangents->count != positions->count)) throw std::runtime_error("glTF TANGENT accessor does not match POSITION");
            if (texcoords != nullptr && (texcoords->type != cgltf_type_vec2 || texcoords->count != positions->count)) throw std::runtime_error("glTF TEXCOORD_0 accessor does not match POSITION");
            ImportedMeshPrimitive imported;
            imported.id = AssetId::derive(sceneId, "mesh/" + std::to_string(meshIndex) + "/primitive/" + std::to_string(primitiveIndex));
            imported.name = sourceMesh.name != nullptr ? sourceMesh.name : "Mesh " + std::to_string(meshIndex);
            imported.mesh.vertices.resize(positions->count);
            for (cgltf_size vertex = 0; vertex < positions->count; ++vertex) {
                Vertex& output = imported.mesh.vertices[vertex];
                output.position = readVec3(positions, vertex, "POSITION");
                if (normals != nullptr) output.normal = readVec3(normals, vertex, "NORMAL");
                if (tangents != nullptr) output.tangent = readVec4(tangents, vertex, "TANGENT");
                if (texcoords != nullptr) output.uv = readVec2(texcoords, vertex, "TEXCOORD_0");
            }
            if (primitive.indices != nullptr) {
                if (primitive.indices->count > options.maximumIndices || primitive.indices->count % 3U != 0U) throw std::runtime_error("glTF index count is invalid or exceeds limit");
                imported.mesh.indices.resize(primitive.indices->count);
                for (cgltf_size index = 0; index < primitive.indices->count; ++index) {
                    const cgltf_size value = cgltf_accessor_read_index(primitive.indices, index);
                    if (value >= positions->count || value > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("glTF index references a vertex outside POSITION");
                    imported.mesh.indices[index] = static_cast<std::uint32_t>(value);
                }
            } else {
                if (positions->count % 3U != 0U) throw std::runtime_error("Non-indexed glTF triangle primitive has incomplete triangles");
                imported.mesh.indices.resize(positions->count);
                for (std::size_t index = 0; index < imported.mesh.indices.size(); ++index) imported.mesh.indices[index] = static_cast<std::uint32_t>(index);
            }
            if (normals == nullptr) { if (!options.generateMissingNormals) throw std::runtime_error("glTF primitive is missing NORMAL"); generateNormals(imported.mesh); }
            if (tangents == nullptr) { if (!options.generateMissingTangents) throw std::runtime_error("glTF primitive is missing TANGENT"); generateTangents(imported.mesh); }
            imported.mesh.bounds = calculateBounds(imported.mesh.vertices);
            if (primitive.material != nullptr) imported.material = AssetId::derive(sceneId, "material/" + std::to_string(primitive.material - data->materials));
            meshPrimitiveIds[meshIndex].push_back(imported.id);
            result.meshes.push_back(std::move(imported));
        }
    }

    result.nodes.resize(data->nodes_count);
    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
        const cgltf_node& node = data->nodes[nodeIndex];
        ImportedSceneNode& output = result.nodes[nodeIndex];
        output.name = node.name != nullptr ? node.name : "Node " + std::to_string(nodeIndex);
        if (node.parent != nullptr) output.parent = static_cast<std::uint32_t>(node.parent - data->nodes);
        cgltf_node_transform_local(&node, output.localTransform.m.data());
        requireFinite(output.localTransform.m.data(), output.localTransform.m.size(), "node transform");
        if (node.mesh != nullptr) {
            const std::size_t meshIndex = static_cast<std::size_t>(node.mesh - data->meshes);
            output.meshPrimitives = meshPrimitiveIds[meshIndex];
            for (const AssetId id : output.meshPrimitives) {
                const auto found = std::ranges::find(result.meshes, id, &ImportedMeshPrimitive::id);
                if (found != result.meshes.end() && found->mesh.bounds.valid) output.localBounds = found->mesh.bounds;
            }
        }
        std::size_t parentHops = 0;
        for (const cgltf_node* parent = node.parent; parent != nullptr; parent = parent->parent) if (++parentHops > data->nodes_count) throw std::runtime_error("glTF node hierarchy contains a cycle");
    }
    result.animations.reserve(data->animations_count);
    for (cgltf_size animationIndex = 0; animationIndex < data->animations_count; ++animationIndex) {
        const cgltf_animation& sourceAnimation = data->animations[animationIndex];
        if (sourceAnimation.channels_count == 0U) {
            throw std::runtime_error("glTF animation contains no channels");
        }
        ImportedAnimationClip clip;
        clip.id = AssetId::derive(sceneId, "animation/" + std::to_string(animationIndex));
        clip.name = sourceAnimation.name != nullptr
            ? sourceAnimation.name
            : "Animation " + std::to_string(animationIndex);
        clip.channels.reserve(sourceAnimation.channels_count);
        for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation.channels_count; ++channelIndex) {
            const cgltf_animation_channel& sourceChannel = sourceAnimation.channels[channelIndex];
            if (sourceChannel.target_node == nullptr || sourceChannel.sampler == nullptr ||
                sourceChannel.sampler->input == nullptr || sourceChannel.sampler->output == nullptr) {
                throw std::runtime_error("glTF animation channel is incomplete");
            }
            const cgltf_accessor& input = *sourceChannel.sampler->input;
            if (input.type != cgltf_type_scalar || input.component_type != cgltf_component_type_r_32f ||
                input.count == 0U || input.count > options.maximumAnimationKeyframes ||
                input.count > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("glTF animation input accessor is invalid or exceeds the keyframe limit");
            }
            ImportedAnimationChannel channel;
            channel.targetNode = static_cast<std::uint32_t>(sourceChannel.target_node - data->nodes);
            channel.target = animationTarget(sourceChannel.target_path);
            channel.interpolation = animationInterpolation(sourceChannel.sampler->interpolation);
            channel.keyframeCount = static_cast<std::uint32_t>(input.count);
            const cgltf_accessor& output = *sourceChannel.sampler->output;
            const cgltf_type expectedOutputType = channel.target == AnimationTarget::Rotation
                ? cgltf_type_vec4
                : channel.target == AnimationTarget::Weights ? cgltf_type_scalar : cgltf_type_vec3;
            const cgltf_size interpolationMultiplier =
                channel.interpolation == AnimationInterpolation::CubicSpline ? 3U : 1U;
            const cgltf_size minimumOutputCount = input.count * interpolationMultiplier;
            const cgltf_size outputComponents = cgltf_num_components(output.type);
            if (outputComponents == 0U || output.type != expectedOutputType ||
                output.component_type != cgltf_component_type_r_32f ||
                (channel.target == AnimationTarget::Weights
                     ? output.count < minimumOutputCount ||
                           output.count % minimumOutputCount != 0U
                     : output.count != minimumOutputCount) ||
                output.count > options.maximumAnimationValues / outputComponents) {
                throw std::runtime_error(
                    "glTF animation output accessor is incompatible with its target or exceeds the value limit");
            }
            std::array<float, 4> outputValue{};
            for (cgltf_size value = 0; value < output.count; ++value) {
                if (!cgltf_accessor_read_float(
                        &output, value, outputValue.data(), outputComponents)) {
                    throw std::runtime_error("Failed to read glTF animation output accessor");
                }
                requireFinite(outputValue.data(), outputComponents, "animation output");
            }
            float previous = 0.0f;
            for (cgltf_size keyframe = 0; keyframe < input.count; ++keyframe) {
                float time = 0.0f;
                if (!cgltf_accessor_read_float(&input, keyframe, &time, 1U) || !std::isfinite(time) ||
                    time < 0.0f || (keyframe != 0U && time <= previous)) {
                    throw std::runtime_error("glTF animation keyframe times must be finite, non-negative, and strictly increasing");
                }
                if (keyframe == 0U) channel.startTime = time;
                previous = time;
            }
            channel.endTime = previous;
            validateAnimationChannel(channel, result.nodes.size());
            clip.duration = std::max(clip.duration, channel.endTime);
            clip.channels.push_back(channel);
        }
        result.animations.push_back(std::move(clip));
    }

    return result;
}

std::vector<std::byte> serializeMeshArtifact(const ImportedMeshPrimitive& mesh) {
    if (!mesh.id.valid() || !mesh.mesh.bounds.valid ||
        mesh.mesh.vertices.size() > kMaximumArtifactElements ||
        mesh.mesh.indices.size() > kMaximumArtifactElements) {
        throw std::runtime_error("Mesh artifact is invalid or oversized");
    }
    BinaryWriter writer;
    writer.pod(kMeshMagic);
    writer.pod(ImportedGltfScene::kMeshArtifactSchemaVersion);
    writeId(writer, mesh.id);
    writeId(writer, mesh.material);
    writer.string(mesh.name);
    writeBounds(writer, mesh.mesh.bounds);
    writer.pod(static_cast<std::uint64_t>(mesh.mesh.vertices.size()));
    writer.pod(static_cast<std::uint64_t>(mesh.mesh.indices.size()));
    const auto vertices = std::as_bytes(std::span{mesh.mesh.vertices});
    writer.output.insert(writer.output.end(), vertices.begin(), vertices.end());
    const auto indices = std::as_bytes(std::span{mesh.mesh.indices});
    writer.output.insert(writer.output.end(), indices.begin(), indices.end());
    return std::move(writer.output);
}

ImportedMeshPrimitive deserializeMeshArtifact(const std::span<const std::byte> bytes) {
    BinaryReader reader{bytes};
    if (reader.pod<std::uint32_t>() != kMeshMagic ||
        reader.pod<std::uint32_t>() != ImportedGltfScene::kMeshArtifactSchemaVersion) {
        throw std::runtime_error("Mesh artifact header is incompatible");
    }
    ImportedMeshPrimitive mesh;
    mesh.id = readId(reader);
    mesh.material = readId(reader);
    mesh.name = reader.string();
    mesh.mesh.bounds = readBounds(reader);
    const auto vertexCount = reader.pod<std::uint64_t>();
    const auto indexCount = reader.pod<std::uint64_t>();
    if (vertexCount > kMaximumArtifactElements || indexCount > kMaximumArtifactElements) {
        throw std::runtime_error("Mesh artifact element count exceeds limit");
    }
    reader.require(static_cast<std::size_t>(vertexCount) * sizeof(Vertex) +
                   static_cast<std::size_t>(indexCount) * sizeof(std::uint32_t));
    mesh.mesh.vertices.resize(static_cast<std::size_t>(vertexCount));
    mesh.mesh.indices.resize(static_cast<std::size_t>(indexCount));
    for (Vertex& vertex : mesh.mesh.vertices) {
        vertex = reader.pod<Vertex>();
    }
    for (std::uint32_t& index : mesh.mesh.indices) {
        index = reader.pod<std::uint32_t>();
    }
    if (!reader.done()) {
        throw std::runtime_error("Mesh artifact has trailing data");
    }
    for (const std::uint32_t index : mesh.mesh.indices) {
        if (index >= mesh.mesh.vertices.size()) {
            throw std::runtime_error("Mesh artifact index is out of range");
        }
    }
    return mesh;
}

std::vector<std::byte> serializeMaterialArtifact(const ImportedMaterial& material) {
    if (!material.id.valid() || material.textures.size() > 64U) {
        throw std::runtime_error("Material artifact is invalid");
    }
    BinaryWriter writer;
    writer.pod(kMaterialMagic);
    writer.pod(ImportedGltfScene::kMaterialArtifactSchemaVersion);
    writeId(writer, material.id);
    writer.string(material.name);
    writer.pod(material.baseColorFactor);
    writer.pod(material.emissiveFactor);
    writer.pod(material.metallicFactor);
    writer.pod(material.roughnessFactor);
    writer.pod(static_cast<std::uint8_t>(material.alphaMode));
    writer.pod(material.alphaCutoff);
    writer.pod(static_cast<std::uint8_t>(material.doubleSided));
    writer.pod(static_cast<std::uint32_t>(material.textures.size()));
    for (const ImportedTextureReference& texture : material.textures) {
        writeId(writer, texture.id);
        writer.string(texture.sourcePath.generic_string());
        writer.pod(static_cast<std::uint8_t>(texture.role));
        writer.pod(static_cast<std::uint8_t>(texture.colorSpace));
        writer.pod(texture.texcoord);
        writer.pod(texture.scale);
        writer.pod(texture.offset);
        writer.pod(texture.textureScale);
        writer.pod(texture.rotation);
    }
    return std::move(writer.output);
}

ImportedMaterial deserializeMaterialArtifact(const std::span<const std::byte> bytes) {
    BinaryReader reader{bytes};
    if (reader.pod<std::uint32_t>() != kMaterialMagic ||
        reader.pod<std::uint32_t>() != ImportedGltfScene::kMaterialArtifactSchemaVersion) {
        throw std::runtime_error("Material artifact header is incompatible");
    }
    ImportedMaterial material;
    material.id = readId(reader);
    material.name = reader.string();
    material.baseColorFactor = reader.pod<Vec4>();
    material.emissiveFactor = reader.pod<Vec3>();
    material.metallicFactor = reader.pod<float>();
    material.roughnessFactor = reader.pod<float>();
    material.alphaMode = static_cast<MaterialAlphaMode>(reader.pod<std::uint8_t>());
    material.alphaCutoff = reader.pod<float>();
    material.doubleSided = reader.pod<std::uint8_t>() != 0U;
    const auto count = reader.pod<std::uint32_t>();
    if (count > 64U) {
        throw std::runtime_error("Material artifact texture count exceeds limit");
    }
    material.textures.resize(count);
    for (ImportedTextureReference& texture : material.textures) {
        texture.id = readId(reader);
        texture.sourcePath = reader.string();
        texture.role = static_cast<TextureRole>(reader.pod<std::uint8_t>());
        texture.colorSpace = static_cast<TextureColorSpace>(reader.pod<std::uint8_t>());
        texture.texcoord = reader.pod<std::uint32_t>();
        texture.scale = reader.pod<float>();
        texture.offset = reader.pod<Vec2>();
        texture.textureScale = reader.pod<Vec2>();
        texture.rotation = reader.pod<float>();
    }
    if (!reader.done()) {
        throw std::runtime_error("Material artifact has trailing data");
    }
    return material;
}

std::vector<std::byte> serializeSceneArtifact(const ImportedGltfScene& scene) {
    if (!scene.sceneId.valid() || scene.nodes.size() > kMaximumArtifactElements) {
        throw std::runtime_error("Scene artifact is invalid or oversized");
    }
    BinaryWriter writer;
    writer.pod(kSceneMagic);
    writer.pod(ImportedGltfScene::kSceneArtifactSchemaVersion);
    writeId(writer, scene.sceneId);
    writer.string(scene.sourcePath.generic_string());
    writer.pod(static_cast<std::uint64_t>(scene.nodes.size()));
    for (const ImportedSceneNode& node : scene.nodes) {
        writer.string(node.name);
        writer.pod(node.parent);
        writer.pod(node.localTransform);
        writeBounds(writer, node.localBounds);
        writer.pod(static_cast<std::uint32_t>(node.meshPrimitives.size()));
        for (const AssetId mesh : node.meshPrimitives) {
            writeId(writer, mesh);
        }
    }
    if (scene.animations.size() > kMaximumArtifactElements) {
        throw std::runtime_error("Scene artifact animation count exceeds limit");
    }
    writer.pod(static_cast<std::uint64_t>(scene.animations.size()));
    for (const ImportedAnimationClip& clip : scene.animations) {
        if (!clip.id.valid() || !std::isfinite(clip.duration) || clip.duration < 0.0f ||
            clip.channels.empty() || clip.channels.size() > kMaximumArtifactElements) {
            throw std::runtime_error("Scene artifact animation is invalid or oversized");
        }
        writeId(writer, clip.id);
        writer.string(clip.name);
        writer.pod(clip.duration);
        writer.pod(static_cast<std::uint64_t>(clip.channels.size()));
        for (const ImportedAnimationChannel& channel : clip.channels) {
            validateAnimationChannel(channel, scene.nodes.size());
            writer.pod(channel.targetNode);
            writer.pod(static_cast<std::uint8_t>(channel.target));
            writer.pod(static_cast<std::uint8_t>(channel.interpolation));
            writer.pod(channel.keyframeCount);
            writer.pod(channel.startTime);
            writer.pod(channel.endTime);
        }
    }
    return std::move(writer.output);
}

ImportedGltfScene deserializeSceneArtifact(const std::span<const std::byte> bytes) {
    BinaryReader reader{bytes};
    if (reader.pod<std::uint32_t>() != kSceneMagic ||
        reader.pod<std::uint32_t>() != ImportedGltfScene::kSceneArtifactSchemaVersion) {
        throw std::runtime_error("Scene artifact header is incompatible");
    }
    ImportedGltfScene scene;
    scene.sceneId = readId(reader);
    scene.sourcePath = reader.string();
    const auto nodeCount = reader.pod<std::uint64_t>();
    if (nodeCount > kMaximumArtifactElements) {
        throw std::runtime_error("Scene artifact node count exceeds limit");
    }
    scene.nodes.resize(static_cast<std::size_t>(nodeCount));
    for (ImportedSceneNode& node : scene.nodes) {
        node.name = reader.string();
        node.parent = reader.pod<std::uint32_t>();
        node.localTransform = reader.pod<Mat4>();
        node.localBounds = readBounds(reader);
        const auto meshCount = reader.pod<std::uint32_t>();
        if (meshCount > (1U << 20U)) {
            throw std::runtime_error("Scene node mesh count exceeds limit");
        }
        node.meshPrimitives.resize(meshCount);
        for (AssetId& mesh : node.meshPrimitives) {
            mesh = readId(reader);
        }
    }
    const auto animationCount = reader.pod<std::uint64_t>();
    if (animationCount > kMaximumArtifactElements) {
        throw std::runtime_error("Scene artifact animation count exceeds limit");
    }
    scene.animations.resize(static_cast<std::size_t>(animationCount));
    for (ImportedAnimationClip& clip : scene.animations) {
        clip.id = readId(reader);
        clip.name = reader.string();
        clip.duration = reader.pod<float>();
        const auto channelCount = reader.pod<std::uint64_t>();
        if (!clip.id.valid() || !std::isfinite(clip.duration) || clip.duration < 0.0f ||
            channelCount == 0U || channelCount > kMaximumArtifactElements) {
            throw std::runtime_error("Scene artifact animation is invalid or oversized");
        }
        clip.channels.resize(static_cast<std::size_t>(channelCount));
        float computedDuration = 0.0f;
        for (ImportedAnimationChannel& channel : clip.channels) {
            channel.targetNode = reader.pod<std::uint32_t>();
            channel.target = static_cast<AnimationTarget>(reader.pod<std::uint8_t>());
            channel.interpolation =
                static_cast<AnimationInterpolation>(reader.pod<std::uint8_t>());
            channel.keyframeCount = reader.pod<std::uint32_t>();
            channel.startTime = reader.pod<float>();
            channel.endTime = reader.pod<float>();
            validateAnimationChannel(channel, scene.nodes.size());
            computedDuration = std::max(computedDuration, channel.endTime);
        }
        if (clip.duration != computedDuration) {
            throw std::runtime_error("Scene artifact animation duration is inconsistent");
        }
    }
    if (!reader.done()) {
        throw std::runtime_error("Scene artifact has trailing data");
    }
    for (const ImportedSceneNode& node : scene.nodes) {
        if (node.parent != kInvalidNode && node.parent >= scene.nodes.size()) {
            throw std::runtime_error("Scene artifact node parent is out of range");
        }
    }
    return scene;
}

void registerGltfImporter(SceneImporterRegistry& registry) {
    static constexpr std::array<std::string_view, 2> extensions{".glb", ".gltf"};
    registry.registerImporter({
        "volkengine.cgltf", 2U, extensions,
        +[](const std::filesystem::path& source, const AssetId sceneId) {
            return importGltfScene(source, sceneId);
        }});
}

} // namespace ve
