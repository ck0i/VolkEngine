#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <stdexcept>
#include <vector>

namespace ve {

enum class FrameGraphResourceKind : std::uint8_t {
    Image,
    Buffer
};

enum class FrameGraphAccess : std::uint8_t {
    Read,
    Write
};

enum class FrameGraphUsage : std::uint8_t {
    ColorAttachment,
    DepthAttachment,
    SampledImage,
    TransferSource,
    Present,
    UniformBuffer,
    StorageBuffer,
    IndirectBuffer,
    TransferDestination,
    HostRead
};

enum class FrameGraphAttachmentLoad : std::uint8_t {
    Load,
    Clear,
    Discard
};

enum class FrameGraphAttachmentStore : std::uint8_t {
    Store,
    Discard
};

class FrameGraph {
public:
    using Index = std::uint32_t;
    static constexpr Index kInvalidIndex = std::numeric_limits<Index>::max();

    struct ResourceHandle {
        Index index = kInvalidIndex;
        [[nodiscard]] bool valid() const { return index != kInvalidIndex; }
    };

    struct PassHandle {
        Index index = kInvalidIndex;
        [[nodiscard]] bool valid() const { return index != kInvalidIndex; }
    };

    struct ResourceDesc {
        const char* name = "Unnamed Resource";
        FrameGraphResourceKind kind = FrameGraphResourceKind::Image;
        bool imported = false;
        std::uint64_t transientBytes = 0;
        std::uint64_t transientAlignment = 1;
        std::uint64_t aliasClass = 0;
        bool hasFinalUsage = false;
        FrameGraphUsage finalUsage = FrameGraphUsage::Present;
    };

    struct PassDesc {
        const char* name = "Unnamed Pass";
        std::array<float, 4> debugColor{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct Edge {
        PassHandle pass{};
        ResourceHandle resource{};
        FrameGraphAccess access = FrameGraphAccess::Read;
        FrameGraphUsage usage = FrameGraphUsage::SampledImage;
        bool attachment = false;
        FrameGraphAttachmentLoad load = FrameGraphAttachmentLoad::Discard;
        FrameGraphAttachmentStore store = FrameGraphAttachmentStore::Discard;
    };

    class PassResources {
    public:
        explicit PassResources(const std::span<const Edge> edges) noexcept : edges_(edges) {}

        [[nodiscard]] const Edge& edge(const ResourceHandle resource) const {
            if (!resource.valid()) {
                throw std::runtime_error("FrameGraph pass requested an invalid resource handle");
            }
            for (const Edge& candidate : edges_) {
                if (candidate.resource.index == resource.index) {
                    return candidate;
                }
            }
            throw std::runtime_error("FrameGraph pass requested an undeclared resource");
        }

        [[nodiscard]] bool contains(const ResourceHandle resource) const noexcept {
            if (!resource.valid()) {
                return false;
            }
            return std::ranges::any_of(edges_, [resource](const Edge& candidate) {
                return candidate.resource.index == resource.index;
            });
        }

        [[nodiscard]] std::span<const Edge> edges() const noexcept { return edges_; }

    private:
        std::span<const Edge> edges_;
    };

    struct ResourceLifetime {
        PassHandle firstPass{};
        PassHandle lastPass{};
        bool used = false;
    };

    struct BarrierIntent {
        ResourceHandle resource{};
        PassHandle pass{};
        FrameGraphAccess access = FrameGraphAccess::Read;
        FrameGraphUsage usage = FrameGraphUsage::SampledImage;
        FrameGraphAccess previousAccess = FrameGraphAccess::Read;
        FrameGraphUsage previousUsage = FrameGraphUsage::SampledImage;
        bool hasPrevious = false;
        bool finalTransition = false;
    };

    struct TransientAllocation {
        Index slot = kInvalidIndex;
        std::uint64_t capacityBytes = 0;
        std::uint64_t alignment = 1;
        bool aliased = false;
    };

    struct TransientStats {
        std::uint64_t requestedBytes = 0;
        std::uint64_t allocatedBytes = 0;
        std::uint64_t aliasedBytes = 0;
        Index allocationCount = 0;
    };

    enum class ExecutionFailure : std::uint8_t {
        None,
        Create,
        Transition,
        Pass,
        Retire
    };

    struct ExecutionResult {
        ExecutionFailure failure = ExecutionFailure::None;
        ResourceHandle resource{};
        PassHandle pass{};
        Index passesExecuted = 0;
        Index barriersApplied = 0;
        Index resourcesCreated = 0;
        Index resourcesRetired = 0;

        [[nodiscard]] bool success() const noexcept { return failure == ExecutionFailure::None; }
    };

    struct ExecutionState {
        std::vector<std::uint8_t> activeResources;
        std::vector<Index> creationOrder;
    };

    struct ExecutionCallbacks {
        void* context = nullptr;
        bool (*createResource)(void*, ResourceHandle, const ResourceDesc&, const TransientAllocation&) noexcept = nullptr;
        bool (*transition)(void*, const BarrierIntent&) noexcept = nullptr;
        bool (*executePass)(void*, PassHandle, const PassDesc&,
                            const PassResources&) noexcept = nullptr;
        bool (*retireResource)(void*, ResourceHandle, const ResourceDesc&, const TransientAllocation&) noexcept = nullptr;
    };

    [[nodiscard]] ResourceHandle addResource(ResourceDesc desc) {
        if (resources_.size() >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::runtime_error("FrameGraph resource id range exhausted");
        }
        if (desc.transientAlignment == 0U) {
            throw std::runtime_error("FrameGraph resource alignment must be nonzero");
        }
        if ((desc.transientAlignment & (desc.transientAlignment - 1U)) != 0U) {
            throw std::runtime_error("FrameGraph resource alignment must be a power of two");
        }
        const ResourceHandle handle{static_cast<Index>(resources_.size())};
        resources_.push_back(desc);
        invalidate();
        return handle;
    }

    [[nodiscard]] PassHandle addPass(PassDesc desc) {
        if (passes_.size() >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::runtime_error("FrameGraph pass id range exhausted");
        }
        const PassHandle handle{static_cast<Index>(passes_.size())};
        passes_.push_back(desc);
        invalidate();
        return handle;
    }

    void read(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage) {
        addEdge(pass, resource, FrameGraphAccess::Read, usage, false,
                FrameGraphAttachmentLoad::Discard, FrameGraphAttachmentStore::Discard);
    }

    void write(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage) {
        addEdge(pass, resource, FrameGraphAccess::Write, usage, false,
                FrameGraphAttachmentLoad::Discard, FrameGraphAttachmentStore::Discard);
    }

    void readAttachment(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage,
                        FrameGraphAttachmentLoad load, FrameGraphAttachmentStore store) {
        addEdge(pass, resource, FrameGraphAccess::Read, usage, true, load, store);
    }

    void writeAttachment(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage,
                         FrameGraphAttachmentLoad load, FrameGraphAttachmentStore store) {
        addEdge(pass, resource, FrameGraphAccess::Write, usage, true, load, store);
    }

    void setFinalUsage(ResourceHandle resource, FrameGraphUsage usage) {
        validateResource(resource);
        validateUsageForResource(resources_[static_cast<std::size_t>(resource.index)], FrameGraphAccess::Read, usage);
        resources_[static_cast<std::size_t>(resource.index)].hasFinalUsage = true;
        resources_[static_cast<std::size_t>(resource.index)].finalUsage = usage;
        invalidate();
    }

    [[nodiscard]] bool hasEdge(PassHandle pass, ResourceHandle resource, FrameGraphAccess access, FrameGraphUsage usage) const {
        validatePass(pass);
        validateResource(resource);
        for (const Edge& edge : edges_) {
            if (edge.pass.index == pass.index &&
                edge.resource.index == resource.index &&
                edge.access == access &&
                edge.usage == usage) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool hasFinalUsage(ResourceHandle resource) const {
        validateResource(resource);
        return resources_[static_cast<std::size_t>(resource.index)].hasFinalUsage;
    }

    [[nodiscard]] FrameGraphUsage finalUsage(ResourceHandle resource) const {
        validateResource(resource);
        const ResourceDesc& desc = resources_[static_cast<std::size_t>(resource.index)];
        if (!desc.hasFinalUsage) {
            throw std::runtime_error("FrameGraph resource has no final usage");
        }
        return desc.finalUsage;
    }

    void compile() {
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
        barrierIntents_.clear();
        finalBarrierIntentIndices_.clear();
        transientAllocations_.clear();
        transientStats_ = {};
        createResourcesByPass_.clear();
        retireResourcesByPass_.clear();
        barrierIntentsByPass_.clear();
        finalBarrierIntentIndicesOrdered_.clear();
        retireResourcesAfterFinal_.clear();
        passEdges_.clear();
        std::vector<std::vector<const Edge*>> edgesByPass(passes_.size());
        std::vector<std::vector<const Edge*>> edgesByResource(resources_.size());
        for (const Edge& edge : edges_) {
            edgesByPass[edge.pass.index].push_back(&edge);
            edgesByResource[edge.resource.index].push_back(&edge);
        }
        passEdges_.assign(passes_.size(), {});
        for (const Edge& edge : edges_) {
            passEdges_[edge.pass.index].push_back(edge);
        }
        for (const ResourceDesc& resource : resources_) {
            if (!resource.imported && resource.transientBytes == 0U) {
                throw std::runtime_error("FrameGraph-owned resource requires a nonzero transient byte size");
            }
        }
        for (std::vector<const Edge*>& resourceEdges : edgesByResource) {
            std::stable_sort(resourceEdges.begin(), resourceEdges.end(), [](const Edge* lhs, const Edge* rhs) {
                return lhs->pass.index < rhs->pass.index;
            });
        }
        std::vector<std::uint8_t> hasWriter(resources_.size(), 0);
        std::vector<std::uint8_t> passHasAccess(passes_.size(), 0);

        for (std::size_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            for (const Edge* edge : edgesByPass[passIndex]) {
                const std::size_t resourceIndex = static_cast<std::size_t>(edge->resource.index);
                passHasAccess[passIndex] = 1;
                if (edge->access == FrameGraphAccess::Read && !resources_[resourceIndex].imported && hasWriter[resourceIndex] == 0U) {
                    throw std::runtime_error("FrameGraph pass reads a non-imported resource before any pass writes it");
                }
                if (edge->access == FrameGraphAccess::Write) {
                    hasWriter[resourceIndex] = 1;
                }
            }

            if (passHasAccess[passIndex] == 0U) {
                throw std::runtime_error("FrameGraph pass has no resource edges");
            }
        }

        const std::size_t passCount = passes_.size();
        std::vector<std::vector<Index>> dependencies(passCount);
        std::vector<Index> lastWriters(resources_.size(), kInvalidIndex);
        auto addDependency = [&](const Index from, const Index to) {
            if (from == kInvalidIndex || from == to) {
                return;
            }
            std::vector<Index>& outgoing = dependencies[from];
            if (std::find(outgoing.begin(), outgoing.end(), to) == outgoing.end()) {
                outgoing.push_back(to);
            }
        };

        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            std::vector<Index> readers;
            for (const Edge* edge : edgesByResource[resourceIndex]) {
                const Index pass = edge->pass.index;
                if (edge->access == FrameGraphAccess::Read) {
                    addDependency(lastWriters[resourceIndex], pass);
                    readers.push_back(pass);
                } else {
                    addDependency(lastWriters[resourceIndex], pass);
                    for (const Index reader : readers) {
                        addDependency(reader, pass);
                    }
                    readers.clear();
                    lastWriters[resourceIndex] = pass;
                }
            }
        }

        std::vector<Index> indegree(passCount, 0);
        for (const std::vector<Index>& outgoing : dependencies) {
            for (const Index target : outgoing) {
                ++indegree[target];
            }
        }
        std::priority_queue<Index, std::vector<Index>, std::greater<Index>> ready;
        for (Index pass = 0; pass < static_cast<Index>(passCount); ++pass) {
            if (indegree[pass] == 0U) {
                ready.push(pass);
            }
        }

        executionOrder_.clear();
        executionOrder_.reserve(passCount);
        while (!ready.empty()) {
            const Index pass = ready.top();
            ready.pop();
            executionOrder_.push_back(PassHandle{pass});
            for (const Index target : dependencies[pass]) {
                if (--indegree[target] == 0U) {
                    ready.push(target);
                }
            }
        }
        if (executionOrder_.size() != passCount) {
            executionOrder_.clear();
            throw std::runtime_error("FrameGraph contains a cyclic pass dependency");
        }

        std::vector<Index> passPositions(passCount, kInvalidIndex);
        for (Index position = 0; position < static_cast<Index>(executionOrder_.size()); ++position) {
            passPositions[executionOrder_[position].index] = position;
        }
        std::vector<Index> firstUses(resources_.size(), kInvalidIndex);
        std::vector<Index> lastUses(resources_.size(), kInvalidIndex);
        for (const Edge& edge : edges_) {
            const std::size_t resourceIndex = static_cast<std::size_t>(edge.resource.index);
            const Index position = passPositions[edge.pass.index];
            if (firstUses[resourceIndex] == kInvalidIndex || position < firstUses[resourceIndex]) {
                firstUses[resourceIndex] = position;
            }
            if (lastUses[resourceIndex] == kInvalidIndex || position > lastUses[resourceIndex]) {
                lastUses[resourceIndex] = position;
            }
        }
        resourceLifetimes_.assign(resources_.size(), ResourceLifetime{});
        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            if (firstUses[resourceIndex] == kInvalidIndex) {
                continue;
            }
            resourceLifetimes_[resourceIndex] = ResourceLifetime{
                executionOrder_[firstUses[resourceIndex]],
                executionOrder_[lastUses[resourceIndex]],
                true};
        }

        struct AllocationSlot {
            FrameGraphResourceKind kind = FrameGraphResourceKind::Image;
            std::uint64_t aliasClass = 0;
            std::uint64_t capacityBytes = 0;
            std::uint64_t alignment = 1;
            Index availableAfter = kInvalidIndex;
            Index useCount = 0;
        };
        std::vector<AllocationSlot> slots;
        transientAllocations_.assign(resources_.size(), TransientAllocation{});
        const auto saturatingAdd = [](const std::uint64_t lhs, const std::uint64_t rhs) {
            return rhs > std::numeric_limits<std::uint64_t>::max() - lhs
                ? std::numeric_limits<std::uint64_t>::max()
                : lhs + rhs;
        };
        for (Index position = 0; position < static_cast<Index>(executionOrder_.size()); ++position) {
            for (Index resourceIndex = 0; resourceIndex < static_cast<Index>(resources_.size()); ++resourceIndex) {
                const ResourceDesc& desc = resources_[resourceIndex];
                if (desc.imported || firstUses[resourceIndex] != position) {
                    continue;
                }
                transientStats_.requestedBytes = saturatingAdd(transientStats_.requestedBytes, desc.transientBytes);
                Index selectedSlot = kInvalidIndex;
                std::uint64_t selectedGrowth = std::numeric_limits<std::uint64_t>::max();
                if (desc.aliasClass != 0U) {
                    for (Index slotIndex = 0; slotIndex < static_cast<Index>(slots.size()); ++slotIndex) {
                        const AllocationSlot& slot = slots[slotIndex];
                        if (slot.kind != desc.kind || slot.aliasClass != desc.aliasClass ||
                            slot.availableAfter >= position) {
                            continue;
                        }
                        const std::uint64_t grownCapacity = std::max(slot.capacityBytes, desc.transientBytes);
                        const std::uint64_t growth = grownCapacity - slot.capacityBytes;
                        if (growth < selectedGrowth) {
                            selectedGrowth = growth;
                            selectedSlot = slotIndex;
                        }
                    }
                }
                if (selectedSlot == kInvalidIndex) {
                    selectedSlot = static_cast<Index>(slots.size());
                    slots.push_back(AllocationSlot{
                        desc.kind, desc.aliasClass, desc.transientBytes, desc.transientAlignment,
                        lastUses[resourceIndex], 1U});
                } else {
                    AllocationSlot& slot = slots[selectedSlot];
                    slot.capacityBytes = std::max(slot.capacityBytes, desc.transientBytes);
                    slot.alignment = std::max(slot.alignment, desc.transientAlignment);
                    slot.availableAfter = lastUses[resourceIndex];
                    ++slot.useCount;
                }
                transientAllocations_[resourceIndex] = TransientAllocation{
                    selectedSlot, 0U, desc.transientAlignment, false};
            }
        }
        transientStats_.allocationCount = static_cast<Index>(slots.size());
        for (const AllocationSlot& slot : slots) {
            transientStats_.allocatedBytes = saturatingAdd(transientStats_.allocatedBytes, slot.capacityBytes);
        }
        transientStats_.aliasedBytes = transientStats_.requestedBytes > transientStats_.allocatedBytes
            ? transientStats_.requestedBytes - transientStats_.allocatedBytes
            : 0U;
        for (Index resourceIndex = 0; resourceIndex < static_cast<Index>(resources_.size()); ++resourceIndex) {
            TransientAllocation& allocation = transientAllocations_[resourceIndex];
            if (allocation.slot == kInvalidIndex) {
                continue;
            }
            const AllocationSlot& slot = slots[allocation.slot];
            allocation.capacityBytes = slot.capacityBytes;
            allocation.alignment = slot.alignment;
            allocation.aliased = slot.useCount > 1U;
        }

        barrierIntents_.clear();
        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            std::vector<const Edge*> orderedEdges = edgesByResource[resourceIndex];
            std::stable_sort(orderedEdges.begin(), orderedEdges.end(), [&](const Edge* lhs, const Edge* rhs) {
                return passPositions[lhs->pass.index] < passPositions[rhs->pass.index];
            });

            const Edge* previous = nullptr;
            for (const Edge* edge : orderedEdges) {
                if (previous == nullptr || previous->access != edge->access || previous->usage != edge->usage ||
                    (previous->access == FrameGraphAccess::Write && edge->access == FrameGraphAccess::Write)) {
                    barrierIntents_.push_back(BarrierIntent{
                        ResourceHandle{static_cast<Index>(resourceIndex)},
                        edge->pass,
                        edge->access,
                        edge->usage,
                        previous != nullptr ? previous->access : FrameGraphAccess::Read,
                        previous != nullptr ? previous->usage : FrameGraphUsage::Present,
                        previous != nullptr,
                        false});
                }
                previous = edge;
            }

            const ResourceDesc& resourceDesc = resources_[resourceIndex];
            if (previous != nullptr && resourceDesc.hasFinalUsage &&
                (previous->access != FrameGraphAccess::Read || previous->usage != resourceDesc.finalUsage)) {
                barrierIntents_.push_back(BarrierIntent{
                    ResourceHandle{static_cast<Index>(resourceIndex)},
                    PassHandle{},
                    FrameGraphAccess::Read,
                    resourceDesc.finalUsage,
                    previous->access,
                    previous->usage,
                    true,
                    true});
            }
        }
        std::stable_sort(barrierIntents_.begin(), barrierIntents_.end(), [&](const BarrierIntent& lhs, const BarrierIntent& rhs) {
            if (lhs.finalTransition != rhs.finalTransition) {
                return !lhs.finalTransition;
            }
            if (lhs.finalTransition) {
                return lhs.resource.index < rhs.resource.index;
            }
            return passPositions[lhs.pass.index] < passPositions[rhs.pass.index];
        });
        finalBarrierIntentIndices_.assign(resources_.size(), kInvalidIndex);
        for (Index intentIndex = 0; intentIndex < static_cast<Index>(barrierIntents_.size()); ++intentIndex) {
            const BarrierIntent& intent = barrierIntents_[intentIndex];
            if (!intent.finalTransition) {
                continue;
            }
            Index& cachedIndex = finalBarrierIntentIndices_[intent.resource.index];
            if (cachedIndex != kInvalidIndex) {
                throw std::runtime_error("FrameGraph resource has multiple final barrier intents");
            }
            cachedIndex = intentIndex;
        }
        barrierIntentsByPass_.assign(passes_.size(), {});
        finalBarrierIntentIndicesOrdered_.clear();
        for (Index intentIndex = 0; intentIndex < static_cast<Index>(barrierIntents_.size()); ++intentIndex) {
            const BarrierIntent& intent = barrierIntents_[intentIndex];
            if (intent.finalTransition) {
                finalBarrierIntentIndicesOrdered_.push_back(intentIndex);
            } else {
                barrierIntentsByPass_[intent.pass.index].push_back(intentIndex);
            }
        }
        createResourcesByPass_.assign(passes_.size(), {});
        retireResourcesByPass_.assign(passes_.size(), {});
        retireResourcesAfterFinal_.clear();
        for (Index resourceIndex = 0; resourceIndex < static_cast<Index>(resources_.size()); ++resourceIndex) {
            if (resources_[resourceIndex].imported || firstUses[resourceIndex] == kInvalidIndex) {
                continue;
            }
            createResourcesByPass_[executionOrder_[firstUses[resourceIndex]].index].push_back(resourceIndex);
            if (resources_[resourceIndex].hasFinalUsage) {
                retireResourcesAfterFinal_.push_back(resourceIndex);
            } else {
                retireResourcesByPass_[executionOrder_[lastUses[resourceIndex]].index].push_back(resourceIndex);
            }
        }
        for (std::vector<Index>& resources : retireResourcesByPass_) {
            std::reverse(resources.begin(), resources.end());
        }
        std::reverse(retireResourcesAfterFinal_.begin(), retireResourcesAfterFinal_.end());
        compiled_ = true;
    }

    [[nodiscard]] const PassDesc& pass(PassHandle handle) const {
        validatePass(handle);
        return passes_[static_cast<std::size_t>(handle.index)];
    }

    [[nodiscard]] const ResourceDesc& resource(ResourceHandle handle) const {
        validateResource(handle);
        return resources_[static_cast<std::size_t>(handle.index)];
    }

    [[nodiscard]] std::size_t passCount() const { return passes_.size(); }
    [[nodiscard]] std::size_t resourceCount() const { return resources_.size(); }
    [[nodiscard]] std::size_t edgeCount() const { return edges_.size(); }
    [[nodiscard]] bool compiled() const { return compiled_; }
    [[nodiscard]] const std::vector<PassHandle>& executionOrder() const { return executionOrder_; }
    [[nodiscard]] const ResourceLifetime& lifetime(ResourceHandle resource) const {
        validateResource(resource);
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        return resourceLifetimes_[static_cast<std::size_t>(resource.index)];
    }
    [[nodiscard]] const std::vector<BarrierIntent>& barrierPlan() const {
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        return barrierIntents_;
    }
    [[nodiscard]] const BarrierIntent& finalBarrierIntent(ResourceHandle resource) const {
        validateResource(resource);
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        const Index intentIndex = finalBarrierIntentIndices_[resource.index];
        if (intentIndex == kInvalidIndex) {
            throw std::runtime_error("FrameGraph resource has no final barrier intent");
        }
        return barrierIntents_[intentIndex];
    }
    [[nodiscard]] const BarrierIntent& barrierIntent(PassHandle pass,
                                                     ResourceHandle resource,
                                                     FrameGraphAccess access,
                                                     FrameGraphUsage usage) const {
        validatePass(pass);
        validateResource(resource);
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        const BarrierIntent* result = nullptr;
        for (const BarrierIntent& intent : barrierIntents_) {
            if (intent.finalTransition ||
                intent.pass.index != pass.index ||
                intent.resource.index != resource.index ||
                intent.access != access ||
                intent.usage != usage) {
                continue;
            }
            if (result != nullptr) {
                throw std::runtime_error("FrameGraph pass/resource/access/usage has multiple barrier intents");
            }
            result = &intent;
        }
        if (result == nullptr) {
            throw std::runtime_error("FrameGraph pass/resource/access/usage has no barrier intent");
        }
        return *result;
    }

    [[nodiscard]] const Edge& edge(PassHandle pass, ResourceHandle resource) const {
        validatePass(pass);
        validateResource(resource);
        for (const Edge& edge : edges_) {
            if (edge.pass.index == pass.index && edge.resource.index == resource.index) {
                return edge;
            }
        }
        throw std::runtime_error("FrameGraph pass has no edge for resource");
    }

    [[nodiscard]] const TransientAllocation& transientAllocation(ResourceHandle resource) const {
        validateResource(resource);
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        return transientAllocations_[resource.index];
    }

    [[nodiscard]] const TransientStats& transientStats() const {
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        return transientStats_;
    }

    [[nodiscard]] ExecutionResult execute(const ExecutionCallbacks& callbacks, ExecutionState& state) const {
        if (!compiled_) {
            throw std::runtime_error("FrameGraph has not been compiled");
        }
        if (callbacks.transition == nullptr || callbacks.executePass == nullptr) {
            throw std::runtime_error("FrameGraph execution requires transition and pass callbacks");
        }
        if (transientStats_.allocationCount > 0U &&
            (callbacks.createResource == nullptr || callbacks.retireResource == nullptr)) {
            throw std::runtime_error("FrameGraph execution requires resource lifecycle callbacks");
        }
        state.activeResources.assign(resources_.size(), 0U);
        state.creationOrder.clear();
        state.creationOrder.reserve(resources_.size());
        ExecutionResult result{};
        const auto retire = [&](const Index resourceIndex) {
            if (state.activeResources[resourceIndex] == 0U) {
                return true;
            }
            const bool retired = callbacks.retireResource(
                callbacks.context, ResourceHandle{resourceIndex}, resources_[resourceIndex],
                transientAllocations_[resourceIndex]);
            if (retired) {
                state.activeResources[resourceIndex] = 0U;
                ++result.resourcesRetired;
            }
            return retired;
        };
        const auto unwind = [&] {
            for (auto it = state.creationOrder.rbegin(); it != state.creationOrder.rend(); ++it) {
                if (!retire(*it) && result.failure == ExecutionFailure::None) {
                    result.failure = ExecutionFailure::Retire;
                    result.resource = ResourceHandle{*it};
                }
            }
        };
        for (const PassHandle passHandle : executionOrder_) {
            for (const Index resourceIndex : createResourcesByPass_[passHandle.index]) {
                if (!callbacks.createResource(callbacks.context, ResourceHandle{resourceIndex},
                                              resources_[resourceIndex], transientAllocations_[resourceIndex])) {
                    result.failure = ExecutionFailure::Create;
                    result.resource = ResourceHandle{resourceIndex};
                    result.pass = passHandle;
                    unwind();
                    return result;
                }
                state.activeResources[resourceIndex] = 1U;
                state.creationOrder.push_back(resourceIndex);
                ++result.resourcesCreated;
            }
            for (const Index intentIndex : barrierIntentsByPass_[passHandle.index]) {
                const BarrierIntent& intent = barrierIntents_[intentIndex];
                if (!callbacks.transition(callbacks.context, intent)) {
                    result.failure = ExecutionFailure::Transition;
                    result.resource = intent.resource;
                    result.pass = passHandle;
                    unwind();
                    return result;
                }
                ++result.barriersApplied;
            }
            const PassResources passResources{passEdges_[passHandle.index]};
            if (!callbacks.executePass(
                    callbacks.context, passHandle, passes_[passHandle.index], passResources)) {
                result.failure = ExecutionFailure::Pass;
                result.pass = passHandle;
                unwind();
                return result;
            }
            ++result.passesExecuted;
            for (const Index resourceIndex : retireResourcesByPass_[passHandle.index]) {
                if (!retire(resourceIndex)) {
                    result.failure = ExecutionFailure::Retire;
                    result.resource = ResourceHandle{resourceIndex};
                    result.pass = passHandle;
                    unwind();
                    return result;
                }
            }
        }
        for (const Index intentIndex : finalBarrierIntentIndicesOrdered_) {
            const BarrierIntent& intent = barrierIntents_[intentIndex];
            if (!callbacks.transition(callbacks.context, intent)) {
                result.failure = ExecutionFailure::Transition;
                result.resource = intent.resource;
                unwind();
                return result;
            }
            ++result.barriersApplied;
        }
        for (const Index resourceIndex : retireResourcesAfterFinal_) {
            if (!retire(resourceIndex)) {
                result.failure = ExecutionFailure::Retire;
                result.resource = ResourceHandle{resourceIndex};
                unwind();
                return result;
            }
        }
        return result;
    }

private:
    void addEdge(PassHandle pass, ResourceHandle resource, FrameGraphAccess access, FrameGraphUsage usage,
                 bool attachment, FrameGraphAttachmentLoad load, FrameGraphAttachmentStore store) {
        validatePass(pass);
        validateResource(resource);
        const ResourceDesc& resourceDesc = resources_[resource.index];
        validateUsageForResource(resourceDesc, access, usage);
        const bool attachmentUsage =
            usage == FrameGraphUsage::ColorAttachment || usage == FrameGraphUsage::DepthAttachment;
        if (attachment != attachmentUsage) {
            throw std::runtime_error(attachmentUsage
                ? "FrameGraph attachment usage requires explicit attachment behavior"
                : "FrameGraph non-attachment usage cannot declare attachment behavior");
        }
        if (attachment && access == FrameGraphAccess::Read && load != FrameGraphAttachmentLoad::Load) {
            throw std::runtime_error("FrameGraph read-only attachment must load existing contents");
        }
        for (const Edge& edge : edges_) {
            if (edge.pass.index != pass.index || edge.resource.index != resource.index) {
                continue;
            }
            if (edge.access == access && edge.usage == usage) {
                throw std::runtime_error("Duplicate FrameGraph edge");
            }
            throw std::runtime_error("FrameGraph pass cannot declare multiple access states for one resource; split the work into passes");
        }
        edges_.push_back(Edge{pass, resource, access, usage, attachment, load, store});
        invalidate();
    }

    static void validateUsageForResource(const ResourceDesc& resource, FrameGraphAccess access,
                                         FrameGraphUsage usage) {
        switch (resource.kind) {
        case FrameGraphResourceKind::Image:
            if (usage != FrameGraphUsage::ColorAttachment && usage != FrameGraphUsage::DepthAttachment &&
                usage != FrameGraphUsage::SampledImage && usage != FrameGraphUsage::TransferSource &&
                usage != FrameGraphUsage::Present && usage != FrameGraphUsage::TransferDestination) {
                throw std::runtime_error("FrameGraph image resource has a buffer-only usage");
            }
            break;
        case FrameGraphResourceKind::Buffer:
            if (usage != FrameGraphUsage::UniformBuffer && usage != FrameGraphUsage::StorageBuffer &&
                usage != FrameGraphUsage::IndirectBuffer && usage != FrameGraphUsage::TransferSource &&
                usage != FrameGraphUsage::TransferDestination && usage != FrameGraphUsage::HostRead) {
                throw std::runtime_error("FrameGraph buffer resource has an image-only usage");
            }
            break;
        default:
            throw std::runtime_error("Unknown FrameGraph resource kind");
        }
        switch (usage) {
        case FrameGraphUsage::ColorAttachment:
        case FrameGraphUsage::DepthAttachment:
        case FrameGraphUsage::StorageBuffer:
            return;
        case FrameGraphUsage::SampledImage:
        case FrameGraphUsage::TransferSource:
        case FrameGraphUsage::Present:
        case FrameGraphUsage::UniformBuffer:
        case FrameGraphUsage::IndirectBuffer:
        case FrameGraphUsage::HostRead:
            if (access != FrameGraphAccess::Read) {
                throw std::runtime_error("FrameGraph usage is read-only");
            }
            return;
        case FrameGraphUsage::TransferDestination:
            if (access != FrameGraphAccess::Write) {
                throw std::runtime_error("FrameGraph transfer destination usage is write-only");
            }
            return;
        }
        throw std::runtime_error("Unknown FrameGraph usage");
    }

    void invalidate() {
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
        barrierIntents_.clear();
        finalBarrierIntentIndices_.clear();
        transientAllocations_.clear();
        transientStats_ = {};
        createResourcesByPass_.clear();
        retireResourcesByPass_.clear();
        retireResourcesAfterFinal_.clear();
        barrierIntentsByPass_.clear();
        finalBarrierIntentIndicesOrdered_.clear();
        passEdges_.clear();
    }

    void validatePass(PassHandle handle) const {
        if (!handle.valid() || static_cast<std::size_t>(handle.index) >= passes_.size()) {
            throw std::runtime_error("Invalid FrameGraph pass handle");
        }
    }

    void validateResource(ResourceHandle handle) const {
        if (!handle.valid() || static_cast<std::size_t>(handle.index) >= resources_.size()) {
            throw std::runtime_error("Invalid FrameGraph resource handle");
        }
    }

    std::vector<ResourceDesc> resources_;
    std::vector<PassDesc> passes_;
    std::vector<Edge> edges_;
    std::vector<PassHandle> executionOrder_;
    std::vector<ResourceLifetime> resourceLifetimes_;
    std::vector<BarrierIntent> barrierIntents_;
    std::vector<Index> finalBarrierIntentIndices_;
    std::vector<TransientAllocation> transientAllocations_;
    TransientStats transientStats_{};
    std::vector<std::vector<Index>> createResourcesByPass_;
    std::vector<std::vector<Index>> retireResourcesByPass_;
    std::vector<Index> retireResourcesAfterFinal_;
    std::vector<std::vector<Index>> barrierIntentsByPass_;
    std::vector<std::vector<Edge>> passEdges_;
    std::vector<Index> finalBarrierIntentIndicesOrdered_;
    bool compiled_ = false;
};

} // namespace ve
