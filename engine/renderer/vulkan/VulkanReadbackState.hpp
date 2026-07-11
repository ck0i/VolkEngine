#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>

namespace ve {

template <typename Buffer>
class VulkanReadbackState {
public:
    VulkanReadbackState() = default;
    VulkanReadbackState(const VulkanReadbackState&) = delete;
    VulkanReadbackState& operator=(const VulkanReadbackState&) = delete;

    void request(std::filesystem::path path) {
        const std::scoped_lock lock{requestMutex_};
        requestPath_ = std::move(path);
    }

    [[nodiscard]] std::optional<std::filesystem::path> takeRequest() {
        const std::scoped_lock lock{requestMutex_};
        if (!requestPath_) return std::nullopt;
        std::filesystem::path path = std::move(*requestPath_);
        requestPath_.reset();
        return path;
    }

    void retry(std::filesystem::path path) {
        const std::scoped_lock lock{requestMutex_};
        if (!requestPath_) requestPath_ = std::move(path);
    }

    void setSwapchainTransferSourceSupported(const bool supported) noexcept {
        swapchainTransferSourceSupported_ = supported;
    }

    [[nodiscard]] bool swapchainTransferSourceSupported() const noexcept {
        return swapchainTransferSourceSupported_;
    }

    [[nodiscard]] Buffer& buffer() noexcept { return buffer_; }
    [[nodiscard]] const Buffer& buffer() const noexcept { return buffer_; }

private:
    mutable std::mutex requestMutex_;
    std::optional<std::filesystem::path> requestPath_;
    bool swapchainTransferSourceSupported_ = false;
    Buffer buffer_{};
};

} // namespace ve
