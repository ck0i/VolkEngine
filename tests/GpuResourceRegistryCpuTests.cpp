#include "renderer/GpuResourceRegistry.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

int gFailureCount = 0;

template <typename T, typename U>
void expectEqual(const std::string_view context, const T& actual, const U& expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

template <typename F>
void expectNoThrow(const std::string_view context, F&& callable) {
    try {
        callable();
    } catch (const std::exception& e) {
        std::cerr << "[FAILED] " << context << ": unexpected exception " << e.what() << '\n';
        ++gFailureCount;
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected non-std exception\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    {
        ve::GpuResourceRegistry registry;
        constexpr std::uint32_t kResourceCount = 160;
        for (std::uint32_t i = 0; i < kResourceCount; ++i) {
            const std::uint32_t id = registry.registerResource(ve::GpuResourceKind::Buffer, "buffer", 4);
            expectEqual("registry ids are monotonic past old cap", id, i + 1U);
        }

        const ve::GpuResourceRegistry::Stats stats = registry.stats();
        expectEqual("registry grows past old 128-resource cap", stats.liveResources, kResourceCount);
        expectEqual("all growth resources are buffers", stats.buffers, kResourceCount);
        expectEqual("growth buffer bytes", stats.bufferBytes, static_cast<std::uint64_t>(kResourceCount) * 4U);
    }

    {
        ve::GpuResourceRegistry registry;
        const std::uint32_t maxBuffer = registry.registerResource(
            ve::GpuResourceKind::Buffer, "Max Buffer", std::numeric_limits<std::uint64_t>::max());
        const std::uint32_t oneByteBuffer = registry.registerResource(ve::GpuResourceKind::Buffer, "One Byte Buffer", 1);

        ve::GpuResourceRegistry::Stats stats = registry.stats();
        expectEqual("buffer byte accounting saturates total", stats.bytes, std::numeric_limits<std::uint64_t>::max());
        expectEqual("buffer byte accounting saturates category", stats.bufferBytes, std::numeric_limits<std::uint64_t>::max());
        expectEqual("saturated buffer accounting retains both resources", stats.liveResources, 2U);
        registry.unregisterResource(maxBuffer);
        stats = registry.stats();
        expectEqual("unregister recomputes saturated bytes", stats.bytes, 1ULL);
        expectEqual("unregister recomputes saturated buffer bytes", stats.bufferBytes, 1ULL);
        expectEqual("unregister preserves one-byte resource", stats.liveResources, 1U);
        (void)oneByteBuffer;
    }

    {
        ve::GpuResourceRegistry registry;
        (void)registry.registerResource(ve::GpuResourceKind::Image, "Max Owned Image",
                                         std::numeric_limits<std::uint64_t>::max());
        (void)registry.registerResource(ve::GpuResourceKind::Image, "Imported Image", 1, true);

        const ve::GpuResourceRegistry::Stats stats = registry.stats();
        expectEqual("image byte accounting saturates total", stats.bytes, std::numeric_limits<std::uint64_t>::max());
        expectEqual("image byte accounting saturates image category", stats.imageBytes, std::numeric_limits<std::uint64_t>::max());
        expectEqual("owned image bytes remain saturated", stats.ownedImageBytes, std::numeric_limits<std::uint64_t>::max());
        expectEqual("imported image bytes retain exact small value", stats.importedImageBytes, 1ULL);
    }

    {
        ve::GpuResourceRegistry registry;
        const std::uint32_t buffer = registry.registerResource(ve::GpuResourceKind::Buffer, "Scene Instances", 64);
        const std::uint32_t ownedImage = registry.registerResource(ve::GpuResourceKind::Image, "HDR", 256);
        const std::uint32_t importedImage = registry.registerResource(ve::GpuResourceKind::Image, "Swapchain", 512, true);

        ve::GpuResourceRegistry::Stats stats = registry.stats();
        expectEqual("mixed stats live count", stats.liveResources, 3U);
        expectEqual("mixed stats buffers", stats.buffers, 1U);
        expectEqual("mixed stats images", stats.images, 2U);
        expectEqual("mixed stats imported images", stats.importedImages, 1U);
        expectEqual("mixed stats total bytes", stats.bytes, 832ULL);
        expectEqual("mixed stats buffer bytes", stats.bufferBytes, 64ULL);
        expectEqual("mixed stats image bytes", stats.imageBytes, 768ULL);
        expectEqual("mixed stats owned image bytes", stats.ownedImageBytes, 256ULL);
        expectEqual("mixed stats imported image bytes", stats.importedImageBytes, 512ULL);

        registry.unregisterResource(ve::GpuResourceRegistry::kInvalidId);
        registry.unregisterResource(std::numeric_limits<std::uint32_t>::max() - 1U);
        registry.unregisterResource(ownedImage);
        registry.unregisterResource(ownedImage);

        stats = registry.stats();
        expectEqual("unregister removes only live matching id", stats.liveResources, 2U);
        expectEqual("unregister leaves buffer live", stats.buffers, 1U);
        expectEqual("unregister leaves imported image live", stats.importedImages, 1U);
        expectEqual("unregister updates total bytes", stats.bytes, 576ULL);
        expectEqual("unregister updates owned image bytes", stats.ownedImageBytes, 0ULL);

        const std::uint32_t replacement = registry.registerResource(ve::GpuResourceKind::Buffer, "Replacement", 32);
        expectEqual("replacement id stays monotonic after compact unregister", replacement, importedImage + 1U);
        stats = registry.stats();
        expectEqual("compact unregister live count", stats.liveResources, 3U);
        expectEqual("compact unregister buffer count", stats.buffers, 2U);
        expectEqual("compact unregister bytes", stats.bytes, 608ULL);
        (void)buffer;
    }

    {
        ve::GpuResourceRegistry registry;
        expectNoThrow("empty and long names are accepted", [&] {
            (void)registry.registerResource(ve::GpuResourceKind::Buffer, {}, 1);
            (void)registry.registerResource(ve::GpuResourceKind::Image, std::string(ve::GpuResourceRegistry::kMaxNameLength * 4U, 'x'), 2);
        });

        const ve::GpuResourceRegistry::Stats stats = registry.stats();
        expectEqual("name edge cases live count", stats.liveResources, 2U);
        expectEqual("name edge cases total bytes", stats.bytes, 3ULL);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "GpuResourceRegistry CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
