#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <functional>
#include <queue>
#include <stdexcept>
#include <vector>

namespace ve {

enum class FrameGraphResourceKind : std::uint8_t {
    Image
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
    Present
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
    };

    struct ResourceLifetime {
        PassHandle firstPass{};
        PassHandle lastPass{};
        bool used = false;
    };

    [[nodiscard]] ResourceHandle addResource(ResourceDesc desc) {
        if (resources_.size() >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::runtime_error("FrameGraph resource id range exhausted");
        }
        const ResourceHandle handle{static_cast<Index>(resources_.size())};
        resources_.push_back(desc);
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
        return handle;
    }

    [[nodiscard]] PassHandle addPass(PassDesc desc) {
        if (passes_.size() >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::runtime_error("FrameGraph pass id range exhausted");
        }
        const PassHandle handle{static_cast<Index>(passes_.size())};
        passes_.push_back(desc);
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
        return handle;
    }

    void read(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage) {
        addEdge(pass, resource, FrameGraphAccess::Read, usage);
    }

    void write(PassHandle pass, ResourceHandle resource, FrameGraphUsage usage) {
        addEdge(pass, resource, FrameGraphAccess::Write, usage);
    }

    void setFinalUsage(ResourceHandle resource, FrameGraphUsage usage) {
        validateResource(resource);
        resources_[static_cast<std::size_t>(resource.index)].hasFinalUsage = true;
        resources_[static_cast<std::size_t>(resource.index)].finalUsage = usage;
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
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
        std::vector<std::uint8_t> hasWriter(resources_.size(), 0);
        std::vector<std::uint8_t> passHasAccess(passes_.size(), 0);

        for (std::size_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
            for (const Edge& edge : edges_) {
                if (static_cast<std::size_t>(edge.pass.index) != passIndex) {
                    continue;
                }

                const std::size_t resourceIndex = static_cast<std::size_t>(edge.resource.index);
                passHasAccess[passIndex] = 1;
                if (edge.access == FrameGraphAccess::Read && !resources_[resourceIndex].imported && hasWriter[resourceIndex] == 0U) {
                    throw std::runtime_error("FrameGraph pass reads a non-imported resource before any pass writes it");
                }
                if (edge.access == FrameGraphAccess::Write) {
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
            for (std::size_t passIndex = 0; passIndex < passCount; ++passIndex) {
                const Index pass = static_cast<Index>(passIndex);
                for (const Edge& edge : edges_) {
                    if (edge.resource.index != static_cast<Index>(resourceIndex) || edge.pass.index != pass) {
                        continue;
                    }
                    if (edge.access == FrameGraphAccess::Read) {
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

private:
    void addEdge(PassHandle pass, ResourceHandle resource, FrameGraphAccess access, FrameGraphUsage usage) {
        validatePass(pass);
        validateResource(resource);
        for (const Edge& edge : edges_) {
            if (edge.pass.index == pass.index &&
                edge.resource.index == resource.index &&
                edge.access == access &&
                edge.usage == usage) {
                throw std::runtime_error("Duplicate FrameGraph edge");
            }
        }
        edges_.push_back(Edge{pass, resource, access, usage});
        compiled_ = false;
        executionOrder_.clear();
        resourceLifetimes_.clear();
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
    bool compiled_ = false;
};

} // namespace ve
