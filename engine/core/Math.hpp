#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ve {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct alignas(16) Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct TransformTRS {
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct alignas(16) Mat4 {
    // Column-major storage, matching GLSL mat4 layout.
    std::array<float, 16> m{};

    [[nodiscard]] static Mat4 identity() {
        Mat4 r{};
        r.m[0] = 1.0f;
        r.m[5] = 1.0f;
        r.m[10] = 1.0f;
        r.m[15] = 1.0f;
        return r;
    }
};

[[nodiscard]] inline Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Vec3 operator*(const Vec3 a, const float s) { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] inline Vec3 operator/(const Vec3 a, const float s) { return {a.x / s, a.y / s, a.z / s}; }

[[nodiscard]] inline float dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] inline Vec3 cross(const Vec3 a, const Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(const Vec3 v) { return std::sqrt(dot(v, v)); }
[[nodiscard]] inline Vec3 normalize(const Vec3 v) {
    const float len = length(v);
    if (len <= 0.000001f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return v / len;
}

[[nodiscard]] inline bool finite(const Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] inline bool finite(const Quat value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] inline bool finite(const TransformTRS& transform) noexcept {
    return finite(transform.translation) && finite(transform.rotation) && finite(transform.scale);
}

[[nodiscard]] inline Quat normalizeQuat(const Quat value) noexcept {
    const float maximumComponent = std::max(
        {std::fabs(value.x), std::fabs(value.y), std::fabs(value.z), std::fabs(value.w)});
    if (!std::isfinite(maximumComponent) || maximumComponent == 0.0f) {
        return {};
    }
    const Quat scaled{
        value.x / maximumComponent,
        value.y / maximumComponent,
        value.z / maximumComponent,
        value.w / maximumComponent};
    const float inverseLength = 1.0f / std::sqrt(
        scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z + scaled.w * scaled.w);
    return {scaled.x * inverseLength,
            scaled.y * inverseLength,
            scaled.z * inverseLength,
            scaled.w * inverseLength};
}

[[nodiscard]] inline Quat rotationY(const float radians) noexcept {
    if (!std::isfinite(radians)) {
        return {};
    }
    const float halfRadians = radians * 0.5f;
    return {0.0f, std::sin(halfRadians), 0.0f, std::cos(halfRadians)};
}

[[nodiscard]] inline Quat slerp(const Quat from, const Quat to, float alpha) noexcept {
    alpha = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f;
    const Quat normalizedFrom = normalizeQuat(from);
    Quat normalizedTo = normalizeQuat(to);
    float cosine = normalizedFrom.x * normalizedTo.x + normalizedFrom.y * normalizedTo.y +
                   normalizedFrom.z * normalizedTo.z + normalizedFrom.w * normalizedTo.w;
    if (cosine < 0.0f) {
        normalizedTo = {-normalizedTo.x, -normalizedTo.y, -normalizedTo.z, -normalizedTo.w};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    if (cosine > 0.9995f) {
        return normalizeQuat({normalizedFrom.x + (normalizedTo.x - normalizedFrom.x) * alpha,
                          normalizedFrom.y + (normalizedTo.y - normalizedFrom.y) * alpha,
                          normalizedFrom.z + (normalizedTo.z - normalizedFrom.z) * alpha,
                          normalizedFrom.w + (normalizedTo.w - normalizedFrom.w) * alpha});
    }
    const float theta = std::acos(cosine);
    const float sine = std::sin(theta);
    if (!std::isfinite(sine) || std::fabs(sine) <= 0.000001f) {
        return normalizedFrom;
    }
    const float fromWeight = std::sin((1.0f - alpha) * theta) / sine;
    const float toWeight = std::sin(alpha * theta) / sine;
    return normalizeQuat({normalizedFrom.x * fromWeight + normalizedTo.x * toWeight,
                      normalizedFrom.y * fromWeight + normalizedTo.y * toWeight,
                      normalizedFrom.z * fromWeight + normalizedTo.z * toWeight,
                      normalizedFrom.w * fromWeight + normalizedTo.w * toWeight});
}

[[nodiscard]] inline Mat4 compose(const TransformTRS& transform) noexcept {
    const Quat rotation = normalizeQuat(transform.rotation);
    const float xx = rotation.x * rotation.x;
    const float yy = rotation.y * rotation.y;
    const float zz = rotation.z * rotation.z;
    const float xy = rotation.x * rotation.y;
    const float xz = rotation.x * rotation.z;
    const float yz = rotation.y * rotation.z;
    const float xw = rotation.x * rotation.w;
    const float yw = rotation.y * rotation.w;
    const float zw = rotation.z * rotation.w;
    Mat4 matrix = Mat4::identity();
    matrix.m[0] = (1.0f - 2.0f * (yy + zz)) * transform.scale.x;
    matrix.m[1] = (2.0f * (xy + zw)) * transform.scale.x;
    matrix.m[2] = (2.0f * (xz - yw)) * transform.scale.x;
    matrix.m[4] = (2.0f * (xy - zw)) * transform.scale.y;
    matrix.m[5] = (1.0f - 2.0f * (xx + zz)) * transform.scale.y;
    matrix.m[6] = (2.0f * (yz + xw)) * transform.scale.y;
    matrix.m[8] = (2.0f * (xz + yw)) * transform.scale.z;
    matrix.m[9] = (2.0f * (yz - xw)) * transform.scale.z;
    matrix.m[10] = (1.0f - 2.0f * (xx + yy)) * transform.scale.z;
    matrix.m[12] = transform.translation.x;
    matrix.m[13] = transform.translation.y;
    matrix.m[14] = transform.translation.z;
    return matrix;
}

[[nodiscard]] inline TransformTRS interpolate(const TransformTRS& previous,
                                              const TransformTRS& current,
                                              float alpha) noexcept {
    alpha = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f;
    return {previous.translation + (current.translation - previous.translation) * alpha,
            slerp(previous.rotation, current.rotation, alpha),
            previous.scale + (current.scale - previous.scale) * alpha};
}

[[nodiscard]] inline std::array<Vec4, 3> normalMatrixColumns(const Mat4& matrix) {
    const Vec3 c0{matrix.m[0], matrix.m[1], matrix.m[2]};
    const Vec3 c1{matrix.m[4], matrix.m[5], matrix.m[6]};
    const Vec3 c2{matrix.m[8], matrix.m[9], matrix.m[10]};
    Vec3 n0 = cross(c1, c2);
    Vec3 n1 = cross(c2, c0);
    Vec3 n2 = cross(c0, c1);
    const float determinant = dot(c0, n0);
    if (std::fabs(determinant) <= 0.000001f) {
        return {Vec4{1.0f, 0.0f, 0.0f, 1.0f},
                Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                Vec4{0.0f, 0.0f, 1.0f, 0.0f}};
    }
    const float invDeterminant = 1.0f / determinant;
    const float handedness = determinant < 0.0f ? -1.0f : 1.0f;
    n0 = n0 * invDeterminant;
    n1 = n1 * invDeterminant;
    n2 = n2 * invDeterminant;
    return {Vec4{n0.x, n0.y, n0.z, handedness},
            Vec4{n1.x, n1.y, n1.z, 0.0f},
            Vec4{n2.x, n2.y, n2.z, 0.0f}};
}

[[nodiscard]] inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r.m[static_cast<std::size_t>(col * 4 + row)] =
                a.m[static_cast<std::size_t>(0 * 4 + row)] * b.m[static_cast<std::size_t>(col * 4 + 0)] +
                a.m[static_cast<std::size_t>(1 * 4 + row)] * b.m[static_cast<std::size_t>(col * 4 + 1)] +
                a.m[static_cast<std::size_t>(2 * 4 + row)] * b.m[static_cast<std::size_t>(col * 4 + 2)] +
                a.m[static_cast<std::size_t>(3 * 4 + row)] * b.m[static_cast<std::size_t>(col * 4 + 3)];
        }
    }
    return r;
}

[[nodiscard]] inline Mat4 translate(const Vec3 t) {
    Mat4 r = Mat4::identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

[[nodiscard]] inline Mat4 scale(const Vec3 s) {
    Mat4 r = Mat4::identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

[[nodiscard]] inline Mat4 rotateY(const float radians) {
    Mat4 r = Mat4::identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

[[nodiscard]] inline Mat4 perspective(const float verticalFovRadians, const float aspect, const float nearPlane, const float farPlane) {
    const float f = 1.0f / std::tan(verticalFovRadians * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f; // Vulkan clip-space Y is inverted relative to OpenGL-style math.
    r.m[10] = nearPlane / (farPlane - nearPlane);
    r.m[11] = -1.0f;
    r.m[14] = (farPlane * nearPlane) / (farPlane - nearPlane);
    return r;
}

[[nodiscard]] inline Mat4 lookAt(const Vec3 eye, const Vec3 center, const Vec3 up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 r = Mat4::identity();
    r.m[0] = s.x;
    r.m[1] = u.x;
    r.m[2] = -f.x;
    r.m[4] = s.y;
    r.m[5] = u.y;
    r.m[6] = -f.y;
    r.m[8] = s.z;
    r.m[9] = u.z;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

constexpr float radians(const float degrees) {
    return degrees * (std::numbers::pi_v<float> / 180.0f);
}

} // namespace ve
