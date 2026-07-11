#pragma once

#include "assets/ContentHash.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ve {

struct AssetId {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool valid() const noexcept { return high != 0U || low != 0U; }
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] static AssetId fromHex(std::string_view value);
    [[nodiscard]] static AssetId derive(AssetId parent, std::string_view stableSubresourceName) noexcept;

    friend bool operator==(const AssetId&, const AssetId&) = default;
    friend auto operator<=>(const AssetId&, const AssetId&) = default;
};

enum class AssetType : std::uint8_t { Mesh, Texture, Material, Scene, GeneratedMesh };
enum class AssetState : std::uint8_t { Missing, SourceKnown, Importing, Ready, Stale, Incompatible, Failed };

struct AssetRecord {
    static constexpr std::uint32_t kSchemaVersion = 1;

    AssetId id;
    AssetType type = AssetType::Mesh;
    std::uint32_t artifactSchemaVersion = 1;
    std::filesystem::path sourcePath;
    ContentHash sourceHash;
    std::string importerId;
    std::uint32_t importerVersion = 0;
    std::string normalizedSettings;
    ContentHash settingsHash;
    std::vector<AssetId> dependencies;
    ContentHash artifactKey;
    std::filesystem::path artifactPath;
    std::string target;
    AssetState state = AssetState::Missing;
    std::string diagnostic;
};

class AssetDatabase {
public:
    static constexpr std::uint32_t kSchemaVersion = 1;
    static constexpr std::size_t kMaximumRecords = 1'000'000;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::span<const AssetRecord> records() const noexcept { return records_; }
    [[nodiscard]] const AssetRecord* find(AssetId id) const noexcept;
    [[nodiscard]] std::vector<AssetId> reverseDependencies(AssetId id) const;

    void replaceAll(std::vector<AssetRecord> records);
    void upsert(AssetRecord record);
    void markChanged(AssetId id, std::string reason);

    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] static AssetDatabase deserialize(std::span<const std::byte> bytes);
    void saveAtomic(const std::filesystem::path& path) const;
    [[nodiscard]] static AssetDatabase load(const std::filesystem::path& path);

private:
    static void validate(std::vector<AssetRecord>& records);
    std::vector<AssetRecord> records_;
    std::uint64_t generation_ = 0;
};

} // namespace ve
