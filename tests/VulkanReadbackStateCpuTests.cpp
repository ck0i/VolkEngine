#include "renderer/vulkan/VulkanReadbackState.hpp"

#include <cassert>
#include <filesystem>

namespace {
struct FakeBuffer {
    int value = 0;
};
}

int main() {
    ve::VulkanReadbackState<FakeBuffer> state;
    assert(!state.takeRequest().has_value());
    state.request("first.ppm");
    state.request("newest.ppm");
    const auto newest = state.takeRequest();
    assert(newest && *newest == std::filesystem::path{"newest.ppm"});
    assert(!state.takeRequest().has_value());

    state.retry("retry.ppm");
    state.retry("ignored.ppm");
    const auto retry = state.takeRequest();
    assert(retry && *retry == std::filesystem::path{"retry.ppm"});

    assert(!state.swapchainTransferSourceSupported());
    state.setSwapchainTransferSourceSupported(true);
    assert(state.swapchainTransferSourceSupported());
    state.buffer().value = 42;
    assert(state.buffer().value == 42);
    return 0;
}
