#include "renderer/FrameGraph.hpp"
#include "renderer/FrameGraphTopology.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

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
    expectEqual("variant index depth-on no screenshot", ve::FrameGraphVariantPolicy::index(true, false), static_cast<std::size_t>(1));
    expectEqual("variant index depth-off screenshot", ve::FrameGraphVariantPolicy::index(false, true), static_cast<std::size_t>(2));
    expectEqual("force-on exposes only depth-on topology", ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::ForceOn, true), true);
    expectEqual("force-on rejects depth-off topology", ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::ForceOn, false), false);
    expectEqual("force-off rejects depth-on topology", ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::ForceOff, true), false);
    expectEqual("force-off exposes only depth-off topology", ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::ForceOff, false), true);
    expectEqual("auto exposes both depth topologies", ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::Auto, true) &&
                                                        ve::FrameGraphVariantPolicy::depthVariantAvailable(ve::DepthPrepassMode::Auto, false), true);
    {
        FrameGraph graph;
        const auto image = graph.addResource({"Usage Validation Image", ve::FrameGraphResourceKind::Image, true});
        const auto pass = graph.addPass({"Usage Validation Pass"});
        expectNoThrow("sampled image reads are valid", [&] {
            graph.read(pass, image, FrameGraphUsage::SampledImage);
        });
        expectThrowsRuntimeError("sampled image writes are rejected", [&] {
            graph.write(pass, image, FrameGraphUsage::SampledImage);
        });
        expectThrowsRuntimeError("transfer source writes are rejected", [&] {
            graph.write(pass, image, FrameGraphUsage::TransferSource);
        });
        expectThrowsRuntimeError("present writes are rejected", [&] {
            graph.write(pass, image, FrameGraphUsage::Present);
        });
        expectThrowsRuntimeError("invalid frame-graph usage is rejected", [&] {
            graph.read(pass, image, static_cast<FrameGraphUsage>(0xff));
        });
    }

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
        expectEqual("compiled execution plan has every pass", graph.executionOrder().size(), static_cast<std::size_t>(4));
        expectEqual("execution plan starts with depth dependency", graph.executionOrder()[0].index, depthPass.index);
        expectEqual("execution plan orders hdr after depth", graph.executionOrder()[1].index, hdrPass.index);
        expectEqual("execution plan orders tonemap after hdr", graph.executionOrder()[2].index, tonemapPass.index);
        expectEqual("execution plan ends with screenshot readback", graph.executionOrder()[3].index, screenshotPass.index);
        const FrameGraph::ResourceLifetime depthLifetime = graph.lifetime(depth);
        expectEqual("depth lifetime starts at prepass", depthLifetime.firstPass.index, depthPass.index);
        expectEqual("depth lifetime ends at hdr pass", depthLifetime.lastPass.index, hdrPass.index);
        expectEqual("depth lifetime is marked used", depthLifetime.used, true);
        const FrameGraph::ResourceLifetime hdrLifetime = graph.lifetime(hdr);
        expectEqual("hdr lifetime starts at hdr pass", hdrLifetime.firstPass.index, hdrPass.index);
        expectEqual("hdr lifetime ends at tonemap", hdrLifetime.lastPass.index, tonemapPass.index);
        const FrameGraph::ResourceLifetime swapchainLifetime = graph.lifetime(swapchain);
        expectEqual("swapchain lifetime starts at tonemap", swapchainLifetime.firstPass.index, tonemapPass.index);
        expectEqual("swapchain lifetime ends at screenshot", swapchainLifetime.lastPass.index, screenshotPass.index);
        const auto& barrierPlan = graph.barrierPlan();
        expectEqual("barrier plan contains first-use and changed-state intents", barrierPlan.size(), static_cast<std::size_t>(7));
        expectEqual("first barrier intent is depth prepass write", barrierPlan[0].pass.index, depthPass.index);
        expectEqual("first barrier intent targets depth", barrierPlan[0].resource.index, depth.index);
        const FrameGraph::BarrierIntent& depthWriteIntent = graph.barrierIntent(
            depthPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment);
        expectEqual("depth write intent targets prepass", depthWriteIntent.pass.index, depthPass.index);
        expectEqual("depth first-use write has no graph predecessor", depthWriteIntent.hasPrevious, false);
        expectEqual("depth write intent is not final", depthWriteIntent.finalTransition, false);
        expectEqual("depth read transition follows depth write", barrierPlan[1].pass.index, hdrPass.index);
        expectEqual("depth read transition records previous write", barrierPlan[1].hasPrevious, true);
        const FrameGraph::BarrierIntent& depthReadIntent = graph.barrierIntent(
            hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment);
        expectEqual("depth read intent targets HDR pass", depthReadIntent.pass.index, hdrPass.index);
        expectEqual("depth read intent records depth write predecessor", static_cast<int>(depthReadIntent.previousUsage), static_cast<int>(FrameGraphUsage::DepthAttachment));
        expectEqual("depth read intent is not final", depthReadIntent.finalTransition, false);
        expectEqual("hdr sampled transition follows hdr write", barrierPlan[3].pass.index, tonemapPass.index);
        const FrameGraph::BarrierIntent& hdrWriteIntent = graph.barrierIntent(
            hdrPass, hdr, FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment);
        expectEqual("HDR write intent targets HDR pass", hdrWriteIntent.pass.index, hdrPass.index);
        expectEqual("HDR first-use write has no graph predecessor", hdrWriteIntent.hasPrevious, false);
        expectEqual("HDR write intent is not final", hdrWriteIntent.finalTransition, false);
        const FrameGraph::BarrierIntent& hdrSampleIntent = graph.barrierIntent(
            tonemapPass, hdr, FrameGraphAccess::Read, FrameGraphUsage::SampledImage);
        expectEqual("HDR sample intent targets tonemap", hdrSampleIntent.pass.index, tonemapPass.index);
        expectEqual("HDR sample intent records color-write predecessor", static_cast<int>(hdrSampleIntent.previousUsage), static_cast<int>(FrameGraphUsage::ColorAttachment));
        expectEqual("HDR sample intent is not final", hdrSampleIntent.finalTransition, false);
        const FrameGraph::BarrierIntent& swapchainWriteIntent = graph.barrierIntent(
            tonemapPass, swapchain, FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment);
        expectEqual("swapchain write intent targets tonemap", swapchainWriteIntent.pass.index, tonemapPass.index);
        expectEqual("swapchain first-use write has no graph predecessor", swapchainWriteIntent.hasPrevious, false);
        expectEqual("swapchain write intent is not final", swapchainWriteIntent.finalTransition, false);
        expectEqual("final present transition is emitted last", barrierPlan.back().finalTransition, true);
        expectEqual("final transition targets swapchain", barrierPlan.back().resource.index, swapchain.index);
        const FrameGraph::BarrierIntent& finalIntent = graph.finalBarrierIntent(swapchain);
        expectEqual("final barrier query returns present intent", static_cast<int>(finalIntent.usage), static_cast<int>(FrameGraphUsage::Present));
        expectEqual("final present intent is read-only", static_cast<int>(finalIntent.access), static_cast<int>(FrameGraphAccess::Read));
        expectEqual("final barrier query identifies final transition", finalIntent.finalTransition, true);
        expectEqual("final barrier query has no destination pass", finalIntent.pass.valid(), false);
        const FrameGraph::BarrierIntent& screenshotIntent = graph.barrierIntent(
            screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
        expectEqual("screenshot intent targets screenshot pass", screenshotIntent.pass.index, screenshotPass.index);
        expectEqual("screenshot intent is not final", screenshotIntent.finalTransition, false);
        expectEqual("screenshot intent records previous color write", static_cast<int>(screenshotIntent.previousUsage), static_cast<int>(FrameGraphUsage::ColorAttachment));
        expectThrowsRuntimeError("invalidated barrier plan unavailable", [&] {
            graph.setFinalUsage(swapchain, FrameGraphUsage::Present);
            (void)graph.barrierPlan();
        });
        graph.setFinalUsage(swapchain, FrameGraphUsage::Present);
        expectEqual("graph mutation invalidates execution plan", graph.compiled(), false);
        expectThrowsRuntimeError("invalidated final barrier unavailable", [&] {
            (void)graph.finalBarrierIntent(swapchain);
        });
        expectThrowsRuntimeError("invalidated lifetime unavailable", [&] {
            (void)graph.lifetime(depth);
        });
        expectEqual("invalidated execution plan is empty", graph.executionOrder().empty(), true);
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
        const FrameGraph::BarrierIntent& depthWriteIntent = graph.barrierIntent(
            hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment);
        expectEqual("depth-off write intent targets HDR pass", depthWriteIntent.pass.index, hdrPass.index);
        expectEqual("depth-off first-use write has no graph predecessor", depthWriteIntent.hasPrevious, false);
        expectEqual("depth-off write intent is not final", depthWriteIntent.finalTransition, false);
    }
    {
        FrameGraph graph;
        const auto image = graph.addResource({"Imported Image", ve::FrameGraphResourceKind::Image, true});
        const auto readPass = graph.addPass({"Read"});
        const auto firstWrite = graph.addPass({"First Write"});
        const auto secondWrite = graph.addPass({"Second Write"});
        graph.read(readPass, image, FrameGraphUsage::SampledImage);
        graph.write(firstWrite, image, FrameGraphUsage::ColorAttachment);
        graph.write(secondWrite, image, FrameGraphUsage::ColorAttachment);
        graph.compile();
        expectEqual("hazard plan retains read-before-write WAR order", graph.executionOrder()[0].index, readPass.index);
        expectEqual("hazard plan retains first WAW writer", graph.executionOrder()[1].index, firstWrite.index);
        expectEqual("hazard plan retains second WAW writer", graph.executionOrder()[2].index, secondWrite.index);
        const FrameGraph::BarrierIntent& secondWriteIntent = graph.barrierIntent(
            secondWrite, image, FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment);
        expectEqual("same-state WAW emits a second barrier intent", secondWriteIntent.pass.index, secondWrite.index);
        expectEqual("same-state WAW records a previous access", secondWriteIntent.hasPrevious, true);
        expectEqual("same-state WAW records the previous write access",
                    static_cast<int>(secondWriteIntent.previousAccess), static_cast<int>(FrameGraphAccess::Write));
        expectEqual("same-state WAW records the previous write usage",
                    static_cast<int>(secondWriteIntent.previousUsage), static_cast<int>(FrameGraphUsage::ColorAttachment));
    }
    {
        FrameGraph graph;
        const auto image = graph.addResource({"Interleaved Image", ve::FrameGraphResourceKind::Image, false});
        const auto writer = graph.addPass({"Writer"});
        const auto reader = graph.addPass({"Reader"});
        graph.read(reader, image, FrameGraphUsage::SampledImage);
        graph.write(writer, image, FrameGraphUsage::ColorAttachment);
        expectNoThrow("interleaved edge declarations preserve pass ordering", [&] {
            graph.compile();
        });
        expectEqual("interleaved plan places writer first", graph.executionOrder()[0].index, writer.index);
        expectEqual("interleaved plan places reader second", graph.executionOrder()[1].index, reader.index);
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
        const auto importedImage = graph.addResource({"Conflicting Image", ve::FrameGraphResourceKind::Image, true});
        const auto pass = graph.addPass({"Conflicting Pass"});
        graph.read(pass, importedImage, FrameGraphUsage::SampledImage);

        expectThrowsRuntimeError("same-pass read/write resource conflict is rejected", [&] {
            graph.write(pass, importedImage, FrameGraphUsage::ColorAttachment);
        });
        expectThrowsRuntimeError("same-pass read usage conflict is rejected", [&] {
            graph.read(pass, importedImage, FrameGraphUsage::TransferSource);
        });
        expectEqual("same-pass conflicts do not append edges", graph.edgeCount(), static_cast<std::size_t>(1));
    }

    {
        FrameGraph graph;
        constexpr std::size_t kLargeGraphCount = 300;
        std::vector<FrameGraph::ResourceHandle> resources;
        std::vector<FrameGraph::PassHandle> passes;
        resources.reserve(kLargeGraphCount);
        passes.reserve(kLargeGraphCount);

        expectNoThrow("adding resources and passes beyond old fixed caps succeeds", [&] {
            for (std::size_t index = 0; index < kLargeGraphCount; ++index) {
                resources.push_back(graph.addResource({"Scalable Resource", ve::FrameGraphResourceKind::Image, false}));
                passes.push_back(graph.addPass({"Scalable Pass"}));
                graph.write(passes.back(), resources.back(), FrameGraphUsage::ColorAttachment);
                if (index > 0) {
                    graph.read(passes.back(), resources[index - 1U], FrameGraphUsage::SampledImage);
                }
            }
        });

        expectEqual("large graph resource count", graph.resourceCount(), kLargeGraphCount);
        expectEqual("large graph pass count", graph.passCount(), kLargeGraphCount);
        expectEqual("large graph edge count", graph.edgeCount(), (kLargeGraphCount * 2U) - 1U);
        expectEqual("wide resource handle keeps index above uint8 range", resources.back().index, static_cast<FrameGraph::Index>(kLargeGraphCount - 1U));
        expectEqual("wide pass handle keeps index above uint8 range", passes.back().index, static_cast<FrameGraph::Index>(kLargeGraphCount - 1U));
        expectNoThrow("large graph compiles beyond old fixed caps", [&] {
            graph.compile();
        });
        expectEqual("large graph read edge survives", graph.hasEdge(passes.back(), resources[kLargeGraphCount - 2U], FrameGraphAccess::Read, FrameGraphUsage::SampledImage), true);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "FrameGraph CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
