#include "assets/TextureArtifact.hpp"

#include "core/FileSystem.hpp"
#include "renderer/ImageLoader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace ve {
namespace {

constexpr std::uint32_t kTextureMagic = 0x31584556U;
constexpr std::size_t kKtx2HeaderBytes = 80U;
constexpr std::size_t kKtx2LevelIndexBytes = 24U;
constexpr std::array<std::byte, 12> kKtx2Identifier{
    std::byte{0xAB}, std::byte{'K'}, std::byte{'T'}, std::byte{'X'}, std::byte{' '},
    std::byte{'2'}, std::byte{'0'}, std::byte{0xBB}, std::byte{0x0D}, std::byte{0x0A},
    std::byte{0x1A}, std::byte{0x0A}};

class Writer {
public:
    template <typename T> void pod(const T value) {
        const auto bytes = std::as_bytes(std::span{&value, 1U});
        output.insert(output.end(), bytes.begin(), bytes.end());
    }
    std::vector<std::byte> output;
};

class Reader {
public:
    explicit Reader(const std::span<const std::byte> input) : input_(input) {}
    template <typename T> T pod() {
        require(sizeof(T));
        T value;
        std::memcpy(&value, input_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }
    void require(const std::size_t count) const {
        if (count > input_.size() - offset_) throw std::runtime_error("Texture artifact is truncated");
    }
    [[nodiscard]] std::span<const std::byte> bytes(const std::size_t count) {
        require(count);
        const auto result = input_.subspan(offset_, count);
        offset_ += count;
        return result;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == input_.size(); }
private:
    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
};

template <typename T>
T readLittle(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw std::runtime_error("KTX2 structure is truncated");
    }
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

std::uint64_t expectedMipBytes(const TextureStorage storage, const std::uint32_t width,
                               const std::uint32_t height) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    switch (storage) {
    case TextureStorage::Rgba8: return pixels * 4U;
    case TextureStorage::Rgba32Float: return pixels * 4U * sizeof(float);
    case TextureStorage::Bc1Rgba:
        return static_cast<std::uint64_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * 8U;
    case TextureStorage::Bc3Rgba:
    case TextureStorage::Bc7Rgba:
        return static_cast<std::uint64_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * 16U;
    }
    throw std::runtime_error("Texture artifact storage is invalid");
}

void validateTexture(const TextureArtifact& texture) {
    if (!texture.id.valid() || texture.width == 0U || texture.height == 0U ||
        texture.mips.empty() || texture.mips.size() > 32U ||
        texture.role > TextureRole::Emissive || texture.colorSpace > TextureColorSpace::Srgb ||
        texture.storage > TextureStorage::Bc7Rgba) {
        throw std::runtime_error("Texture artifact metadata is invalid");
    }
    std::uint32_t width = texture.width;
    std::uint32_t height = texture.height;
    std::uint64_t offset = 0U;
    for (const TextureMip& mip : texture.mips) {
        const std::uint64_t expected = expectedMipBytes(texture.storage, width, height);
        if (mip.width != width || mip.height != height || mip.offset != offset ||
            mip.size != expected || expected > std::numeric_limits<std::uint64_t>::max() - offset) {
            throw std::runtime_error("Texture artifact mip layout is invalid");
        }
        offset += expected;
        width = std::max(1U, width / 2U);
        height = std::max(1U, height / 2U);
    }
    if (offset != texture.data.size()) {
        throw std::runtime_error("Texture artifact payload size is inconsistent");
    }
    if (texture.storage == TextureStorage::Rgba32Float) {
        for (std::size_t offsetBytes = 0; offsetBytes < texture.data.size(); offsetBytes += sizeof(float)) {
            float value = 0.0f;
            std::memcpy(&value, texture.data.data() + offsetBytes, sizeof(value));
            if (!std::isfinite(value)) {
                throw std::runtime_error("Texture artifact HDR payload contains NaN or infinity");
            }
        }
    }
}

struct BlockFormat {
    TextureStorage storage;
    TextureColorSpace colorSpace;
};

BlockFormat blockFormat(const std::uint32_t vkFormat) {
    switch (vkFormat) {
    case 131U:
    case 133U: return {TextureStorage::Bc1Rgba, TextureColorSpace::Linear};
    case 132U:
    case 134U: return {TextureStorage::Bc1Rgba, TextureColorSpace::Srgb};
    case 137U: return {TextureStorage::Bc3Rgba, TextureColorSpace::Linear};
    case 138U: return {TextureStorage::Bc3Rgba, TextureColorSpace::Srgb};
    case 145U: return {TextureStorage::Bc7Rgba, TextureColorSpace::Linear};
    case 146U: return {TextureStorage::Bc7Rgba, TextureColorSpace::Srgb};
    default:
        throw std::runtime_error("KTX2 vkFormat is not a supported BC1/BC3/BC7 GPU format: " +
                                 std::to_string(vkFormat));
    }
}

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

bool overlaps(const ByteRange left, const ByteRange right) {
    return left.offset < right.offset + right.length &&
           right.offset < left.offset + left.length;
}

void appendDisjointRegion(const std::span<const std::byte> source,
                          std::vector<ByteRange>& regions, const std::uint64_t offset,
                          const std::uint64_t length, const char* name) {
    if (length == 0U) return;
    if (offset > source.size() || length > source.size() - offset) {
        throw std::runtime_error(std::string("KTX2 ") + name + " region is out of range");
    }
    const ByteRange candidate{offset, length};
    if (std::ranges::any_of(regions, [&](const ByteRange existing) {
            return overlaps(candidate, existing);
        })) {
        throw std::runtime_error(std::string("KTX2 ") + name + " region overlaps another region");
    }
    regions.push_back(candidate);
}

TextureArtifact importKtx2(const std::span<const std::byte> source, const AssetId id,
                           const TextureRole role, const TextureColorSpace requestedColorSpace,
                           const TextureImportOptions& options) {
    if (source.size() < kKtx2HeaderBytes ||
        !std::ranges::equal(kKtx2Identifier, source.first(kKtx2Identifier.size()))) {
        throw std::runtime_error("KTX2 identifier is missing or invalid");
    }
    const std::uint32_t vkFormat = readLittle<std::uint32_t>(source, 12U);
    const std::uint32_t typeSize = readLittle<std::uint32_t>(source, 16U);
    const std::uint32_t width = readLittle<std::uint32_t>(source, 20U);
    const std::uint32_t height = readLittle<std::uint32_t>(source, 24U);
    const std::uint32_t depth = readLittle<std::uint32_t>(source, 28U);
    const std::uint32_t layers = readLittle<std::uint32_t>(source, 32U);
    const std::uint32_t faces = readLittle<std::uint32_t>(source, 36U);
    const std::uint32_t levelCount = readLittle<std::uint32_t>(source, 40U);
    const std::uint32_t supercompression = readLittle<std::uint32_t>(source, 44U);
    if (width == 0U || height == 0U || depth != 0U || layers > 1U || faces != 1U ||
        levelCount == 0U || levelCount > 32U || typeSize != 1U) {
        throw std::runtime_error("KTX2 must be a bounded two-dimensional single-layer texture");
    }
    std::uint32_t maximumLevelCount = 1U;
    for (std::uint32_t mipWidth = width, mipHeight = height;
         mipWidth > 1U || mipHeight > 1U; ++maximumLevelCount) {
        mipWidth = std::max(1U, mipWidth / 2U);
        mipHeight = std::max(1U, mipHeight / 2U);
    }
    if (levelCount > maximumLevelCount) {
        throw std::runtime_error("KTX2 level count exceeds the complete mip chain");
    }
    if (supercompression != 0U) {
        throw std::runtime_error("KTX2 supercompression requires a BasisLZ/Zstd transcoder and is unsupported");
    }
    const BlockFormat format = blockFormat(vkFormat);
    if (format.colorSpace != requestedColorSpace) {
        throw std::runtime_error("KTX2 vkFormat color space does not match the material texture role");
    }
    const std::uint64_t levelIndexEnd = kKtx2HeaderBytes +
        static_cast<std::uint64_t>(levelCount) * kKtx2LevelIndexBytes;
    if (levelIndexEnd > source.size()) throw std::runtime_error("KTX2 level index is truncated");
    const std::uint32_t dfdOffset = readLittle<std::uint32_t>(source, 48U);
    const std::uint32_t dfdLength = readLittle<std::uint32_t>(source, 52U);
    const std::uint32_t kvdOffset = readLittle<std::uint32_t>(source, 56U);
    const std::uint32_t kvdLength = readLittle<std::uint32_t>(source, 60U);
    const std::uint64_t sgdOffset = readLittle<std::uint64_t>(source, 64U);
    const std::uint64_t sgdLength = readLittle<std::uint64_t>(source, 72U);
    if ((dfdLength != 0U && dfdOffset % 4U != 0U) ||
        (kvdLength != 0U && kvdOffset % 4U != 0U) ||
        (sgdLength != 0U && sgdOffset % 8U != 0U)) {
        throw std::runtime_error("KTX2 metadata region alignment is invalid");
    }
    std::vector<ByteRange> occupied{{0U, levelIndexEnd}};
    appendDisjointRegion(source, occupied, dfdOffset, dfdLength,
                         "data-format descriptor");
    appendDisjointRegion(source, occupied, kvdOffset, kvdLength, "key/value data");
    appendDisjointRegion(source, occupied, sgdOffset, sgdLength,
                         "supercompression global data");

    struct SourceMip {
        TextureMip target;
        ByteRange source;
    };
    std::vector<SourceMip> sourceMips;
    sourceMips.reserve(levelCount);
    std::uint64_t payloadOffset = 0U;
    std::uint32_t mipWidth = width;
    std::uint32_t mipHeight = height;
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const std::size_t index =
            kKtx2HeaderBytes + static_cast<std::size_t>(level) * kKtx2LevelIndexBytes;
        const std::uint64_t sourceOffset = readLittle<std::uint64_t>(source, index);
        const std::uint64_t sourceLength = readLittle<std::uint64_t>(source, index + 8U);
        const std::uint64_t uncompressedLength =
            readLittle<std::uint64_t>(source, index + 16U);
        const std::uint64_t expected = expectedMipBytes(format.storage, mipWidth, mipHeight);
        if (sourceOffset % 8U != 0U || sourceLength != expected ||
            uncompressedLength != sourceLength || sourceOffset > source.size() ||
            sourceLength > source.size() - sourceOffset ||
            payloadOffset > options.maximumPayloadBytes ||
            expected > options.maximumPayloadBytes - payloadOffset) {
            throw std::runtime_error("KTX2 mip level layout or byte size is invalid");
        }
        appendDisjointRegion(source, occupied, sourceOffset, sourceLength, "mip level");
        sourceMips.push_back({
            {mipWidth, mipHeight, payloadOffset, sourceLength},
            {sourceOffset, sourceLength}});
        payloadOffset += sourceLength;
        mipWidth = std::max(1U, mipWidth / 2U);
        mipHeight = std::max(1U, mipHeight / 2U);
    }

    TextureArtifact texture;
    texture.id = id;
    texture.role = role;
    texture.colorSpace = format.colorSpace;
    texture.storage = format.storage;
    texture.width = width;
    texture.height = height;
    texture.mips.reserve(levelCount);
    texture.data.reserve(static_cast<std::size_t>(payloadOffset));
    for (const SourceMip& mip : sourceMips) {
        texture.mips.push_back(mip.target);
        const auto levelBytes = source.subspan(static_cast<std::size_t>(mip.source.offset),
                                               static_cast<std::size_t>(mip.source.length));
        texture.data.insert(texture.data.end(), levelBytes.begin(), levelBytes.end());
    }
    validateTexture(texture);
    return texture;
}

} // namespace

TextureArtifact importTextureArtifact(const std::filesystem::path& path, const AssetId id,
                                      const TextureRole role,
                                      const TextureColorSpace colorSpace,
                                      const TextureImportOptions& options) {
    if (!id.valid()) throw std::invalid_argument("Texture AssetId is invalid");
    const std::vector<std::byte> source = readBinaryFile(path, options.maximumSourceBytes);
    const std::string extension = lowercaseExtension(path);
    if (extension == ".ktx2") return importKtx2(source, id, role, colorSpace, options);

    TextureArtifact texture;
    texture.id = id;
    texture.role = role;
    texture.colorSpace = colorSpace;
    if (extension == ".hdr") {
        if (colorSpace != TextureColorSpace::Linear) {
            throw std::runtime_error("HDR texture artifacts must use linear color space");
        }
        LoadedImageRgba32F image = loadImageRgba32F(source, path.generic_string());
        texture.storage = TextureStorage::Rgba32Float;
        texture.width = image.width;
        texture.height = image.height;
        const auto bytes = std::as_bytes(std::span{image.pixels});
        if (bytes.size() > options.maximumPayloadBytes) {
            throw std::runtime_error("Decoded HDR texture exceeds the payload limit");
        }
        texture.data.assign(bytes.begin(), bytes.end());
    } else {
        LoadedImageRgba8 image = loadImageRgba8(source, path.generic_string());
        texture.storage = TextureStorage::Rgba8;
        texture.width = image.width;
        texture.height = image.height;
        if (image.pixels.size() > options.maximumPayloadBytes) {
            throw std::runtime_error("Decoded texture exceeds the payload limit");
        }
        const auto bytes = std::as_bytes(std::span{image.pixels});
        texture.data.assign(bytes.begin(), bytes.end());
    }
    texture.mips.push_back({texture.width, texture.height, 0U, texture.data.size()});
    validateTexture(texture);
    return texture;
}

std::vector<std::byte> serializeTextureArtifact(const TextureArtifact& texture) {
    validateTexture(texture);
    Writer writer;
    writer.pod(kTextureMagic);
    writer.pod(TextureArtifact::kSchemaVersion);
    writer.pod(texture.id.high);
    writer.pod(texture.id.low);
    writer.pod(static_cast<std::uint8_t>(texture.role));
    writer.pod(static_cast<std::uint8_t>(texture.colorSpace));
    writer.pod(static_cast<std::uint8_t>(texture.storage));
    writer.pod(texture.width);
    writer.pod(texture.height);
    writer.pod(static_cast<std::uint32_t>(texture.mips.size()));
    writer.pod(static_cast<std::uint64_t>(texture.data.size()));
    for (const TextureMip& mip : texture.mips) {
        writer.pod(mip.width);
        writer.pod(mip.height);
        writer.pod(mip.offset);
        writer.pod(mip.size);
    }
    writer.output.insert(writer.output.end(), texture.data.begin(), texture.data.end());
    return std::move(writer.output);
}

TextureArtifact deserializeTextureArtifact(const std::span<const std::byte> bytes) {
    Reader reader{bytes};
    if (reader.pod<std::uint32_t>() != kTextureMagic ||
        reader.pod<std::uint32_t>() != TextureArtifact::kSchemaVersion) {
        throw std::runtime_error("Texture artifact header is incompatible");
    }
    TextureArtifact texture;
    texture.id.high = reader.pod<std::uint64_t>();
    texture.id.low = reader.pod<std::uint64_t>();
    texture.role = static_cast<TextureRole>(reader.pod<std::uint8_t>());
    texture.colorSpace = static_cast<TextureColorSpace>(reader.pod<std::uint8_t>());
    texture.storage = static_cast<TextureStorage>(reader.pod<std::uint8_t>());
    texture.width = reader.pod<std::uint32_t>();
    texture.height = reader.pod<std::uint32_t>();
    const std::uint32_t mipCount = reader.pod<std::uint32_t>();
    const std::uint64_t payloadBytes = reader.pod<std::uint64_t>();
    if (mipCount == 0U || mipCount > 32U || payloadBytes > 512U * 1024U * 1024U) {
        throw std::runtime_error("Texture artifact counts exceed limits");
    }
    texture.mips.resize(mipCount);
    for (TextureMip& mip : texture.mips) {
        mip.width = reader.pod<std::uint32_t>();
        mip.height = reader.pod<std::uint32_t>();
        mip.offset = reader.pod<std::uint64_t>();
        mip.size = reader.pod<std::uint64_t>();
    }
    const auto payload = reader.bytes(static_cast<std::size_t>(payloadBytes));
    texture.data.assign(payload.begin(), payload.end());
    if (!reader.done()) throw std::runtime_error("Texture artifact has trailing data");
    validateTexture(texture);
    return texture;
}

} // namespace ve
