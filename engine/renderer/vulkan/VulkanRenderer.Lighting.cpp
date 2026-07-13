#include "renderer/vulkan/VulkanRendererImpl.hpp"
#include <algorithm>
#include <array>

#include <bit>
#include <cstring>
#include <cmath>
#include <vector>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

constexpr float kShadowGuardPixels = 2.0F;
constexpr float kInverseTau = 1.0F / (2.0F * 3.14159265359F);

[[nodiscard]] Vec4 transformPoint(const Mat4& matrix,
                                  const Vec3 point) noexcept {
    return {
        matrix.m[0] * point.x + matrix.m[4] * point.y +
            matrix.m[8] * point.z + matrix.m[12],
        matrix.m[1] * point.x + matrix.m[5] * point.y +
            matrix.m[9] * point.z + matrix.m[13],
        matrix.m[2] * point.x + matrix.m[6] * point.y +
            matrix.m[10] * point.z + matrix.m[14],
        matrix.m[3] * point.x + matrix.m[7] * point.y +
            matrix.m[11] * point.z + matrix.m[15],
    };
}

[[nodiscard]] Mat4 orthographicReverseZ(
    const float left, const float right, const float bottom, const float top,
    const float farZ, const float nearZ) {
    if (!(right > left && top > bottom && nearZ > farZ)) {
        throw std::runtime_error("Shadow orthographic bounds are invalid");
    }
    Mat4 result{};
    result.m[0] = 2.0F / (right - left);
    result.m[5] = -2.0F / (top - bottom);
    result.m[10] = 1.0F / (nearZ - farZ);
    result.m[12] = -(right + left) / (right - left);
    result.m[13] = (top + bottom) / (top - bottom);
    result.m[14] = -farZ / (nearZ - farZ);
    result.m[15] = 1.0F;
    return result;
}

[[nodiscard]] Vec4 shadowAtlasRect(const std::uint32_t slot) noexcept {
    const std::uint32_t column = slot % 4U;
    const std::uint32_t row = slot / 4U;
    const float atlasExtent = static_cast<float>(kShadowAtlasExtent);
    const float usableExtent =
        static_cast<float>(kShadowAtlasSlotExtent) -
        2.0F * kShadowGuardPixels;
    return {
        usableExtent / atlasExtent, usableExtent / atlasExtent,
        (static_cast<float>(column * kShadowAtlasSlotExtent) +
         kShadowGuardPixels) /
            atlasExtent,
        (static_cast<float>(row * kShadowAtlasSlotExtent) +
         kShadowGuardPixels) /
            atlasExtent,
    };
}

struct ShadowMatrixPlan {
    std::array<Mat4, kShadowAtlasSlotCount> matrices{};
    std::array<Vec4, kShadowAtlasSlotCount> atlasRects{};
    Vec4 cascadeSplits{};
    ShadowAtlasAssignment assignment{};
    std::uint32_t viewCount = 0;
};

[[nodiscard]] ShadowMatrixPlan buildShadowMatrixPlan(
    const Camera& camera, const RenderDirectionalLight& directional,
    const std::span<const RenderLocalLight> lights,
    const bool shadowsEnabled) {
    ShadowMatrixPlan result;
    if (!shadowsEnabled) {
        result.assignment.directionalCascadeCount = 0U;
        result.assignment.localShadowCount = 0U;
        result.assignment.localLightSlots.fill(-1);
        return result;
    }
    result.assignment = assignShadowAtlasSlots(directional, lights);

    const float nearDistance = camera.nearPlane();
    const float farDistance = std::min(camera.farPlane(), 120.0F);
    std::array<float, kDirectionalShadowCascadeCount + 1U> splitDistances{};
    splitDistances[0] = nearDistance;
    constexpr float splitLambda = 0.65F;
    for (std::uint32_t cascade = 1U;
         cascade <= kDirectionalShadowCascadeCount; ++cascade) {
        const float fraction =
            static_cast<float>(cascade) /
            static_cast<float>(kDirectionalShadowCascadeCount);
        const float logarithmic =
            nearDistance * std::pow(farDistance / nearDistance, fraction);
        const float uniform =
            nearDistance + (farDistance - nearDistance) * fraction;
        splitDistances[cascade] =
            logarithmic * splitLambda + uniform * (1.0F - splitLambda);
    }
    result.cascadeSplits = {
        splitDistances[1], splitDistances[2], splitDistances[3],
        splitDistances[kDirectionalShadowCascadeCount]};

    const Vec3 cameraForward = camera.forward();
    const Vec3 cameraRight = camera.right();
    const Vec3 cameraUp = normalize(cross(cameraRight, cameraForward));
    const Vec3 cameraPosition = camera.position();
    const Vec3 lightDirection{
        directional.directionIntensity.x,
        directional.directionIntensity.y,
        directional.directionIntensity.z};
    const Vec3 lightUp = std::abs(lightDirection.y) > 0.95F
                             ? Vec3{1.0F, 0.0F, 0.0F}
                             : Vec3{0.0F, 1.0F, 0.0F};
    const float tangent = std::tan(camera.verticalFov() * 0.5F);

    for (std::uint32_t cascade = 0U;
         cascade < result.assignment.directionalCascadeCount; ++cascade) {
        std::array<Vec3, 8> corners{};
        std::size_t cornerIndex = 0U;
        for (const float distance :
             {splitDistances[cascade], splitDistances[cascade + 1U]}) {
            const float halfHeight = tangent * distance;
            const float halfWidth = halfHeight * camera.aspect();
            const Vec3 center = cameraPosition + cameraForward * distance;
            for (const float y : {-1.0F, 1.0F}) {
                for (const float x : {-1.0F, 1.0F}) {
                    corners[cornerIndex++] =
                        center + cameraRight * (x * halfWidth) +
                        cameraUp * (y * halfHeight);
                }
            }
        }
        Vec3 center{};
        for (const Vec3 corner : corners) center = center + corner;
        center = center * (1.0F / static_cast<float>(corners.size()));
        float radius = 0.0F;
        for (const Vec3 corner : corners) {
            radius = std::max(radius, length(corner - center));
        }
        radius = std::ceil(radius * 16.0F) / 16.0F;
        const Mat4 lightView =
            lookAt(center - lightDirection * (radius + 80.0F), center,
                   lightUp);
        float minimumX = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = std::numeric_limits<float>::lowest();
        for (const Vec3 corner : corners) {
            const Vec4 lightSpace = transformPoint(lightView, corner);
            minimumX = std::min(minimumX, lightSpace.x);
            maximumX = std::max(maximumX, lightSpace.x);
            minimumY = std::min(minimumY, lightSpace.y);
            maximumY = std::max(maximumY, lightSpace.y);
            minimumZ = std::min(minimumZ, lightSpace.z);
            maximumZ = std::max(maximumZ, lightSpace.z);
        }
        const float halfExtent =
            std::max(maximumX - minimumX, maximumY - minimumY) * 0.525F;
        const float usablePixels =
            static_cast<float>(kShadowAtlasSlotExtent) -
            2.0F * kShadowGuardPixels;
        const float texelWorldSize = (2.0F * halfExtent) / usablePixels;
        const float centerX =
            std::round((minimumX + maximumX) * 0.5F / texelWorldSize) *
            texelWorldSize;
        const float centerY =
            std::round((minimumY + maximumY) * 0.5F / texelWorldSize) *
            texelWorldSize;
        const Mat4 projection = orthographicReverseZ(
            centerX - halfExtent, centerX + halfExtent,
            centerY - halfExtent, centerY + halfExtent, minimumZ - 50.0F,
            maximumZ + 50.0F);
        result.matrices[cascade] = projection * lightView;
        result.atlasRects[cascade] = shadowAtlasRect(cascade);
    }

    for (std::size_t lightIndex = 0; lightIndex < lights.size();
         ++lightIndex) {
        const std::int32_t slot =
            result.assignment.localLightSlots[lightIndex];
        if (slot < 0) continue;
        const RenderLocalLight& light = lights[lightIndex];
        const Vec3 position{light.positionRange.x, light.positionRange.y,
                            light.positionRange.z};
        const Vec3 direction{light.directionOuterCone.x,
                             light.directionOuterCone.y,
                             light.directionOuterCone.z};
        const Vec3 up = std::abs(direction.y) > 0.95F
                            ? Vec3{1.0F, 0.0F, 0.0F}
                            : Vec3{0.0F, 1.0F, 0.0F};
        const float fieldOfView =
            2.0F * std::acos(std::clamp(light.directionOuterCone.w,
                                        0.01F, 0.999F));
        result.matrices[static_cast<std::size_t>(slot)] =
            perspective(fieldOfView, 1.0F, 0.05F, light.positionRange.w) *
            lookAt(position, position + direction, up);
        result.atlasRects[static_cast<std::size_t>(slot)] =
            shadowAtlasRect(static_cast<std::uint32_t>(slot));
    }
    result.viewCount =
        result.assignment.directionalCascadeCount +
        result.assignment.localShadowCount;
    return result;
}

} // namespace

void VulkanRenderer::Impl::createShadowResources() {
    resourceOwner_.shadowAtlas = createImage(
        {kShadowAtlasExtent, kShadowAtlasExtent}, findShadowDepthFormat(),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    setObjectName(VK_OBJECT_TYPE_IMAGE,
                  handleToUint64(resourceOwner_.shadowAtlas.image),
                  "Shadow Atlas Image");
    setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                  handleToUint64(resourceOwner_.shadowAtlas.view),
                  "Shadow Atlas View");
    vmaSetAllocationName(deviceOwner_.allocator,
                         resourceOwner_.shadowAtlas.allocation,
                         "Shadow Atlas Allocation");
    resourceOwner_.shadowAtlas.resourceId =
        resourceOwner_.registry.registerResource(
            GpuResourceKind::Image, "Shadow Atlas",
            resourceOwner_.shadowAtlas.allocationBytes);
}

void VulkanRenderer::Impl::updateFrameLightingDescriptors(
    const std::size_t frameIndex) const {
    const FrameResources& frame = frameOwner_.frames.at(frameIndex);
    std::array<VkDescriptorBufferInfo, 6> infos{};
    infos[0] = {frame.localLights.buffer, 0U, frame.localLights.size};
    infos[1] = {frame.lightTileHeaders.buffer, 0U,
                frame.lightTileHeaders.size};
    infos[2] = {frame.lightTileIndices.buffer, 0U,
                frame.lightTileIndices.size};
    infos[3] = {frame.lightingUniforms.buffer, 0U,
                sizeof(GpuLightingUniforms)};
    infos[4] = {frame.lightListCounters.buffer, 0U,
                sizeof(GpuLightListCounters)};
    infos[5] = {frame.shadowInstanceIndices.buffer, 0U,
                frame.shadowInstanceIndices.size};
    const VkDescriptorImageInfo shadowInfo{
        resourceOwner_.shadowSampler, resourceOwner_.shadowAtlas.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo environmentInfo{
        resourceOwner_.environmentSampler,
        resourceOwner_.environmentMap.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo depthPyramidInfo{
        resourceOwner_.linearSampler, resourceOwner_.depthPyramid.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 9> writes{};
    for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet =
            resourceOwner_.lightingDescriptorSets[frameIndex];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1U;
        if (binding == 5U || binding == 7U || binding == 8U) {
            writes[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = binding == 5U ? &shadowInfo
                : binding == 7U ? &environmentInfo
                                : &depthPyramidInfo;
        } else {
            writes[binding].descriptorType =
                binding == 3U ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo =
                &infos[binding == 6U ? 5U : binding];
        }
    }
    vkUpdateDescriptorSets(deviceOwner_.device,
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0U, nullptr);
}

void VulkanRenderer::Impl::ensureLightTileCapacity(
    FrameResources& frame, const std::size_t frameIndex,
    const std::size_t requiredTileCount) {
    if (requiredTileCount <= frame.lightTileCapacity) return;
    constexpr std::size_t maximumPowerOfTwo =
        std::bit_floor(std::numeric_limits<std::size_t>::max());
    if (requiredTileCount > maximumPowerOfTwo) {
        throw std::runtime_error("Forward+ tile capacity overflows size_t");
    }
    const std::size_t capacity = std::bit_ceil(requiredTileCount);
    constexpr VkDeviceSize indexStride =
        sizeof(std::uint32_t) * kMaximumLightsPerTile;
    constexpr VkDeviceSize maximumBufferSize =
        std::numeric_limits<VkDeviceSize>::max();
    if (capacity > maximumBufferSize / sizeof(LightTileHeader) ||
        capacity > maximumBufferSize / indexStride) {
        throw std::runtime_error(
            "Forward+ tile buffer size overflows VkDeviceSize");
    }
    const VkDeviceSize headerBytes =
        sizeof(LightTileHeader) * static_cast<VkDeviceSize>(capacity);
    const VkDeviceSize indexBytes =
        indexStride * static_cast<VkDeviceSize>(capacity);
    const VkDeviceSize limit =
        deviceOwner_.physicalDeviceProperties.limits.maxStorageBufferRange;
    if (headerBytes > limit || indexBytes > limit) {
        throw std::runtime_error("Forward+ tile storage exceeds Vulkan limits");
    }

    Buffer headers;
    Buffer indices;
    try {
        headers = createBuffer(headerBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        indices = createBuffer(indexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (...) {
        destroyBuffer(headers);
        destroyBuffer(indices);
        throw;
    }
    const std::string prefix = "Frame " + std::to_string(frameIndex);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(headers.buffer),
                  prefix + " Light Tile Header Buffer");
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(indices.buffer),
                  prefix + " Light Tile Index Buffer");
    headers.resourceId = resourceOwner_.registry.registerResource(
        GpuResourceKind::Buffer, prefix + " Light Tile Header Buffer",
        headers.size);
    indices.resourceId = resourceOwner_.registry.registerResource(
        GpuResourceKind::Buffer, prefix + " Light Tile Index Buffer",
        indices.size);

    destroyBuffer(frame.lightTileHeaders);
    destroyBuffer(frame.lightTileIndices);
    frame.lightTileHeaders = headers;
    frame.lightTileIndices = indices;
    frame.lightTileCapacity = capacity;
    updateFrameLightingDescriptors(frameIndex);
    logger()->info("Grew frame {} Forward+ capacity to {} tiles", frameIndex,
                   capacity);
}

void VulkanRenderer::Impl::ensureShadowCasterCapacity(
    FrameResources& frame, const std::size_t frameIndex,
    const std::size_t requiredInstanceCount) {
    if (requiredInstanceCount <= frame.shadowInstanceIndexCapacity) return;
    constexpr std::size_t maximumPowerOfTwo =
        std::bit_floor(std::numeric_limits<std::size_t>::max());
    if (requiredInstanceCount > maximumPowerOfTwo) {
        throw std::runtime_error("Shadow caster capacity overflows size_t");
    }
    const std::size_t capacity = std::bit_ceil(requiredInstanceCount);
    if (capacity > std::numeric_limits<VkDeviceSize>::max() /
                       sizeof(std::uint32_t)) {
        throw std::runtime_error(
            "Shadow caster buffer size overflows VkDeviceSize");
    }
    const VkDeviceSize bytes =
        sizeof(std::uint32_t) * static_cast<VkDeviceSize>(capacity);
    if (bytes >
        deviceOwner_.physicalDeviceProperties.limits.maxStorageBufferRange) {
        throw std::runtime_error(
            "Shadow caster storage exceeds Vulkan limits");
    }
    Buffer replacement = createBuffer(
        bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        checkVk(vmaMapMemory(deviceOwner_.allocator, replacement.allocation,
                             &replacement.mapped),
                "vmaMapMemory shadow instance indices");
        const std::string name =
            "Frame " + std::to_string(frameIndex) +
            " Shadow Instance Index Buffer";
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(replacement.buffer), name);
        vmaSetAllocationName(deviceOwner_.allocator, replacement.allocation,
                             (name + " Allocation").c_str());
        replacement.resourceId =
            resourceOwner_.registry.registerResource(
                GpuResourceKind::Buffer, name, replacement.size);
    } catch (...) {
        destroyBuffer(replacement);
        throw;
    }
    destroyBuffer(frame.shadowInstanceIndices);
    frame.shadowInstanceIndices = replacement;
    frame.shadowInstanceIndexCapacity = capacity;
    updateFrameLightingDescriptors(frameIndex);
}

void VulkanRenderer::Impl::prepareShadowCasters(
    FrameResources& frame, const std::size_t frameIndex,
    const Camera&, const SceneRenderList& renderItems) {
    if (!indirectSceneDrawsEnabled_ || !config_.shadows ||
        frame.shadowViewCount == 0U) {
        frame.shadowCommandCounts.fill(0U);
        frame.shadowViewCount = 0U;
        frame.shadowHasAlphaMaskedCasters = false;
        frame.shadowCasterCacheValid = false;
        frame.cachedShadowViewCount = 0U;
        return;
    }
    if (renderItems.size() > std::numeric_limits<std::uint32_t>::max() ||
        renderItems.size() >
            std::numeric_limits<std::size_t>::max() / frame.shadowViewCount) {
        throw std::runtime_error("Shadow caster count overflows renderer limits");
    }
    const std::size_t maximumIndexCount =
        renderItems.size() * frame.shadowViewCount;
    if (maximumIndexCount > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "Per-view shadow caster indices exceed uint32 range");
    }
    ensureShadowCasterCapacity(frame, frameIndex, maximumIndexCount);

    const auto* lighting =
        static_cast<const GpuLightingUniforms*>(frame.lightingUniforms.mapped);
    const bool matricesUnchanged =
        frame.cachedShadowViewCount == frame.shadowViewCount &&
        std::equal(
            lighting->shadowViewProjection.begin(),
            lighting->shadowViewProjection.begin() + frame.shadowViewCount,
            frame.cachedShadowViewProjection.begin(), sameMatrix);
    if (frame.shadowCasterCacheValid &&
        !frame.shadowCasterLayoutChangedThisFrame && matricesUnchanged) {
        return;
    }

    const std::size_t meshCount = resourceOwner_.sceneMeshes.size();
    frame.shadowCountScratch.resize(meshCount);
    frame.shadowCursorScratch.resize(meshCount);
    frame.shadowIndexScratch.resize(maximumIndexCount);
    frame.shadowVisibleItemScratch.reserve(renderItems.size());
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(
        frame.shadowIndirectCommands.mapped);
    std::size_t nextIndex = 0U;
    std::size_t nextCommand = 0U;
    frame.shadowHasAlphaMaskedCasters = false;

    const auto meshIndexFor = [&](const SceneRenderItem& item,
                                  const float shadowPixelScale) {
        const bool farSphere =
            shadowPixelScale > 0.0F &&
            item.boundsRadius * shadowPixelScale < 4.32F;
        return item.mesh == builtin_assets::kSphere
                   ? sceneMeshBatchIndex(
                         farSphere ? SceneMeshBatchId::SphereLow
                                   : SceneMeshBatchId::SphereShadow)
                   : meshBatchIndex(item.mesh);
    };
    for (std::uint32_t viewIndex = 0U;
         viewIndex < frame.shadowViewCount; ++viewIndex) {
        frame.shadowCommandOffsets[viewIndex] =
            static_cast<std::uint32_t>(nextCommand);
        const Frustum frustum =
            extractFrustumPlanes(lighting->shadowViewProjection[viewIndex]);
        float shadowPixelScale = 0.0F;
        if (lighting->directional.parameters[0] != 0U &&
            viewIndex < kDirectionalShadowCascadeCount) {
            const Mat4& matrix =
                lighting->shadowViewProjection[viewIndex];
            const float horizontalScale =
                length(Vec3{matrix.m[0], matrix.m[4], matrix.m[8]});
            const float verticalScale =
                length(Vec3{matrix.m[1], matrix.m[5], matrix.m[9]});
            shadowPixelScale =
                0.5F * static_cast<float>(kShadowAtlasSlotExtent) *
                std::max(horizontalScale, verticalScale);
        }
        std::fill(frame.shadowCountScratch.begin(),
                  frame.shadowCountScratch.end(), 0U);
        frame.shadowVisibleItemScratch.clear();
        const auto acceptItem = [&](const std::size_t itemIndex) {
            const SceneRenderItem& item = renderItems[itemIndex];
            frame.shadowVisibleItemScratch.push_back(
                static_cast<std::uint32_t>(itemIndex));
            ++frame.shadowCountScratch[
                meshIndexFor(item, shadowPixelScale)];
            frame.shadowHasAlphaMaskedCasters |=
                (static_cast<std::uint32_t>(item.material.flags.x) &
                 MaterialFeatureAlphaMask) != 0U;
        };
        const auto classifyItem = [&](const std::size_t itemIndex) {
            const SceneRenderItem& item = renderItems[itemIndex];
            if (classifySphereAgainstFrustum(
                    frustum, item.boundsCenter, item.boundsRadius) !=
                FrustumSphereClassification::Outside) {
                acceptItem(itemIndex);
            }
        };

        if (renderItems.materialGridTilesCoverRange()) {
            const SceneGridRange& grid = renderItems.materialGridRange();
            for (std::size_t itemIndex = 0U; itemIndex < grid.firstItem;
                 ++itemIndex) {
                classifyItem(itemIndex);
            }
            for (const SceneGridTile& tile : renderItems.materialGridTiles()) {
                const FrustumSphereClassification tileClassification =
                    classifySphereAgainstFrustum(
                        frustum, tile.boundsCenter, tile.boundsRadius);
                if (tileClassification ==
                    FrustumSphereClassification::Outside) {
                    continue;
                }
                for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd;
                     ++row) {
                    const std::size_t rowBase =
                        grid.firstItem +
                        static_cast<std::size_t>(row) * grid.columns;
                    for (std::uint32_t column = tile.columnBegin;
                         column < tile.columnEnd; ++column) {
                        const std::size_t itemIndex = rowBase + column;
                        if (tileClassification ==
                            FrustumSphereClassification::Inside) {
                            acceptItem(itemIndex);
                        } else {
                            classifyItem(itemIndex);
                        }
                    }
                }
            }
            const std::size_t gridEnd =
                grid.firstItem +
                static_cast<std::size_t>(grid.rows) * grid.columns;
            for (std::size_t itemIndex = gridEnd;
                 itemIndex < renderItems.size(); ++itemIndex) {
                classifyItem(itemIndex);
            }
        } else {
            for (std::size_t itemIndex = 0U;
                 itemIndex < renderItems.size(); ++itemIndex) {
                classifyItem(itemIndex);
            }
        }

        for (std::size_t meshIndex = 0U; meshIndex < meshCount; ++meshIndex) {
            const std::uint32_t instanceCount =
                frame.shadowCountScratch[meshIndex];
            if (instanceCount == 0U) continue;
            const GpuMesh& mesh = resourceOwner_.sceneMeshes[meshIndex];
            frame.shadowCursorScratch[meshIndex] =
                static_cast<std::uint32_t>(nextIndex);
            commands[nextCommand++] = {
                mesh.indexCount, instanceCount, mesh.firstIndex,
                mesh.vertexOffset, static_cast<std::uint32_t>(nextIndex)};
            nextIndex += instanceCount;
        }
        for (const std::uint32_t itemIndex :
             frame.shadowVisibleItemScratch) {
            const SceneRenderItem& item = renderItems[itemIndex];
            frame.shadowIndexScratch[
                frame.shadowCursorScratch[
                    meshIndexFor(item, shadowPixelScale)]++] = itemIndex;
        }
        frame.shadowCommandCounts[viewIndex] =
            static_cast<std::uint32_t>(
                nextCommand - frame.shadowCommandOffsets[viewIndex]);
    }

    frame.shadowIndexScratch.resize(nextIndex);
    if (!frame.shadowIndexScratch.empty()) {
        std::memcpy(frame.shadowInstanceIndices.mapped,
                    frame.shadowIndexScratch.data(),
                    frame.shadowIndexScratch.size() *
                        sizeof(std::uint32_t));
    }
    std::copy_n(lighting->shadowViewProjection.begin(), frame.shadowViewCount,
                frame.cachedShadowViewProjection.begin());
    frame.cachedShadowViewCount = frame.shadowViewCount;
    const VulkanBufferSyncState hostWrite{
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT};
    frame.shadowInstanceIndices.syncState = hostWrite;
    frame.shadowIndirectCommands.syncState = hostWrite;
    frame.shadowCasterCacheValid = true;
}

void VulkanRenderer::Impl::validateLightLists(FrameResources& frame) const {
    if (!frame.submittedOnce) return;
    const auto* counters = static_cast<const GpuLightListCounters*>(
        frame.lightListCounters.mapped);
    frame.completedLightListOverflowCount = counters->overflowCount;
    frame.completedLightListCountersValid = true;
}

void VulkanRenderer::Impl::prepareLighting(
    FrameResources& frame, const std::size_t frameIndex,
    const Camera& camera, const SceneRenderList& renderItems,
    const Mat4& viewProjection) {
    const std::span<const RenderLocalLight> lights = renderItems.localLights();
    const std::span<const RenderReflectionProbe> probes =
        renderItems.reflectionProbes();
    const std::uint32_t columns =
        (swapchainOwner_.extent.width + kLightTileSize - 1U) /
        kLightTileSize;
    const std::uint32_t rows =
        (swapchainOwner_.extent.height + kLightTileSize - 1U) /
        kLightTileSize;
    if (rows != 0U &&
        columns > std::numeric_limits<std::size_t>::max() / rows) {
        throw std::runtime_error("Forward+ tile count overflows size_t");
    }
    const std::size_t tileCount = static_cast<std::size_t>(columns) * rows;
    ensureLightTileCapacity(frame, frameIndex, tileCount);

    const ShadowMatrixPlan shadowPlan = buildShadowMatrixPlan(
        camera, renderItems.directionalLight(), lights,
        indirectSceneDrawsEnabled_ && config_.shadows);
    const auto premultiplyIntensity = [](Vec4& value) noexcept {
        value.x *= value.w;
        value.y *= value.w;
        value.z *= value.w;
        value.w = 1.0F;
    };
    auto* gpuLights =
        static_cast<RenderLocalLight*>(frame.localLights.mapped);
    for (std::size_t index = 0; index < lights.size(); ++index) {
        gpuLights[index] = lights[index];
        premultiplyIntensity(gpuLights[index].colorIntensity);
        // GPU ABI: parameters.y replaces the consumed authoring shadow flag
        // with the invariant needed by the fragment-light loop.
        const float range = gpuLights[index].positionRange.w;
        gpuLights[index].parameters[1] =
            std::bit_cast<std::uint32_t>(1.0F / (range * range));
        if (gpuLights[index].parameters[0] ==
            static_cast<std::uint32_t>(LocalLightType::Spot)) {
            const float innerCosine =
                static_cast<float>(gpuLights[index].parameters[2]) / 65535.0F;
            const float coneWidth =
                innerCosine - gpuLights[index].directionOuterCone.w;
            const float inverseConeWidth =
                coneWidth > 0.0F
                    ? 1.0F / coneWidth
                    : std::numeric_limits<float>::max();
            gpuLights[index].parameters[2] =
                std::bit_cast<std::uint32_t>(inverseConeWidth);
        }
        const std::int32_t slot =
            shadowPlan.assignment.localLightSlots[index];
        gpuLights[index].parameters[3] =
            slot >= 0 ? static_cast<std::uint32_t>(slot) + 1U : 0U;
    }
    GpuLightingUniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    uniforms.directional = renderItems.directionalLight();
    const float directionalIntensity =
        uniforms.directional.directionIntensity.w;
    uniforms.directional.color.x *= directionalIntensity;
    uniforms.directional.color.y *= directionalIntensity;
    uniforms.directional.color.z *= directionalIntensity;
    uniforms.directional.directionIntensity.w = 1.0F;
    if (!indirectSceneDrawsEnabled_ || !config_.shadows) {
        uniforms.directional.parameters[0] = 0U;
    }
    uniforms.environment = renderItems.environment();
    uniforms.environment.parameters.y *= kInverseTau;
    premultiplyIntensity(uniforms.environment.skyColorIntensity);
    premultiplyIntensity(uniforms.environment.groundColorIntensity);
    uniforms.environmentDiffuseRadiance =
        resourceOwner_.environmentDiffuseRadiance;
    uniforms.environment.parameters.z =
        static_cast<float>(probes.size());
    std::copy(probes.begin(), probes.end(),
              uniforms.reflectionProbes.begin());
    uniforms.environment.parameters.w = static_cast<float>(
        resourceOwner_.environmentMap.mipLevels - 1U);
    frame.exposureScale =
        config_.exposure * uniforms.environment.parameters.x;
    frame.reflectionProbeCount =
        static_cast<std::uint32_t>(probes.size());
    frame.shadowAtlasOverflowCount =
        config_.shadows ? shadowPlan.assignment.overflowCount : 0U;
    uniforms.counts = {static_cast<std::uint32_t>(lights.size()), columns,
                       rows, shadowPlan.viewCount};
    uniforms.viewport = {
        static_cast<float>(swapchainOwner_.extent.width),
        static_cast<float>(swapchainOwner_.extent.height),
        1.0F / static_cast<float>(swapchainOwner_.extent.width),
        1.0F / static_cast<float>(swapchainOwner_.extent.height)};
    uniforms.shadowViewProjection = shadowPlan.matrices;
    uniforms.shadowUvScaleBias = shadowPlan.atlasRects;
    uniforms.cascadeSplits = shadowPlan.cascadeSplits;
    const std::array<float, 3> cascadeSplits{
        shadowPlan.cascadeSplits.x, shadowPlan.cascadeSplits.y,
        shadowPlan.cascadeSplits.z};
    float previousSplit = 0.0F;
    for (std::size_t cascade = 0U; cascade < cascadeSplits.size();
         ++cascade) {
        const float split = cascadeSplits[cascade];
        uniforms.directional.parameters[cascade + 1U] =
            std::bit_cast<std::uint32_t>(
                previousSplit + (split - previousSplit) * 0.9F);
        previousSplit = split;
    }
    frame.shadowViewCount = shadowPlan.viewCount;
    std::memcpy(frame.lightingUniforms.mapped, &uniforms, sizeof(uniforms));
    std::memset(frame.lightListCounters.mapped, 0,
                sizeof(GpuLightListCounters));
    const VulkanBufferSyncState hostWrite{
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT};
    frame.localLights.syncState = hostWrite;
    frame.lightingUniforms.syncState = hostWrite;
    frame.lightListCounters.syncState = hostWrite;
    frame.submittedLocalLightCount =
        static_cast<std::uint32_t>(lights.size());
}

void VulkanRenderer::Impl::recordLightAssignment(
    const VkCommandBuffer commandBuffer, const std::uint32_t tileColumns,
    const std::uint32_t tileRows, const bool depthBoundsEnabled) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelineOwner_.lightAssignment);
    const VkDescriptorSet descriptorSet =
        resourceOwner_.lightingDescriptorSets[frameOwner_.currentFrame];
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineOwner_.lightAssignmentLayout, 1U, 1U,
                            &descriptorSet, 0U, nullptr);
    const LightAssignmentPushConstants push{
        depthBoundsEnabled
            ? (resourceOwner_.depthPyramidExtremaEnabled ? 2U : 1U)
            : 0U};
    vkCmdPushConstants(commandBuffer, pipelineOwner_.lightAssignmentLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (tileColumns + 7U) / 8U,
                  (tileRows + 7U) / 8U, 1U);
}

} // namespace ve
