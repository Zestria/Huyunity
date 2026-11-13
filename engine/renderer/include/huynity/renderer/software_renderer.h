#pragma once

#include "huynity/image/image_buffer.h"
#include "huynity/render_contracts/render_snapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <list>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace huynity::renderer {

enum class RenderError {
    SizeMismatch,    // размеры framebuffer не совпадают с размерами снимка
    InvalidResource, // draw item ссылается на отсутствующий mesh-ресурс или вершину
    InvalidMaterial, // draw item ссылается на отсутствующий материал
};

using render_contracts::TextureFilter;
using render_contracts::TextureWrap;

namespace detail {

struct ProjectedVertex {
    double ndcX{};
    double ndcY{};
    double invZ{};   // 1/z для перспективы, 1 для ортографической камеры
    double zOverW{}; // z * invZ: даёт z = sum(w_i * zOverW_i) / sum(w_i * invZ_i)
};

/// @brief Параметры проекции одной камеры снимка.
///
/// View space: +z направлен вглубь сцены, камера в начале координат. Недопустимая
/// камера (нечисловые или противоречивые параметры) не проецирует ни одной вершины,
/// что согласовано с culling на стороне подготовки кадра.
struct Projection {
    double tanHalfFov{};
    double aspect{};
    double halfWidth{};
    double halfHeight{};
    double nearPlane{};
    double farPlane{};
    bool isPerspective{};

    [[nodiscard]] bool isValid() const noexcept {
        if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) || farPlane <= nearPlane) {
            return false;
        }
        if (isPerspective) {
            return nearPlane > 0 && std::isfinite(tanHalfFov) && tanHalfFov > 0 && aspect > 0;
        }
        return nearPlane >= 0 && halfWidth > 0 && halfHeight > 0 && std::isfinite(halfWidth) &&
               std::isfinite(halfHeight);
    }

    /// @return вершину в NDC или nullopt, если она вне [nearPlane, farPlane];
    /// частичное отсечение треугольников не выполняется.
    [[nodiscard]] std::optional<ProjectedVertex> project(const math::Point3& view) const noexcept {
        const double z = view[2];
        if (!isValid() || z < nearPlane || z > farPlane) {
            return std::nullopt;
        }
        if (isPerspective) {
            const double halfHeightAtDepth = z * tanHalfFov;
            return ProjectedVertex{view[0] / (halfHeightAtDepth * aspect), view[1] / halfHeightAtDepth, 1 / z, 1};
        }
        return ProjectedVertex{view[0] / halfWidth, view[1] / halfHeight, 1, z};
    }
};

[[nodiscard]] inline Projection makeProjection(const render_contracts::Camera3& camera, std::size_t imageWidth,
                                               std::size_t imageHeight) noexcept {
    Projection result;
    if (imageHeight != 0) {
        result.aspect = static_cast<double>(imageWidth) / static_cast<double>(imageHeight);
    }
    std::visit(
        [&](const auto& value) {
            using CameraType = std::decay_t<decltype(value)>;
            result.nearPlane = value.nearPlane;
            result.farPlane = value.farPlane;
            if constexpr (std::is_same_v<CameraType, render_contracts::PerspectiveCamera3>) {
                result.isPerspective = true;
                result.tanHalfFov = std::tan(value.verticalFieldOfView / 2);
            } else {
                result.halfWidth = value.width / 2;
                result.halfHeight = value.height / 2;
            }
        },
        camera);
    return result;
}

[[nodiscard]] inline math::Vector3 normalized(const math::Vector3& value) noexcept {
    const double magnitude = math::length(value);
    if (magnitude == 0 || !std::isfinite(magnitude)) {
        return {};
    }
    return value * (1 / magnitude);
}

[[nodiscard]] inline render_contracts::Color scaleColor(const render_contracts::Color& color, double factor) noexcept {
    return {color.red * factor, color.green * factor, color.blue * factor, color.alpha};
}

inline void addTo(render_contracts::Color& target, const render_contracts::Color& addition) noexcept {
    target.red += addition.red;
    target.green += addition.green;
    target.blue += addition.blue;
}

[[nodiscard]] inline render_contracts::Color multiplyColors(const render_contracts::Color& first,
                                                            const render_contracts::Color& second) noexcept {
    return {first.red * second.red, first.green * second.green, first.blue * second.blue, first.alpha * second.alpha};
}

/// @brief Направленный свет, перенесённый в view space.
///
/// DirectionalLight::direction задаёт направление распространения света в мировых
/// координатах; грань освещена, если её нормаль смотрит навстречу (к источнику).
struct ViewDirectionalLight {
    math::Vector3 towardLight;
    render_contracts::Color color;
    double intensity{};
};

[[nodiscard]] inline render_contracts::TriangleIndices readTriangle(std::span<const std::byte> bytes,
                                                                    std::size_t index) noexcept {
    render_contracts::TriangleIndices triangle;
    std::memcpy(&triangle, bytes.data() + index * sizeof(triangle), sizeof(triangle));
    return triangle;
}

/// @brief Кеш производных shading-данных на версию mesh-ресурса.
///
/// Нормали граней вычисляются один раз для неизменного ресурса: ключом служат
/// владелец ресурса, адреса и размеры массивов вершин и треугольников. Кеш не продлевает
/// жизнь владельца; записи истёкших ресурсов удаляются перед следующим запросом.
/// Выданные результаты сохраняют адрес и содержимое, пока жив владеющий ресурс.
/// Без владеющего owner нормали пересчитываются во временный буфер; его результат
/// действителен до следующего успешного запроса того же вида без владельца.
class ShadingDataCache {
public:
    [[nodiscard]] std::expected<const std::vector<math::Vector3>*, RenderError>
    faceNormals(const render_contracts::MeshResource3& resource) {
        removeExpiredEntries();
        if (resource.triangles.size() % sizeof(render_contracts::TriangleIndices) != 0) {
            return std::unexpected(RenderError::InvalidResource);
        }
        const std::size_t triangleCount = resource.triangles.size() / sizeof(render_contracts::TriangleIndices);
        const bool hasOwner = resource.owner.use_count() != 0;
        if (hasOwner) {
            for (const auto& entry : m_entries) {
                if (!entry.owner.owner_before(resource.owner) && !resource.owner.owner_before(entry.owner) &&
                    entry.positions == resource.positions.data() && entry.triangles == resource.triangles.data() &&
                    entry.vertexCount == resource.positions.size() && entry.triangleCount == triangleCount) {
                    ++m_hits;
                    return &entry.normals;
                }
            }
        }
        auto normals = computeNormals(resource, triangleCount);
        if (!normals) {
            return std::unexpected(normals.error());
        }
        ++m_misses;
        if (!hasOwner) {
            m_uncachedFaceNormals = std::move(*normals);
            return &m_uncachedFaceNormals;
        }
        m_entries.push_back({resource.owner, resource.positions.data(), resource.triangles.data(),
                             resource.positions.size(), triangleCount, std::move(*normals)});
        return &m_entries.back().normals;
    }

    [[nodiscard]] std::expected<const std::vector<math::Vector3>*, RenderError>
    vertexNormals(const render_contracts::MeshResource3& resource) {
        removeExpiredEntries();
        if (resource.triangles.size() % sizeof(render_contracts::TriangleIndices) != 0) {
            return std::unexpected(RenderError::InvalidResource);
        }
        const std::size_t triangleCount = resource.triangles.size() / sizeof(render_contracts::TriangleIndices);
        const std::size_t vertexCount = resource.positions.size();
        const bool hasOwner = resource.owner.use_count() != 0;
        if (hasOwner) {
            for (const auto& entry : m_vertexEntries) {
                if (!entry.owner.owner_before(resource.owner) && !resource.owner.owner_before(entry.owner) &&
                    entry.positions == resource.positions.data() && entry.triangles == resource.triangles.data() &&
                    entry.vertexCount == vertexCount && entry.triangleCount == triangleCount) {
                    ++m_hits;
                    return &entry.normals;
                }
            }
        }
        auto normals = computeVertexNormals(resource, triangleCount, vertexCount);
        if (!normals) {
            return std::unexpected(normals.error());
        }
        ++m_misses;
        if (!hasOwner) {
            m_uncachedVertexNormals = std::move(*normals);
            return &m_uncachedVertexNormals;
        }
        m_vertexEntries.push_back({resource.owner, resource.positions.data(), resource.triangles.data(), vertexCount,
                                   triangleCount, std::move(*normals)});
        return &m_vertexEntries.back().normals;
    }

    [[nodiscard]] std::size_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::size_t misses() const noexcept { return m_misses; }

private:
    struct Entry {
        std::weak_ptr<const void> owner;
        const math::Point3* positions;
        const std::byte* triangles;
        std::size_t vertexCount;
        std::size_t triangleCount;
        std::vector<math::Vector3> normals;
    };

    void removeExpiredEntries() {
        const auto expired = [](const auto& entry) { return entry.owner.expired(); };
        std::erase_if(m_entries, expired);
        std::erase_if(m_vertexEntries, expired);
    }

    [[nodiscard]] static std::expected<std::vector<math::Vector3>, RenderError>
    computeNormals(const render_contracts::MeshResource3& resource, std::size_t triangleCount) {
        std::vector<math::Vector3> normals;
        normals.reserve(triangleCount);
        for (std::size_t index = 0; index < triangleCount; ++index) {
            const auto triangle = readTriangle(resource.triangles, index);
            if (triangle.a >= resource.positions.size() || triangle.b >= resource.positions.size() ||
                triangle.c >= resource.positions.size()) {
                return std::unexpected(RenderError::InvalidResource);
            }
            const auto normal = math::cross(resource.positions[triangle.b] - resource.positions[triangle.a],
                                            resource.positions[triangle.c] - resource.positions[triangle.a]);
            normals.push_back(normalized(normal));
        }
        return normals;
    }

    [[nodiscard]] static std::expected<std::vector<math::Vector3>, RenderError>
    computeVertexNormals(const render_contracts::MeshResource3& resource, std::size_t triangleCount,
                         std::size_t vertexCount) {
        std::vector<math::Vector3> normals(vertexCount, math::Vector3{});
        for (std::size_t index = 0; index < triangleCount; ++index) {
            const auto triangle = readTriangle(resource.triangles, index);
            if (triangle.a >= vertexCount || triangle.b >= vertexCount || triangle.c >= vertexCount) {
                return std::unexpected(RenderError::InvalidResource);
            }
            const auto normal = math::cross(resource.positions[triangle.b] - resource.positions[triangle.a],
                                            resource.positions[triangle.c] - resource.positions[triangle.a]);
            normals[triangle.a] = normals[triangle.a] + normal;
            normals[triangle.b] = normals[triangle.b] + normal;
            normals[triangle.c] = normals[triangle.c] + normal;
        }
        for (auto& normal : normals) {
            normal = normalized(normal);
        }
        return normals;
    }

    std::list<Entry> m_entries;
    std::list<Entry> m_vertexEntries;
    std::vector<math::Vector3> m_uncachedFaceNormals;
    std::vector<math::Vector3> m_uncachedVertexNormals;
    std::size_t m_hits{};
    std::size_t m_misses{};
};

[[nodiscard]] inline std::byte toByte(double channel) noexcept {
    return static_cast<std::byte>(std::lround(std::clamp(channel, 0.0, 1.0) * 255));
}

inline void writePixel(image::ImageBuffer& framebuffer, std::size_t x, std::size_t y,
                       const render_contracts::Color& color) noexcept {
    const auto bytes = framebuffer.bytes();
    const std::size_t offset = y * framebuffer.stride() + x * 4;
    bytes[offset] = toByte(color.red);
    bytes[offset + 1] = toByte(color.green);
    bytes[offset + 2] = toByte(color.blue);
    bytes[offset + 3] = toByte(color.alpha);
}

[[nodiscard]] inline render_contracts::Color readPixel(const image::ImageBuffer& buffer, std::size_t x,
                                                       std::size_t y) noexcept {
    const auto bytes = buffer.bytes();
    const std::size_t offset = y * buffer.stride() + x * 4;
    if (offset + 3 >= bytes.size()) {
        return {0, 0, 0, 0};
    }
    return {
        static_cast<double>(std::to_integer<std::uint8_t>(bytes[offset])) / 255.0,
        static_cast<double>(std::to_integer<std::uint8_t>(bytes[offset + 1])) / 255.0,
        static_cast<double>(std::to_integer<std::uint8_t>(bytes[offset + 2])) / 255.0,
        static_cast<double>(std::to_integer<std::uint8_t>(bytes[offset + 3])) / 255.0,
    };
}

[[nodiscard]] inline double applyWrap(double coordinate, TextureWrap wrap) noexcept {
    if (wrap == TextureWrap::Clamp) {
        return std::clamp(coordinate, 0.0, 1.0);
    }
    const double wrapped = coordinate - std::floor(coordinate);
    return std::clamp(wrapped, 0.0, 1.0);
}

/// @brief Выборка цвета из текстуры методом ближайшего соседа по UV-координатам.
[[nodiscard]] inline render_contracts::Color sampleTextureNearest(const image::ImageBuffer& texture, double u, double v,
                                                                  TextureWrap wrap = TextureWrap::Clamp) noexcept {
    if (texture.width() == 0 || texture.height() == 0 || texture.bytes().empty() || !std::isfinite(u) ||
        !std::isfinite(v)) {
        return {0, 0, 0, 0};
    }
    const double wrappedU = applyWrap(u, wrap);
    const double wrappedV = applyWrap(v, wrap);

    const auto width = texture.width();
    const auto height = texture.height();

    const double coordX = std::floor(wrappedU * static_cast<double>(width));
    const auto x = static_cast<std::size_t>(std::clamp(coordX, 0.0, static_cast<double>(width - 1)));

    const double coordY = std::floor(wrappedV * static_cast<double>(height));
    const auto y = static_cast<std::size_t>(std::clamp(coordY, 0.0, static_cast<double>(height - 1)));

    return readPixel(texture, x, y);
}

/// @brief Выборка цвета из текстуры методом билинейной интерполяции по UV-координатам.
[[nodiscard]] inline render_contracts::Color sampleTextureBilinear(const image::ImageBuffer& texture, double u,
                                                                   double v,
                                                                   TextureWrap wrap = TextureWrap::Clamp) noexcept {
    if (texture.width() == 0 || texture.height() == 0 || texture.bytes().empty() || !std::isfinite(u) ||
        !std::isfinite(v)) {
        return {0, 0, 0, 0};
    }
    const double wrappedU = applyWrap(u, wrap);
    const double wrappedV = applyWrap(v, wrap);

    const auto width = texture.width();
    const auto height = texture.height();

    const double px = wrappedU * static_cast<double>(width - 1);
    const double py = wrappedV * static_cast<double>(height - 1);

    const double floorX = std::floor(px);
    const auto x0 = static_cast<std::size_t>(std::clamp(floorX, 0.0, static_cast<double>(width - 1)));
    const auto x1 = std::min(x0 + 1, width - 1);
    const double fx = std::clamp(px - floorX, 0.0, 1.0);

    const double floorY = std::floor(py);
    const auto y0 = static_cast<std::size_t>(std::clamp(floorY, 0.0, static_cast<double>(height - 1)));
    const auto y1 = std::min(y0 + 1, height - 1);
    const double fy = std::clamp(py - floorY, 0.0, 1.0);

    const auto c00 = readPixel(texture, x0, y0);
    const auto c10 = readPixel(texture, x1, y0);
    const auto c01 = readPixel(texture, x0, y1);
    const auto c11 = readPixel(texture, x1, y1);

    const auto lerp = [](double a, double b, double t) noexcept { return (1.0 - t) * a + t * b; };

    const double topRed = lerp(c00.red, c10.red, fx);
    const double topGreen = lerp(c00.green, c10.green, fx);
    const double topBlue = lerp(c00.blue, c10.blue, fx);
    const double topAlpha = lerp(c00.alpha, c10.alpha, fx);

    const double bottomRed = lerp(c01.red, c11.red, fx);
    const double bottomGreen = lerp(c01.green, c11.green, fx);
    const double bottomBlue = lerp(c01.blue, c11.blue, fx);
    const double bottomAlpha = lerp(c01.alpha, c11.alpha, fx);

    return {
        lerp(topRed, bottomRed, fy),
        lerp(topGreen, bottomGreen, fy),
        lerp(topBlue, bottomBlue, fy),
        lerp(topAlpha, bottomAlpha, fy),
    };
}

/// @brief Выборка цвета из текстуры с заданным методом фильтрации и режимом наложения.
[[nodiscard]] inline render_contracts::Color sampleTexture(const image::ImageBuffer& texture, double u, double v,
                                                           TextureFilter filter = TextureFilter::Nearest,
                                                           TextureWrap wrap = TextureWrap::Clamp) noexcept {
    if (filter == TextureFilter::Bilinear) {
        return sampleTextureBilinear(texture, u, v, wrap);
    }
    return sampleTextureNearest(texture, u, v, wrap);
}

inline void clear(image::ImageBuffer& framebuffer, const render_contracts::Color& color) noexcept {
    const std::array<std::byte, 4> pixel{toByte(color.red), toByte(color.green), toByte(color.blue),
                                         toByte(color.alpha)};
    for (std::size_t y = 0; y < framebuffer.height(); ++y) {
        auto row = framebuffer.row(y);
        for (std::size_t x = 0; x < framebuffer.width(); ++x) {
            std::memcpy(row.data() + x * pixel.size(), pixel.data(), pixel.size());
        }
    }
}

[[nodiscard]] constexpr double edgeFunction(double pointX, double pointY, double otherX, double otherY, double sampleX,
                                            double sampleY) noexcept {
    return (otherX - pointX) * (sampleY - pointY) - (sampleX - pointX) * (otherY - pointY);
}

/// @brief Заполняет треугольник с depth-тестом; меньшая view-глубина перекрывает большую.
inline void rasterizeTriangle(const ProjectedVertex& first, const ProjectedVertex& second, const ProjectedVertex& third,
                              const render_contracts::Color& color, image::ImageBuffer& framebuffer,
                              std::vector<double>& depth) noexcept {
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const std::array<double, 3> screenX{(first.ndcX + 1) * 0.5 * width, (second.ndcX + 1) * 0.5 * width,
                                        (third.ndcX + 1) * 0.5 * width};
    const std::array<double, 3> screenY{(1 - first.ndcY) * 0.5 * height, (1 - second.ndcY) * 0.5 * height,
                                        (1 - third.ndcY) * 0.5 * height};
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (area == 0) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    const double invArea = 1.0 / area;
    const std::array<ProjectedVertex, 3> vertices{first, second, third};
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) * invArea;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) * invArea;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) * invArea;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            const double invZ = weight0 * vertices[0].invZ + weight1 * vertices[1].invZ + weight2 * vertices[2].invZ;
            if (invZ <= 0) {
                continue;
            }
            const double z =
                (weight0 * vertices[0].zOverW + weight1 * vertices[1].zOverW + weight2 * vertices[2].zOverW) / invZ;
            const std::size_t pixel = y * framebuffer.width() + x;
            if (z < depth[pixel]) {
                depth[pixel] = z;
                writePixel(framebuffer, x, y, color);
            }
        }
    }
}

/// @brief Заполняет 3D-треугольник с затенением Гуро: барицентрическая интерполяция цвета по вершинам и depth-тест.
inline void rasterizeTriangleGouraud(const ProjectedVertex& first, const ProjectedVertex& second,
                                     const ProjectedVertex& third, const render_contracts::Color& c0,
                                     const render_contracts::Color& c1, const render_contracts::Color& c2,
                                     image::ImageBuffer& framebuffer, std::vector<double>& depth) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0 || depth.empty()) {
        return;
    }
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const std::array<double, 3> screenX{(first.ndcX + 1) * 0.5 * width, (second.ndcX + 1) * 0.5 * width,
                                        (third.ndcX + 1) * 0.5 * width};
    const std::array<double, 3> screenY{(1 - first.ndcY) * 0.5 * height, (1 - second.ndcY) * 0.5 * height,
                                        (1 - third.ndcY) * 0.5 * height};
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (std::abs(area) <= 1e-9) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    const double invArea = 1.0 / area;
    const std::array<ProjectedVertex, 3> vertices{first, second, third};
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) * invArea;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) * invArea;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) * invArea;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            const double invZ = weight0 * vertices[0].invZ + weight1 * vertices[1].invZ + weight2 * vertices[2].invZ;
            if (invZ <= 0) {
                continue;
            }
            const double z =
                (weight0 * vertices[0].zOverW + weight1 * vertices[1].zOverW + weight2 * vertices[2].zOverW) / invZ;
            const std::size_t pixel = y * framebuffer.width() + x;
            if (pixel < depth.size() && z < depth[pixel]) {
                depth[pixel] = z;
                const render_contracts::Color interpolatedColor{
                    weight0 * c0.red + weight1 * c1.red + weight2 * c2.red,
                    weight0 * c0.green + weight1 * c1.green + weight2 * c2.green,
                    weight0 * c0.blue + weight1 * c1.blue + weight2 * c2.blue,
                    weight0 * c0.alpha + weight1 * c1.alpha + weight2 * c2.alpha,
                };
                writePixel(framebuffer, x, y, interpolatedColor);
            }
        }
    }
}

/// @brief Заполняет 3D-треугольник с наложением текстуры, перспективно-корректной интерполяцией UV,
/// затенением Гуро и Z-буфером.
inline void rasterizeTriangleTextured3D(const ProjectedVertex& first, const ProjectedVertex& second,
                                        const ProjectedVertex& third, const std::array<math::Point2, 3>& uv,
                                        const image::ImageBuffer& texture, const render_contracts::Color& c0,
                                        const render_contracts::Color& c1, const render_contracts::Color& c2,
                                        image::ImageBuffer& framebuffer, std::vector<double>& depth,
                                        TextureFilter filter = TextureFilter::Nearest,
                                        TextureWrap wrap = TextureWrap::Clamp) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0 || depth.empty() || texture.width() == 0 ||
        texture.height() == 0) {
        return;
    }
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const std::array<double, 3> screenX{(first.ndcX + 1) * 0.5 * width, (second.ndcX + 1) * 0.5 * width,
                                        (third.ndcX + 1) * 0.5 * width};
    const std::array<double, 3> screenY{(1 - first.ndcY) * 0.5 * height, (1 - second.ndcY) * 0.5 * height,
                                        (1 - third.ndcY) * 0.5 * height};
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (std::abs(area) <= 1e-9) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    const double invArea = 1.0 / area;
    const std::array<ProjectedVertex, 3> vertices{first, second, third};
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) * invArea;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) * invArea;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) * invArea;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            const double invZ = weight0 * vertices[0].invZ + weight1 * vertices[1].invZ + weight2 * vertices[2].invZ;
            if (invZ <= 0) {
                continue;
            }
            const double z =
                (weight0 * vertices[0].zOverW + weight1 * vertices[1].zOverW + weight2 * vertices[2].zOverW) / invZ;
            const std::size_t pixel = y * framebuffer.width() + x;
            if (pixel < depth.size() && z < depth[pixel]) {
                const double u = (weight0 * uv[0][0] * vertices[0].invZ + weight1 * uv[1][0] * vertices[1].invZ +
                                  weight2 * uv[2][0] * vertices[2].invZ) /
                                 invZ;
                const double v = (weight0 * uv[0][1] * vertices[0].invZ + weight1 * uv[1][1] * vertices[1].invZ +
                                  weight2 * uv[2][1] * vertices[2].invZ) /
                                 invZ;
                const auto texColor = sampleTexture(texture, u, v, filter, wrap);
                const render_contracts::Color lightColor{
                    weight0 * c0.red + weight1 * c1.red + weight2 * c2.red,
                    weight0 * c0.green + weight1 * c1.green + weight2 * c2.green,
                    weight0 * c0.blue + weight1 * c1.blue + weight2 * c2.blue,
                    weight0 * c0.alpha + weight1 * c1.alpha + weight2 * c2.alpha,
                };
                const auto finalColor = multiplyColors(texColor, lightColor);
                if (finalColor.alpha <= 0.0) {
                    continue;
                }
                depth[pixel] = z;
                writePixel(framebuffer, x, y, finalColor);
            }
        }
    }
}

/// @brief Заполняет 3D-треугольник с наложением текстуры, перспективно-корректной интерполяцией UV,
/// единым цветом освещения/материала и Z-буфером.
inline void rasterizeTriangleTextured3D(const ProjectedVertex& first, const ProjectedVertex& second,
                                        const ProjectedVertex& third, const std::array<math::Point2, 3>& uv,
                                        const image::ImageBuffer& texture, const render_contracts::Color& color,
                                        image::ImageBuffer& framebuffer, std::vector<double>& depth,
                                        TextureFilter filter = TextureFilter::Nearest,
                                        TextureWrap wrap = TextureWrap::Clamp) noexcept {
    rasterizeTriangleTextured3D(first, second, third, uv, texture, color, color, color, framebuffer, depth, filter,
                                wrap);
}

/// @brief Перегрузка rasterizeTriangleTextured3D, принимающая UV-координаты отдельными точками.
inline void rasterizeTriangleTextured3D(const ProjectedVertex& first, const ProjectedVertex& second,
                                        const ProjectedVertex& third, const math::Point2& uv0, const math::Point2& uv1,
                                        const math::Point2& uv2, const image::ImageBuffer& texture,
                                        const render_contracts::Color& color, image::ImageBuffer& framebuffer,
                                        std::vector<double>& depth, TextureFilter filter = TextureFilter::Nearest,
                                        TextureWrap wrap = TextureWrap::Clamp) noexcept {
    rasterizeTriangleTextured3D(first, second, third, std::array<math::Point2, 3>{uv0, uv1, uv2}, texture, color, color,
                                color, framebuffer, depth, filter, wrap);
}

/// @brief Смешивает цвет с содержимым framebuffer по правилу source-over.
inline void blendPixel(image::ImageBuffer& framebuffer, std::size_t x, std::size_t y,
                       const render_contracts::Color& color) noexcept {
    const auto bytes = framebuffer.bytes();
    const std::size_t offset = y * framebuffer.stride() + x * 4;
    const double alpha = std::clamp(color.alpha, 0.0, 1.0);
    const double inverse = 1 - alpha;
    const std::array<double, 3> source{color.red, color.green, color.blue};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double destination = static_cast<double>(std::to_integer<int>(bytes[offset + channel])) / 255;
        bytes[offset + channel] = toByte(source[channel] * alpha + destination * inverse);
    }
    const double destinationAlpha = static_cast<double>(std::to_integer<int>(bytes[offset + 3])) / 255;
    bytes[offset + 3] = toByte(alpha + destinationAlpha * inverse);
}

/// @brief Растеризует 2D-отрезок между двумя точками целочисленным алгоритмом Брезенхэма.
///
/// Обрабатывает все 8 октантов с накоплением ошибки. Пиксели вне границ кадрового буфера отсекаются.
/// При alpha < 1.0 используется blendPixel, иначе прямое writePixel.
inline void rasterizeLine2D(int x0, int y0, int x1, int y1, const render_contracts::Color& color,
                            image::ImageBuffer& framebuffer) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0) {
        return;
    }
    const auto plot = [&](int x, int y) noexcept {
        if (x >= 0 && y >= 0 && static_cast<std::size_t>(x) < framebuffer.width() &&
            static_cast<std::size_t>(y) < framebuffer.height()) {
            const auto ux = static_cast<std::size_t>(x);
            const auto uy = static_cast<std::size_t>(y);
            if (color.alpha < 1.0) {
                blendPixel(framebuffer, ux, uy, color);
            } else {
                writePixel(framebuffer, ux, uy, color);
            }
        }
    };
    const std::int64_t dx = std::abs(static_cast<std::int64_t>(x1) - static_cast<std::int64_t>(x0));
    const int sx = x0 < x1 ? 1 : -1;
    const std::int64_t dy = -std::abs(static_cast<std::int64_t>(y1) - static_cast<std::int64_t>(y0));
    const int sy = y0 < y1 ? 1 : -1;
    std::int64_t error = dx + dy;

    int currentX = x0;
    int currentY = y0;
    while (true) {
        plot(currentX, currentY);
        if (currentX == x1 && currentY == y1) {
            break;
        }
        const std::int64_t e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            currentX += sx;
        }
        if (e2 <= dx) {
            error += dx;
            currentY += sy;
        }
    }
}

/// @brief Растеризует 3D-отрезок целочисленным алгоритмом Брезенхэма с линейной интерполяцией глубины и Z-буфером.
///
/// Пиксель рисуется и глубина обновляется, если вычисленное z < depth[y * width + x].
inline void rasterizeLine3D(int x0, int y0, double z0, int x1, int y1, double z1, const render_contracts::Color& color,
                            image::ImageBuffer& framebuffer, std::vector<double>& depth) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0 || depth.empty()) {
        return;
    }
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    int currentStep = 0;

    const auto plot = [&](int x, int y, double z) noexcept {
        if (x >= 0 && y >= 0 && static_cast<std::size_t>(x) < framebuffer.width() &&
            static_cast<std::size_t>(y) < framebuffer.height()) {
            const auto ux = static_cast<std::size_t>(x);
            const auto uy = static_cast<std::size_t>(y);
            const std::size_t pixel = uy * framebuffer.width() + ux;
            if (pixel < depth.size() && z < depth[pixel]) {
                depth[pixel] = z;
                if (color.alpha < 1.0) {
                    blendPixel(framebuffer, ux, uy, color);
                } else {
                    writePixel(framebuffer, ux, uy, color);
                }
            }
        }
    };

    const std::int64_t dx = std::abs(static_cast<std::int64_t>(x1) - static_cast<std::int64_t>(x0));
    const int sx = x0 < x1 ? 1 : -1;
    const std::int64_t dy = -std::abs(static_cast<std::int64_t>(y1) - static_cast<std::int64_t>(y0));
    const int sy = y0 < y1 ? 1 : -1;
    std::int64_t error = dx + dy;

    int currentX = x0;
    int currentY = y0;
    while (true) {
        const double t =
            steps == 0 ? 0.0 : std::clamp(static_cast<double>(currentStep) / static_cast<double>(steps), 0.0, 1.0);
        const double z = z0 + t * (z1 - z0);
        plot(currentX, currentY, z);
        ++currentStep;

        if (currentX == x1 && currentY == y1) {
            break;
        }
        const std::int64_t e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            currentX += sx;
        }
        if (e2 <= dx) {
            error += dx;
            currentY += sy;
        }
    }
}

/// @brief Минимальный flat shading: ambient плюс диффузная составляющая направленных
/// источников по нормали грани. Обратная грань (при выключенном culling) освещается
/// с перевёрнутой нормалью.
[[nodiscard]] inline render_contracts::Color shadeFlat(const render_contracts::Material& material,
                                                       const render_contracts::AmbientLight& ambient,
                                                       const std::vector<ViewDirectionalLight>& lights,
                                                       const math::Vector3& viewNormal, bool isFront) noexcept {
    const auto normal = isFront ? viewNormal : viewNormal * -1;
    auto light = scaleColor(ambient.color, ambient.intensity);
    for (const auto& directional : lights) {
        const double factor = std::max(0.0, math::dot(normal, directional.towardLight));
        addTo(light, scaleColor(directional.color, directional.intensity * factor));
    }
    return multiplyColors(material.color, light);
}

/// @brief Растеризует World3D-пасс: camera transforms, backface culling, depth buffer
/// и flat shading с кешированными нормалями граней.
///
/// Backface culling выполняется в view space: лицевой считается грань, чья нормаль
/// cross(b - a, c - a) направлена к камере (для ортографической камеры — в -z).
/// Кешированная локальная нормаль переносится в view space линейной частью
/// преобразования с нормализацией; для неоднородного масштаба это приближение.
inline std::expected<void, RenderError> renderWorld3D(const render_contracts::RenderSnapshot& snapshot,
                                                      const render_contracts::World3DPass& pass,
                                                      image::ImageBuffer& framebuffer, std::vector<double>& depth,
                                                      ShadingDataCache& shadingCache) {
    const auto projection = makeProjection(pass.camera, snapshot.imageWidth, snapshot.imageHeight);
    const auto worldToView = std::visit([](const auto& value) { return value.worldToView; }, pass.camera);
    std::vector<ViewDirectionalLight> viewLights;
    viewLights.reserve(pass.lighting.directional.size());
    for (const auto& directional : pass.lighting.directional) {
        viewLights.push_back({normalized(math::transformVector(worldToView, directional.direction) * -1),
                              directional.color, directional.intensity});
    }
    for (const auto& item : pass.drawItems) {
        if (item.resource >= snapshot.resources3.size()) {
            return std::unexpected(RenderError::InvalidResource);
        }
        if (item.material >= snapshot.materials.size() || !snapshot.materials[item.material]) {
            return std::unexpected(RenderError::InvalidMaterial);
        }
        const auto& resource = snapshot.resources3[item.resource];
        const auto& material = *snapshot.materials[item.material];
        const auto localToView = math::compose(worldToView, item.localToWorld);
        const bool isGouraud = (item.shading == render_contracts::ShadingModel::Gouraud);
        const bool isTextured =
            (material.texture != nullptr) && (resource.texCoords.size() == resource.positions.size());
        const std::vector<math::Vector3>* faceNormals = nullptr;
        const std::vector<math::Vector3>* vertexNormals = nullptr;
        if (isGouraud) {
            auto normals = shadingCache.vertexNormals(resource);
            if (!normals) {
                return std::unexpected(normals.error());
            }
            vertexNormals = *normals;
        } else {
            auto normals = shadingCache.faceNormals(resource);
            if (!normals) {
                return std::unexpected(normals.error());
            }
            faceNormals = *normals;
        }
        const std::size_t triangleCount = resource.triangles.size() / sizeof(render_contracts::TriangleIndices);
        for (std::size_t index = 0; index < triangleCount; ++index) {
            const auto triangle = readTriangle(resource.triangles, index);
            const auto viewFirst = math::transformPoint(localToView, resource.positions[triangle.a]);
            const auto viewSecond = math::transformPoint(localToView, resource.positions[triangle.b]);
            const auto viewThird = math::transformPoint(localToView, resource.positions[triangle.c]);
            // ориентация грани нужна и при выключенном culling: обратная грань
            // освещается с перевёрнутой нормалью
            const auto faceNormal = math::cross(viewSecond - viewFirst, viewThird - viewFirst);
            const math::Vector3 fromOrigin{viewFirst[0], viewFirst[1], viewFirst[2]};
            const bool isFront = projection.isPerspective ? math::dot(faceNormal, fromOrigin) < 0 : faceNormal[2] < 0;
            if (item.backfaceCulling && !isFront) {
                continue;
            }
            const auto first = projection.project(viewFirst);
            const auto second = projection.project(viewSecond);
            const auto third = projection.project(viewThird);
            if (!first || !second || !third) {
                continue;
            }
            if (isGouraud) {
                const auto n0View = normalized(math::transformVector(localToView, (*vertexNormals)[triangle.a]));
                const auto n1View = normalized(math::transformVector(localToView, (*vertexNormals)[triangle.b]));
                const auto n2View = normalized(math::transformVector(localToView, (*vertexNormals)[triangle.c]));

                const auto c0 = shadeFlat(material, pass.lighting.ambient, viewLights, n0View, isFront);
                const auto c1 = shadeFlat(material, pass.lighting.ambient, viewLights, n1View, isFront);
                const auto c2 = shadeFlat(material, pass.lighting.ambient, viewLights, n2View, isFront);

                if (item.wireframe) {
                    const double width = static_cast<double>(framebuffer.width());
                    const double height = static_cast<double>(framebuffer.height());
                    const int x0 = static_cast<int>(std::lround((first->ndcX + 1) * 0.5 * width));
                    const int y0 = static_cast<int>(std::lround((1 - first->ndcY) * 0.5 * height));
                    const double z0 = viewFirst[2];
                    const int x1 = static_cast<int>(std::lround((second->ndcX + 1) * 0.5 * width));
                    const int y1 = static_cast<int>(std::lround((1 - second->ndcY) * 0.5 * height));
                    const double z1 = viewSecond[2];
                    const int x2 = static_cast<int>(std::lround((third->ndcX + 1) * 0.5 * width));
                    const int y2 = static_cast<int>(std::lround((1 - third->ndcY) * 0.5 * height));
                    const double z2 = viewThird[2];

                    rasterizeLine3D(x0, y0, z0, x1, y1, z1, c0, framebuffer, depth);
                    rasterizeLine3D(x1, y1, z1, x2, y2, z2, c1, framebuffer, depth);
                    rasterizeLine3D(x2, y2, z2, x0, y0, z0, c2, framebuffer, depth);
                } else if (isTextured) {
                    const std::array<math::Point2, 3> uvs{
                        resource.texCoords[triangle.a], resource.texCoords[triangle.b], resource.texCoords[triangle.c]};
                    rasterizeTriangleTextured3D(*first, *second, *third, uvs, *material.texture, c0, c1, c2,
                                                framebuffer, depth, material.filter, material.wrap);
                } else {
                    rasterizeTriangleGouraud(*first, *second, *third, c0, c1, c2, framebuffer, depth);
                }
            } else {
                const auto viewNormal = normalized(math::transformVector(localToView, (*faceNormals)[index]));
                const auto shaded = shadeFlat(material, pass.lighting.ambient, viewLights, viewNormal, isFront);
                if (item.wireframe) {
                    const double width = static_cast<double>(framebuffer.width());
                    const double height = static_cast<double>(framebuffer.height());
                    const int x0 = static_cast<int>(std::lround((first->ndcX + 1) * 0.5 * width));
                    const int y0 = static_cast<int>(std::lround((1 - first->ndcY) * 0.5 * height));
                    const double z0 = viewFirst[2];
                    const int x1 = static_cast<int>(std::lround((second->ndcX + 1) * 0.5 * width));
                    const int y1 = static_cast<int>(std::lround((1 - second->ndcY) * 0.5 * height));
                    const double z1 = viewSecond[2];
                    const int x2 = static_cast<int>(std::lround((third->ndcX + 1) * 0.5 * width));
                    const int y2 = static_cast<int>(std::lround((1 - third->ndcY) * 0.5 * height));
                    const double z2 = viewThird[2];

                    rasterizeLine3D(x0, y0, z0, x1, y1, z1, shaded, framebuffer, depth);
                    rasterizeLine3D(x1, y1, z1, x2, y2, z2, shaded, framebuffer, depth);
                    rasterizeLine3D(x2, y2, z2, x0, y0, z0, shaded, framebuffer, depth);
                } else if (isTextured) {
                    const std::array<math::Point2, 3> uvs{
                        resource.texCoords[triangle.a], resource.texCoords[triangle.b], resource.texCoords[triangle.c]};
                    rasterizeTriangleTextured3D(*first, *second, *third, uvs, *material.texture, shaded, framebuffer,
                                                depth, material.filter, material.wrap);
                } else {
                    rasterizeTriangle(*first, *second, *third, shaded, framebuffer, depth);
                }
            }
        }
    }
    return {};
}

/// @brief Заполняет 2D-треугольник в экранных координатах (пиксели, y вниз) с alpha blending.
inline void rasterizeTriangle2D(const std::array<double, 3>& screenX, const std::array<double, 3>& screenY,
                                const render_contracts::Color& color, image::ImageBuffer& framebuffer) noexcept {
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (area == 0) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) / area;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) / area;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) / area;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            blendPixel(framebuffer, x, y, color);
        }
    }
}

/// @brief Заполняет 2D-треугольник в экранных координатах с барицентрической интерполяцией цвета (Гуро) и alpha
/// blending.
inline void rasterizeTriangleGouraud2D(const std::array<double, 3>& screenX, const std::array<double, 3>& screenY,
                                       const render_contracts::Color& c0, const render_contracts::Color& c1,
                                       const render_contracts::Color& c2, image::ImageBuffer& framebuffer) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0) {
        return;
    }
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (std::abs(area) <= 1e-9) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) / area;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) / area;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) / area;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            const render_contracts::Color interpolatedColor{
                weight0 * c0.red + weight1 * c1.red + weight2 * c2.red,
                weight0 * c0.green + weight1 * c1.green + weight2 * c2.green,
                weight0 * c0.blue + weight1 * c1.blue + weight2 * c2.blue,
                weight0 * c0.alpha + weight1 * c1.alpha + weight2 * c2.alpha,
            };
            blendPixel(framebuffer, x, y, interpolatedColor);
        }
    }
}

/// @brief Перегрузка rasterizeTriangleGouraud2D, принимающая точки math::Point2.
inline void rasterizeTriangleGouraud2D(const math::Point2& p0, const math::Point2& p1, const math::Point2& p2,
                                       const render_contracts::Color& c0, const render_contracts::Color& c1,
                                       const render_contracts::Color& c2, image::ImageBuffer& framebuffer) noexcept {
    rasterizeTriangleGouraud2D(std::array<double, 3>{p0[0], p1[0], p2[0]}, std::array<double, 3>{p0[1], p1[1], p2[1]},
                               c0, c1, c2, framebuffer);
}

/// @brief Заполняет 2D-треугольник в экранных координатах с наложением текстуры и alpha blending.
inline void rasterizeTriangleTextured2D(const std::array<double, 3>& screenX, const std::array<double, 3>& screenY,
                                        const std::array<math::Point2, 3>& uv, const image::ImageBuffer& texture,
                                        const render_contracts::Color& tintColor, image::ImageBuffer& framebuffer,
                                        TextureFilter filter = TextureFilter::Nearest,
                                        TextureWrap wrap = TextureWrap::Clamp) noexcept {
    if (framebuffer.width() == 0 || framebuffer.height() == 0 || texture.width() == 0 || texture.height() == 0) {
        return;
    }
    const double width = static_cast<double>(framebuffer.width());
    const double height = static_cast<double>(framebuffer.height());
    const double area = edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], screenX[2], screenY[2]);
    if (std::abs(area) <= 1e-9) {
        return;
    }
    const double minXf = std::min({screenX[0], screenX[1], screenX[2]});
    const double maxXf = std::max({screenX[0], screenX[1], screenX[2]});
    const double minYf = std::min({screenY[0], screenY[1], screenY[2]});
    const double maxYf = std::max({screenY[0], screenY[1], screenY[2]});
    if (maxXf < 0 || maxYf < 0 || minXf >= width || minYf >= height) {
        return;
    }
    const auto clampX = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, width - 1)));
    };
    const auto clampY = [&](double value) {
        return static_cast<std::size_t>(std::floor(std::clamp(value, 0.0, height - 1)));
    };
    for (std::size_t y = clampY(minYf); y <= clampY(maxYf); ++y) {
        for (std::size_t x = clampX(minXf); x <= clampX(maxXf); ++x) {
            const double sampleX = static_cast<double>(x) + 0.5;
            const double sampleY = static_cast<double>(y) + 0.5;
            const double weight0 =
                edgeFunction(screenX[1], screenY[1], screenX[2], screenY[2], sampleX, sampleY) / area;
            const double weight1 =
                edgeFunction(screenX[2], screenY[2], screenX[0], screenY[0], sampleX, sampleY) / area;
            const double weight2 =
                edgeFunction(screenX[0], screenY[0], screenX[1], screenY[1], sampleX, sampleY) / area;
            constexpr double tolerance = -1e-9;
            if (weight0 < tolerance || weight1 < tolerance || weight2 < tolerance) {
                continue;
            }
            const double u = weight0 * uv[0][0] + weight1 * uv[1][0] + weight2 * uv[2][0];
            const double v = weight0 * uv[0][1] + weight1 * uv[1][1] + weight2 * uv[2][1];
            const auto texColor = sampleTexture(texture, u, v, filter, wrap);
            const auto finalColor = multiplyColors(texColor, tintColor);
            if (finalColor.alpha <= 0.0) {
                continue;
            }
            blendPixel(framebuffer, x, y, finalColor);
        }
    }
}

/// @brief Перегрузка rasterizeTriangleTextured2D, принимающая точки и UV-координаты math::Point2.
inline void rasterizeTriangleTextured2D(const math::Point2& p0, const math::Point2& p1, const math::Point2& p2,
                                        const math::Point2& uv0, const math::Point2& uv1, const math::Point2& uv2,
                                        const image::ImageBuffer& texture, const render_contracts::Color& tintColor,
                                        image::ImageBuffer& framebuffer, TextureFilter filter = TextureFilter::Nearest,
                                        TextureWrap wrap = TextureWrap::Clamp) noexcept {
    rasterizeTriangleTextured2D(std::array<double, 3>{p0[0], p1[0], p2[0]}, std::array<double, 3>{p0[1], p1[1], p2[1]},
                                std::array<math::Point2, 3>{uv0, uv1, uv2}, texture, tintColor, framebuffer, filter,
                                wrap);
}

/// @brief Растеризует 2D-проход (World2D или Hud2D) алгоритмом художника с alpha blending.
///
/// Внутри прохода draw items исполняются в стабильном порядке (layer, order) независимо
/// от порядка подачи. World2D проецирует мировые координаты через собственную камеру
/// с соглашениями ортографической проекции; Hud2D (camera == nullopt) использует
/// экранные координаты в пикселях с началом в левом верхнем углу, ось y вниз.
/// Недопустимая камера даёт пустой проход, что согласовано с culling при подготовке.
inline std::expected<void, RenderError> render2DPass(const render_contracts::RenderSnapshot& snapshot,
                                                     std::span<const render_contracts::DrawItem2> items,
                                                     const std::optional<render_contracts::Camera2>& camera,
                                                     image::ImageBuffer& framebuffer) {
    std::vector<const render_contracts::DrawItem2*> sorted;
    sorted.reserve(items.size());
    for (const auto& item : items) {
        sorted.push_back(&item);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* first, const auto* second) {
        return std::tie(first->layer, first->order) < std::tie(second->layer, second->order);
    });
    double halfWidth = 0;
    double halfHeight = 0;
    bool isCameraValid = false;
    if (camera) {
        halfWidth = camera->width / 2;
        halfHeight = camera->height / 2;
        isCameraValid = halfWidth > 0 && halfHeight > 0 && std::isfinite(halfWidth) && std::isfinite(halfHeight);
    }
    for (const auto* item : sorted) {
        if (item->resource >= snapshot.resources2.size()) {
            return std::unexpected(RenderError::InvalidResource);
        }
        if (item->material >= snapshot.materials.size() || !snapshot.materials[item->material]) {
            return std::unexpected(RenderError::InvalidMaterial);
        }
        const auto& resource = snapshot.resources2[item->resource];
        if (resource.triangles.size() % sizeof(render_contracts::TriangleIndices) != 0) {
            return std::unexpected(RenderError::InvalidResource);
        }
        if (camera && !isCameraValid) {
            continue;
        }
        const auto& material = *snapshot.materials[item->material];
        const bool isTextured =
            (material.texture != nullptr) && (resource.texCoords.size() == resource.positions.size());
        const auto localToView = camera ? math::compose(camera->worldToView, item->localToWorld) : item->localToWorld;
        const std::size_t triangleCount = resource.triangles.size() / sizeof(render_contracts::TriangleIndices);
        for (std::size_t index = 0; index < triangleCount; ++index) {
            const auto triangle = readTriangle(resource.triangles, index);
            if (triangle.a >= resource.positions.size() || triangle.b >= resource.positions.size() ||
                triangle.c >= resource.positions.size()) {
                return std::unexpected(RenderError::InvalidResource);
            }
            const std::array<math::Point2, 3> view{math::transformPoint(localToView, resource.positions[triangle.a]),
                                                   math::transformPoint(localToView, resource.positions[triangle.b]),
                                                   math::transformPoint(localToView, resource.positions[triangle.c])};
            std::array<double, 3> screenX;
            std::array<double, 3> screenY;
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                if (camera) {
                    const double ndcX = view[vertex][0] / halfWidth;
                    const double ndcY = view[vertex][1] / halfHeight;
                    screenX[vertex] = (ndcX + 1) * 0.5 * static_cast<double>(framebuffer.width());
                    screenY[vertex] = (1 - ndcY) * 0.5 * static_cast<double>(framebuffer.height());
                } else {
                    screenX[vertex] = view[vertex][0];
                    screenY[vertex] = view[vertex][1];
                }
            }
            if (item->wireframe) {
                const int x0 = static_cast<int>(std::lround(screenX[0]));
                const int y0 = static_cast<int>(std::lround(screenY[0]));
                const int x1 = static_cast<int>(std::lround(screenX[1]));
                const int y1 = static_cast<int>(std::lround(screenY[1]));
                const int x2 = static_cast<int>(std::lround(screenX[2]));
                const int y2 = static_cast<int>(std::lround(screenY[2]));

                rasterizeLine2D(x0, y0, x1, y1, material.color, framebuffer);
                rasterizeLine2D(x1, y1, x2, y2, material.color, framebuffer);
                rasterizeLine2D(x2, y2, x0, y0, material.color, framebuffer);
            } else if (isTextured) {
                const std::array<math::Point2, 3> uvs{resource.texCoords[triangle.a], resource.texCoords[triangle.b],
                                                      resource.texCoords[triangle.c]};
                rasterizeTriangleTextured2D(screenX, screenY, uvs, *material.texture, material.color, framebuffer,
                                            material.filter, material.wrap);
            } else {
                rasterizeTriangle2D(screenX, screenY, material.color, framebuffer);
            }
        }
    }
    return {};
}

} // namespace detail

/// @brief Синхронный программный рендерер снимка с кешем shading-данных.
///
/// Один экземпляр переиспользует нормали граней неизменной геометрии между кадрами;
/// статистика hits/misses показывает работу кеша.
class SoftwareRenderer {
public:
    /// @brief Растеризует снимок в framebuffer (headless-потребитель RenderSnapshot).
    ///
    /// Passes исполняются в порядке снимка: World3D — с общим depth buffer, World2D и
    /// Hud2D — алгоритмом художника с alpha blending. Зависит только от Render
    /// Contracts, Image и Math.
    [[nodiscard]] std::expected<void, RenderError> render(const render_contracts::RenderSnapshot& snapshot,
                                                          image::ImageBuffer& framebuffer) {
        if (framebuffer.width() != snapshot.imageWidth || framebuffer.height() != snapshot.imageHeight) {
            return std::unexpected(RenderError::SizeMismatch);
        }
        detail::clear(framebuffer, snapshot.clearColor);
        const std::size_t pixelCount = snapshot.imageWidth * snapshot.imageHeight;
        if (m_depthBuffer.size() != pixelCount) {
            m_depthBuffer.resize(pixelCount);
        }
        std::fill(m_depthBuffer.begin(), m_depthBuffer.end(), std::numeric_limits<double>::infinity());
        for (const auto& pass : snapshot.passes) {
            std::expected<void, RenderError> rendered;
            if (const auto* world3 = std::get_if<render_contracts::World3DPass>(&pass)) {
                rendered = detail::renderWorld3D(snapshot, *world3, framebuffer, m_depthBuffer, m_shadingCache);
            } else if (const auto* world2 = std::get_if<render_contracts::World2DPass>(&pass)) {
                rendered = detail::render2DPass(snapshot, world2->drawItems, world2->camera, framebuffer);
            } else {
                const auto& hud = std::get<render_contracts::Hud2DPass>(pass);
                rendered = detail::render2DPass(snapshot, hud.drawItems, std::nullopt, framebuffer);
            }
            if (!rendered) {
                return rendered;
            }
        }
        return {};
    }

    [[nodiscard]] std::size_t shadingCacheHits() const noexcept { return m_shadingCache.hits(); }
    [[nodiscard]] std::size_t shadingCacheMisses() const noexcept { return m_shadingCache.misses(); }

private:
    detail::ShadingDataCache m_shadingCache;
    std::vector<double> m_depthBuffer;
};

/// @brief Одноразовый вызов без сохранения кеша между кадрами.
/// Для переиспользования shading-данных неизменной геометрии используйте экземпляр SoftwareRenderer.
[[nodiscard]] inline std::expected<void, RenderError> render(const render_contracts::RenderSnapshot& snapshot,
                                                             image::ImageBuffer& framebuffer) {
    SoftwareRenderer renderer;
    return renderer.render(snapshot, framebuffer);
}

/// @brief Растеризует 2D-отрезок целочисленным алгоритмом Брезенхэма.
using detail::rasterizeLine2D;

/// @brief Растеризует 3D-отрезок целочисленным алгоритмом Брезенхэма с Z-буфером.
using detail::rasterizeLine3D;

/// @brief Растеризует 3D-треугольник с барицентрической интерполяцией цвета вершин (Гуро) и Z-буфером.
using detail::rasterizeTriangleGouraud;

/// @brief Растеризует 2D-треугольник с барицентрической интерполяцией цвета вершин (Гуро) и alpha blending.
using detail::rasterizeTriangleGouraud2D;

/// @brief Растеризует 2D-треугольник с наложением текстуры и alpha blending.
using detail::rasterizeTriangleTextured2D;

/// @brief Растеризует 3D-треугольник с наложением текстуры, перспективно-корректной интерполяцией UV и Z-буфером.
using detail::rasterizeTriangleTextured3D;

/// @brief Кеш производных shading-данных (нормали граней и вершин) на версию mesh-ресурса.
using detail::ShadingDataCache;

/// @brief Выборка цвета из текстуры методом ближайшего соседа.
using detail::sampleTextureNearest;

/// @brief Выборка цвета из текстуры методом билинейной интерполяции.
using detail::sampleTextureBilinear;

/// @brief Выборка цвета из текстуры с заданным методом фильтрации и режимом наложения.
using detail::sampleTexture;

} // namespace huynity::renderer
