#pragma once

#include "assets/AssetDatabase.hpp"
#include "renderer/Geometry.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ve {

enum class TextureColorSpace : std::uint8_t { Linear, Srgb };
enum class TextureRole : std::uint8_t { BaseColor, Normal, MetallicRoughness, Occlusion, Emissive };
enum class MaterialAlphaMode : std::uint8_t { Opaque, Mask };

struct ImportedTextureReference {
    AssetId id;
    std::filesystem::path sourcePath;
    TextureRole role = TextureRole::BaseColor;
    TextureColorSpace colorSpace = TextureColorSpace::Linear;
    std::uint32_t texcoord = 0;
    float scale = 1.0f;
    Vec2 offset{};
    Vec2 textureScale{1.0f, 1.0f};
    float rotation = 0.0f;
};

struct ImportedMaterial {
    AssetId id;
    std::string name;
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    Vec3 emissiveFactor{};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    std::vector<ImportedTextureReference> textures;
};

struct ImportedMeshPrimitive {
    AssetId id;
    std::string name;
    MeshData mesh;
    AssetId material;
};

struct ImportedSceneNode {
    std::string name;
    std::uint32_t parent = std::numeric_limits<std::uint32_t>::max();
    Mat4 localTransform = Mat4::identity();
    std::vector<AssetId> meshPrimitives;
    MeshBounds localBounds{};
};

struct ImportedGltfScene {
    static constexpr std::uint32_t kArtifactSchemaVersion = 2;
    AssetId sceneId;
    std::filesystem::path sourcePath;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedMeshPrimitive> meshes;
    std::vector<ImportedSceneNode> nodes;
    std::vector<std::string> optionalFeatureDiagnostics;
};

struct GltfImportOptions {
    std::size_t maximumSourceBytes = 256U * 1024U * 1024U;
    std::size_t maximumVertices = 16U * 1024U * 1024U;
    std::size_t maximumIndices = 64U * 1024U * 1024U;
    bool generateMissingNormals = true;
    bool generateMissingTangents = true;
};

[[nodiscard]] ImportedGltfScene importGltfScene(const std::filesystem::path& path, AssetId sceneId,
                                                const GltfImportOptions& options = {});

[[nodiscard]] std::vector<std::byte> serializeMeshArtifact(const ImportedMeshPrimitive& mesh);
[[nodiscard]] ImportedMeshPrimitive deserializeMeshArtifact(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serializeMaterialArtifact(const ImportedMaterial& material);
[[nodiscard]] ImportedMaterial deserializeMaterialArtifact(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serializeSceneArtifact(const ImportedGltfScene& scene);
[[nodiscard]] ImportedGltfScene deserializeSceneArtifact(std::span<const std::byte> bytes);

} // namespace ve
