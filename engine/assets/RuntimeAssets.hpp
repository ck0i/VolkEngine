#pragma once

#include "assets/AssetDatabase.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ve {

enum class RuntimeAssetState : std::uint8_t { Unloaded, Loading, Ready, Failed, Retiring };

template <typename Tag>
struct RuntimeAssetHandle {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max() && generation != 0U;
    }
    friend bool operator==(const RuntimeAssetHandle&, const RuntimeAssetHandle&) = default;
    friend auto operator<=>(const RuntimeAssetHandle&, const RuntimeAssetHandle&) = default;
    friend std::ostream& operator<<(std::ostream& output, const RuntimeAssetHandle handle) {
        return output << handle.index << ':' << handle.generation;
    }
};

struct MeshAssetTag;
struct TextureAssetTag;
struct MaterialAssetTag;
using MeshAssetHandle = RuntimeAssetHandle<MeshAssetTag>;
using TextureAssetHandle = RuntimeAssetHandle<TextureAssetTag>;
using MaterialAssetHandle = RuntimeAssetHandle<MaterialAssetTag>;

namespace builtin_assets {
inline constexpr MeshAssetHandle kCube{0U, 1U};
inline constexpr MeshAssetHandle kSphere{1U, 1U};
inline constexpr MeshAssetHandle kGroundPlane{2U, 1U};
inline constexpr MeshAssetHandle kReferenceMesh{3U, 1U};
inline constexpr AssetId kReferenceSceneId{0x7f8e9daabbccddeeULL, 0x1021324354657687ULL};
} // namespace builtin_assets

template <typename T, typename Tag>
class RuntimeAssetRegistry {
public:
    using Handle = RuntimeAssetHandle<Tag>;

    [[nodiscard]] Handle request(const AssetId id) {
        if (!id.valid()) throw std::invalid_argument("Runtime asset request has an invalid AssetId");
        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            Slot& slot = slots_[index];
            if (slot.id == id && slot.state != RuntimeAssetState::Unloaded) return {index, slot.generation};
        }
        std::uint32_t index;
        if (free_.empty()) {
            if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Runtime asset slot range exhausted");
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back({});
        } else {
            index = free_.back();
            free_.pop_back();
        }
        Slot& slot = slots_[index];
        slot.id = id;
        slot.state = RuntimeAssetState::Loading;
        slot.diagnostic.clear();
        return {index, slot.generation};
    }

    void publish(const Handle handle, T value) {
        Slot& slot = checked(handle);
        if (slot.state != RuntimeAssetState::Loading && slot.state != RuntimeAssetState::Ready &&
            slot.state != RuntimeAssetState::Failed) {
            throw std::runtime_error("Runtime asset publish requires loading, failed, or ready state");
        }
        slot.value.emplace(std::move(value));
        slot.state = RuntimeAssetState::Ready;
        slot.diagnostic.clear();
    }

    void fail(const Handle handle, std::string diagnostic) {
        Slot& slot = checked(handle);
        if (diagnostic.empty()) throw std::invalid_argument("Runtime asset failure diagnostic must not be empty");
        if (!slot.value.has_value()) slot.state = RuntimeAssetState::Failed;
        slot.diagnostic = std::move(diagnostic);
    }

    void retry(const Handle handle) {
        Slot& slot = checked(handle);
        if (slot.state != RuntimeAssetState::Failed) throw std::runtime_error("Only failed runtime assets can retry");
        slot.state = RuntimeAssetState::Loading;
        slot.diagnostic.clear();
    }

    void retire(const Handle handle) {
        Slot& slot = checked(handle);
        slot.state = RuntimeAssetState::Retiring;
    }

    void release(const Handle handle) {
        Slot& slot = checked(handle);
        slot.value.reset();
        slot.id = {};
        slot.state = RuntimeAssetState::Unloaded;
        slot.diagnostic.clear();
        ++slot.generation;
        if (slot.generation == 0U) ++slot.generation;
        free_.push_back(handle.index);
    }

    [[nodiscard]] T* resolve(const Handle handle) noexcept {
        Slot* slot = findSlot(handle);
        return slot != nullptr && slot->state == RuntimeAssetState::Ready ? &*slot->value : nullptr;
    }
    [[nodiscard]] const T* resolve(const Handle handle) const noexcept {
        const Slot* slot = findSlot(handle);
        return slot != nullptr && slot->state == RuntimeAssetState::Ready ? &*slot->value : nullptr;
    }
    [[nodiscard]] RuntimeAssetState state(const Handle handle) const noexcept {
        const Slot* slot = findSlot(handle);
        return slot != nullptr ? slot->state : RuntimeAssetState::Unloaded;
    }
    [[nodiscard]] const std::string* diagnostic(const Handle handle) const noexcept {
        const Slot* slot = findSlot(handle);
        return slot != nullptr && !slot->diagnostic.empty() ? &slot->diagnostic : nullptr;
    }

private:
    struct Slot {
        AssetId id;
        std::uint32_t generation = 1U;
        RuntimeAssetState state = RuntimeAssetState::Unloaded;
        std::optional<T> value;
        std::string diagnostic;
    };
    [[nodiscard]] Slot* findSlot(const Handle handle) noexcept {
        return handle.valid() && handle.index < slots_.size() && slots_[handle.index].generation == handle.generation
            ? &slots_[handle.index] : nullptr;
    }
    [[nodiscard]] const Slot* findSlot(const Handle handle) const noexcept {
        return handle.valid() && handle.index < slots_.size() && slots_[handle.index].generation == handle.generation
            ? &slots_[handle.index] : nullptr;
    }
    Slot& checked(const Handle handle) {
        Slot* slot = findSlot(handle);
        if (slot == nullptr) throw std::runtime_error("Runtime asset handle is stale or invalid");
        return *slot;
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
};

} // namespace ve
