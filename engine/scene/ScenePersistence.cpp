#include "scene/ScenePersistence.hpp"

#include "core/FileSystem.hpp"
#include "renderer/SceneRenderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ve {
namespace {

constexpr std::uint32_t kVersion1 = 1U;
constexpr std::uint32_t kVersion2 = 2U;
constexpr std::uint8_t kTransformMask = 1U << 0U;
constexpr std::uint8_t kParentMask = 1U << 1U;
constexpr std::uint8_t kRenderableMask = 1U << 2U;
constexpr std::uint8_t kKnownMask = kTransformMask | kParentMask | kRenderableMask;
constexpr std::size_t kHeaderSize = 12U;
constexpr std::size_t kV2RecordPrefixSize = 16U + 2U;

[[nodiscard]] bool validMesh(const SceneMeshId mesh) noexcept {
    return static_cast<std::uint8_t>(mesh) <= static_cast<std::uint8_t>(SceneMeshId::ImportedModel);
}

[[nodiscard]] bool finite(const Vec4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] bool validRenderable(const WorldSceneRenderable &renderable) noexcept {
    return validMesh(renderable.mesh) && finite(renderable.material.albedoRoughness) &&
           finite(renderable.material.emissiveMetallic) && finite(renderable.material.flags) &&
           finite(renderable.localBounds.center) && std::isfinite(renderable.localBounds.radius) &&
           (!renderable.localBounds.valid || renderable.localBounds.radius >= 0.0F);
}

void validateLimits(const ScenePersistenceLimits &limits) {
    if (limits.maxEntities > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("Scene persistence entity limit exceeds format range");
    }
    if (limits.maxBytes < kHeaderSize) {
        throw std::invalid_argument("Scene persistence byte limit cannot contain a header");
    }
    if (limits.maxNameBytes > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        throw std::invalid_argument("Scene persistence name limit exceeds format range");
    }
}

[[noreturn]] void formatError(const char *message) {
    throw std::runtime_error(message);
}

class Writer final {
public:
    explicit Writer(const std::size_t maximumBytes) : maximumBytes_(maximumBytes) {}

    void reserve(const std::size_t bytes) {
        bytes_.reserve(std::min(bytes, maximumBytes_));
    }
    void u8(const std::uint8_t value) {
        ensure(1U);
        bytes_.push_back(static_cast<std::byte>(value));
    }
    void u16(const std::uint16_t value) {
        ensure(2U);
        for (std::uint32_t shift = 0U; shift < 16U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    void u32(const std::uint32_t value) {
        ensure(4U);
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    void u64(const std::uint64_t value) {
        ensure(8U);
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    void bytes(const std::string_view value) {
        ensure(value.size());
        for (const char character : value)
            bytes_.push_back(static_cast<std::byte>(character));
    }
    void f32(const float value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    [[nodiscard]] std::vector<std::byte> take() && {
        return std::move(bytes_);
    }

private:
    void ensure(const std::size_t count) const {
        if (count > maximumBytes_ - bytes_.size())
            formatError("Scene persistence output exceeds byte limit");
    }
    std::size_t maximumBytes_;
    std::vector<std::byte> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1U);
        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }
    [[nodiscard]] std::uint16_t u16() {
        require(2U);
        std::uint16_t value = 0U;
        for (std::uint32_t shift = 0U; shift < 16U; shift += 8U) {
            value |= static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes_[position_++])) << shift);
        }
        return value;
    }
    [[nodiscard]] std::uint32_t u32() {
        require(4U);
        std::uint32_t value = 0U;
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[position_++])) << shift;
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        require(8U);
        std::uint64_t value = 0U;
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[position_++])) << shift;
        return value;
    }
    [[nodiscard]] float f32() {
        return std::bit_cast<float>(u32());
    }
    [[nodiscard]] std::string string(const std::size_t count) {
        require(count);
        std::string value;
        value.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            value.push_back(static_cast<char>(std::to_integer<std::uint8_t>(bytes_[position_++])));
        }
        return value;
    }
    [[nodiscard]] bool exhausted() const noexcept {
        return position_ == bytes_.size();
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

private:
    void require(const std::size_t count) const {
        if (count > bytes_.size() - position_)
            formatError("Truncated scene persistence data");
    }
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0U;
};

struct Record {
    SceneEntityId id{};
    std::string name{};
    std::uint8_t mask = 0U;
    TransformTRS transform{};
    SceneEntityId parent{};
    WorldSceneRenderable renderable{};
};

struct LegacyRecord {
    std::uint8_t mask = 0U;
    TransformTRS transform{};
    std::uint32_t parent = 0U;
    WorldSceneRenderable renderable{};
};

void writeVec3(Writer &writer, const Vec3 value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
}
void writeVec4(Writer &writer, const Vec4 value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
    writer.f32(value.w);
}
[[nodiscard]] Vec3 readVec3(Reader &reader) {
    return {reader.f32(), reader.f32(), reader.f32()};
}
[[nodiscard]] Vec4 readVec4(Reader &reader) {
    return {reader.f32(), reader.f32(), reader.f32(), reader.f32()};
}

[[nodiscard]] std::size_t payloadSize(const std::uint8_t mask, const bool stableParent) {
    std::size_t size = 1U;
    if ((mask & kTransformMask) != 0U)
        size += 10U * sizeof(float);
    if ((mask & kParentMask) != 0U)
        size += stableParent ? 16U : 4U;
    if ((mask & kRenderableMask) != 0U)
        size += 1U + 12U * sizeof(float) + 3U * sizeof(float) + sizeof(float) + 1U + 1U;
    return size;
}

void writePayload(Writer &writer, const Record &record) {
    writer.u8(record.mask);
    if ((record.mask & kTransformMask) != 0U) {
        writeVec3(writer, record.transform.translation);
        writer.f32(record.transform.rotation.x);
        writer.f32(record.transform.rotation.y);
        writer.f32(record.transform.rotation.z);
        writer.f32(record.transform.rotation.w);
        writeVec3(writer, record.transform.scale);
    }
    if ((record.mask & kParentMask) != 0U) {
        writer.u64(record.parent.high);
        writer.u64(record.parent.low);
    }
    if ((record.mask & kRenderableMask) != 0U) {
        writer.u8(static_cast<std::uint8_t>(record.renderable.mesh));
        writeVec4(writer, record.renderable.material.albedoRoughness);
        writeVec4(writer, record.renderable.material.emissiveMetallic);
        writeVec4(writer, record.renderable.material.flags);
        writeVec3(writer, record.renderable.localBounds.center);
        writer.f32(record.renderable.localBounds.radius);
        writer.u8(record.renderable.localBounds.valid ? 1U : 0U);
        writer.u8(record.renderable.visible ? 1U : 0U);
    }
}

void readPayload(Reader &reader, Record &record, const bool stableParent) {
    record.mask = reader.u8();
    if ((record.mask & ~kKnownMask) != 0U)
        formatError("Unknown scene component mask");
    if ((record.mask & kTransformMask) != 0U) {
        record.transform.translation = readVec3(reader);
        record.transform.rotation = {reader.f32(), reader.f32(), reader.f32(), reader.f32()};
        record.transform.scale = readVec3(reader);
        if (!finite(record.transform))
            formatError("Non-finite scene transform");
    }
    if ((record.mask & kParentMask) != 0U) {
        record.parent = stableParent ? SceneEntityId{reader.u64(), reader.u64()} : SceneEntityId{0U, reader.u32()};
        if (stableParent && !record.parent.valid())
            formatError("Invalid scene parent identity");
    }
    if ((record.mask & kRenderableMask) != 0U) {
        const std::uint8_t mesh = reader.u8();
        if (mesh > static_cast<std::uint8_t>(SceneMeshId::ImportedModel))
            formatError("Invalid scene mesh identifier");
        record.renderable.mesh = static_cast<SceneMeshId>(mesh);
        record.renderable.material.albedoRoughness = readVec4(reader);
        record.renderable.material.emissiveMetallic = readVec4(reader);
        record.renderable.material.flags = readVec4(reader);
        record.renderable.localBounds.center = readVec3(reader);
        record.renderable.localBounds.radius = reader.f32();
        const std::uint8_t boundsValid = reader.u8();
        const std::uint8_t visible = reader.u8();
        if (boundsValid > 1U || visible > 1U)
            formatError("Invalid scene boolean");
        record.renderable.localBounds.valid = boundsValid != 0U;
        record.renderable.visible = visible != 0U;
        if (!validRenderable(record.renderable))
            formatError("Invalid scene renderable");
    }
}

[[nodiscard]] bool recordIdLess(const Record &record, const SceneEntityId id) noexcept {
    return record.id < id;
}
[[nodiscard]] const Record *findRecord(const std::vector<Record> &records, const SceneEntityId id) {
    const auto found = std::lower_bound(records.begin(), records.end(), id, recordIdLess);
    return found != records.end() && found->id == id ? &*found : nullptr;
}


void validateGraph(const std::vector<Record> &records, const bool requireReferencedIdentityOnly) {
    std::vector<std::uint8_t> state(records.size(), 0U);
    std::vector<std::uint32_t> incoming(records.size(), 0U);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const Record &record = records[index];
        if (!record.id.valid())
            formatError("Invalid scene entity identity");
        if (index != 0U && records[index - 1U].id == record.id)
            formatError("Duplicate scene entity identity");
        if ((record.mask & kParentMask) != 0U) {
            const Record *parent = findRecord(records, record.parent);
            if (parent == nullptr || parent == &record)
                formatError("Invalid scene parent reference");
            ++incoming[static_cast<std::size_t>(parent - records.data())];
        }
    }
    if (requireReferencedIdentityOnly) {
        for (std::size_t index = 0U; index < records.size(); ++index) {
            if (records[index].mask == 0U && incoming[index] == 0U) {
                formatError("Unreferenced identity-only scene record");
            }
        }
    }
    std::vector<std::size_t> path;
    path.reserve(records.size());
    for (std::size_t start = 0U; start < records.size(); ++start) {
        if (state[start] != 0U)
            continue;
        path.clear();
        for (std::size_t current = start;;) {
            if (state[current] == 1U)
                formatError("Cyclic scene parent graph");
            if (state[current] == 2U)
                break;
            state[current] = 1U;
            path.push_back(current);
            if ((records[current].mask & kParentMask) == 0U)
                break;
            const Record *parent = findRecord(records, records[current].parent);
            current = static_cast<std::size_t>(parent - records.data());
        }
        for (const std::size_t index : path)
            state[index] = 2U;
    }
}

void constructWorld(World &destination, const std::vector<Record> &records) {
    World temporary;
    temporary.reserveEntities(records.size());
    std::size_t transforms = 0U, parents = 0U, renderables = 0U;
    for (const Record &record : records) {
        transforms += (record.mask & kTransformMask) != 0U;
        parents += (record.mask & kParentMask) != 0U;
        renderables += (record.mask & kRenderableMask) != 0U;
    }
    temporary.reserveComponents<WorldSceneIdentity>(records.size());
    temporary.reserveComponents<WorldSceneTransform>(transforms);
    temporary.reserveComponents<WorldSceneParent>(parents);
    temporary.reserveComponents<WorldSceneRenderable>(renderables);
    std::vector<World::Entity> entities;
    entities.reserve(records.size());
    for (const Record &record : records) {
        const World::Entity entity = temporary.createEntity();
        entities.push_back(entity);
        temporary.emplace<WorldSceneIdentity>(entity, WorldSceneIdentity{record.id, record.name});
        if ((record.mask & kTransformMask) != 0U)
            temporary.emplace<WorldSceneTransform>(entity, WorldSceneTransform{record.transform, 0U});
        if ((record.mask & kRenderableMask) != 0U)
            temporary.emplace<WorldSceneRenderable>(entity, record.renderable);
    }
    for (std::size_t index = 0U; index < records.size(); ++index)
        if ((records[index].mask & kParentMask) != 0U) {
            const Record *parent = findRecord(records, records[index].parent);
            temporary.emplace<WorldSceneParent>(
                entities[index], WorldSceneParent{entities[static_cast<std::size_t>(parent - records.data())]});
        }
    destination = std::move(temporary);
}

} // namespace

std::vector<std::byte> encodeWorldScene(const World &world, const ScenePersistenceLimits &limits) {
    validateLimits(limits);
    std::vector<World::Entity> entities;
    std::size_t gatherCapacity = 0U;
    const auto addGatherCapacity = [&](const std::size_t count) {
        gatherCapacity = count > limits.maxEntities - gatherCapacity ? limits.maxEntities : gatherCapacity + count;
    };
    addGatherCapacity(world.componentCount<WorldSceneTransform>());
    addGatherCapacity(world.componentCount<WorldSceneParent>());
    addGatherCapacity(world.componentCount<WorldSceneRenderable>());
    addGatherCapacity(world.componentCount<WorldSceneIdentity>());
    entities.reserve(gatherCapacity);
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(gatherCapacity);
    const auto include = [&](const World::Entity entity) {
        const std::uint64_t key = (static_cast<std::uint64_t>(entity.index) << 32U) | entity.generation;
        if (seen.contains(key))
            return;
        if (entities.size() == limits.maxEntities) {
            formatError("Scene entity count exceeds limit");
        }
        seen.insert(key);
        entities.push_back(entity);
    };
    world.each<WorldSceneTransform>([&](const World::Entity entity, const WorldSceneTransform &) { include(entity); });
    world.each<WorldSceneParent>([&](const World::Entity entity, const WorldSceneParent &parent) {
        if (!world.alive(parent.parent))
            formatError("Dangling scene parent");
        include(entity);
        include(parent.parent);
    });
    world.each<WorldSceneRenderable>(
        [&](const World::Entity entity, const WorldSceneRenderable &) { include(entity); });
    world.each<WorldSceneIdentity>([&](const World::Entity entity, const WorldSceneIdentity &) { include(entity); });

    std::vector<Record> records;
    records.reserve(entities.size());
    std::size_t totalNames = 0U;
    for (const World::Entity entity : entities) {
        const WorldSceneIdentity *identity = world.tryGet<WorldSceneIdentity>(entity);
        if (identity == nullptr || !identity->id.valid())
            formatError("Scene entity lacks a valid identity");
        if (identity->name.size() > limits.maxNameBytes ||
            identity->name.size() > std::numeric_limits<std::uint16_t>::max() ||
            !validWorldSceneName(identity->name))
            formatError("Invalid scene entity name");
        if (identity->name.size() > limits.maxTotalNameBytes - totalNames)
            formatError("Scene persistence total name bytes exceeds limit");
        totalNames += identity->name.size();
        Record record{};
        record.id = identity->id;
        record.name = identity->name;
        if (const WorldSceneTransform *transform = world.tryGet<WorldSceneTransform>(entity); transform != nullptr) {
            if (!finite(transform->current))
                formatError("Non-finite scene transform");
            record.mask |= kTransformMask;
            record.transform = transform->current;
        }
        if (const WorldSceneParent *parent = world.tryGet<WorldSceneParent>(entity); parent != nullptr) {
            const WorldSceneIdentity *parentIdentity = world.tryGet<WorldSceneIdentity>(parent->parent);
            if (parentIdentity == nullptr || !parentIdentity->id.valid())
                formatError("Scene parent lacks a valid identity");
            record.mask |= kParentMask;
            record.parent = parentIdentity->id;
        }
        if (const WorldSceneRenderable *renderable = world.tryGet<WorldSceneRenderable>(entity);
            renderable != nullptr) {
            if (!validRenderable(*renderable))
                formatError("Invalid scene renderable");
            record.mask |= kRenderableMask;
            record.renderable = *renderable;
        }
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(),
              [](const Record &left, const Record &right) { return left.id < right.id; });
    validateGraph(records, false);
    std::size_t totalSize = kHeaderSize;
    for (const Record &record : records) {
        const std::size_t recordSize = kV2RecordPrefixSize + record.name.size() + payloadSize(record.mask, true);
        if (recordSize > limits.maxBytes - totalSize)
            formatError("Scene persistence output exceeds byte limit");
        totalSize += recordSize;
    }
    Writer writer(limits.maxBytes);
    writer.reserve(totalSize);
    writer.u8(static_cast<std::uint8_t>('V'));
    writer.u8(static_cast<std::uint8_t>('E'));
    writer.u8(static_cast<std::uint8_t>('S'));
    writer.u8(static_cast<std::uint8_t>('N'));
    writer.u32(kVersion2);
    writer.u32(static_cast<std::uint32_t>(records.size()));
    for (const Record &record : records) {
        writer.u64(record.id.high);
        writer.u64(record.id.low);
        writer.u16(static_cast<std::uint16_t>(record.name.size()));
        writer.bytes(record.name);
        writePayload(writer, record);
    }
    return std::move(writer).take();
}

void decodeWorldScene(World &destination, const std::span<const std::byte> bytes,
                      const ScenePersistenceLimits &limits) {
    validateLimits(limits);
    if (bytes.size() > limits.maxBytes)
        formatError("Scene persistence input exceeds byte limit");
    Reader reader(bytes);
    if (reader.u8() != static_cast<std::uint8_t>('V') || reader.u8() != static_cast<std::uint8_t>('E') ||
        reader.u8() != static_cast<std::uint8_t>('S') || reader.u8() != static_cast<std::uint8_t>('N'))
        formatError("Invalid scene persistence magic");
    const std::uint32_t version = reader.u32();
    if (version != kVersion1 && version != kVersion2)
        formatError("Unsupported scene persistence version");
    const std::uint32_t count = reader.u32();
    if (count > limits.maxEntities)
        formatError("Scene entity count exceeds limit");
    if (version == kVersion1 && count > reader.remaining()) {
        formatError("Scene entity count exceeds available record data");
    }
    if (version == kVersion2 && count > reader.remaining() / (kV2RecordPrefixSize + 1U)) {
        formatError("Scene entity count exceeds available record data");
    }
    std::vector<Record> records;
    records.reserve(count);
    std::size_t totalNames = 0U;
    for (std::uint32_t index = 0U; index < count; ++index) {
        Record record{};
        if (version == kVersion2) {
            record.id = {reader.u64(), reader.u64()};
            if (!record.id.valid())
                formatError("Invalid scene entity identity");
            const std::size_t nameSize = reader.u16();
            if (nameSize > limits.maxNameBytes || nameSize > limits.maxTotalNameBytes - totalNames)
                formatError("Scene persistence name bytes exceeds limit");
            record.name = reader.string(nameSize);
            if (!validWorldSceneName(record.name))
                formatError("Invalid scene entity name");
            totalNames += nameSize;
            readPayload(reader, record, true);
        } else {
            record.id = {0U, static_cast<std::uint64_t>(index) + 1U};
            readPayload(reader, record, false);
        }
        records.push_back(std::move(record));
    }
    if (!reader.exhausted())
        formatError("Trailing scene persistence data");
    if (version == kVersion1) {
        for (Record &record : records)
            if ((record.mask & kParentMask) != 0U) {
                const std::size_t parentIndex = static_cast<std::size_t>(record.parent.low);
                if (record.parent.high != 0U || parentIndex >= records.size() ||
                    parentIndex == static_cast<std::size_t>(&record - records.data())) {
                    formatError("Invalid scene parent reference");
                }
                record.parent = records[static_cast<std::size_t>(record.parent.low)].id;
            }
    }
    std::sort(records.begin(), records.end(),
              [](const Record &left, const Record &right) { return left.id < right.id; });
    validateGraph(records, version == kVersion1);
    constructWorld(destination, records);
}

void saveWorldScene(const World &world, const std::filesystem::path &path, const ScenePersistenceLimits &limits) {
    writeBinaryFileAtomic(path, encodeWorldScene(world, limits));
}
void loadWorldScene(World &destination, const std::filesystem::path &path, const ScenePersistenceLimits &limits) {
    decodeWorldScene(destination, readBinaryFile(path, limits.maxBytes), limits);
}

} // namespace ve
