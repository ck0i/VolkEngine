#include "core/Math.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
namespace {

int gFailureCount = 0;



template <typename T>
void expectNearly(std::string_view context, const T actual, const T expected, const T epsilon = static_cast<T>(1.0e-5)) {
    const T delta = static_cast<T>(std::fabs(static_cast<double>(actual - expected)));
    if (delta > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << " (eps=" << epsilon << ")\n";
        ++gFailureCount;
    }
}

void expectVec3Nearly(std::string_view context, const ve::Vec3& actual, const ve::Vec3 expected, const float epsilon = 1.0e-5f) {
    const std::string label{context};
    expectNearly(label + " x", actual.x, expected.x, epsilon);
    expectNearly(label + " y", actual.y, expected.y, epsilon);
    expectNearly(label + " z", actual.z, expected.z, epsilon);
}

void expectVec3Finite(std::string_view context, const ve::Vec3& actual) {
    if (!std::isfinite(actual.x) || !std::isfinite(actual.y) || !std::isfinite(actual.z)) {
        std::cerr << "[FAILED] " << context << ": vector contains non-finite value\n";
        ++gFailureCount;
    }
}

[[nodiscard]] ve::Vec3 transformByMat3(const ve::Mat4& matrix, const ve::Vec3& vector) {
    return {
        matrix.m[0] * vector.x + matrix.m[4] * vector.y + matrix.m[8] * vector.z,
        matrix.m[1] * vector.x + matrix.m[5] * vector.y + matrix.m[9] * vector.z,
        matrix.m[2] * vector.x + matrix.m[6] * vector.y + matrix.m[10] * vector.z
    };
}

[[nodiscard]] float projectDepth(const ve::Mat4& projection, const float viewSpaceZ) {
    const float clipZ = projection.m[10] * viewSpaceZ + projection.m[14];
    const float clipW = projection.m[11] * viewSpaceZ;
    return clipZ / clipW;
}

[[nodiscard]] ve::Vec3 transformByNormalColumns(const std::array<ve::Vec4, 3>& normalColumns, const ve::Vec3& vector) {
    return {
        normalColumns.at(0).x * vector.x + normalColumns.at(1).x * vector.y + normalColumns.at(2).x * vector.z,
        normalColumns.at(0).y * vector.x + normalColumns.at(1).y * vector.y + normalColumns.at(2).y * vector.z,
        normalColumns.at(0).z * vector.x + normalColumns.at(1).z * vector.y + normalColumns.at(2).z * vector.z
    };
}

void expectTransformedNormalOrthogonalToTransformedSurfaceAxis(std::string_view context,
                                                              const ve::Mat4& model,
                                                              const std::array<ve::Vec4, 3>& normalColumns,
                                                              const ve::Vec3& tangent,
                                                              const ve::Vec3& normal,
                                                              const float epsilon = 1.0e-5f) {
    const ve::Vec3 transformedTangent = transformByMat3(model, tangent);
    const ve::Vec3 transformedNormal = transformByNormalColumns(normalColumns, normal);

    expectVec3Finite(std::string(context) + " transformed tangent", transformedTangent);
    expectVec3Finite(std::string(context) + " transformed normal", transformedNormal);
    expectNearly(std::string(context) + " transformed normal orthogonal to transformed tangent",
                ve::dot(transformedTangent, transformedNormal),
                0.0f,
                epsilon);
}

} // namespace

int main() {
    {
        constexpr float nearPlane = 0.05f;
        constexpr float farPlane = 500.0f;
        const ve::Mat4 projection = ve::perspective(ve::radians(65.0f), 16.0f / 9.0f, nearPlane, farPlane);
        expectNearly("reverse-Z perspective maps near plane to depth 1", projectDepth(projection, -nearPlane), 1.0f, 1.0e-5f);
        expectNearly("reverse-Z perspective maps far plane to depth 0", projectDepth(projection, -farPlane), 0.0f, 1.0e-5f);
        const float midDepth = projectDepth(projection, -10.0f);
        if (!(midDepth > 0.0f && midDepth < 1.0f)) {
            std::cerr << "[FAILED] reverse-Z perspective keeps mid-depth inside [0,1]: got " << midDepth << '\n';
            ++gFailureCount;
        }
    }

    {
        const auto normalColumns = ve::normalMatrixColumns(ve::scale({2.0f, 4.0f, 0.5f}));
        expectVec3Nearly("scale(2,4,0.5) normalMatrix0",
                         ve::Vec3{normalColumns.at(0).x, normalColumns.at(0).y, normalColumns.at(0).z},
                         ve::Vec3{0.5f, 0.0f, 0.0f});
        expectVec3Nearly("scale(2,4,0.5) normalMatrix1",
                         ve::Vec3{normalColumns.at(1).x, normalColumns.at(1).y, normalColumns.at(1).z},
                         ve::Vec3{0.0f, 0.25f, 0.0f});
        expectVec3Nearly("scale(2,4,0.5) normalMatrix2",
                         ve::Vec3{normalColumns.at(2).x, normalColumns.at(2).y, normalColumns.at(2).z},
                         ve::Vec3{0.0f, 0.0f, 2.0f});
        expectNearly("scale(2,4,0.5) handedness", normalColumns.at(0).w, 1.0f);
    }

    {
        const ve::Mat4 model = ve::rotateY(ve::radians(47.0f)) * ve::scale({2.0f, 4.0f, 0.5f});
        const auto normalColumns = ve::normalMatrixColumns(model);

        expectNearly("rotation+scale normalMatrix0 handedness", normalColumns.at(0).w, 1.0f);

        const ve::Vec3 tangentX{1.0f, 0.0f, 0.0f};
        const ve::Vec3 normalZ{0.0f, 0.0f, 1.0f};
        const ve::Vec3 tangentY{0.0f, 1.0f, 0.0f};
        const ve::Vec3 normalX{1.0f, 0.0f, 0.0f};
        const ve::Vec3 diagonalTangent = ve::normalize(ve::Vec3{1.0f, 0.0f, 1.0f});
        const ve::Vec3 diagonalNormal = ve::normalize(ve::Vec3{-1.0f, 0.0f, 1.0f});

        expectTransformedNormalOrthogonalToTransformedSurfaceAxis("rotation+scale preserves orthogonality between tangentX and normalZ",
                                                               model,
                                                               normalColumns,
                                                               tangentX,
                                                               normalZ,
                                                               1.0e-4f);
        expectTransformedNormalOrthogonalToTransformedSurfaceAxis("rotation+scale preserves orthogonality between tangentY and normalX",
                                                               model,
                                                               normalColumns,
                                                               tangentY,
                                                               normalX,
                                                               1.0e-4f);
        expectTransformedNormalOrthogonalToTransformedSurfaceAxis("rotation+scale preserves orthogonality in diagonal XZ basis",
                                                               model,
                                                               normalColumns,
                                                               diagonalTangent,
                                                               diagonalNormal,
                                                               1.0e-4f);
    }

    {
        const auto normalColumns = ve::normalMatrixColumns(ve::scale({-2.0f, 4.0f, 0.5f}));
        expectNearly("negative scale handedness", normalColumns.at(0).w, -1.0f);
        expectVec3Nearly("negative scale normalMatrix0",
                         ve::Vec3{normalColumns.at(0).x, normalColumns.at(0).y, normalColumns.at(0).z},
                         ve::Vec3{-0.5f, 0.0f, 0.0f});
        expectVec3Nearly("negative scale normalMatrix1",
                         ve::Vec3{normalColumns.at(1).x, normalColumns.at(1).y, normalColumns.at(1).z},
                         ve::Vec3{0.0f, 0.25f, 0.0f});
        expectVec3Nearly("negative scale normalMatrix2",
                         ve::Vec3{normalColumns.at(2).x, normalColumns.at(2).y, normalColumns.at(2).z},
                         ve::Vec3{0.0f, 0.0f, 2.0f});
    }

    {
        const auto singular = ve::scale({0.0f, 4.0f, 2.0f}) * ve::translate({3.0f, 5.0f, -7.0f});
        const auto normalColumns = ve::normalMatrixColumns(singular);
        expectVec3Nearly("singular scale fallback normalMatrix0",
                         ve::Vec3{normalColumns.at(0).x, normalColumns.at(0).y, normalColumns.at(0).z},
                         ve::Vec3{1.0f, 0.0f, 0.0f});
        expectVec3Nearly("singular scale fallback normalMatrix1",
                         ve::Vec3{normalColumns.at(1).x, normalColumns.at(1).y, normalColumns.at(1).z},
                         ve::Vec3{0.0f, 1.0f, 0.0f});
        expectVec3Nearly("singular scale fallback normalMatrix2",
                         ve::Vec3{normalColumns.at(2).x, normalColumns.at(2).y, normalColumns.at(2).z},
                         ve::Vec3{0.0f, 0.0f, 1.0f});
        expectNearly("singular scale fallback handedness", normalColumns.at(0).w, 1.0f);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "Math CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
