#include "assets/DerivedDataCache.hpp"

#include "core/FileSystem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace ve {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'V'}, std::byte{'E'}, std::byte{'A'}, std::byte{'R'},
    std::byte{'T'}, std::byte{0}, std::byte{0}, std::byte{1}};
constexpr std::size_t kHeaderBytes = 8U + 1U + 4U + 8U + 32U;

void appendU32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) output.push_back(static_cast<std::byte>(value >> (index * 8U)));
}
void appendU64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) output.push_back(static_cast<std::byte>(value >> (index * 8U)));
}
void appendString(std::vector<std::byte>& output, const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("DDC key string is too large");
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    output.insert(output.end(), bytes.begin(), bytes.end());
}

std::uint32_t readU32(const std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    return value;
}
std::uint64_t readU64(const std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    return value;
}

class DirectoryLock {
public:
    explicit DirectoryLock(std::filesystem::path path) : path_(std::move(path)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::error_code error;
        while (!std::filesystem::create_directory(path_, error)) {
            if (error && error != std::errc::file_exists) throw std::runtime_error("Failed to acquire DDC publication lock: " + error.message());
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Timed out acquiring DDC publication lock");
            error.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ~DirectoryLock() { std::error_code error; std::filesystem::remove(path_, error); }
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
private:
    std::filesystem::path path_;
};

const char* typeDirectory(const ArtifactType type) {
    switch (type) {
    case ArtifactType::Mesh: return "mesh";
    case ArtifactType::Texture: return "texture";
    case ArtifactType::Material: return "material";
    case ArtifactType::Scene: return "scene";
    }
    throw std::runtime_error("Unknown artifact type");
}

} // namespace

ContentHash makeDerivedDataKey(const DerivedDataKeyInput& input) {
    if (input.importerId.empty() || input.artifactSchemaVersion == 0U || input.targetPlatform.empty()) {
        throw std::invalid_argument("DDC key input is incomplete");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(128U + input.importerId.size() + input.targetPlatform.size() + input.gpuFormat.size() +
                  input.dependencyArtifactHashes.size() * 32U);
    bytes.insert(bytes.end(), input.sourceHash.bytes.begin(), input.sourceHash.bytes.end());
    appendString(bytes, input.importerId);
    appendU32(bytes, input.importerVersion);
    bytes.insert(bytes.end(), input.settingsHash.bytes.begin(), input.settingsHash.bytes.end());
    appendU32(bytes, static_cast<std::uint32_t>(input.dependencyArtifactHashes.size()));
    for (const ContentHash& dependency : input.dependencyArtifactHashes) bytes.insert(bytes.end(), dependency.bytes.begin(), dependency.bytes.end());
    bytes.push_back(static_cast<std::byte>(input.type));
    appendU32(bytes, input.artifactSchemaVersion);
    appendString(bytes, input.targetPlatform);
    appendString(bytes, input.gpuFormat);
    return hashBytes(bytes);
}

std::filesystem::path DerivedDataCache::artifactPath(const ContentHash key, const ArtifactType type) const {
    const std::string hash = key.hex();
    return root_ / typeDirectory(type) / hash.substr(0U, 2U) / (hash + ".veart");
}

bool DerivedDataCache::contains(const ContentHash key, const ArtifactType type) const {
    std::error_code error;
    return std::filesystem::is_regular_file(artifactPath(key, type), error) && !error;
}

ArtifactBlob DerivedDataCache::load(const ContentHash key, const ArtifactType expectedType,
                                    const std::uint32_t expectedSchemaVersion) const {
    const std::filesystem::path path = artifactPath(key, expectedType);
    const std::vector<std::byte> bytes = readBinaryFile(path, static_cast<std::size_t>(kMaximumArtifactBytes + kHeaderBytes));
    if (bytes.size() < kHeaderBytes) throw std::runtime_error("DDC artifact is truncated: " + path.string());
    const std::span<const std::byte> view{bytes};
    if (!std::ranges::equal(view.first(kMagic.size()), kMagic)) throw std::runtime_error("DDC artifact magic is invalid: " + path.string());
    const auto type = static_cast<ArtifactType>(std::to_integer<std::uint8_t>(view[8U]));
    if (type != expectedType) throw std::runtime_error("DDC artifact type does not match request: " + path.string());
    const std::uint32_t schema = readU32(view, 9U);
    if (schema != expectedSchemaVersion) throw std::runtime_error("DDC artifact schema version is incompatible: " + path.string());
    const std::uint64_t payloadSize = readU64(view, 13U);
    if (payloadSize > kMaximumArtifactBytes || payloadSize != bytes.size() - kHeaderBytes) throw std::runtime_error("DDC artifact payload size is invalid: " + path.string());
    ContentHash storedHash;
    std::ranges::copy(view.subspan(21U, storedHash.bytes.size()), storedHash.bytes.begin());
    const std::span<const std::byte> payload = view.subspan(kHeaderBytes);
    const ContentHash actualHash = hashBytes(payload);
    if (storedHash != actualHash) throw std::runtime_error("DDC artifact checksum mismatch: " + path.string());
    return ArtifactBlob{type, schema, actualHash, {payload.begin(), payload.end()}};
}

bool DerivedDataCache::publish(const ContentHash key, const ArtifactType type, const std::uint32_t schemaVersion,
                               const std::span<const std::byte> payload) {
    if (schemaVersion == 0U) throw std::invalid_argument("Artifact schema version must be nonzero");
    if (payload.size() > kMaximumArtifactBytes) throw std::runtime_error("Artifact payload exceeds size limit");
    const std::filesystem::path path = artifactPath(key, type);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) throw std::runtime_error("Failed to create DDC directory: " + error.message());
    DirectoryLock lock{path.string() + ".lock"};
    if (contains(key, type)) {
        const ArtifactBlob existing = load(key, type, schemaVersion);
        if (existing.payloadHash != hashBytes(payload)) throw std::runtime_error("DDC key collision has conflicting payload content");
        return false;
    }
    std::vector<std::byte> serialized;
    serialized.reserve(kHeaderBytes + payload.size());
    serialized.insert(serialized.end(), kMagic.begin(), kMagic.end());
    serialized.push_back(static_cast<std::byte>(type));
    appendU32(serialized, schemaVersion);
    appendU64(serialized, static_cast<std::uint64_t>(payload.size()));
    const ContentHash payloadHash = hashBytes(payload);
    serialized.insert(serialized.end(), payloadHash.bytes.begin(), payloadHash.bytes.end());
    serialized.insert(serialized.end(), payload.begin(), payload.end());
    writeBinaryFileAtomic(path, serialized);
    static_cast<void>(load(key, type, schemaVersion));
    return true;
}

} // namespace ve
