#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>

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
    static constexpr std::uint8_t kInvalidIndex = 0xffU;
    static constexpr std::size_t kMaxResources = 8;
    static constexpr std::size_t kMaxPasses = 8;
    static constexpr std::size_t kMaxEdges = 24;

    struct ResourceHandle {
        std::uint8_t index = kInvalidIndex;
        [[nodiscard]] bool valid() const { return index != kInvalidIndex; }
    };

    struct PassHandle {
        std::uint8_t index = kInvalidIndex;
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

    [[nodiscard]] ResourceHandle addResource(ResourceDesc desc) {
        if (resourceCount_ >= resources_.size()) {
            throw std::runtime_error("FrameGraph resource capacity exceeded");
        }
        const ResourceHandle handle{static_cast<std::uint8_t>(resourceCount_)};
        resources_[resourceCount_] = desc;
        ++resourceCount_;
        compiled_ = false;
        return handle;
    }

    [[nodiscard]] PassHandle addPass(PassDesc desc) {
        if (passCount_ >= passes_.size()) {
            throw std::runtime_error("FrameGraph pass capacity exceeded");
        }
        const PassHandle handle{static_cast<std::uint8_t>(passCount_)};
        passes_[passCount_] = desc;
        ++passCount_;
        compiled_ = false;
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
        resources_[resource.index].hasFinalUsage = true;
        resources_[resource.index].finalUsage = usage;
        compiled_ = false;
    }

    [[nodiscard]] bool hasEdge(PassHandle pass, ResourceHandle resource, FrameGraphAccess access, FrameGraphUsage usage) const {
        validatePass(pass);
        validateResource(resource);
        for (std::size_t edgeIndex = 0; edgeIndex < edgeCount_; ++edgeIndex) {
            const Edge& edge = edges_[edgeIndex];
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
        return resources_[resource.index].hasFinalUsage;
    }

    [[nodiscard]] FrameGraphUsage finalUsage(ResourceHandle resource) const {
        validateResource(resource);
        if (!resources_[resource.index].hasFinalUsage) {
            throw std::runtime_error("FrameGraph resource has no final usage");
        }
        return resources_[resource.index].finalUsage;
    }

    void compile() {
        std::array<bool, kMaxResources> hasWriter{};
        std::array<bool, kMaxPasses> passHasAccess{};

        for (std::size_t passIndex = 0; passIndex < passCount_; ++passIndex) {
            for (std::size_t edgeIndex = 0; edgeIndex < edgeCount_; ++edgeIndex) {
                const Edge& edge = edges_[edgeIndex];
                if (edge.pass.index != passIndex) {
                    continue;
                }

                const std::size_t resourceIndex = edge.resource.index;
                passHasAccess[passIndex] = true;
                if (edge.access == FrameGraphAccess::Read && !resources_[resourceIndex].imported && !hasWriter[resourceIndex]) {
                    throw std::runtime_error("FrameGraph pass reads a non-imported resource before any pass writes it");
                }
                if (edge.access == FrameGraphAccess::Write) {
                    hasWriter[resourceIndex] = true;
                }
            }
            if (!passHasAccess[passIndex]) {
                throw std::runtime_error("FrameGraph pass has no resource edges");
            }
        }

        compiled_ = true;
    }

    [[nodiscard]] const PassDesc& pass(PassHandle handle) const {
        validatePass(handle);
        return passes_[handle.index];
    }

    [[nodiscard]] const ResourceDesc& resource(ResourceHandle handle) const {
        validateResource(handle);
        return resources_[handle.index];
    }

    [[nodiscard]] std::size_t passCount() const { return passCount_; }
    [[nodiscard]] std::size_t resourceCount() const { return resourceCount_; }
    [[nodiscard]] std::size_t edgeCount() const { return edgeCount_; }
    [[nodiscard]] bool compiled() const { return compiled_; }

private:
    void addEdge(PassHandle pass, ResourceHandle resource, FrameGraphAccess access, FrameGraphUsage usage) {
        validatePass(pass);
        validateResource(resource);
        for (std::size_t edgeIndex = 0; edgeIndex < edgeCount_; ++edgeIndex) {
            const Edge& edge = edges_[edgeIndex];
            if (edge.pass.index == pass.index &&
                edge.resource.index == resource.index &&
                edge.access == access &&
                edge.usage == usage) {
                throw std::runtime_error("Duplicate FrameGraph edge");
            }
        }
        if (edgeCount_ >= edges_.size()) {
            throw std::runtime_error("FrameGraph edge capacity exceeded");
        }
        edges_[edgeCount_] = Edge{pass, resource, access, usage};
        ++edgeCount_;
        compiled_ = false;
    }

    void validatePass(PassHandle handle) const {
        if (!handle.valid() || handle.index >= passCount_) {
            throw std::runtime_error("Invalid FrameGraph pass handle");
        }
    }

    void validateResource(ResourceHandle handle) const {
        if (!handle.valid() || handle.index >= resourceCount_) {
            throw std::runtime_error("Invalid FrameGraph resource handle");
        }
    }

    std::array<ResourceDesc, kMaxResources> resources_{};
    std::array<PassDesc, kMaxPasses> passes_{};
    std::array<Edge, kMaxEdges> edges_{};
    std::size_t resourceCount_ = 0;
    std::size_t passCount_ = 0;
    std::size_t edgeCount_ = 0;
    bool compiled_ = false;
};

} // namespace ve
