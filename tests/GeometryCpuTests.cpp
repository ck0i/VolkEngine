#include "renderer/Geometry.hpp"

#include <array>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>


namespace {

int gFailureCount = 0;

template <typename T, typename U>
void expectEqual(std::string_view context, const T& actual, const U& expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}


template <typename T, typename U>
void expectNotEqual(std::string_view context, const T& actual, const U& expectedDifferent) {
    if (actual == expectedDifferent) {
        std::cerr << "[FAILED] " << context << ": expected value to differ from " << expectedDifferent << ", got same value\n";
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

template <typename F>
void expectRuntimeErrorContains(std::string_view context, std::string_view expectedMessage, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error containing \"" << expectedMessage << "\" but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error& e) {
        if (std::string_view{e.what()}.find(expectedMessage) == std::string_view::npos) {
            std::cerr << "[FAILED] " << context << ": expected runtime_error containing \"" << expectedMessage << "\" but got " << e.what()
                      << '\n';
            ++gFailureCount;
        }
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw different exception\n";
        ++gFailureCount;
    }
}

void expectNearly(std::string_view context, const float actual, const float expected, const float epsilon = 1.0e-6f) {
    if (std::fabs(actual - expected) > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << " (eps=" << epsilon << ')'
                  << '\n';
        ++gFailureCount;
    }
}

void expectVec3Nearly(std::string_view context, const ve::Vec3& actual, const ve::Vec3 expected, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectNearly(label + " x", actual.x, expected.x, epsilon);
    expectNearly(label + " y", actual.y, expected.y, epsilon);
    expectNearly(label + " z", actual.z, expected.z, epsilon);
}

void expectVec3NotNearly(std::string_view context, const ve::Vec3& actual, const ve::Vec3 expected, const float epsilon = 1.0e-5f) {
    const float dx = std::fabs(actual.x - expected.x);
    const float dy = std::fabs(actual.y - expected.y);
    const float dz = std::fabs(actual.z - expected.z);
    if (dx <= epsilon && dy <= epsilon && dz <= epsilon) {
        std::cerr << "[FAILED] " << context << ": vector was unexpectedly equal\n";
        ++gFailureCount;
    }
}

void expectVec2Nearly(std::string_view context, const ve::Vec2& actual, const ve::Vec2 expected, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectNearly(label + " u", actual.x, expected.x, epsilon);
    expectNearly(label + " v", actual.y, expected.y, epsilon);
}
void expectTangentBasisContract(std::string_view context, const ve::Vec4& tangent, const ve::Vec3& normal, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    const ve::Vec3 tangentDirection{tangent.x, tangent.y, tangent.z};
    if (!std::isfinite(tangent.x) || !std::isfinite(tangent.y) || !std::isfinite(tangent.z) || !std::isfinite(tangent.w) || !std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
        std::cerr << "[FAILED] " << label << ": tangent and normal components must be finite\n";
        ++gFailureCount;
        return;
    }

    const float tangentLength = ve::length(tangentDirection);
    expectNearly(label + " tangent xyz is unit length", tangentLength, 1.0f, epsilon);
    expectNearly(label + " tangent xyz is orthogonal to normal", ve::dot(tangentDirection, normal), 0.0f, epsilon);
    if (!((std::fabs(tangent.w - 1.0f) <= epsilon) || (std::fabs(tangent.w + 1.0f) <= epsilon))) {
        std::cerr << "[FAILED] " << label << ": tangent.w should be +1 or -1, got " << tangent.w << '\n';
        ++gFailureCount;
    }
}

void expectTangentIsPositiveXWithPositiveHandedness(std::string_view context, const ve::Vec4& tangent, const ve::Vec3& normal, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectTangentBasisContract(context, tangent, normal, epsilon);
    expectVec3Nearly(label + " tangent xyz is +X", ve::Vec3{tangent.x, tangent.y, tangent.z}, {1.0f, 0.0f, 0.0f}, epsilon);
    expectNearly(label + " tangent handedness is +1", tangent.w, 1.0f, epsilon);
}
void expectTangentIsPositiveXWithNegativeHandedness(std::string_view context, const ve::Vec4& tangent, const ve::Vec3& normal, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectTangentBasisContract(context, tangent, normal, epsilon);
    expectVec3Nearly(label + " tangent xyz is +X", ve::Vec3{tangent.x, tangent.y, tangent.z}, {1.0f, 0.0f, 0.0f}, epsilon);
    expectNearly(label + " tangent handedness is -1", tangent.w, -1.0f, epsilon);
}


void expectMeshBoundsNearly(std::string_view context, const ve::MeshBounds& bounds, const ve::Vec3 expectedCenter, const float expectedRadius, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectEqual(label + " valid", bounds.valid, true);
    expectVec3Nearly(label + " center", bounds.center, expectedCenter, epsilon);
    expectNearly(label + " radius", bounds.radius, expectedRadius, epsilon);
}

void expectMeshBoundsFromVertices(std::string_view context, const ve::MeshData& mesh, const float epsilon = 1.0e-5f) {
    if (mesh.vertices.empty()) {
        std::cerr << "[FAILED] " << context << ": mesh has no vertices\n";
        ++gFailureCount;
        return;
    }

    ve::Vec3 minPosition = mesh.vertices.at(0).position;
    ve::Vec3 maxPosition = mesh.vertices.at(0).position;
    for (const auto& vertex : mesh.vertices) {
        minPosition.x = std::min(minPosition.x, vertex.position.x);
        minPosition.y = std::min(minPosition.y, vertex.position.y);
        minPosition.z = std::min(minPosition.z, vertex.position.z);
        maxPosition.x = std::max(maxPosition.x, vertex.position.x);
        maxPosition.y = std::max(maxPosition.y, vertex.position.y);
        maxPosition.z = std::max(maxPosition.z, vertex.position.z);
    }
    const ve::Vec3 expectedCenter = (minPosition + maxPosition) * 0.5f;
    float expectedRadius = 0.0f;
    for (const auto& vertex : mesh.vertices) {
        const float distance = ve::length(vertex.position - expectedCenter);
        if (distance > expectedRadius) {
            expectedRadius = distance;
        }
    }
    expectMeshBoundsNearly(context, mesh.bounds, expectedCenter, expectedRadius, epsilon);
}

[[nodiscard]] std::vector<std::array<std::uint32_t, 3>> sortedTriangleKeys(const std::vector<std::uint32_t>& indices) {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    triangles.reserve(indices.size() / 3U);
    for (std::size_t index = 0; index + 2U < indices.size(); index += 3U) {
        std::array<std::uint32_t, 3> triangle{indices[index], indices[index + 1U], indices[index + 2U]};
        std::sort(triangle.begin(), triangle.end());
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

[[nodiscard]] std::uint32_t countVertexCacheMisses(const std::vector<std::uint32_t>& indices, const std::uint32_t cacheSize) {
    std::vector<std::uint32_t> cache;
    cache.reserve(cacheSize);
    std::uint32_t misses = 0;
    for (const std::uint32_t index : indices) {
        if (const auto found = std::find(cache.begin(), cache.end(), index); found != cache.end()) {
            cache.erase(found);
        } else {
            ++misses;
        }
        cache.insert(cache.begin(), index);
        if (cache.size() > cacheSize) {
            cache.pop_back();
        }
    }
    return misses;
}




std::filesystem::path writeObjFixture(std::string_view name, std::string_view contents) {
    const auto nowSigned = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    const std::uint64_t now = static_cast<std::uint64_t>(nowSigned);
    static std::uint64_t fixtureSequence = 0ULL;
    const std::string fileName = std::string(name) + "_" + std::to_string(now) + "_" + std::to_string(fixtureSequence++) + ".obj";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream fixture(path, std::ios::binary | std::ios::trunc);
    if (!fixture) {
        throw std::runtime_error("Failed to create OBJ fixture " + path.string());
    }
    fixture.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!fixture) {
        throw std::runtime_error("Failed to write OBJ fixture " + path.string());
    }
    return path;
}

void removeIfExists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

int main() {
    std::vector<std::filesystem::path> fixtures;
    const auto addFixture = [&](std::string_view name, std::string_view body) {
        const auto path = writeObjFixture(name, body);
        fixtures.push_back(path);
        return path;
    };

    {
        const auto fixture = addFixture("geometry_cpu_quad",
                                       "# square quad with position/uv/normal\n"
                                       "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 1 1 0\n"
                                       "v 0 1 0\n"
                                       "vt 0 0\n"
                                       "vt 1 0\n"
                                       "vt 1 1\n"
                                       "vt 0 1\n"
                                       "vn 0 0 1\n"
                                       "f 1/1/1 2/2/1 3/3/1 4/4/1\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh triangulates quads with fan order", [&] {
            mesh = ve::loadObjMesh(fixture);
        });

        expectEqual("quad fixture produces two triangles", mesh.indices.size(), static_cast<std::size_t>(6));
        expectEqual("quad fixture deduplicates shared corner tuples", mesh.vertices.size(), static_cast<std::size_t>(4));
        if (mesh.indices.size() == 6U && mesh.vertices.size() == 4U) {
            expectEqual("quad fixture first triangle indices", mesh.indices[0], static_cast<std::uint32_t>(0));
            expectEqual("quad fixture first triangle second index", mesh.indices[1], static_cast<std::uint32_t>(1));
            expectEqual("quad fixture first triangle third index", mesh.indices[2], static_cast<std::uint32_t>(2));
            expectEqual("quad fixture second triangle first index", mesh.indices[3], static_cast<std::uint32_t>(0));
            expectEqual("quad fixture second triangle second index", mesh.indices[4], static_cast<std::uint32_t>(2));
            expectEqual("quad fixture second triangle third index", mesh.indices[5], static_cast<std::uint32_t>(3));

            expectVec3Nearly("quad vertex0 position", mesh.vertices.at(0).position, {0.0f, 0.0f, 0.0f});
            expectVec3Nearly("quad vertex1 position", mesh.vertices.at(1).position, {1.0f, 0.0f, 0.0f});
            expectVec3Nearly("quad vertex2 position", mesh.vertices.at(2).position, {1.0f, 1.0f, 0.0f});
            expectVec3Nearly("quad vertex3 position", mesh.vertices.at(3).position, {0.0f, 1.0f, 0.0f});
            expectVec2Nearly("quad vertex0 uv", mesh.vertices.at(0).uv, {0.0f, 0.0f});
            expectVec2Nearly("quad vertex1 uv", mesh.vertices.at(1).uv, {1.0f, 0.0f});
            expectVec2Nearly("quad vertex2 uv", mesh.vertices.at(2).uv, {1.0f, 1.0f});
            expectVec2Nearly("quad vertex3 uv", mesh.vertices.at(3).uv, {0.0f, 1.0f});
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                expectVec3Nearly("quad vertex normal is unit z", mesh.vertices.at(i).normal, {0.0f, 0.0f, 1.0f});
            }
            expectTangentIsPositiveXWithPositiveHandedness("quad vertex0 tangent", mesh.vertices.at(0).tangent, mesh.vertices.at(0).normal);
            expectTangentIsPositiveXWithPositiveHandedness("quad vertex1 tangent", mesh.vertices.at(1).tangent, mesh.vertices.at(1).normal);
            expectTangentIsPositiveXWithPositiveHandedness("quad vertex2 tangent", mesh.vertices.at(2).tangent, mesh.vertices.at(2).normal);
            expectTangentIsPositiveXWithPositiveHandedness("quad vertex3 tangent", mesh.vertices.at(3).tangent, mesh.vertices.at(3).normal);
        }
    }
    {
        const auto fixture = addFixture("geometry_cpu_loose_tokens",
                                       "# leading plus signs, extra vector fields, and inline comments\n"
                                       "v +0 +0 +0 1 # tolerated position w\n"
                                       "v +1 +0 +0 1 # tolerated position w\n"
                                       "v +0 +1 +0 1 # tolerated position w\n"
                                       "vt +0 +0 0 # tolerated texcoord w\n"
                                       "vt +1 +0 0 # tolerated texcoord w\n"
                                       "vt +0 +1 0 # tolerated texcoord w\n"
                                       "vn +0 +0 +1 # inline normal comment\n"
                                       "f 1/1/1 2/2/1 3/3/1 # face parser stops at comment\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh preserves loose OBJ token compatibility", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("loose token mesh has one triangle", mesh.indices.size(), static_cast<std::size_t>(3));
        expectEqual("loose token mesh dedupes triangle vertices", mesh.vertices.size(), static_cast<std::size_t>(3));
        if (mesh.indices.size() == 3U && mesh.vertices.size() == 3U) {
            expectVec3Nearly("loose token vertex0 position", mesh.vertices.at(0).position, {0.0f, 0.0f, 0.0f});
            expectVec3Nearly("loose token vertex1 position", mesh.vertices.at(1).position, {1.0f, 0.0f, 0.0f});
            expectVec3Nearly("loose token vertex2 position", mesh.vertices.at(2).position, {0.0f, 1.0f, 0.0f});
            expectVec2Nearly("loose token vertex0 uv", mesh.vertices.at(0).uv, {0.0f, 0.0f});
            expectVec2Nearly("loose token vertex1 uv", mesh.vertices.at(1).uv, {1.0f, 0.0f});
            expectVec2Nearly("loose token vertex2 uv", mesh.vertices.at(2).uv, {0.0f, 1.0f});
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                expectVec3Nearly("loose token normal", mesh.vertices.at(i).normal, {0.0f, 0.0f, 1.0f});
                expectTangentIsPositiveXWithPositiveHandedness(std::string("loose token tangent ") + std::to_string(i), mesh.vertices.at(i).tangent, mesh.vertices.at(i).normal);
            }
        }
    }

    {
        const auto fixture = addFixture("geometry_cpu_negative", 
                                       "# negative indexing with explicit uv/normal\n"
                                       "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 1 1 0\n"
                                       "v 0 1 0\n"
                                       "vt 0 0\n"
                                       "vt 1 0\n"
                                       "vt 1 1\n"
                                       "vn 0 0 1\n"
                                       "f -4/1/1 -3/2/1 -2/3/1\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh resolves negative indices", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("negative index mesh has one triangle", mesh.indices.size(), static_cast<std::size_t>(3));
        expectEqual("negative index mesh dedupes triangle vertices", mesh.vertices.size(), static_cast<std::size_t>(3));
        if (mesh.indices.size() == 3U && mesh.vertices.size() == 3U) {
            expectVec3Nearly("negative index resolves to first vertex", mesh.vertices.at(mesh.indices.at(0)).position, {0.0f, 0.0f, 0.0f});
            expectVec3Nearly("negative index resolves to second vertex", mesh.vertices.at(mesh.indices.at(1)).position, {1.0f, 0.0f, 0.0f});
            expectVec3Nearly("negative index resolves to third vertex", mesh.vertices.at(mesh.indices.at(2)).position, {1.0f, 1.0f, 0.0f});
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            expectVec3Nearly("negative index generated normal", mesh.vertices.at(i).normal, {0.0f, 0.0f, 1.0f});
        }
    }

    {
        const auto fixture = addFixture("geometry_cpu_mixed_formats",
                                       "# same position reused across mixed OBJ tuple formats\n"
                                       "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 0 1 0\n"
                                       "vt 0 0\n"
                                       "vt 1 0\n"
                                       "vt 0 1\n"
                                       "vn 0 0 1\n"
                                       "vn 0 1 0\n"
                                       "f 1/1 2/2 3/3\n"
                                       "f 1/1 2/2 3/3\n"
                                       "f 1//2 2//2 3//2\n"
                                       "f 1//2 2//2 3//2\n"
                                       "f 1/2/1 2/3/1 3/1/1\n"
                                       "f 1/2/1 2/3/1 3/1/1\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh handles mixed v/vt, v//vn, and v/vt/vn tuples", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("mixed format mesh has duplicated-face indices deduplicated by tuple", mesh.indices.size(), static_cast<std::size_t>(18));
        expectEqual("mixed format mesh preserves three unique tuple groups", mesh.vertices.size(), static_cast<std::size_t>(9));
        if (mesh.indices.size() == 18U && mesh.vertices.size() == 9U) {
            expectEqual("mixed format group 1 repeats exact tuple", mesh.indices[0], mesh.indices[3]);
            expectEqual("mixed format group 2 repeats exact tuple", mesh.indices[6], mesh.indices[9]);
            expectEqual("mixed format group 3 repeats exact tuple", mesh.indices[12], mesh.indices[15]);
            expectNotEqual("mixed format group 1 and 2 use different deduped tuples", mesh.indices[0], mesh.indices[6]);
            expectNotEqual("mixed format group 2 and 3 use different deduped tuples", mesh.indices[6], mesh.indices[12]);

            expectVec3Nearly("mixed format generated normal for v/vt tuple", mesh.vertices.at(0).normal, {0.0f, 0.0f, 1.0f});
            expectTangentIsPositiveXWithPositiveHandedness("mixed format v//vn fallback vertex0 tangent", mesh.vertices.at(3).tangent, mesh.vertices.at(3).normal);
            expectTangentIsPositiveXWithPositiveHandedness("mixed format v//vn fallback vertex1 tangent", mesh.vertices.at(4).tangent, mesh.vertices.at(4).normal);
            expectTangentIsPositiveXWithPositiveHandedness("mixed format v//vn fallback vertex2 tangent", mesh.vertices.at(5).tangent, mesh.vertices.at(5).normal);
            expectVec3Nearly("mixed format explicit normal for v//vn tuple", mesh.vertices.at(3).normal, {0.0f, 1.0f, 0.0f});
            expectVec3Nearly("mixed format explicit normal for v/vt/vn tuple", mesh.vertices.at(6).normal, {0.0f, 0.0f, 1.0f});
            expectVec2Nearly("mixed format v/vt/vn tuple keeps uv for first vertex", mesh.vertices.at(6).uv, {1.0f, 0.0f});
            expectVec3NotNearly("mixed format keeps explicit normal distinct from generated", mesh.vertices.at(3).normal, mesh.vertices.at(0).normal);
        }
    }

    {
        const auto fixture = addFixture("geometry_cpu_generated_normals",
                                       "# generated normals when vn absent\n"
                                       "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 0 1 0\n"
                                       "f 1 2 3\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh generates normalized normals when normal indices are absent", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("generated normals mesh has one triangle", mesh.indices.size(), static_cast<std::size_t>(3));
        expectEqual("generated normals mesh has 3 unique vertices", mesh.vertices.size(), static_cast<std::size_t>(3));
        if (mesh.indices.size() == 3U && mesh.vertices.size() == 3U) {
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                expectVec3Nearly("generated vertex normal", mesh.vertices.at(i).normal, {0.0f, 0.0f, 1.0f});
                expectNearly("generated normal is unit length", ve::length(mesh.vertices.at(i).normal), 1.0f, 1.0e-6f);
            }
            expectTangentIsPositiveXWithPositiveHandedness("generated normal vertex0 fallback tangent", mesh.vertices.at(0).tangent, mesh.vertices.at(0).normal);
            expectTangentIsPositiveXWithPositiveHandedness("generated normal vertex1 fallback tangent", mesh.vertices.at(1).tangent, mesh.vertices.at(1).normal);
            expectTangentIsPositiveXWithPositiveHandedness("generated normal vertex2 fallback tangent", mesh.vertices.at(2).tangent, mesh.vertices.at(2).normal);
        }
    }

    {
        const auto fixture = addFixture("geometry_cpu_degenerate_fan",
                                       "# first fan triangle is collinear and should not reach the index buffer\n"
                                       "v 0 0 0\n"
                                       "v 1000 0 0\n"
                                       "v 2 0 0\n"
                                       "v 1 1 0\n"
                                       "v 0 1 0\n"
                                       "f 1 2 3 4 5\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh drops degenerate OBJ fan triangles before emitting indices", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("degenerate fan skips zero-area triangle indices", mesh.indices.size(), static_cast<std::size_t>(6));
        expectEqual("degenerate fan compacts skipped-only tuple vertices", mesh.vertices.size(), static_cast<std::size_t>(4));
        expectVec3Nearly("degenerate fan bounds ignore skipped-only far vertex", mesh.bounds.center, {1.0f, 0.5f, 0.0f});
        expectNearly("degenerate fan radius ignores skipped-only far vertex", mesh.bounds.radius, 1.1180340f, 1.0e-5f);
    }

    {
        const auto fixture = addFixture("geometry_cpu_tiny_valid_triangle",
                                       "# tiny but well-shaped triangle must survive scale-aware degeneracy filtering\n"
                                       "v 0 0 0\n"
                                       "v 0.00000001 0 0\n"
                                       "v 0 0.00000001 0\n"
                                       "f 1 2 3\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh keeps tiny non-collinear OBJ triangles", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("tiny valid triangle emits one triangle", mesh.indices.size(), static_cast<std::size_t>(3));
        expectEqual("tiny valid triangle keeps three vertices", mesh.vertices.size(), static_cast<std::size_t>(3));
    }

    {
        ve::MeshData mesh;
        expectNoThrow("createCubeMesh computes bounds from vertex positions", [&] {
            mesh = ve::createCubeMesh();
        });
        expectMeshBoundsFromVertices("createCubeMesh bounds", mesh);
    }
    {
        ve::MeshData mesh;
        expectNoThrow("createUvSphereMesh computes bounds from vertex positions", [&] {
            mesh = ve::createUvSphereMesh(6U, 12U);
        });
        expectMeshBoundsFromVertices("createUvSphereMesh bounds", mesh);
    }
    {
        ve::MeshData mesh;
        constexpr float planeHalfExtent = 4.0f;
        expectNoThrow("createPlaneMesh computes bounds from vertex positions", [&] {
            mesh = ve::createPlaneMesh(planeHalfExtent, 12.0f);
        });
        expectMeshBoundsFromVertices("createPlaneMesh bounds", mesh);
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            expectTangentIsPositiveXWithNegativeHandedness(std::string("createPlaneMesh vertex ") + std::to_string(i) + " tangent", mesh.vertices.at(i).tangent, mesh.vertices.at(i).normal);
        }
    }
    {
        ve::MeshData mesh;
        constexpr float gridHalfExtent = 3.0f;
        constexpr std::uint32_t gridDivisions = 8U;
        expectNoThrow("createGridMesh computes bounds from vertex positions", [&] {
            mesh = ve::createGridMesh(gridHalfExtent, gridDivisions);
        });
        expectMeshBoundsFromVertices("createGridMesh bounds", mesh);
        expectEqual("createGridMesh uses line-list-like index count", mesh.indices.size(), static_cast<std::size_t>(4U) * static_cast<std::size_t>(gridDivisions + 1U));
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            expectTangentIsPositiveXWithPositiveHandedness(std::string("createGridMesh vertex ") + std::to_string(i) + " tangent", mesh.vertices.at(i).tangent, mesh.vertices.at(i).normal);
        }
    }
    expectThrowsRuntimeError("createUvSphereMesh rejects meshes outside 32-bit range", [] {
        (void)ve::createUvSphereMesh(65536U, 65536U);
    });

    expectThrowsRuntimeError("createGridMesh rejects meshes outside 32-bit range", [] {
        constexpr std::uint32_t overflowingDivisions = (std::numeric_limits<std::uint32_t>::max() / 4U) + 1U;
        (void)ve::createGridMesh(1.0f, overflowingDivisions);
    });

    {
        const auto fixture = addFixture("geometry_cpu_mirrored_uv",
                                       "# mirrored UV quad with mirrored determinant\n"
                                       "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 1 1 0\n"
                                       "v 0 1 0\n"
                                       "vt 0 0\n"
                                       "vt 1 0\n"
                                       "vt 1 -1\n"
                                       "vt 0 -1\n"
                                       "vn 0 0 1\n"
                                       "f 1/1/1 2/2/1 3/3/1 4/4/1\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh computes negative tangent handedness for mirrored UV", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("mirrored UV fixture produces two triangles", mesh.indices.size(), static_cast<std::size_t>(6));
        expectEqual("mirrored UV fixture deduplicates shared corner tuples", mesh.vertices.size(), static_cast<std::size_t>(4));
        if (mesh.indices.size() == 6U && mesh.vertices.size() == 4U) {
            expectVec3Nearly("mirrored UV vertex0 position", mesh.vertices.at(0).position, {0.0f, 0.0f, 0.0f});
            expectVec3Nearly("mirrored UV vertex1 position", mesh.vertices.at(1).position, {1.0f, 0.0f, 0.0f});
            expectVec3Nearly("mirrored UV vertex2 position", mesh.vertices.at(2).position, {1.0f, 1.0f, 0.0f});
            expectVec3Nearly("mirrored UV vertex3 position", mesh.vertices.at(3).position, {0.0f, 1.0f, 0.0f});
            expectVec2Nearly("mirrored UV vertex0 uv", mesh.vertices.at(0).uv, {0.0f, 0.0f});
            expectVec2Nearly("mirrored UV vertex1 uv", mesh.vertices.at(1).uv, {1.0f, 0.0f});
            expectVec2Nearly("mirrored UV vertex2 uv", mesh.vertices.at(2).uv, {1.0f, -1.0f});
            expectVec2Nearly("mirrored UV vertex3 uv", mesh.vertices.at(3).uv, {0.0f, -1.0f});
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                expectVec3Nearly("mirrored UV vertex normal is unit z", mesh.vertices.at(i).normal, {0.0f, 0.0f, 1.0f});
                expectTangentIsPositiveXWithNegativeHandedness(std::string("mirrored UV vertex ") + std::to_string(i) + " tangent", mesh.vertices.at(i).tangent, mesh.vertices.at(i).normal);
            }
        }
    }

    {
        const auto fixture = addFixture("geometry_cpu_asymmetric_bounds",
                                       "# asymmetric OBJ fixture with non-origin bounds\n"
                                       "v -1 -1 0\n"
                                       "v 3 -1 0\n"
                                       "v 3 4 5\n"
                                       "v -1 4 5\n"
                                       "f 1 2 3 4\n");
        ve::MeshData mesh;
        expectNoThrow("loadObjMesh computes bounds from asymmetric vertex positions", [&] {
            mesh = ve::loadObjMesh(fixture);
        });
        expectEqual("asymmetric OBJ fixture produces two triangles", mesh.indices.size(), static_cast<std::size_t>(6));
        expectEqual("asymmetric OBJ fixture has four unique vertices", mesh.vertices.size(), static_cast<std::size_t>(4));
        expectMeshBoundsFromVertices("asymmetric OBJ bounds", mesh);
        expectVec3NotNearly("asymmetric OBJ bounds center is not origin", mesh.bounds.center, {0.0f, 0.0f, 0.0f});
    }
    {
        std::vector<std::uint32_t> indices{
            0U, 1U, 2U,
            6U, 7U, 8U,
            2U, 1U, 3U,
            8U, 7U, 9U,
            3U, 1U, 4U,
            9U, 7U, 10U,
            4U, 1U, 5U,
            10U, 7U, 11U};
        const std::vector<std::uint32_t> original = indices;
        const auto originalTriangles = sortedTriangleKeys(original);
        const std::uint32_t originalMisses = countVertexCacheMisses(original, 3U);
        expectNoThrow("optimizeTriangleIndexOrderForVertexCache accepts valid triangles", [&] {
            ve::optimizeTriangleIndexOrderForVertexCache(indices, 12U, 3U);
        });
        if (sortedTriangleKeys(indices) != originalTriangles) {
            std::cerr << "[FAILED] vertex-cache optimizer should preserve triangle topology\n";
            ++gFailureCount;
        }
        if (indices == original) {
            std::cerr << "[FAILED] vertex-cache optimizer should reorder poor triangle order\n";
            ++gFailureCount;
        }
        const std::uint32_t optimizedMisses = countVertexCacheMisses(indices, 3U);
        if (!(optimizedMisses < originalMisses)) {
            std::cerr << "[FAILED] vertex-cache optimizer should reduce cache misses: before "
                      << originalMisses << ", after " << optimizedMisses << '\n';
            ++gFailureCount;
        }
    }

    expectThrowsRuntimeError("vertex-cache optimizer rejects incomplete triangle lists", [] {
        std::vector<std::uint32_t> indices{0U, 1U, 2U, 0U};
        ve::optimizeTriangleIndexOrderForVertexCache(indices, 3U, 3U);
    });

    expectThrowsRuntimeError("vertex-cache optimizer rejects out-of-range triangle indices", [] {
        std::vector<std::uint32_t> indices{0U, 1U, 3U};
        ve::optimizeTriangleIndexOrderForVertexCache(indices, 3U, 3U);
    });
    {
        const auto malformedFixture = addFixture("geometry_cpu_malformed", "v 0 0 0\nf 1 2\n");
        expectThrowsRuntimeError("loadObjMesh rejects malformed face with two vertices", [&] {
            (void)ve::loadObjMesh(malformedFixture);
        });

        const auto outOfRangeFixture = addFixture("geometry_cpu_out_of_range", "v 0 0 0\nf 2 1 1\n");
        expectThrowsRuntimeError("loadObjMesh rejects out-of-range vertex index", [&] {
            (void)ve::loadObjMesh(outOfRangeFixture);
        });

        const auto uint32RangeFixture = addFixture("geometry_cpu_uint32_range_index", "v 0 0 0\nf 2147483648 1 1\n");
        expectRuntimeErrorContains("loadObjMesh parses uint32-range OBJ indices before range validation", "out-of-range position index", [&] {
            (void)ve::loadObjMesh(uint32RangeFixture);
        });

        const auto emptyFixture = addFixture("geometry_cpu_empty", "# no geometry here\n# only comments\n");
        expectThrowsRuntimeError("loadObjMesh rejects OBJ without faces", [&] {
            (void)ve::loadObjMesh(emptyFixture);
        });
    }

    for (const auto& fixture : fixtures) {
        removeIfExists(fixture);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "Geometry CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}