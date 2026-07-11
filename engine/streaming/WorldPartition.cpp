#include "streaming/WorldPartition.hpp"

#include "core/BinaryIO.hpp"
#include "core/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace ve {
namespace {

constexpr std::uint32_t kPartitionMagic = 0x50574556U;
constexpr std::uint32_t kPartitionVersion = 1U;
constexpr std::uint32_t kInvalidParent =
    std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] bool finiteVec3(const Vec3 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool positiveOverlap(const WorldPartitionCell &left,
                                   const WorldPartitionCell &right) noexcept {
  return std::abs(left.center.x - right.center.x) <
             left.halfExtent + right.halfExtent &&
         std::abs(left.center.z - right.center.z) <
             left.halfExtent + right.halfExtent;
}

[[nodiscard]] float distanceToCenter(const WorldPartitionCell &cell,
                                     const Vec3 point) noexcept {
  const float dx = point.x - cell.center.x;
  const float dz = point.z - cell.center.z;
  return std::sqrt(dx * dx + dz * dz);
}
[[nodiscard]] bool sameOrigin(const Vec3 left, const Vec3 right) noexcept {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

class Writer final {
public:
  template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
  void pod(const T value) {
    appendLittleEndian(output_, value);
  }
  void floating(const float value) { appendLittleEndianFloat(output_, value); }
  void string(const std::string_view value) {
    pod(static_cast<std::uint32_t>(value.size()));
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    output_.insert(output_.end(), bytes.begin(), bytes.end());
  }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(output_); }

private:
  std::vector<std::byte> output_;
};

class Reader final {
public:
  Reader(const std::span<const std::byte> bytes, const std::size_t maximum)
      : bytes_(bytes) {
    if (bytes.size() > maximum)
      throw std::runtime_error("Partition manifest exceeds configured bytes");
  }

  template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
  [[nodiscard]] T pod() {
    const T value = readLittleEndian<T>(bytes_, offset_);
    offset_ += sizeof(T);
    return value;
  }
  [[nodiscard]] float floating() {
    const float value = readLittleEndianFloat(bytes_, offset_);
    offset_ += sizeof(float);
    return value;
  }
  [[nodiscard]] std::string string(const std::size_t maximum) {
    const std::size_t size = pod<std::uint32_t>();
    if (size > maximum || offset_ > bytes_.size() ||
        size > bytes_.size() - offset_) {
      throw std::runtime_error("Partition path is truncated or oversized");
    }
    std::string value(size, '\0');
    if (size != 0U)
      std::memcpy(value.data(), bytes_.data() + offset_, size);
    offset_ += size;
    return value;
  }
  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

private:
  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0U;
};

[[nodiscard]] std::filesystem::path
validateArtifactPath(const std::filesystem::path &path,
                     const std::filesystem::path &root) {
  if (path.empty() || path.generic_string().find('\0') != std::string::npos)
    throw std::runtime_error("Partition artifact path is invalid");
  if (path.is_absolute())
    return path.lexically_normal();
  const std::filesystem::path normalized = path.lexically_normal();
  if (normalized.empty() || *normalized.begin() == "..")
    throw std::runtime_error("Partition artifact path escapes its root");
  return root.empty() ? normalized : (root / normalized).lexically_normal();
}

struct CombinedRecord {
  SceneEntityId id;
  std::string name;
  SceneEntityId parent;
  TransformTRS transform;
  std::uint8_t renderable = 0U;
  AuthoringRenderable renderableValue;
};

} // namespace

void validateWorldPartition(const WorldPartitionManifest &manifest,
                            const WorldPartitionLimits &limits) {
  if (limits.maximumCells == 0U || limits.maximumDependenciesPerCell == 0U ||
      limits.maximumPathBytes == 0U || limits.maximumManifestBytes == 0U) {
    throw std::invalid_argument("Partition limits must be positive");
  }
  if (manifest.cells.empty() || manifest.cells.size() > limits.maximumCells)
    throw std::runtime_error("Partition cell count is invalid");

  for (std::size_t index = 0U; index < manifest.cells.size(); ++index) {
    const WorldPartitionCell &cell = manifest.cells[index];
    if (!cell.id.valid() || !finiteVec3(cell.center) ||
        !std::isfinite(cell.halfExtent) || cell.halfExtent <= 0.0F ||
        !std::isfinite(cell.splitDistance) || cell.splitDistance < 0.0F ||
        cell.estimatedBytes == 0U ||
        cell.dependencies.size() > limits.maximumDependenciesPerCell) {
      throw std::runtime_error("Partition cell metadata is invalid");
    }
    const std::string path = cell.artifactPath.generic_string();
    if (path.empty() || path.size() > limits.maximumPathBytes ||
        path.find('\0') != std::string::npos) {
      throw std::runtime_error("Partition artifact path is invalid");
    }
    static_cast<void>(validateArtifactPath(cell.artifactPath, {}));
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (manifest.cells[prior].id == cell.id)
        throw std::runtime_error("Partition cell identity is duplicated");
    }
    std::vector<ResidencyKey> dependencies = cell.dependencies;
    std::ranges::sort(dependencies);
    if (std::ranges::adjacent_find(dependencies) != dependencies.end())
      throw std::runtime_error("Partition dependency is duplicated");
    for (const ResidencyKey dependency : dependencies) {
      if (!dependency.valid() ||
          dependency.resourceClass >= ResidencyClass::Count)
        throw std::runtime_error("Partition dependency identity is invalid");
    }
  }

  for (const WorldPartitionCell &cell : manifest.cells) {
    if (!cell.parent.valid())
      continue;
    const auto parent =
        std::ranges::find(manifest.cells, cell.parent, &WorldPartitionCell::id);
    if (parent == manifest.cells.end())
      throw std::runtime_error("Partition parent is missing");
    if (cell.halfExtent > parent->halfExtent ||
        std::abs(cell.center.x - parent->center.x) + cell.halfExtent >
            parent->halfExtent + 0.0001F ||
        std::abs(cell.center.z - parent->center.z) + cell.halfExtent >
            parent->halfExtent + 0.0001F) {
      throw std::runtime_error("Partition child escapes its parent bounds");
    }
    AssetId ancestor = cell.parent;
    for (std::size_t depth = 0U; ancestor.valid(); ++depth) {
      if (depth >= manifest.cells.size())
        throw std::runtime_error("Partition hierarchy contains a cycle");
      const auto found =
          std::ranges::find(manifest.cells, ancestor, &WorldPartitionCell::id);
      if (found == manifest.cells.end())
        throw std::runtime_error("Partition ancestor is missing");
      if (found->id == cell.id)
        throw std::runtime_error("Partition hierarchy contains a cycle");
      ancestor = found->parent;
    }
  }

  for (const WorldPartitionCell &parent : manifest.cells) {
    std::vector<const WorldPartitionCell *> children;
    for (const WorldPartitionCell &candidate : manifest.cells) {
      if (candidate.parent == parent.id)
        children.push_back(&candidate);
    }
    if (children.empty())
      continue;
    if (parent.splitDistance <= 0.0F)
      throw std::runtime_error("Partition branch has no split distance");
    double childArea = 0.0;
    for (std::size_t index = 0U; index < children.size(); ++index) {
      childArea +=
          4.0 * children[index]->halfExtent * children[index]->halfExtent;
      for (std::size_t prior = 0U; prior < index; ++prior) {
        if (positiveOverlap(*children[index], *children[prior]))
          throw std::runtime_error("Partition siblings overlap");
      }
    }
    const double parentArea = 4.0 * parent.halfExtent * parent.halfExtent;
    if (std::abs(childArea - parentArea) > parentArea * 0.0001)
      throw std::runtime_error("Partition children do not cover their parent");
  }

  std::vector<const WorldPartitionCell *> roots;
  for (const WorldPartitionCell &cell : manifest.cells) {
    if (!cell.parent.valid())
      roots.push_back(&cell);
  }
  if (roots.empty())
    throw std::runtime_error("Partition has no root cells");
  for (std::size_t index = 0U; index < roots.size(); ++index) {
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (positiveOverlap(*roots[index], *roots[prior]))
        throw std::runtime_error("Partition roots overlap");
    }
  }
}

std::vector<std::byte>
encodeWorldPartition(const WorldPartitionManifest &manifest,
                     const WorldPartitionLimits &limits) {
  validateWorldPartition(manifest, limits);
  std::vector<WorldPartitionCell> cells = manifest.cells;
  std::ranges::sort(cells, {}, &WorldPartitionCell::id);
  Writer writer;
  writer.pod(kPartitionMagic);
  writer.pod(kPartitionVersion);
  writer.pod(static_cast<std::uint64_t>(cells.size()));
  for (WorldPartitionCell &cell : cells) {
    writer.pod(cell.id.high);
    writer.pod(cell.id.low);
    writer.pod(cell.parent.high);
    writer.pod(cell.parent.low);
    writer.floating(cell.center.x);
    writer.floating(cell.center.y);
    writer.floating(cell.center.z);
    writer.floating(cell.halfExtent);
    writer.floating(cell.splitDistance);
    writer.pod(cell.estimatedBytes);
    writer.string(cell.artifactPath.generic_string());
    std::ranges::sort(cell.dependencies);
    writer.pod(static_cast<std::uint32_t>(cell.dependencies.size()));
    for (const ResidencyKey dependency : cell.dependencies) {
      writer.pod(dependency.id.high);
      writer.pod(dependency.id.low);
      writer.pod(static_cast<std::uint32_t>(dependency.resourceClass));
    }
  }
  std::vector<std::byte> bytes = std::move(writer).take();
  if (bytes.size() > limits.maximumManifestBytes)
    throw std::runtime_error("Partition manifest exceeds configured bytes");
  return bytes;
}

WorldPartitionManifest
decodeWorldPartition(const std::span<const std::byte> bytes,
                     const std::filesystem::path &artifactRoot,
                     const WorldPartitionLimits &limits) {
  Reader reader{bytes, limits.maximumManifestBytes};
  if (reader.pod<std::uint32_t>() != kPartitionMagic ||
      reader.pod<std::uint32_t>() != kPartitionVersion) {
    throw std::runtime_error("Partition manifest header is incompatible");
  }
  const std::size_t count = reader.pod<std::uint64_t>();
  if (count == 0U || count > limits.maximumCells)
    throw std::runtime_error("Partition cell count exceeds configured limits");
  WorldPartitionManifest manifest;
  manifest.cells.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    WorldPartitionCell cell;
    cell.id = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
    cell.parent = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
    cell.center = {reader.floating(), reader.floating(), reader.floating()};
    cell.halfExtent = reader.floating();
    cell.splitDistance = reader.floating();
    cell.estimatedBytes = reader.pod<std::uint64_t>();
    cell.artifactPath = validateArtifactPath(
        reader.string(limits.maximumPathBytes), artifactRoot);
    const std::size_t dependencyCount = reader.pod<std::uint32_t>();
    if (dependencyCount > limits.maximumDependenciesPerCell)
      throw std::runtime_error("Partition dependency count exceeds limits");
    cell.dependencies.reserve(dependencyCount);
    for (std::size_t dependency = 0U; dependency < dependencyCount;
         ++dependency) {
      ResidencyKey key;
      key.id = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
      key.resourceClass =
          static_cast<ResidencyClass>(reader.pod<std::uint32_t>());
      cell.dependencies.push_back(key);
    }
    manifest.cells.push_back(std::move(cell));
  }
  if (!reader.done())
    throw std::runtime_error("Partition manifest has trailing data");
  validateWorldPartition(manifest, limits);
  std::ranges::sort(manifest.cells, {}, &WorldPartitionCell::id);
  return manifest;
}

void saveWorldPartition(const WorldPartitionManifest &manifest,
                        const std::filesystem::path &path,
                        const WorldPartitionLimits &limits) {
  writeBinaryFileAtomic(path, encodeWorldPartition(manifest, limits));
}

WorldPartitionManifest loadWorldPartition(const std::filesystem::path &path,
                                          const WorldPartitionLimits &limits) {
  return decodeWorldPartition(readBinaryFile(path, limits.maximumManifestBytes),
                              path.parent_path(), limits);
}

CookedWorld combineCookedWorldCells(
    const std::span<const std::span<const std::byte>> encodedCells,
    const Vec3 worldOrigin, const CookedWorldLimits &limits) {
  if (!finiteVec3(worldOrigin))
    throw std::invalid_argument("World origin is non-finite");
  std::vector<CombinedRecord> records;
  for (const std::span<const std::byte> encoded : encodedCells) {
    const CookedWorld cell = decodeCookedWorld(encoded, limits);
    if (records.size() > limits.maximumEntities - cell.identities.size())
      throw std::runtime_error("Combined world exceeds entity limits");
    const std::size_t base = records.size();
    records.reserve(base + cell.identities.size());
    for (std::size_t index = 0U; index < cell.identities.size(); ++index) {
      SceneEntityId parent;
      if (cell.parentIndices[index] != kInvalidParent)
        parent = cell.identities[cell.parentIndices[index]];
      TransformTRS transform = cell.transforms[index];
      if (!parent.valid())
        transform.translation = transform.translation - worldOrigin;
      records.push_back({cell.identities[index], cell.names[index], parent,
                         transform, cell.renderableMask[index],
                         cell.renderables[index]});
    }
  }
  std::ranges::sort(records, {}, &CombinedRecord::id);
  if (std::ranges::adjacent_find(records, {}, &CombinedRecord::id) !=
      records.end()) {
    throw std::runtime_error("Partition cells contain duplicate entity IDs");
  }

  CookedWorld world;
  const std::size_t count = records.size();
  world.identities.reserve(count);
  world.names.reserve(count);
  world.parentIndices.reserve(count);
  world.transforms.reserve(count);
  world.renderableMask.reserve(count);
  world.renderables.reserve(count);
  for (const CombinedRecord &record : records) {
    world.identities.push_back(record.id);
    world.names.push_back(record.name);
    std::uint32_t parentIndex = kInvalidParent;
    if (record.parent.valid()) {
      const auto found = std::ranges::lower_bound(records, record.parent, {},
                                                  &CombinedRecord::id);
      if (found == records.end() || found->id != record.parent)
        throw std::runtime_error(
            "Partition entity parent crosses a missing cell");
      parentIndex = static_cast<std::uint32_t>(found - records.begin());
    }
    world.parentIndices.push_back(parentIndex);
    world.transforms.push_back(record.transform);
    world.renderableMask.push_back(record.renderable);
    world.renderables.push_back(record.renderableValue);
  }
  validateCookedWorld(world, limits);
  return world;
}

WorldPartitionRuntime::WorldPartitionRuntime(ResidencyManager &residency,
                                             WorldPartitionManifest manifest,
                                             PartitionRuntimeConfig config)
    : residency_(&residency), manifest_(std::move(manifest)), config_(config) {
  validateWorldPartition(manifest_);
  if (!std::isfinite(config_.prefetchMargin) || config_.prefetchMargin < 0.0F ||
      !std::isfinite(config_.originCellSize) ||
      config_.originCellSize <= 0.0F ||
      !std::isfinite(config_.originShiftDistance) ||
      config_.originShiftDistance < config_.originCellSize) {
    throw std::invalid_argument("Partition runtime configuration is invalid");
  }
  std::ranges::sort(manifest_.cells, {}, &WorldPartitionCell::id);
  children_.resize(manifest_.cells.size());
  for (std::size_t index = 0U; index < manifest_.cells.size(); ++index) {
    const WorldPartitionCell &cell = manifest_.cells[index];
    if (!cell.parent.valid()) {
      roots_.push_back(cell.id);
    } else {
      const auto parent = std::ranges::lower_bound(manifest_.cells, cell.parent,
                                                   {}, &WorldPartitionCell::id);
      children_[static_cast<std::size_t>(parent - manifest_.cells.begin())]
          .push_back(index);
    }
    residency_->registerResource({{cell.id, ResidencyClass::WorldCell},
                                  cell.artifactPath,
                                  cell.estimatedBytes,
                                  cell.dependencies,
                                  "world-cell-" + cell.id.hex()});
  }
  std::ranges::sort(roots_);
}

const WorldPartitionCell *
WorldPartitionRuntime::find(const AssetId id) const noexcept {
  const auto found = std::ranges::lower_bound(manifest_.cells, id, {},
                                              &WorldPartitionCell::id);
  return found != manifest_.cells.end() && found->id == id ? &*found : nullptr;
}

void WorldPartitionRuntime::selectCell(const WorldPartitionCell &cell,
                                       const Vec3 observer,
                                       std::vector<AssetId> &output) const {
  const std::size_t index =
      static_cast<std::size_t>(&cell - manifest_.cells.data());
  if (!children_[index].empty() &&
      distanceToCenter(cell, observer) <= cell.splitDistance) {
    for (const std::size_t child : children_[index])
      selectCell(manifest_.cells[child], observer, output);
    return;
  }
  output.push_back(cell.id);
}

void WorldPartitionRuntime::collectPrefetch(
    const WorldPartitionCell &cell, const Vec3 observer,
    std::vector<AssetId> &output) const {
  const std::size_t index =
      static_cast<std::size_t>(&cell - manifest_.cells.data());
  if (children_[index].empty())
    return;
  const float distance = distanceToCenter(cell, observer);
  if (distance > cell.splitDistance + config_.prefetchMargin)
    return;
  if (distance > cell.splitDistance) {
    for (const std::size_t child : children_[index])
      output.push_back(manifest_.cells[child].id);
    return;
  }
  for (const std::size_t child : children_[index])
    collectPrefetch(manifest_.cells[child], observer, output);
}

bool WorldPartitionRuntime::coversObserver(const std::span<const AssetId> cells,
                                           const Vec3 observer) const noexcept {
  for (const AssetId id : cells) {
    const WorldPartitionCell *cell = find(id);
    if (cell != nullptr &&
        std::abs(observer.x - cell->center.x) <= cell->halfExtent &&
        std::abs(observer.z - cell->center.z) <= cell->halfExtent) {
      return true;
    }
  }
  return false;
}

Vec3 WorldPartitionRuntime::desiredOrigin(const Vec3 observer) const noexcept {
  if (std::max(std::abs(observer.x - origin_.x),
               std::abs(observer.z - origin_.z)) <=
      config_.originShiftDistance) {
    return origin_;
  }
  return {
      std::floor(observer.x / config_.originCellSize) * config_.originCellSize,
      0.0F,
      std::floor(observer.z / config_.originCellSize) * config_.originCellSize};
}

void WorldPartitionRuntime::update(const Vec3 globalObserver,
                                   const std::uint64_t frameIndex) {
  if (!finiteVec3(globalObserver))
    throw std::invalid_argument("Partition observer is non-finite");
  residency_->update();
  desired_.clear();
  prefetch_.clear();
  for (const AssetId root : roots_) {
    selectCell(*find(root), globalObserver, desired_);
    collectPrefetch(*find(root), globalObserver, prefetch_);
  }
  std::ranges::sort(desired_);
  std::ranges::sort(prefetch_);
  prefetch_.erase(std::ranges::unique(prefetch_).begin(), prefetch_.end());

  residency_->beginFrame(frameIndex);
  for (const AssetId cell : active_)
    static_cast<void>(residency_->request({cell, ResidencyClass::WorldCell},
                                          config_.activePriority, true));
  if (pending_) {
    for (const AssetId cell : pending_->cells)
      static_cast<void>(residency_->request({cell, ResidencyClass::WorldCell},
                                            config_.activePriority, true));
  }
  for (const AssetId cell : desired_)
    static_cast<void>(residency_->request({cell, ResidencyClass::WorldCell},
                                          config_.desiredPriority, true));
  for (const AssetId cell : prefetch_) {
    if (!std::ranges::binary_search(desired_, cell))
      static_cast<void>(residency_->request({cell, ResidencyClass::WorldCell},
                                            config_.prefetchPriority, false));
  }
  residency_->endFrame();

  bool ready = true;
  std::uint32_t pendingCount = 0U;
  for (const AssetId cell : desired_) {
    const ResidencyState state =
        residency_->state({cell, ResidencyClass::WorldCell});
    ready = ready && state == ResidencyState::Resident;
    pendingCount += state != ResidencyState::Resident;
  }
  const Vec3 nextOrigin = desiredOrigin(globalObserver);
  const bool frontierChanged = desired_ != active_;
  const bool originChanged = !sameOrigin(nextOrigin, origin_);
  if (ready && !pending_ && (frontierChanged || originChanged) &&
      desired_ != rejectedFrontier_) {
    buildPublication(desired_, nextOrigin);
  }

  ++metrics_.traversalFrames;
  metrics_.activeCells = static_cast<std::uint32_t>(active_.size());
  metrics_.desiredCells = static_cast<std::uint32_t>(desired_.size());
  metrics_.pendingCells = pendingCount;
  metrics_.worldOrigin = origin_;
  if (!coversObserver(active_, globalObserver))
    ++metrics_.coverageGapFrames;
  if (frontierChanged && !active_.empty())
    ++metrics_.retainedFrontierFrames;
}

void WorldPartitionRuntime::buildPublication(
    const std::vector<AssetId> &desired, const Vec3 origin) {
  std::vector<std::span<const std::byte>> payloads;
  payloads.reserve(desired.size());
  for (const AssetId cell : desired) {
    const std::span<const std::byte> payload =
        residency_->payload({cell, ResidencyClass::WorldCell});
    if (payload.empty())
      return;
    payloads.push_back(payload);
  }
  try {
    PartitionPublication publication;
    publication.revision = nextRevision_++;
    publication.worldOrigin = origin;
    publication.world =
        combineCookedWorldCells(payloads, origin, config_.cookedWorldLimits);
    publication.cells = desired;
    pending_ = std::move(publication);
  } catch (...) {
    rejectedFrontier_ = desired;
    ++metrics_.partialLoadFailures;
  }
}

const PartitionPublication *
WorldPartitionRuntime::pendingPublication() const noexcept {
  return pending_ ? &*pending_ : nullptr;
}

void WorldPartitionRuntime::commitPublication(const std::uint64_t revision) {
  if (!pending_ || pending_->revision != revision)
    throw std::invalid_argument("Partition publication revision is stale");
  const bool shifted = !sameOrigin(origin_, pending_->worldOrigin);
  active_ = pending_->cells;
  origin_ = pending_->worldOrigin;
  pending_.reset();
  rejectedFrontier_.clear();
  ++metrics_.publications;
  metrics_.originShifts += shifted;
  metrics_.activeCells = static_cast<std::uint32_t>(active_.size());
  metrics_.worldOrigin = origin_;
}

void WorldPartitionRuntime::rejectPublication(
    const std::uint64_t revision) noexcept {
  if (!pending_ || pending_->revision != revision)
    return;
  rejectedFrontier_ = pending_->cells;
  pending_.reset();
  ++metrics_.partialLoadFailures;
}

void WorldPartitionRuntime::retryRejectedFrontier() {
  for (const AssetId cell : rejectedFrontier_) {
    if (std::ranges::find(active_, cell) == active_.end())
      residency_->evict({cell, ResidencyClass::WorldCell});
  }
  rejectedFrontier_.clear();
}

void WorldPartitionRuntime::resetTraversalMetrics() noexcept {
  metrics_.traversalFrames = 0U;
  metrics_.coverageGapFrames = 0U;
  metrics_.retainedFrontierFrames = 0U;
}

std::span<const AssetId> WorldPartitionRuntime::activeCells() const noexcept {
  return active_;
}

Vec3 WorldPartitionRuntime::worldOrigin() const noexcept { return origin_; }

PartitionMetrics WorldPartitionRuntime::metrics() const noexcept {
  return metrics_;
}

} // namespace ve
