#include "renderer/Geometry.hpp"

#include <cmath>
#include <numbers>

namespace ve {

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
    return mesh;
}

MeshData createUvSphereMesh(const std::uint32_t rings, const std::uint32_t segments) {
    MeshData mesh{};
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
    return mesh;
}

MeshData createGridMesh(const float halfExtent, const std::uint32_t divisions) {
    MeshData mesh{};
    const float step = (halfExtent * 2.0f) / static_cast<float>(divisions);
    for (std::uint32_t i = 0; i <= divisions; ++i) {
        const float x = -halfExtent + step * static_cast<float>(i);
        const float z = -halfExtent + step * static_cast<float>(i);
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{x, 0.0f, -halfExtent}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}});
        mesh.vertices.push_back({{x, 0.0f, halfExtent}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}});
        mesh.vertices.push_back({{-halfExtent, 0.0f, z}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}});
        mesh.vertices.push_back({{halfExtent, 0.0f, z}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}});
        mesh.indices.push_back(base);
        mesh.indices.push_back(base + 1U);
        mesh.indices.push_back(base + 2U);
        mesh.indices.push_back(base + 3U);
    }
    return mesh;
}

} // namespace ve
