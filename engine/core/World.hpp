#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ve {

class World final {
public:
    using Index = std::uint32_t;
    static constexpr Index kInvalidIndex = std::numeric_limits<Index>::max();

    struct Entity {
        Index index = kInvalidIndex;
        std::uint32_t generation = 0;

        [[nodiscard]] bool valid() const noexcept {
            return index != kInvalidIndex && generation != 0U;
        }
        friend bool operator==(const Entity&, const Entity&) = default;
    };

    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    ~World() = default;

    [[nodiscard]] Entity createEntity() {
        if (!freeIndices_.empty()) {
            const Index index = freeIndices_.back();
            freeIndices_.pop_back();
            Slot& slot = slots_[index];
            slot.alive = true;
            ++aliveCount_;
            return Entity{index, slot.generation};
        }

        if (slots_.size() >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::runtime_error("World entity index range exhausted");
        }
        slots_.push_back(Slot{});
        const Index index = static_cast<Index>(slots_.size() - 1U);
        slots_.back().alive = true;
        ++aliveCount_;
        return Entity{index, slots_.back().generation};
    }

    [[nodiscard]] bool destroyEntity(const Entity entity) {
        if (!alive(entity)) {
            return false;
        }

        for (auto& [unusedType, pool] : pools_) {
            (void)unusedType;
            pool->remove(entity.index);
        }

        Slot& slot = slots_[entity.index];
        slot.alive = false;
        --aliveCount_;
        if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++slot.generation;
            freeIndices_.push_back(entity.index);
        }
        return true;
    }

    void clear() {
        freeIndices_.reserve(slots_.size());
        pools_.clear();
        aliveCount_ = 0;
        freeIndices_.clear();
        for (Index index = 0; index < static_cast<Index>(slots_.size()); ++index) {
            Slot& slot = slots_[index];
            slot.alive = false;
            if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
                ++slot.generation;
                freeIndices_.push_back(index);
            }
        }
    }

    [[nodiscard]] bool alive(const Entity entity) const noexcept {
        return entity.valid() && entity.index < slots_.size() && slots_[entity.index].alive &&
               slots_[entity.index].generation == entity.generation;
    }

    [[nodiscard]] std::size_t entityCount() const noexcept {
        return aliveCount_;
    }

    template <typename T, typename... Args>
    T& emplace(const Entity entity, Args&&... args) {
        static_assert(!std::is_const_v<T> && !std::is_reference_v<T>, "World components must be object types");
        ensureAlive(entity);
        ComponentPool<T>& pool = getOrCreatePool<T>();
        if (pool.contains(entity.index)) {
            throw std::logic_error("World entity already contains this component type");
        }
        return pool.emplace(entity, std::forward<Args>(args)...);
    }

    template <typename T>
    [[nodiscard]] T* tryGet(const Entity entity) noexcept {
        static_assert(!std::is_const_v<T> && !std::is_reference_v<T>, "World components must be object types");
        if (!alive(entity)) {
            return nullptr;
        }
        ComponentPool<T>* pool = findPool<T>();
        return pool == nullptr ? nullptr : pool->tryGet(entity.index);
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(const Entity entity) const noexcept {
        static_assert(!std::is_const_v<T> && !std::is_reference_v<T>, "World components must be object types");
        if (!alive(entity)) {
            return nullptr;
        }
        const ComponentPool<T>* pool = findPool<T>();
        return pool == nullptr ? nullptr : pool->tryGet(entity.index);
    }

    template <typename T>
    [[nodiscard]] bool contains(const Entity entity) const noexcept {
        return tryGet<T>(entity) != nullptr;
    }

    template <typename T>
    bool remove(const Entity entity) {
        if (!alive(entity)) {
            return false;
        }
        ComponentPool<T>* pool = findPool<T>();
        return pool != nullptr && pool->remove(entity.index);
    }

    template <typename T>
    [[nodiscard]] std::size_t componentCount() const noexcept {
        const ComponentPool<T>* pool = findPool<T>();
        return pool == nullptr ? 0U : pool->size();
    }

    template <typename T, typename Function>
    void each(Function&& function) {
        ComponentPool<T>* pool = findPool<T>();
        if (pool != nullptr) {
            pool->each(std::forward<Function>(function));
        }
    }

    template <typename T, typename Function>
    void each(Function&& function) const {
        const ComponentPool<T>* pool = findPool<T>();
        if (pool != nullptr) {
            pool->each(std::forward<Function>(function));
        }
    }

private:
    static constexpr Index kInvalidDenseIndex = kInvalidIndex;

    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
    };

    struct IComponentPool {
        virtual ~IComponentPool() = default;
        virtual bool remove(Index entityIndex) noexcept = 0;
    };

    template <typename T>
    class ComponentPool final : public IComponentPool {
    public:
        static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                      "World components must be nothrow move constructible and assignable");

        template <typename... Args>
        T& emplace(const Entity entity, Args&&... args) {
            ensureSparseCapacity(entity.index);
            const Index denseIndex = static_cast<Index>(components_.size());
            components_.emplace_back(std::forward<Args>(args)...);
            try {
                entities_.push_back(entity);
            } catch (...) {
                components_.pop_back();
                throw;
            }
            sparse_[entity.index] = denseIndex;
            return components_.back();
        }

        [[nodiscard]] bool contains(const Index entityIndex) const noexcept {
            return entityIndex < sparse_.size() && sparse_[entityIndex] != kInvalidDenseIndex;
        }

        [[nodiscard]] T* tryGet(const Index entityIndex) noexcept {
            return contains(entityIndex) ? &components_[sparse_[entityIndex]] : nullptr;
        }

        [[nodiscard]] const T* tryGet(const Index entityIndex) const noexcept {
            return contains(entityIndex) ? &components_[sparse_[entityIndex]] : nullptr;
        }

        bool remove(const Index entityIndex) noexcept override {
            if (!contains(entityIndex)) {
                return false;
            }
            const Index denseIndex = sparse_[entityIndex];
            const Index lastIndex = static_cast<Index>(components_.size() - 1U);
            if (denseIndex != lastIndex) {
                components_[denseIndex] = std::move(components_[lastIndex]);
                entities_[denseIndex] = entities_[lastIndex];
                sparse_[entities_[denseIndex].index] = denseIndex;
            }
            components_.pop_back();
            entities_.pop_back();
            sparse_[entityIndex] = kInvalidDenseIndex;
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return components_.size();
        }

        template <typename Function>
        void each(Function&& function) {
            for (Index denseIndex = 0; denseIndex < static_cast<Index>(components_.size()); ++denseIndex) {
                function(entities_[denseIndex], components_[denseIndex]);
            }
        }

        template <typename Function>
        void each(Function&& function) const {
            for (Index denseIndex = 0; denseIndex < static_cast<Index>(components_.size()); ++denseIndex) {
                function(entities_[denseIndex], components_[denseIndex]);
            }
        }

    private:
        void ensureSparseCapacity(const Index entityIndex) {
            if (entityIndex >= sparse_.size()) {
                sparse_.resize(static_cast<std::size_t>(entityIndex) + 1U, kInvalidDenseIndex);
            }
        }

        std::vector<Index> sparse_;
        std::vector<Entity> entities_;
        std::vector<T> components_;
    };

    template <typename T>
    ComponentPool<T>& getOrCreatePool() {
        const std::type_index type = typeid(T);
        if (const auto existing = pools_.find(type); existing != pools_.end()) {
            return *static_cast<ComponentPool<T>*>(existing->second.get());
        }
        auto newPool = std::make_unique<ComponentPool<T>>();
        auto [it, inserted] = pools_.try_emplace(type, std::move(newPool));
        (void)inserted;
        return *static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <typename T>
    [[nodiscard]] ComponentPool<T>* findPool() noexcept {
        const auto it = pools_.find(typeid(T));
        return it == pools_.end() ? nullptr : static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <typename T>
    [[nodiscard]] const ComponentPool<T>* findPool() const noexcept {
        const auto it = pools_.find(typeid(T));
        return it == pools_.end() ? nullptr : static_cast<const ComponentPool<T>*>(it->second.get());
    }

    void ensureAlive(const Entity entity) const {
        if (!alive(entity)) {
            throw std::invalid_argument("World entity is not alive");
        }
    }

    std::vector<Slot> slots_;
    std::vector<Index> freeIndices_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools_;
    std::size_t aliveCount_ = 0;
};

} // namespace ve
