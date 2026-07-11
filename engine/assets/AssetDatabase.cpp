#include "assets/AssetDatabase.hpp"

#include "core/FileSystem.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'V'}, std::byte{'E'}, std::byte{'A'}, std::byte{'S'},
    std::byte{'D'}, std::byte{'B'}, std::byte{0}, std::byte{1}};
constexpr std::uint32_t kMaximumStringBytes = 1U << 20U;
constexpr std::uint32_t kMaximumDependencies = 1U << 16U;
constexpr char kHexDigits[] = "0123456789abcdef";

std::uint8_t nibble(const char value) {
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
    throw std::invalid_argument("AssetId contains a non-hexadecimal character");
}

class Writer {
public:
    template <typename T> void integer(T value) {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_.push_back(static_cast<std::byte>(value >> (index * 8U)));
        }
    }
    void raw(const std::span<const std::byte> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    void string(const std::string_view value) {
        if (value.size() > kMaximumStringBytes) throw std::runtime_error("Asset database string exceeds size limit");
        integer(static_cast<std::uint32_t>(value.size()));
        raw(std::as_bytes(std::span{value.data(), value.size()}));
    }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }
private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}
    template <typename T> T integer() {
        static_assert(std::is_unsigned_v<T>);
        require(sizeof(T));
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            value |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(bytes_[offset_ + index])) << (index * 8U);
        }
        offset_ += sizeof(T);
        return static_cast<T>(value);
    }
    std::span<const std::byte> raw(const std::size_t size) {
        require(size);
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }
    std::string string() {
        const std::uint32_t size = integer<std::uint32_t>();
        if (size > kMaximumStringBytes) throw std::runtime_error("Asset database string exceeds size limit");
        const auto data = raw(size);
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }
private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - offset_) throw std::runtime_error("Asset database is truncated");
    }
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

void writeId(Writer& writer, const AssetId id) { writer.integer(id.high); writer.integer(id.low); }
AssetId readId(Reader& reader) { return {reader.integer<std::uint64_t>(), reader.integer<std::uint64_t>()}; }
void writeHash(Writer& writer, const ContentHash& hash) { writer.raw(hash.bytes); }
ContentHash readHash(Reader& reader) {
    ContentHash hash;
    const auto bytes = reader.raw(hash.bytes.size());
    std::ranges::copy(bytes, hash.bytes.begin());
    return hash;
}

std::string pathString(const std::filesystem::path& path) { return path.generic_string(); }

} // namespace

std::string AssetId::hex() const {
    std::string result(32U, '0');
    const std::array values{high, low};
    for (std::size_t word = 0; word < values.size(); ++word) {
        for (std::size_t index = 0; index < 16U; ++index) {
            const unsigned shift = static_cast<unsigned>((15U - index) * 4U);
            result[word * 16U + index] = kHexDigits[(values[word] >> shift) & 0xfU];
        }
    }
    return result;
}

AssetId AssetId::fromHex(const std::string_view value) {
    if (value.size() != 32U) throw std::invalid_argument("AssetId must contain exactly 32 hexadecimal characters");
    AssetId result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint64_t& word = index < 16U ? result.high : result.low;
        word = (word << 4U) | nibble(value[index]);
    }
    if (!result.valid()) throw std::invalid_argument("AssetId must not be zero");
    return result;
}

AssetId AssetId::derive(const AssetId parent, const std::string_view stableSubresourceName) noexcept {
    std::array<std::byte, 16> parentBytes{};
    for (std::size_t index = 0; index < 8U; ++index) {
        parentBytes[index] = static_cast<std::byte>(parent.high >> ((7U - index) * 8U));
        parentBytes[index + 8U] = static_cast<std::byte>(parent.low >> ((7U - index) * 8U));
    }
    std::vector<std::byte> input;
    input.reserve(parentBytes.size() + stableSubresourceName.size());
    input.insert(input.end(), parentBytes.begin(), parentBytes.end());
    const auto nameBytes = std::as_bytes(std::span{stableSubresourceName.data(), stableSubresourceName.size()});
    input.insert(input.end(), nameBytes.begin(), nameBytes.end());
    const ContentHash hash = hashBytes(input);
    AssetId result;
    for (std::size_t index = 0; index < 8U; ++index) {
        result.high = (result.high << 8U) | std::to_integer<std::uint8_t>(hash.bytes[index]);
        result.low = (result.low << 8U) | std::to_integer<std::uint8_t>(hash.bytes[index + 8U]);
    }
    if (!result.valid()) result.low = 1U;
    return result;
}

const AssetRecord* AssetDatabase::find(const AssetId id) const noexcept {
    const auto iterator = std::lower_bound(records_.begin(), records_.end(), id,
        [](const AssetRecord& record, const AssetId value) { return record.id < value; });
    return iterator != records_.end() && iterator->id == id ? &*iterator : nullptr;
}

std::vector<AssetId> AssetDatabase::reverseDependencies(const AssetId id) const {
    std::vector<AssetId> result;
    for (const AssetRecord& record : records_) {
        if (std::ranges::find(record.dependencies, id) != record.dependencies.end()) result.push_back(record.id);
    }
    return result;
}

void AssetDatabase::validate(std::vector<AssetRecord>& records) {
    if (records.size() > kMaximumRecords) throw std::runtime_error("Asset database exceeds record limit");
    std::ranges::sort(records, {}, &AssetRecord::id);
    for (std::size_t index = 0; index < records.size(); ++index) {
        AssetRecord& record = records[index];
        if (!record.id.valid()) throw std::runtime_error("Asset database contains an invalid zero ID");
        if (index > 0U && records[index - 1U].id == record.id) throw std::runtime_error("Asset database contains a duplicate ID");
        if (record.artifactSchemaVersion == 0U) throw std::runtime_error("Asset record artifact schema version is invalid");
        if (record.sourcePath.is_absolute()) throw std::runtime_error("Asset source paths must be project-relative");
        if (record.importerId.empty() && record.type != AssetType::GeneratedMesh) throw std::runtime_error("Asset record importer ID is missing");
        if (record.dependencies.size() > kMaximumDependencies) throw std::runtime_error("Asset record exceeds dependency limit");
        std::ranges::sort(record.dependencies);
        if (std::ranges::adjacent_find(record.dependencies) != record.dependencies.end()) throw std::runtime_error("Asset record contains duplicate dependencies");
    }
    for (const AssetRecord& record : records) {
        for (const AssetId dependency : record.dependencies) {
            if (dependency == record.id) throw std::runtime_error("Asset dependency cycle contains a self-reference");
            const auto found = std::lower_bound(records.begin(), records.end(), dependency,
                [](const AssetRecord& candidate, const AssetId value) { return candidate.id < value; });
            if (found == records.end() || found->id != dependency) throw std::runtime_error("Asset record references a missing dependency");
        }
    }
    std::vector<std::uint8_t> colors(records.size(), 0U);
    std::function<void(std::size_t)> visit = [&](const std::size_t index) {
        if (colors[index] == 1U) throw std::runtime_error("Asset dependency graph contains a cycle");
        if (colors[index] == 2U) return;
        colors[index] = 1U;
        for (const AssetId dependency : records[index].dependencies) {
            const auto found = std::lower_bound(records.begin(), records.end(), dependency,
                [](const AssetRecord& candidate, const AssetId value) { return candidate.id < value; });
            visit(static_cast<std::size_t>(found - records.begin()));
        }
        colors[index] = 2U;
    };
    for (std::size_t index = 0; index < records.size(); ++index) visit(index);
}

void AssetDatabase::replaceAll(std::vector<AssetRecord> records) {
    validate(records);
    records_ = std::move(records);
    ++generation_;
}

void AssetDatabase::upsert(AssetRecord record) {
    std::vector<AssetRecord> replacement = records_;
    const auto iterator = std::lower_bound(replacement.begin(), replacement.end(), record.id,
        [](const AssetRecord& candidate, const AssetId id) { return candidate.id < id; });
    if (iterator != replacement.end() && iterator->id == record.id) *iterator = std::move(record);
    else replacement.insert(iterator, std::move(record));
    validate(replacement);
    records_ = std::move(replacement);
    ++generation_;
}

void AssetDatabase::markChanged(const AssetId id, std::string reason) {
    if (find(id) == nullptr) throw std::runtime_error("Cannot invalidate an unknown asset ID");
    std::vector<AssetRecord> replacement = records_;
    std::vector<AssetId> pending{id};
    std::size_t cursor = 0;
    while (cursor < pending.size()) {
        const AssetId staleId = pending[cursor++];
        auto iterator = std::lower_bound(replacement.begin(), replacement.end(), staleId,
            [](const AssetRecord& record, const AssetId value) { return record.id < value; });
        if (iterator->state != AssetState::Stale) {
            iterator->state = AssetState::Stale;
            iterator->diagnostic = reason;
        }
        for (const AssetRecord& candidate : replacement) {
            if (std::ranges::find(candidate.dependencies, staleId) != candidate.dependencies.end() &&
                std::ranges::find(pending, candidate.id) == pending.end()) {
                pending.push_back(candidate.id);
            }
        }
    }
    records_ = std::move(replacement);
    ++generation_;
}

std::vector<std::byte> AssetDatabase::serialize() const {
    Writer writer;
    writer.raw(kMagic);
    writer.integer(kSchemaVersion);
    writer.integer(generation_);
    writer.integer(static_cast<std::uint32_t>(records_.size()));
    for (const AssetRecord& record : records_) {
        writeId(writer, record.id);
        writer.integer(static_cast<std::uint8_t>(record.type));
        writer.integer(record.artifactSchemaVersion);
        writer.string(pathString(record.sourcePath));
        writeHash(writer, record.sourceHash);
        writer.string(record.importerId);
        writer.integer(record.importerVersion);
        writer.string(record.normalizedSettings);
        writeHash(writer, record.settingsHash);
        writer.integer(static_cast<std::uint32_t>(record.dependencies.size()));
        for (const AssetId dependency : record.dependencies) writeId(writer, dependency);
        writeHash(writer, record.artifactKey);
        writer.string(pathString(record.artifactPath));
        writer.string(record.target);
        writer.integer(static_cast<std::uint8_t>(record.state));
        writer.string(record.diagnostic);
    }
    return std::move(writer).take();
}

AssetDatabase AssetDatabase::deserialize(const std::span<const std::byte> bytes) {
    Reader reader{bytes};
    if (!std::ranges::equal(reader.raw(kMagic.size()), kMagic)) throw std::runtime_error("Asset database magic is invalid");
    const std::uint32_t schema = reader.integer<std::uint32_t>();
    if (schema != kSchemaVersion) throw std::runtime_error("Asset database schema version is incompatible");
    AssetDatabase database;
    database.generation_ = reader.integer<std::uint64_t>();
    const std::uint32_t count = reader.integer<std::uint32_t>();
    if (count > kMaximumRecords) throw std::runtime_error("Asset database exceeds record limit");
    std::vector<AssetRecord> records;
    records.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        AssetRecord record;
        record.id = readId(reader);
        record.type = static_cast<AssetType>(reader.integer<std::uint8_t>());
        record.artifactSchemaVersion = reader.integer<std::uint32_t>();
        record.sourcePath = reader.string();
        record.sourceHash = readHash(reader);
        record.importerId = reader.string();
        record.importerVersion = reader.integer<std::uint32_t>();
        record.normalizedSettings = reader.string();
        record.settingsHash = readHash(reader);
        const std::uint32_t dependencyCount = reader.integer<std::uint32_t>();
        if (dependencyCount > kMaximumDependencies) throw std::runtime_error("Asset record exceeds dependency limit");
        record.dependencies.reserve(dependencyCount);
        for (std::uint32_t dependency = 0; dependency < dependencyCount; ++dependency) record.dependencies.push_back(readId(reader));
        record.artifactKey = readHash(reader);
        record.artifactPath = reader.string();
        record.target = reader.string();
        record.state = static_cast<AssetState>(reader.integer<std::uint8_t>());
        record.diagnostic = reader.string();
        records.push_back(std::move(record));
    }
    if (!reader.done()) throw std::runtime_error("Asset database has trailing data");
    validate(records);
    database.records_ = std::move(records);
    return database;
}

void AssetDatabase::saveAtomic(const std::filesystem::path& path) const { writeBinaryFileAtomic(path, serialize()); }
AssetDatabase AssetDatabase::load(const std::filesystem::path& path) { return deserialize(readBinaryFile(path)); }

} // namespace ve
