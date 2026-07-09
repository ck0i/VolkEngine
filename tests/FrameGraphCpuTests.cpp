#include "renderer/FrameGraph.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

int gFailureCount = 0;

template <typename T, typename U>
void expectEqual(std::string_view context, const T& actual, const U& expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

template <typename F>
void expectNoThrow(std::string_view context, F&& callable) {
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

template <typename F>
void expectThrowsRuntimeError(std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw different exception\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    using ve::FrameGraph;
    using ve::FrameGraphAccess;
    using ve::FrameGraphUsage;

    {
        FrameGraph graph;
        const auto depth = graph.addResource({"Depth Image", ve::FrameGraphResourceKind::Image, false});
        const auto hdr = graph.addResource({"HDR Color Image", ve::FrameGraphResourceKind::Image, false});
        const auto swapchain = graph.addResource({"Swapchain Image", ve::FrameGraphResourceKind::Image, true});

        const auto depthPass = graph.addPass({"Depth Prepass"});
        const auto hdrPass = graph.addPass({"HDR Scene Pass"});
        const auto tonemapPass = graph.addPass({"Tonemap + ImGui Pass"});
        const auto screenshotPass = graph.addPass({"Screenshot Readback Pass"});

        graph.write(depthPass, depth, FrameGraphUsage::DepthAttachment);
        graph.read(hdrPass, depth, FrameGraphUsage::DepthAttachment);
        graph.write(hdrPass, hdr, FrameGraphUsage::ColorAttachment);
        graph.read(tonemapPass, hdr, FrameGraphUsage::SampledImage);
        graph.write(tonemapPass, swapchain, FrameGraphUsage::ColorAttachment);
        graph.read(screenshotPass, swapchain, FrameGraphUsage::TransferSource);
        graph.setFinalUsage(swapchain, FrameGraphUsage::Present);

        expectNoThrow("compile accepts mode-like DAG with depth prepass", [&] {
            graph.compile();
        });
        expectEqual("mode-like DAG with prepass compiles", graph.compiled(), true);
        expectEqual("mode-like DAG with prepass edge count", graph.edgeCount(), static_cast<std::size_t>(6));
        expectEqual("mode-like DAG with prepass pass count", graph.passCount(), static_cast<std::size_t>(4));
        expectEqual("mode-like DAG with prepass has depth prepass write", graph.hasEdge(depthPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment), true);
        expectEqual("mode-like DAG with prepass hdr reads depth", graph.hasEdge(hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment), true);
        expectEqual("mode-like DAG with prepass has screenshot transfer-source read", graph.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::TransferSource), true);
        expectEqual("mode-like DAG with prepass screenshot read is not sampled image", graph.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::SampledImage), false);
        expectEqual("mode-like DAG with prepass final usage is marked", graph.hasFinalUsage(swapchain), true);
        expectEqual("mode-like DAG with prepass has final usage", static_cast<int>(graph.finalUsage(swapchain)), static_cast<int>(FrameGraphUsage::Present));
    }

    {
        FrameGraph graph;
        const auto depth = graph.addResource({"Depth Image", ve::FrameGraphResourceKind::Image, false});
        const auto hdr = graph.addResource({"HDR Color Image", ve::FrameGraphResourceKind::Image, false});
        const auto swapchain = graph.addResource({"Swapchain Image", ve::FrameGraphResourceKind::Image, true});

        const auto hdrPass = graph.addPass({"HDR Scene Pass"});
        const auto tonemapPass = graph.addPass({"Tonemap + ImGui Pass"});
        const auto screenshotPass = graph.addPass({"Screenshot Readback Pass"});

        graph.write(hdrPass, depth, FrameGraphUsage::DepthAttachment);
        graph.write(hdrPass, hdr, FrameGraphUsage::ColorAttachment);
        graph.read(tonemapPass, hdr, FrameGraphUsage::SampledImage);
        graph.write(tonemapPass, swapchain, FrameGraphUsage::ColorAttachment);
        graph.read(screenshotPass, swapchain, FrameGraphUsage::TransferSource);
        graph.setFinalUsage(swapchain, FrameGraphUsage::Present);

        expectNoThrow("compile accepts mode-like DAG without depth prepass", [&] {
            graph.compile();
        });
        expectEqual("mode-like DAG without prepass compiles", graph.compiled(), true);
        expectEqual("mode-like DAG without prepass edge count", graph.edgeCount(), static_cast<std::size_t>(5));
        expectEqual("mode-like DAG without prepass pass count", graph.passCount(), static_cast<std::size_t>(3));
        expectEqual("mode-like DAG without prepass hdr writes depth", graph.hasEdge(hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment), true);
        expectEqual("mode-like DAG without prepass hdr does not read depth", graph.hasEdge(hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment), false);
        expectEqual("mode-like DAG without prepass screenshot pass reads swapchain", graph.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::TransferSource), true);
        expectEqual("mode-like DAG without prepass screenshot read is not sampled image", graph.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::SampledImage), false);
        expectEqual("mode-like DAG without prepass final usage is marked", graph.hasFinalUsage(swapchain), true);
        expectEqual("mode-like DAG without prepass has final usage", static_cast<int>(graph.finalUsage(swapchain)), static_cast<int>(FrameGraphUsage::Present));
    }

    {
        FrameGraph graph;
        const auto localImage = graph.addResource({"Local Image", ve::FrameGraphResourceKind::Image, false});
        const auto invalidPass = graph.addPass({"Read before write"});
        graph.read(invalidPass, localImage, FrameGraphUsage::SampledImage);

        expectThrowsRuntimeError("compile rejects non-imported read-before-write", [&] {
            graph.compile();
        });
    }

    {
        FrameGraph graph;
        (void)graph.addPass({"No Edges"});
        expectThrowsRuntimeError("compile rejects pass with no resource edges", [&] {
            graph.compile();
        });
    }

    {
        FrameGraph graph;
        const auto localImage = graph.addResource({"Local Image", ve::FrameGraphResourceKind::Image, false});
        const auto writePass = graph.addPass({"Write pass"});
        graph.write(writePass, localImage, FrameGraphUsage::ColorAttachment);
        graph.compile();

        expectThrowsRuntimeError("finalUsage throws when usage not set", [&] {
            (void)graph.finalUsage(localImage);
        });
    }

    {
        FrameGraph graph;
        const auto importedImage = graph.addResource({"Imported Image", ve::FrameGraphResourceKind::Image, true});
        const auto localImage = graph.addResource({"Local Image", ve::FrameGraphResourceKind::Image, false});

        const auto samplePass = graph.addPass({"Sample External"});
        const auto writePass = graph.addPass({"Write Local"});
        const auto presentPass = graph.addPass({"Read Local"});

        graph.read(samplePass, importedImage, FrameGraphUsage::SampledImage);
        graph.write(writePass, localImage, FrameGraphUsage::ColorAttachment);
        graph.read(presentPass, localImage, FrameGraphUsage::SampledImage);

        expectNoThrow("compile for hasEdge behavior fixture", [&] {
            graph.compile();
        });

        expectEqual("hasEdge finds imported read edge",
                    graph.hasEdge(samplePass, importedImage, FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                    true);
        expectEqual("hasEdge finds local write edge",
                    graph.hasEdge(writePass, localImage, FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment),
                    true);
        expectEqual("hasEdge finds local read edge",
                    graph.hasEdge(presentPass, localImage, FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                    true);
        expectEqual("hasEdge rejects swapped access", graph.hasEdge(writePass, localImage, FrameGraphAccess::Read, FrameGraphUsage::ColorAttachment), false);
        expectEqual("hasEdge rejects swapped pass", graph.hasEdge(presentPass, importedImage, FrameGraphAccess::Read, FrameGraphUsage::SampledImage), false);
    }
    {
        FrameGraph graph;
        const auto localImage = graph.addResource({"Local Image", ve::FrameGraphResourceKind::Image, false});
        const auto writePass = graph.addPass({"Duplicate Write"});
        graph.write(writePass, localImage, FrameGraphUsage::ColorAttachment);

        expectThrowsRuntimeError("write() rejects duplicate exact edge", [&] {
            graph.write(writePass, localImage, FrameGraphUsage::ColorAttachment);
        });
    }

    {
        FrameGraph graph;
        const auto importedImage = graph.addResource({"Imported Image", ve::FrameGraphResourceKind::Image, true});
        const auto readPass = graph.addPass({"Duplicate Read"});
        graph.read(readPass, importedImage, FrameGraphUsage::SampledImage);

        expectThrowsRuntimeError("read() rejects duplicate exact edge", [&] {
            graph.read(readPass, importedImage, FrameGraphUsage::SampledImage);
        });
    }

    {
        FrameGraph graph;
        const auto firstPass = graph.addPass({"Edge Writer"});
        const auto overflowPass = graph.addPass({"Overflowing Pass"});

        const auto edgeResource0 = graph.addResource({"Edge Resource 0", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource1 = graph.addResource({"Edge Resource 1", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource2 = graph.addResource({"Edge Resource 2", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource3 = graph.addResource({"Edge Resource 3", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource4 = graph.addResource({"Edge Resource 4", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource5 = graph.addResource({"Edge Resource 5", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource6 = graph.addResource({"Edge Resource 6", ve::FrameGraphResourceKind::Image, false});
        const auto edgeResource7 = graph.addResource({"Edge Resource 7", ve::FrameGraphResourceKind::Image, false});

        const FrameGraph::ResourceHandle resources[FrameGraph::kMaxResources] = {
            edgeResource0,
            edgeResource1,
            edgeResource2,
            edgeResource3,
            edgeResource4,
            edgeResource5,
            edgeResource6,
            edgeResource7,
        };

        const FrameGraphUsage usages[] = {
            FrameGraphUsage::ColorAttachment,
            FrameGraphUsage::DepthAttachment,
            FrameGraphUsage::SampledImage,
        };

        expectNoThrow("write() fills capacity with unique edge tuples", [&] {
            for (std::size_t edgeIndex = 0; edgeIndex < FrameGraph::kMaxEdges; ++edgeIndex) {
                graph.write(
                    firstPass,
                    resources[edgeIndex % FrameGraph::kMaxResources],
                    usages[edgeIndex % 3]);
            }
        });

        expectThrowsRuntimeError("write() rejects edge capacity overflow", [&] {
            graph.write(overflowPass, edgeResource0, FrameGraphUsage::ColorAttachment);
        });
    }

    {
        FrameGraph graph;
        expectNoThrow("adding resource exactly at capacity succeeds", [&] {
            for (std::size_t resourceIndex = 0; resourceIndex < FrameGraph::kMaxResources; ++resourceIndex) {
                (void)graph.addResource({"Extra", ve::FrameGraphResourceKind::Image, false});
            }
        });
        expectThrowsRuntimeError("addResource() rejects resource overflow after capacity", [&] {
            (void)graph.addResource({"Overflow", ve::FrameGraphResourceKind::Image, false});
        });
    }

    {
        FrameGraph graph;
        expectNoThrow("adding pass exactly at capacity succeeds", [&] {
            for (std::size_t passIndex = 0; passIndex < FrameGraph::kMaxPasses; ++passIndex) {
                (void)graph.addPass({"Pass"});
            }
        });
        expectThrowsRuntimeError("addPass() rejects pass overflow after capacity", [&] {
            (void)graph.addPass({"Overflow"});
        });
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "FrameGraph CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
