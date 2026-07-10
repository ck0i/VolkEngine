#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>


namespace ve::detail {

template <typename... Components>
struct DistinctWorldComponentTypes;

template <>
struct DistinctWorldComponentTypes<> : std::true_type {};

template <typename First, typename... Rest>
struct DistinctWorldComponentTypes<First, Rest...>
    : std::bool_constant<((!std::is_same_v<First, Rest>) && ...) &&
                         DistinctWorldComponentTypes<Rest...>::value> {};

template <typename Component>
inline constexpr bool validWorldComponentType =
    std::is_object_v<Component> && !std::is_const_v<Component> && !std::is_volatile_v<Component> &&
    !std::is_reference_v<Component>;

template <typename... Components>
inline constexpr bool validWorldComponentTypes =
    (validWorldComponentType<Components> && ...) && DistinctWorldComponentTypes<Components...>::value;

} // namespace ve::detail

namespace ve {

class WorldCommandBuffer;

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
    World(World&& other) {
        other.ensureStructuralMutationAllowed();
        slots_ = std::move(other.slots_);
        freeIndices_ = std::move(other.freeIndices_);
        pools_ = std::move(other.pools_);
        aliveCount_ = other.aliveCount_;
        other.slots_.clear();
        other.freeIndices_.clear();
        other.pools_.clear();
        other.aliveCount_ = 0U;
        other.instanceToken_ = nextInstanceToken();
    }
    World& operator=(World&& other) {
        ensureStructuralMutationAllowed();
        other.ensureStructuralMutationAllowed();
        if (this != &other) {
            slots_ = std::move(other.slots_);
            freeIndices_ = std::move(other.freeIndices_);
            pools_ = std::move(other.pools_);
            aliveCount_ = other.aliveCount_;
            other.slots_.clear();
            other.freeIndices_.clear();
            other.pools_.clear();
            other.aliveCount_ = 0U;
            instanceToken_ = nextInstanceToken();
            other.instanceToken_ = nextInstanceToken();
        }
        return *this;
    }
    ~World() = default;

    [[nodiscard]] std::uint64_t instanceToken() const noexcept {
        return instanceToken_;
    }

    [[nodiscard]] Entity createEntity() {
        ensureStructuralMutationAllowed();
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
        ensureStructuralMutationAllowed();
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
        ensureStructuralMutationAllowed();
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
        instanceToken_ = nextInstanceToken();
    }

    [[nodiscard]] bool alive(const Entity entity) const noexcept {
        return entity.valid() && entity.index < slots_.size() && slots_[entity.index].alive &&
               slots_[entity.index].generation == entity.generation;
    }

    [[nodiscard]] std::size_t entityCount() const noexcept {
        return aliveCount_;
    }

    void reserveEntities(const std::size_t capacity) {
        ensureStructuralMutationAllowed();
        if (capacity > static_cast<std::size_t>(kInvalidIndex)) {
            throw std::invalid_argument("World entity capacity exceeds index range");
        }
        slots_.reserve(capacity);
        freeIndices_.reserve(capacity);
    }

    [[nodiscard]] std::size_t entityCapacity() const noexcept {
        return slots_.capacity();
    }

    template <typename T>
    void reserveComponents(const std::size_t capacity) {
        ensureStructuralMutationAllowed();
        static_assert(detail::validWorldComponentType<T>, "World components must be object types");
        getOrCreatePool<T>().reserve(capacity);
    }

    template <typename T>
    [[nodiscard]] std::size_t componentCapacity() const noexcept {
        static_assert(detail::validWorldComponentType<T>, "World components must be object types");
        const ComponentPool<T>* pool = findPool<T>();
        return pool == nullptr ? 0U : pool->capacity();
    }

    template <typename T, typename... Args>
    T& emplace(const Entity entity, Args&&... args) {
        ensureStructuralMutationAllowed();
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
        ensureStructuralMutationAllowed();
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

    template <typename... Components, typename Function>
    void each(Function&& function) {
        static_assert(sizeof...(Components) > 0U, "World queries require at least one component type");
        static_assert(detail::validWorldComponentTypes<Components...>,
                      "World queries require distinct, non-const object component types");

        auto pools = std::tuple<ComponentPool<Components>*...>{findPool<Components>()...};
        bool available = true;
        std::apply([&](auto*... pool) { available = ((pool != nullptr) && ...); }, pools);
        if (!available) {
            return;
        }
        QueryScope queryScope(*this);

        std::array<std::size_t, sizeof...(Components)> poolSizes{};
        std::size_t sizeIndex = 0U;
        std::apply([&](auto*... pool) { ((poolSizes[sizeIndex++] = pool->size()), ...); }, pools);

        std::size_t smallestPoolIndex = 0U;
        for (std::size_t index = 1U; index < poolSizes.size(); ++index) {
            if (poolSizes[index] < poolSizes[smallestPoolIndex]) {
                smallestPoolIndex = index;
            }
        }
        eachFromSmallestPool(pools, smallestPoolIndex, function);
    }

    template <typename... Components, typename Function>
    void each(Function&& function) const {
        static_assert(sizeof...(Components) > 0U, "World queries require at least one component type");
        static_assert(detail::validWorldComponentTypes<Components...>,
                      "World queries require distinct, non-const object component types");

        auto pools = std::tuple<const ComponentPool<Components>*...>{findPool<Components>()...};
        bool available = true;
        std::apply([&](const auto*... pool) { available = ((pool != nullptr) && ...); }, pools);
        if (!available) {
            return;
        }
        QueryScope queryScope(*this);

        std::array<std::size_t, sizeof...(Components)> poolSizes{};
        std::size_t sizeIndex = 0U;
        std::apply([&](const auto*... pool) { ((poolSizes[sizeIndex++] = pool->size()), ...); }, pools);

        std::size_t smallestPoolIndex = 0U;
        for (std::size_t index = 1U; index < poolSizes.size(); ++index) {
            if (poolSizes[index] < poolSizes[smallestPoolIndex]) {
                smallestPoolIndex = index;
            }
        }
        eachFromSmallestPool(pools, smallestPoolIndex, function);
    }

private:
    friend class WorldCommandBuffer;
    [[nodiscard]] static std::uint64_t nextInstanceToken() noexcept {
        std::uint64_t token = nextInstanceToken_.fetch_add(1U, std::memory_order_relaxed);
        while (token == 0U) {
            token = nextInstanceToken_.fetch_add(1U, std::memory_order_relaxed);
        }
        return token;
    }

    static constexpr Index kInvalidDenseIndex = kInvalidIndex;

    class QueryScope final {
    public:
        explicit QueryScope(const World& world) noexcept : world_(world) {
            ++world_.activeQueryCount_;
        }
        QueryScope(const QueryScope&) = delete;
        QueryScope& operator=(const QueryScope&) = delete;
        ~QueryScope() {
            --world_.activeQueryCount_;
        }

    private:
        const World& world_;
    };

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

        void reserve(const std::size_t capacity) {
            components_.reserve(capacity);
            entities_.reserve(capacity);
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return components_.capacity();
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

    template <std::size_t AnchorIndex, typename Function, typename... Components>
    static void eachFromPool(std::tuple<ComponentPool<Components>*...>& pools, Function& function) {
        std::get<AnchorIndex>(pools)->each([&](const Entity entity, auto&) {
            const auto matched = [&]<std::size_t... PoolIndices>(std::index_sequence<PoolIndices...>) {
                return std::tuple<Components*...>{std::get<PoolIndices>(pools)->tryGet(entity.index)...};
            }(std::make_index_sequence<sizeof...(Components)>{});
            std::apply(
                [&](auto*... component) {
                    if (((component != nullptr) && ...)) {
                        function(entity, *component...);
                    }
                },
                matched);
        });
    }

    template <std::size_t AnchorIndex, typename Function, typename... Components>
    static void eachFromPool(const std::tuple<const ComponentPool<Components>*...>& pools, Function& function) {
        std::get<AnchorIndex>(pools)->each([&](const Entity entity, const auto&) {
            const auto matched = [&]<std::size_t... PoolIndices>(std::index_sequence<PoolIndices...>) {
                return std::tuple<const Components*...>{std::get<PoolIndices>(pools)->tryGet(entity.index)...};
            }(std::make_index_sequence<sizeof...(Components)>{});
            std::apply(
                [&](const auto*... component) {
                    if (((component != nullptr) && ...)) {
                        function(entity, *component...);
                    }
                },
                matched);
        });
    }

    template <std::size_t Candidate = 0U, typename Function, typename... Components>
    static void eachFromSmallestPool(std::tuple<ComponentPool<Components>*...>& pools,
                                     const std::size_t smallestPoolIndex, Function& function) {
        if constexpr (Candidate < sizeof...(Components)) {
            if (Candidate == smallestPoolIndex) {
                eachFromPool<Candidate>(pools, function);
            } else {
                eachFromSmallestPool<Candidate + 1U>(pools, smallestPoolIndex, function);
            }
        }
    }

    template <std::size_t Candidate = 0U, typename Function, typename... Components>
    static void eachFromSmallestPool(const std::tuple<const ComponentPool<Components>*...>& pools,
                                     const std::size_t smallestPoolIndex, Function& function) {
        if constexpr (Candidate < sizeof...(Components)) {
            if (Candidate == smallestPoolIndex) {
                eachFromPool<Candidate>(pools, function);
            } else {
                eachFromSmallestPool<Candidate + 1U>(pools, smallestPoolIndex, function);
            }
        }
    }

    template <typename T>
    [[nodiscard]] const ComponentPool<T>* findPool() const noexcept {
        const auto it = pools_.find(typeid(T));
        return it == pools_.end() ? nullptr : static_cast<const ComponentPool<T>*>(it->second.get());
    }

    void ensureStructuralMutationAllowed() const {
        if (activeQueryCount_ != 0U) {
            throw std::logic_error("World structural mutation is forbidden during a query");
        }
    }

    void ensureAlive(const Entity entity) const {
        if (!alive(entity)) {
            throw std::invalid_argument("World entity is not alive");
        }
    }

    inline static std::atomic<std::uint64_t> nextInstanceToken_{1U};
    std::uint64_t instanceToken_ = nextInstanceToken();
    std::vector<Slot> slots_;
    std::vector<Index> freeIndices_;
    mutable std::size_t activeQueryCount_ = 0U;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools_;
    std::size_t aliveCount_ = 0;
};

class WorldCommandBuffer final {
public:
    struct PlaybackResult {
        std::size_t applied = 0U;
        std::size_t rejected = 0U;
    };

    WorldCommandBuffer() = default;
    WorldCommandBuffer(const WorldCommandBuffer&) = delete;
    WorldCommandBuffer& operator=(const WorldCommandBuffer&) = delete;
    WorldCommandBuffer(WorldCommandBuffer&& other) {
        if (other.playbackActive_) {
            throw std::logic_error("Cannot move a World command buffer during playback");
        }
        pending_ = std::move(other.pending_);
        executing_ = std::move(other.executing_);
        nextCommand_ = std::exchange(other.nextCommand_, 0U);
    }
    WorldCommandBuffer& operator=(WorldCommandBuffer&& other) {
        if (this != &other) {
            if (playbackActive_ || other.playbackActive_) {
                throw std::logic_error("Cannot move a World command buffer during playback");
            }
            pending_ = std::move(other.pending_);
            executing_ = std::move(other.executing_);
            nextCommand_ = std::exchange(other.nextCommand_, 0U);
        }
        return *this;
    }
    ~WorldCommandBuffer() = default;

    void destroy(const World::Entity entity) {
        pending_.emplace_back([entity](World& world) {
            return world.alive(entity) && world.destroyEntity(entity);
        });
    }

    template <typename T>
    void remove(const World::Entity entity) {
        static_assert(detail::validWorldComponentType<T>, "World components must be object types");
        pending_.emplace_back([entity](World& world) {
            return world.alive(entity) && world.remove<T>(entity);
        });
    }

    template <typename T>
    void emplace(const World::Entity entity, T ownedValue) {
        static_assert(detail::validWorldComponentType<T>, "World components must be object types");
        static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                      "World components must be nothrow move constructible and assignable");
        pending_.emplace_back([entity, value = std::move(ownedValue)](World& world) mutable {
            if (!world.alive(entity) || world.contains<T>(entity)) {
                return false;
            }
            world.emplace<T>(entity, std::move(value));
            return true;
        });
    }

    [[nodiscard]] PlaybackResult playback(World& world) {
        world.ensureStructuralMutationAllowed();
        if (playbackActive_) {
            throw std::logic_error("Recursive World command buffer playback is forbidden");
        }
        playbackActive_ = true;
        try {
            if (executing_.empty()) {
                executing_.swap(pending_);
                nextCommand_ = 0U;
            }

            PlaybackResult result;
            while (nextCommand_ < executing_.size()) {
                std::move_only_function<bool(World&)>& command = executing_[nextCommand_++];
                if (command(world)) {
                    ++result.applied;
                } else {
                    ++result.rejected;
                }
            }
            executing_.clear();
            nextCommand_ = 0U;
            playbackActive_ = false;
            return result;
        } catch (...) {
            if (nextCommand_ == executing_.size()) {
                executing_.clear();
                nextCommand_ = 0U;
            }
            playbackActive_ = false;
            throw;
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0U;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return pending_.size() + executing_.size() - nextCommand_;
    }

private:
    std::vector<std::move_only_function<bool(World&)>> pending_;
    std::vector<std::move_only_function<bool(World&)>> executing_;
    bool playbackActive_ = false;
    std::size_t nextCommand_ = 0U;
};

} // namespace ve
