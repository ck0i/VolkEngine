#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <algorithm>
#include <exception>
#include <tuple>
#include <utility>

namespace ve {

void VulkanRenderer::Impl::reloadReferenceAssets(
    ReferenceAssetBundle candidate) {
  const ReferenceAssetBundle &current = referenceAssets();
  if (candidate.scene.meshes.size() != current.scene.meshes.size()) {
    throw std::runtime_error(
        "Runtime asset reload cannot change the authored mesh count");
  }

  using TextureKey =
      std::tuple<std::filesystem::path, TextureRole, TextureColorSpace>;
  std::vector<TextureKey> candidateTextures;
  for (const ImportedMaterial &material : candidate.scene.materials) {
    for (const ImportedTextureReference &texture : material.textures) {
      const TextureKey key{texture.sourcePath, texture.role,
                           texture.colorSpace};
      if (std::ranges::find(candidateTextures, key) ==
          candidateTextures.end()) {
        candidateTextures.push_back(key);
      }
    }
  }
  if (candidateTextures.size() != resourceOwner_.materialTextures.size()) {
    throw std::runtime_error("Runtime asset reload cannot change the allocated "
                             "material texture count");
  }
  if (kBaseSceneMeshBatchOrder.size() + candidate.scene.meshes.size() >
      deviceOwner_.info.maxDrawIndirectCount) {
    throw std::runtime_error(
        "Runtime asset reload exceeds the selected device draw-command limit");
  }

  waitIdle();

  struct AssetGpuState {
    Buffer sceneVertexBuffer;
    Buffer sceneIndexBuffer;
    Buffer clusterData;
    Buffer clusterHierarchy;
    Buffer meshClusterRanges;
    std::vector<GpuMeshClusterRange> sceneMeshClusterRanges;
    std::vector<GpuCluster> sceneClusters;
    std::vector<GpuClusterNode> sceneClusterHierarchy;
    std::vector<std::uint32_t> sceneClusterRoots;
    std::vector<GpuMesh> sceneMeshes;
    std::vector<MeshBounds> sceneMeshBounds;
    std::vector<std::uint32_t> sceneMeshTriangleCounts;
    std::vector<ImageResource> materialTextures;
    std::vector<TextureRole> materialTextureRoles;
    std::array<std::size_t, vulkan_renderer_detail::kMaterialTextureCount>
        referenceMaterialTextureIndices{};
    std::vector<MaterialTextureBinding> materialTextureBindings;
  };

  const auto takeCurrentState = [&] {
    AssetGpuState state;
    state.sceneVertexBuffer =
        std::exchange(resourceOwner_.sceneVertexBuffer, {});
    state.sceneIndexBuffer = std::exchange(resourceOwner_.sceneIndexBuffer, {});
    state.clusterData = std::exchange(resourceOwner_.clusterData, {});
    state.clusterHierarchy = std::exchange(resourceOwner_.clusterHierarchy, {});
    state.meshClusterRanges =
        std::exchange(resourceOwner_.meshClusterRanges, {});
    state.sceneMeshClusterRanges =
        std::exchange(resourceOwner_.sceneMeshClusterRanges, {});
    state.sceneClusters = std::exchange(resourceOwner_.sceneClusters, {});
    state.sceneClusterHierarchy =
        std::exchange(resourceOwner_.sceneClusterHierarchy, {});
    state.sceneClusterRoots =
        std::exchange(resourceOwner_.sceneClusterRoots, {});
    state.sceneMeshes = std::exchange(resourceOwner_.sceneMeshes, {});
    state.sceneMeshBounds = std::exchange(resourceOwner_.sceneMeshBounds, {});
    state.sceneMeshTriangleCounts =
        std::exchange(resourceOwner_.sceneMeshTriangleCounts, {});
    state.materialTextures = std::exchange(resourceOwner_.materialTextures, {});
    state.materialTextureRoles =
        std::exchange(resourceOwner_.materialTextureRoles, {});
    state.referenceMaterialTextureIndices =
        resourceOwner_.referenceMaterialTextureIndices;
    resourceOwner_.referenceMaterialTextureIndices.fill(0U);
    state.materialTextureBindings =
        std::exchange(resourceOwner_.materialTextureBindings, {});
    return state;
  };

  const auto restoreState = [&](AssetGpuState &state) {
    resourceOwner_.sceneVertexBuffer =
        std::exchange(state.sceneVertexBuffer, {});
    resourceOwner_.sceneIndexBuffer = std::exchange(state.sceneIndexBuffer, {});
    resourceOwner_.clusterData = std::exchange(state.clusterData, {});
    resourceOwner_.clusterHierarchy = std::exchange(state.clusterHierarchy, {});
    resourceOwner_.meshClusterRanges =
        std::exchange(state.meshClusterRanges, {});
    resourceOwner_.sceneMeshClusterRanges =
        std::move(state.sceneMeshClusterRanges);
    resourceOwner_.sceneClusters = std::move(state.sceneClusters);
    resourceOwner_.sceneClusterHierarchy =
        std::move(state.sceneClusterHierarchy);
    resourceOwner_.sceneClusterRoots = std::move(state.sceneClusterRoots);
    resourceOwner_.sceneMeshes = std::move(state.sceneMeshes);
    resourceOwner_.sceneMeshBounds = std::move(state.sceneMeshBounds);
    resourceOwner_.sceneMeshTriangleCounts =
        std::move(state.sceneMeshTriangleCounts);
    resourceOwner_.materialTextures = std::move(state.materialTextures);
    resourceOwner_.materialTextureRoles = std::move(state.materialTextureRoles);
    resourceOwner_.referenceMaterialTextureIndices =
        state.referenceMaterialTextureIndices;
    resourceOwner_.materialTextureBindings =
        std::move(state.materialTextureBindings);
  };

  const auto destroyState = [&](AssetGpuState &state) {
    destroyBuffer(state.sceneVertexBuffer);
    destroyBuffer(state.sceneIndexBuffer);
    destroyBuffer(state.clusterData);
    destroyBuffer(state.clusterHierarchy);
    destroyBuffer(state.meshClusterRanges);
    for (ImageResource &texture : state.materialTextures) {
      destroyImage(texture);
    }
    state.materialTextures.clear();
  };

  const auto updateAssetDescriptors = [&] {
    const auto samplerForRole = [&](const TextureRole role) {
      switch (role) {
      case TextureRole::Normal:
        return resourceOwner_.normalTextureSampler;
      case TextureRole::MetallicRoughness:
      case TextureRole::Occlusion:
        return resourceOwner_.ormTextureSampler;
      case TextureRole::BaseColor:
      case TextureRole::Emissive:
        return resourceOwner_.textureSampler;
      }
      return resourceOwner_.textureSampler;
    };

    std::vector<VkDescriptorImageInfo> imageInfos;
    if (resourceOwner_.bindlessMaterialsEnabled) {
      imageInfos.reserve(resourceOwner_.materialTextures.size());
      for (std::size_t index = 0;
           index < resourceOwner_.materialTextures.size(); ++index) {
        imageInfos.push_back(
            {samplerForRole(resourceOwner_.materialTextureRoles[index]),
             resourceOwner_.materialTextures[index].view,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      }
    } else {
      constexpr std::array<TextureRole,
                           vulkan_renderer_detail::kMaterialTextureCount>
          roles{TextureRole::BaseColor, TextureRole::Normal,
                TextureRole::MetallicRoughness};
      imageInfos.reserve(roles.size());
      for (std::size_t role = 0; role < roles.size(); ++role) {
        imageInfos.push_back(
            {samplerForRole(roles[role]),
             resourceOwner_.materialTextures
                 .at(resourceOwner_.referenceMaterialTextureIndices[role])
                 .view,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      }
    }
    for (const VkDescriptorSet set : resourceOwner_.sceneDescriptorSets) {
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = set;
      write.dstBinding = 3U;
      write.descriptorCount = static_cast<std::uint32_t>(imageInfos.size());
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = imageInfos.data();
      vkUpdateDescriptorSets(deviceOwner_.device, 1U, &write, 0U, nullptr);
    }
    if (indirectSceneDrawsEnabled_) {
      for (std::size_t frameIndex = 0; frameIndex < kMaxFramesInFlight;
           ++frameIndex) {
        updateFrameCullDescriptors(frameIndex);
      }
    }
  };

  AssetGpuState previous = takeCurrentState();
  ReferenceAssetBundle *const stableBundle = resourceOwner_.referenceAssets;
  const RenderStats previousStats = stats_;
  resourceOwner_.referenceAssets = &candidate;
  try {
    createMeshes();
    createTextureResources();
    updateAssetDescriptors();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    resourceOwner_.referenceAssets = stableBundle;
    try {
      waitIdle();
    } catch (...) {
    }
    AssetGpuState failed = takeCurrentState();
    destroyState(failed);
    restoreState(previous);
    stats_ = previousStats;
    updateAssetDescriptors();
    std::rethrow_exception(failure);
  }

  resourceOwner_.referenceAssets = stableBundle;
  destroyState(previous);
  static_assert(std::is_nothrow_move_assignable_v<ReferenceAssetBundle>);
  *stableBundle = std::move(candidate);
  for (FrameResources &frame : frameOwner_.frames) {
    frame.cachedGpuRenderItems.clear();
    frame.gpuRenderItemCacheValid = false;
    frame.gpuRenderItemsChangedThisFrame = true;
    frame.shadowCasterCacheValid = false;
  }
  gridVisibilityCache_ = {};
  logger()->info(
      "Published runtime reference assets: {} records, {} meshes, {} textures",
      stableBundle->database.records().size(),
      stableBundle->scene.meshes.size(),
      resourceOwner_.materialTextures.size());
}

} // namespace ve
