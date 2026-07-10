#include "renderer/Geometry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace ve {
namespace {

constexpr std::uint32_t kMissingObjIndex = std::numeric_limits<std::uint32_t>::max();

struct ObjFaceVertex {
    std::int64_t position = 0;
    std::int64_t texcoord = 0;
    std::int64_t normal = 0;
};

struct ObjVertexKey {
    std::uint32_t position = 0;
    std::uint32_t texcoord = kMissingObjIndex;
    std::uint32_t normal = kMissingObjIndex;

    [[nodiscard]] bool operator==(const ObjVertexKey&) const = default;
};

struct ObjVertexKeyHash {
    [[nodiscard]] std::size_t operator()(const ObjVertexKey& key) const noexcept {
        std::size_t hash = key.position;
        hash ^= (static_cast<std::size_t>(key.texcoord) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U));
        hash ^= (static_cast<std::size_t>(key.normal) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U));
        return hash;
    }
};

struct ObjRecordCounts {
    std::size_t positions = 0;
    std::size_t texcoords = 0;
    std::size_t normals = 0;
    std::size_t faceVertices = 0;
    std::size_t triangleIndices = 0;
    std::size_t maxFaceVertices = 0;
};

constexpr float kVectorEpsilon = 0.000001f;
constexpr float kVectorEpsilonSquared = kVectorEpsilon * kVectorEpsilon;
constexpr float kTriangleDegenerateSinEpsilon = 0.000001f;
constexpr float kTriangleDegenerateSinEpsilonSquared = kTriangleDegenerateSinEpsilon * kTriangleDegenerateSinEpsilon;

[[nodiscard]] bool isNearlyZero(const Vec3 vector) noexcept {
    const float lengthSquared = dot(vector, vector);
    return !std::isfinite(lengthSquared) || lengthSquared <= kVectorEpsilonSquared;
}

[[nodiscard]] bool isDegenerateTriangle(const Vec3 edge0, const Vec3 edge1, const Vec3 faceNormal) noexcept {
    const float edgeScaleSquared = dot(edge0, edge0) * dot(edge1, edge1);
    return dot(faceNormal, faceNormal) <= edgeScaleSquared * kTriangleDegenerateSinEpsilonSquared;
}

[[nodiscard]] Vec3 normalizeOr(const Vec3 vector, const Vec3 fallback) noexcept {
    const float lengthSquared = dot(vector, vector);
    if (!std::isfinite(lengthSquared) || lengthSquared <= kVectorEpsilonSquared) {
        return fallback;
    }
    return vector * (1.0f / std::sqrt(lengthSquared));
}

[[nodiscard]] std::string_view nextObjToken(std::string_view& text) noexcept {
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        text = {};
        return {};
    }
    text.remove_prefix(first);
    const std::size_t end = text.find_first_of(whitespace);
    if (end == std::string_view::npos) {
        const std::string_view token = text;
        text = {};
        return token;
    }
    const std::string_view token = text.substr(0, end);
    text.remove_prefix(end);
    return token;
}

[[nodiscard]] std::runtime_error objError(const std::filesystem::path& path, const std::uint64_t line, const std::string_view message) {
    return std::runtime_error("OBJ load failed for " + path.string() + ":" + std::to_string(line) + ": " + std::string(message));
}

[[nodiscard]] ObjRecordCounts countObjRecords(std::ifstream& file, const std::filesystem::path& path) {
    ObjRecordCounts counts{};
    std::string lineText;
    std::uint64_t lineNumber = 0;
    const auto checkedAdd = [&](std::size_t& target, const std::size_t value, const std::uint64_t line, const char* fieldName) {
        if (target > std::numeric_limits<std::size_t>::max() - value) {
            throw objError(path, line, std::string("OBJ ") + fieldName + " count exceeds addressable range");
        }
        target += value;
    };

    while (std::getline(file, lineText)) {
        ++lineNumber;
        const std::size_t first = lineText.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || lineText[first] == '#') {
            continue;
        }

        std::string_view fields{lineText};
        fields.remove_prefix(first);
        const std::string_view keyword = nextObjToken(fields);
        if (keyword == "v") {
            checkedAdd(counts.positions, 1U, lineNumber, "position");
        } else if (keyword == "vt") {
            checkedAdd(counts.texcoords, 1U, lineNumber, "texcoord");
        } else if (keyword == "vn") {
            checkedAdd(counts.normals, 1U, lineNumber, "normal");
        } else if (keyword == "f") {
            std::size_t faceVertices = 0;
            while (true) {
                const std::string_view token = nextObjToken(fields);
                if (token.empty() || token.front() == '#') {
                    break;
                }
                ++faceVertices;
            }
            counts.maxFaceVertices = std::max(counts.maxFaceVertices, faceVertices);
            checkedAdd(counts.faceVertices, faceVertices, lineNumber, "face vertex");
            if (faceVertices >= 3U) {
                const std::size_t triangleCount = faceVertices - 2U;
                if (triangleCount > std::numeric_limits<std::size_t>::max() / 3U) {
                    throw objError(path, lineNumber, "OBJ triangle index count exceeds addressable range");
                }
                checkedAdd(counts.triangleIndices, triangleCount * 3U, lineNumber, "triangle index");
            }
        }
    }
    return counts;
}

[[nodiscard]] std::int64_t parseObjIndex(const std::filesystem::path& path, const std::uint64_t line, const std::string_view token, const char* fieldName) {
    if (token.empty()) {
        throw objError(path, line, std::string("missing ") + fieldName + " index");
    }
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size() || value == 0) {
        throw objError(path, line, std::string("invalid ") + fieldName + " index");
    }
    return value;
}

[[nodiscard]] ObjFaceVertex parseFaceVertex(const std::filesystem::path& path, const std::uint64_t line, const std::string_view token) {
    ObjFaceVertex vertex{};
    const std::size_t firstSlash = token.find('/');
    if (firstSlash == std::string_view::npos) {
        vertex.position = parseObjIndex(path, line, token, "position");
        return vertex;
    }

    vertex.position = parseObjIndex(path, line, token.substr(0, firstSlash), "position");
    const std::size_t secondSlash = token.find('/', firstSlash + 1U);
    const auto parseOptional = [&](const std::string_view part, const char* fieldName) {
        return part.empty() ? 0 : parseObjIndex(path, line, part, fieldName);
    };
    if (secondSlash == std::string_view::npos) {
        vertex.texcoord = parseOptional(token.substr(firstSlash + 1U), "texcoord");
        return vertex;
    }
    if (token.find('/', secondSlash + 1U) != std::string_view::npos) {
        throw objError(path, line, "face vertex has too many slash-separated fields");
    }
    vertex.texcoord = parseOptional(token.substr(firstSlash + 1U, secondSlash - firstSlash - 1U), "texcoord");
    vertex.normal = parseOptional(token.substr(secondSlash + 1U), "normal");
    return vertex;
}

[[nodiscard]] std::uint32_t resolveObjIndex(const std::filesystem::path& path, const std::uint64_t line, const std::int64_t rawIndex, const std::size_t count, const char* fieldName) {
    if (count == 0U) {
        throw objError(path, line, std::string("face references missing ") + fieldName + " data");
    }
    const auto count64 = static_cast<std::int64_t>(count);
    const std::int64_t resolved = rawIndex > 0 ? rawIndex - 1 : count64 + rawIndex;
    if (resolved < 0 || resolved >= count64 || resolved >= static_cast<std::int64_t>(kMissingObjIndex)) {
        throw objError(path, line, std::string("out-of-range ") + fieldName + " index");
    }
    return static_cast<std::uint32_t>(resolved);
}

[[nodiscard]] std::uint32_t resolveOptionalObjIndex(const std::filesystem::path& path, const std::uint64_t line, const std::int64_t rawIndex, const std::size_t count, const char* fieldName) {
    return rawIndex == 0 ? kMissingObjIndex : resolveObjIndex(path, line, rawIndex, count, fieldName);
}

[[nodiscard]] float parseObjFloat(const std::filesystem::path& path, const std::uint64_t line, std::string_view token, const char* fieldName) {
    if (token.empty()) {
        throw objError(path, line, std::string("malformed ") + fieldName);
    }
    if (token.front() == '+') {
        token.remove_prefix(1U);
    }
    float value = 0.0f;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
        throw objError(path, line, std::string("malformed ") + fieldName);
    }
    if (!std::isfinite(value)) {
        throw objError(path, line, std::string("non-finite ") + fieldName);
    }
    return value;
}

template <typename Vec>
void appendObjVector(std::vector<Vec>& values, std::string_view fields, const std::filesystem::path& path, const std::uint64_t line, const char* fieldName) {
    const auto parseRequired = [&] {
        const std::string_view token = nextObjToken(fields);
        if (token.empty() || token.front() == '#') {
            throw objError(path, line, std::string("malformed ") + fieldName);
        }
        return parseObjFloat(path, line, token, fieldName);
    };
    Vec value{};
    value.x = parseRequired();
    value.y = parseRequired();
    if constexpr (requires { value.z; }) {
        value.z = parseRequired();
    }
    values.push_back(value);
}

[[nodiscard]] MeshBounds calculateMeshBounds(const std::vector<Vertex>& vertices) noexcept {
    if (vertices.empty()) {
        return {};
    }

    double minimumX = static_cast<double>(vertices.front().position.x);
    double minimumY = static_cast<double>(vertices.front().position.y);
    double minimumZ = static_cast<double>(vertices.front().position.z);
    double maximumX = minimumX;
    double maximumY = minimumY;
    double maximumZ = minimumZ;
    for (const Vertex& vertex : vertices) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) || !std::isfinite(vertex.position.z)) {
            return {};
        }
        const double x = static_cast<double>(vertex.position.x);
        const double y = static_cast<double>(vertex.position.y);
        const double z = static_cast<double>(vertex.position.z);
        minimumX = std::min(minimumX, x);
        minimumY = std::min(minimumY, y);
        minimumZ = std::min(minimumZ, z);
        maximumX = std::max(maximumX, x);
        maximumY = std::max(maximumY, y);
        maximumZ = std::max(maximumZ, z);
    }

    const double centerX = (minimumX + maximumX) * 0.5;
    const double centerY = (minimumY + maximumY) * 0.5;
    const double centerZ = (minimumZ + maximumZ) * 0.5;
    double radiusSquared = 0.0;
    for (const Vertex& vertex : vertices) {
        const double offsetX = static_cast<double>(vertex.position.x) - centerX;
        const double offsetY = static_cast<double>(vertex.position.y) - centerY;
        const double offsetZ = static_cast<double>(vertex.position.z) - centerZ;
        radiusSquared = std::max(radiusSquared, offsetX * offsetX + offsetY * offsetY + offsetZ * offsetZ);
    }
    const double radius = std::sqrt(radiusSquared);
    constexpr double maxFloat = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(centerZ) || !std::isfinite(radius) ||
        radius < 0.0 || radius > maxFloat) {
        return {};
    }
    return MeshBounds{Vec3{static_cast<float>(centerX), static_cast<float>(centerY), static_cast<float>(centerZ)},
                      static_cast<float>(radius), true};
}

[[nodiscard]] Vec3 fallbackTangentForNormal(Vec3 normal) noexcept {
    normal = normalizeOr(normal, {});
    if (isNearlyZero(normal)) {
        return {1.0f, 0.0f, 0.0f};
    }
    const Vec3 axis = std::fabs(normal.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    return normalizeOr(axis - normal * dot(axis, normal), {1.0f, 0.0f, 0.0f});
}

void calculateMeshTangents(MeshData& mesh) {
    if ((mesh.indices.size() % 3U) != 0U) {
        throw std::runtime_error("Triangle-indexed mesh has incomplete triangles");
    }
    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::runtime_error("Triangle-indexed mesh references a vertex outside the mesh");
        }
    }

    std::vector<Vec3> tangentSums(mesh.vertices.size());
    std::vector<Vec3> bitangentSums(mesh.vertices.size());
    const auto accumulateTriangle = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        const Vertex& v0 = mesh.vertices[a];
        const Vertex& v1 = mesh.vertices[b];
        const Vertex& v2 = mesh.vertices[c];
        const Vec3 edge1 = v1.position - v0.position;
        const Vec3 edge2 = v2.position - v0.position;
        const Vec2 deltaUv1{v1.uv.x - v0.uv.x, v1.uv.y - v0.uv.y};
        const Vec2 deltaUv2{v2.uv.x - v0.uv.x, v2.uv.y - v0.uv.y};
        const float determinant = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
        if (std::fabs(determinant) <= 0.0000001f) {
            return;
        }
        const float invDeterminant = 1.0f / determinant;
        const Vec3 tangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * invDeterminant;
        const Vec3 bitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * invDeterminant;
        if (isNearlyZero(tangent) || isNearlyZero(bitangent)) {
            return;
        }
        for (const std::uint32_t index : std::array{a, b, c}) {
            tangentSums[index] = tangentSums[index] + tangent;
            bitangentSums[index] = bitangentSums[index] + bitangent;
        }
    };

    for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
        accumulateTriangle(mesh.indices[index], mesh.indices[index + 1U], mesh.indices[index + 2U]);
    }
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        Vertex& vertex = mesh.vertices[index];
        const Vec3 normal = normalizeOr(vertex.normal, {0.0f, 1.0f, 0.0f});
        Vec3 tangent = tangentSums[index] - normal * dot(normal, tangentSums[index]);
        if (isNearlyZero(tangent)) {
            tangent = fallbackTangentForNormal(normal);
        } else {
            tangent = normalizeOr(tangent, {1.0f, 0.0f, 0.0f});
        }
        const Vec3 bitangent = bitangentSums[index];
        const float handedness = dot(cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = {tangent.x, tangent.y, tangent.z, handedness};
    }
}

void compactMeshToReferencedVertices(MeshData& mesh) {
    std::vector<std::uint32_t> remap(mesh.vertices.size(), kMissingObjIndex);
    for (const std::uint32_t index : mesh.indices) {
        if (index >= remap.size()) {
            throw std::runtime_error("Triangle-indexed mesh references a vertex outside the mesh during compaction");
        }
        remap[index] = 0U;
    }

    std::uint32_t compactVertexCount = 0;
    for (std::uint32_t& index : remap) {
        if (index != kMissingObjIndex) {
            index = compactVertexCount++;
        }
    }
    if (compactVertexCount == mesh.vertices.size()) {
        return;
    }

    for (std::uint32_t& index : mesh.indices) {
        index = remap[index];
    }
    for (std::size_t oldIndex = 0; oldIndex < remap.size(); ++oldIndex) {
        const std::uint32_t newIndex = remap[oldIndex];
        if (newIndex != kMissingObjIndex) {
            mesh.vertices[newIndex] = mesh.vertices[oldIndex];
        }
    }
    mesh.vertices.resize(compactVertexCount);
}

void finalizeMesh(MeshData& mesh) {
    compactMeshToReferencedVertices(mesh);
    mesh.bounds = calculateMeshBounds(mesh.vertices);
    if (!mesh.bounds.valid) {
        throw std::runtime_error("Mesh bounds are non-finite or outside float range");
    }
    calculateMeshTangents(mesh);
}

} // namespace
void optimizeTriangleIndexOrderForVertexCache(std::vector<std::uint32_t>& indices, const std::size_t vertexCount, const std::uint32_t cacheSize) {
    if ((indices.size() % 3U) != 0U) {
        throw std::runtime_error("Mesh index count must be a triangle-list multiple of three during vertex-cache optimization");
    }
    const std::size_t triangleCount = indices.size() / 3U;
    const std::size_t triangleIndexCount = triangleCount * 3U;
    if (triangleCount >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Mesh triangle count exceeds optimizer range");
    }
    for (std::size_t index = 0; index < triangleIndexCount; ++index) {
        if (indices[index] >= vertexCount) {
            throw std::runtime_error("Mesh index exceeds vertex count during vertex-cache optimization");
        }
    }
    if (triangleCount < 2U || cacheSize == 0U) {
        return;
    }

    std::vector<std::size_t> vertexTriangleOffsets(vertexCount + 1U);
    for (std::size_t index = 0; index < triangleIndexCount; ++index) {
        ++vertexTriangleOffsets[static_cast<std::size_t>(indices[index]) + 1U];
    }
    for (std::size_t vertex = 1; vertex < vertexTriangleOffsets.size(); ++vertex) {
        vertexTriangleOffsets[vertex] += vertexTriangleOffsets[vertex - 1U];
    }
    std::vector<std::uint32_t> vertexTriangles(triangleIndexCount);
    std::vector<std::size_t> vertexTriangleCursors = vertexTriangleOffsets;
    for (std::uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        const std::size_t offset = static_cast<std::size_t>(triangle) * 3U;
        vertexTriangles[vertexTriangleCursors[indices[offset + 0U]]++] = triangle;
        vertexTriangles[vertexTriangleCursors[indices[offset + 1U]]++] = triangle;
        vertexTriangles[vertexTriangleCursors[indices[offset + 2U]]++] = triangle;
    }

    std::vector<std::uint8_t> emitted(triangleCount);
    std::vector<std::uint32_t> optimized;
    optimized.reserve(triangleIndexCount);
    std::vector<std::uint32_t> cache;
    cache.reserve(std::min<std::size_t>(cacheSize, vertexCount));
    std::vector<std::uint32_t> candidates;
    std::vector<std::uint32_t> candidateStamp(triangleCount);
    std::uint32_t stamp = 0;
    std::uint32_t nextTriangle = 0;

    const auto cacheContains = [&](const std::uint32_t vertex) {
        return std::find(cache.begin(), cache.end(), vertex) != cache.end();
    };
    const auto triangleScore = [&](const std::uint32_t triangle) {
        const std::size_t offset = static_cast<std::size_t>(triangle) * 3U;
        return (cacheContains(indices[offset + 0U]) ? 1 : 0) +
               (cacheContains(indices[offset + 1U]) ? 1 : 0) +
               (cacheContains(indices[offset + 2U]) ? 1 : 0);
    };
    const auto touchVertex = [&](const std::uint32_t vertex) {
        if (const auto found = std::find(cache.begin(), cache.end(), vertex); found != cache.end()) {
            cache.erase(found);
        }
        cache.insert(cache.begin(), vertex);
        if (cache.size() > cacheSize) {
            cache.pop_back();
        }
    };
    const auto emitTriangle = [&](const std::uint32_t triangle) {
        const std::size_t offset = static_cast<std::size_t>(triangle) * 3U;
        optimized.push_back(indices[offset + 0U]);
        optimized.push_back(indices[offset + 1U]);
        optimized.push_back(indices[offset + 2U]);
        emitted[triangle] = 1U;
        touchVertex(indices[offset + 2U]);
        touchVertex(indices[offset + 1U]);
        touchVertex(indices[offset + 0U]);
    };
    const auto chooseCandidate = [&]() {
        candidates.clear();
        ++stamp;
        for (const std::uint32_t vertex : cache) {
            for (std::size_t cursor = vertexTriangleOffsets[vertex]; cursor < vertexTriangleOffsets[static_cast<std::size_t>(vertex) + 1U]; ++cursor) {
                const std::uint32_t triangle = vertexTriangles[cursor];
                if (emitted[triangle] == 0U && candidateStamp[triangle] != stamp) {
                    candidateStamp[triangle] = stamp;
                    candidates.push_back(triangle);
                }
            }
        }
        std::uint32_t bestTriangle = std::numeric_limits<std::uint32_t>::max();
        int bestScore = -1;
        for (const std::uint32_t triangle : candidates) {
            const int score = triangleScore(triangle);
            if (score > bestScore || (score == bestScore && triangle < bestTriangle)) {
                bestScore = score;
                bestTriangle = triangle;
            }
        }
        return bestTriangle;
    };

    for (std::uint32_t emittedCount = 0; emittedCount < triangleCount; ++emittedCount) {
        std::uint32_t triangle = chooseCandidate();
        if (triangle == std::numeric_limits<std::uint32_t>::max()) {
            while (nextTriangle < triangleCount && emitted[nextTriangle] != 0U) {
                ++nextTriangle;
            }
            triangle = nextTriangle;
        }
        emitTriangle(triangle);
    }
    std::copy(optimized.begin(), optimized.end(), indices.begin());
}

void optimizeVertexFetchOrder(MeshData& mesh) {
    if (mesh.indices.empty()) {
        mesh.vertices.clear();
        mesh.bounds = {};
        return;
    }
    if (mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Mesh vertex count exceeds uint32 range during vertex-fetch optimization");
    }

    std::vector<std::uint32_t> remap(mesh.vertices.size(), kMissingObjIndex);
    std::vector<Vertex> reorderedVertices;
    reorderedVertices.reserve(mesh.vertices.size());
    for (std::uint32_t& index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::runtime_error("Mesh index exceeds vertex count during vertex-fetch optimization");
        }
        std::uint32_t& remappedIndex = remap[index];
        if (remappedIndex == kMissingObjIndex) {
            remappedIndex = static_cast<std::uint32_t>(reorderedVertices.size());
            reorderedVertices.push_back(mesh.vertices[index]);
        }
        index = remappedIndex;
    }
    mesh.vertices = std::move(reorderedVertices);
    mesh.bounds = calculateMeshBounds(mesh.vertices);
}

MeshData createCubeMesh() {
    MeshData mesh{};
    mesh.vertices = {
        {{-1, -1,  1}, {0, 0, 1}, {0, 0}}, {{ 1, -1,  1}, {0, 0, 1}, {1, 0}}, {{ 1,  1,  1}, {0, 0, 1}, {1, 1}}, {{-1,  1,  1}, {0, 0, 1}, {0, 1}},
        {{ 1, -1, -1}, {0, 0,-1}, {0, 0}}, {{-1, -1, -1}, {0, 0,-1}, {1, 0}}, {{-1,  1, -1}, {0, 0,-1}, {1, 1}}, {{ 1,  1, -1}, {0, 0,-1}, {0, 1}},
        {{-1, -1, -1}, {-1,0,0}, {0, 0}}, {{-1, -1,  1}, {-1,0,0}, {1, 0}}, {{-1,  1,  1}, {-1,0,0}, {1, 1}}, {{-1,  1, -1}, {-1,0,0}, {0, 1}},
        {{ 1, -1,  1}, {1, 0,0}, {0, 0}}, {{ 1, -1, -1}, {1, 0,0}, {1, 0}}, {{ 1,  1, -1}, {1, 0,0}, {1, 1}}, {{ 1,  1,  1}, {1, 0,0}, {0, 1}},
        {{-1,  1,  1}, {0, 1,0}, {0, 0}}, {{ 1,  1,  1}, {0, 1,0}, {1, 0}}, {{ 1,  1, -1}, {0, 1,0}, {1, 1}}, {{-1,  1, -1}, {0, 1,0}, {0, 1}},
        {{-1, -1, -1}, {0,-1,0}, {0, 0}}, {{ 1, -1, -1}, {0,-1,0}, {1, 0}}, {{ 1, -1,  1}, {0,-1,0}, {1, 1}}, {{-1, -1,  1}, {0,-1,0}, {0, 1}},
    };
    mesh.indices = {
        0, 1, 2, 2, 3, 0,       4, 5, 6, 6, 7, 4,
        8, 9,10,10,11, 8,      12,13,14,14,15,12,
        16,17,18,18,19,16,     20,21,22,22,23,20,
    };
    finalizeMesh(mesh);
    return mesh;
}

MeshData createUvSphereMesh(const std::uint32_t rings, const std::uint32_t segments) {
    if (rings == 0U || segments == 0U) {
        throw std::runtime_error("UV sphere rings and segments must be positive");
    }
    MeshData mesh{};
    const std::uint64_t vertexCount64 = (static_cast<std::uint64_t>(rings) + 1ULL) * (static_cast<std::uint64_t>(segments) + 1ULL);
    const std::uint64_t indexCount64 = static_cast<std::uint64_t>(rings) * static_cast<std::uint64_t>(segments) * 6ULL;
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        indexCount64 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("UV sphere exceeds renderer 32-bit mesh range");
    }
    mesh.vertices.reserve(static_cast<std::size_t>(vertexCount64));
    mesh.indices.reserve(static_cast<std::size_t>(indexCount64));
    for (std::uint32_t ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float theta = v * std::numbers::pi_v<float>;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * std::numbers::pi_v<float> * 2.0f;
            const Vec3 normal{std::cos(phi) * sinTheta, cosTheta, std::sin(phi) * sinTheta};
            mesh.vertices.push_back({normal, normal, {u, v}});
        }
    }

    for (std::uint32_t ring = 0; ring < rings; ++ring) {
        for (std::uint32_t segment = 0; segment < segments; ++segment) {
            const std::uint32_t a = ring * (segments + 1U) + segment;
            const std::uint32_t b = a + segments + 1U;
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(a + 1U);
            mesh.indices.push_back(a + 1U);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b + 1U);
        }
    }
    finalizeMesh(mesh);
    return mesh;
}

MeshData createPlaneMesh(const float halfExtent, const float uvScale) {
    MeshData mesh{};
    mesh.vertices = {
        {{-halfExtent, 0.0f, -halfExtent}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ halfExtent, 0.0f, -halfExtent}, {0.0f, 1.0f, 0.0f}, {uvScale, 0.0f}},
        {{ halfExtent, 0.0f,  halfExtent}, {0.0f, 1.0f, 0.0f}, {uvScale, uvScale}},
        {{-halfExtent, 0.0f,  halfExtent}, {0.0f, 1.0f, 0.0f}, {0.0f, uvScale}},
    };
    mesh.indices = {0, 2, 1, 0, 3, 2};
    finalizeMesh(mesh);
    return mesh;
}

MeshData loadObjMesh(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open OBJ mesh: " + path.string());
    }
    const ObjRecordCounts recordCounts = countObjRecords(file, path);
    file.clear();
    file.seekg(0, std::ios::beg);
    if (!file) {
        throw std::runtime_error("Failed to rewind OBJ mesh: " + path.string());
    }


    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::vector<std::uint8_t> validNormals;
    MeshData mesh{};
    std::vector<Vec3> generatedNormalSums;
    std::vector<std::uint8_t> hasExplicitNormal;
    std::unordered_map<ObjVertexKey, std::uint32_t, ObjVertexKeyHash> vertexLookup;
    if (recordCounts.positions > static_cast<std::size_t>(kMissingObjIndex) ||
        recordCounts.texcoords > static_cast<std::size_t>(kMissingObjIndex) ||
        recordCounts.normals > static_cast<std::size_t>(kMissingObjIndex)) {
        throw std::runtime_error("OBJ attribute record count exceeds renderer 32-bit mesh range: " + path.string());
    }
    if (recordCounts.faceVertices > static_cast<std::size_t>(kMissingObjIndex)) {
        throw std::runtime_error("OBJ mesh unique vertex estimate exceeds renderer 32-bit mesh range: " + path.string());
    }
    if (recordCounts.triangleIndices > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("OBJ mesh triangle index estimate exceeds renderer 32-bit mesh range: " + path.string());
    }
    const std::size_t estimatedUniqueVertices = recordCounts.faceVertices;
    positions.reserve(recordCounts.positions);
    texcoords.reserve(recordCounts.texcoords);
    normals.reserve(recordCounts.normals);
    validNormals.reserve(recordCounts.normals);
    mesh.vertices.reserve(estimatedUniqueVertices);
    mesh.indices.reserve(recordCounts.triangleIndices);
    generatedNormalSums.reserve(estimatedUniqueVertices);
    hasExplicitNormal.reserve(estimatedUniqueVertices);
    vertexLookup.reserve(estimatedUniqueVertices);
    const auto appendVertex = [&](const ObjFaceVertex& faceVertex, const std::uint64_t line) {
        const std::uint32_t position = resolveObjIndex(path, line, faceVertex.position, positions.size(), "position");
        const std::uint32_t texcoord = resolveOptionalObjIndex(path, line, faceVertex.texcoord, texcoords.size(), "texcoord");
        std::uint32_t normal = resolveOptionalObjIndex(path, line, faceVertex.normal, normals.size(), "normal");
        if (normal != kMissingObjIndex && validNormals[normal] == 0U) {
            normal = kMissingObjIndex;
        }
        const ObjVertexKey key{position, texcoord, normal};
        if (const auto found = vertexLookup.find(key); found != vertexLookup.end()) {
            return found->second;
        }
        if (mesh.vertices.size() >= static_cast<std::size_t>(kMissingObjIndex)) {
            throw objError(path, line, "mesh exceeds 32-bit index range");
        }
        const std::uint32_t index = static_cast<std::uint32_t>(mesh.vertices.size());
        vertexLookup.emplace(key, index);
        mesh.vertices.push_back(Vertex{positions[position],
                                       normal == kMissingObjIndex ? Vec3{} : normals[normal],
                                       texcoord == kMissingObjIndex ? Vec2{} : texcoords[texcoord]});
        generatedNormalSums.push_back({});
        hasExplicitNormal.push_back(normal == kMissingObjIndex ? 0U : 1U);
        return index;
    };

    const auto appendTriangle = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        const Vec3 edge0 = mesh.vertices[b].position - mesh.vertices[a].position;
        const Vec3 edge1 = mesh.vertices[c].position - mesh.vertices[a].position;
        const Vec3 faceNormal = cross(edge0, edge1);
        if (isDegenerateTriangle(edge0, edge1, faceNormal)) {
            return;
        }
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        for (const std::uint32_t index : std::array{a, b, c}) {
            if (hasExplicitNormal[index] == 0U) {
                generatedNormalSums[index] = generatedNormalSums[index] + faceNormal;
            }
        }
    };

    std::vector<std::uint32_t> faceIndices;
    faceIndices.reserve(std::max<std::size_t>(3U, std::min<std::size_t>(recordCounts.maxFaceVertices, 64U)));
    std::string lineText;
    std::uint64_t lineNumber = 0;
    while (std::getline(file, lineText)) {
        ++lineNumber;
        const std::size_t first = lineText.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || lineText[first] == '#') {
            continue;
        }

        std::string_view fields{lineText};
        fields.remove_prefix(first);
        const std::string_view keyword = nextObjToken(fields);
        if (keyword == "v") {
            appendObjVector(positions, fields, path, lineNumber, "position");
        } else if (keyword == "vt") {
            appendObjVector(texcoords, fields, path, lineNumber, "texcoord");
        } else if (keyword == "vn") {
            appendObjVector(normals, fields, path, lineNumber, "normal");
            normals.back() = normalizeOr(normals.back(), {});
            validNormals.push_back(isNearlyZero(normals.back()) ? 0U : 1U);
        } else if (keyword == "f") {
            faceIndices.clear();
            while (true) {
                const std::string_view token = nextObjToken(fields);
                if (token.empty() || token.front() == '#') {
                    break;
                }
                faceIndices.push_back(appendVertex(parseFaceVertex(path, lineNumber, token), lineNumber));
            }
            if (faceIndices.size() < 3U) {
                throw objError(path, lineNumber, "face has fewer than three vertices");
            }
            for (std::size_t i = 1; i + 1U < faceIndices.size(); ++i) {
                appendTriangle(faceIndices[0], faceIndices[i], faceIndices[i + 1U]);
            }
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        throw std::runtime_error("OBJ mesh contains no renderable faces: " + path.string());
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (hasExplicitNormal[i] == 0U) {
            mesh.vertices[i].normal = normalizeOr(generatedNormalSums[i], {0.0f, 1.0f, 0.0f});
        }
    }
    finalizeMesh(mesh);
    return mesh;
}

} // namespace ve
