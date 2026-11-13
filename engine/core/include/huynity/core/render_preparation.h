#pragma once

#include "huynity/core/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <memory>
#include <numbers>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace huynity::core {

struct PreparedDraw3 {
    std::shared_ptr<const objectlib::TessellatedGeometry> geometry;
    math::Transform3 localToWorld;
    std::shared_ptr<const render_contracts::Material> material;
    bool wireframe{false};
};

struct PreparedDraw2 {
    std::shared_ptr<const objectlib::TessellatedGeometry> geometry;
    math::Transform2 localToWorld;
    std::shared_ptr<const render_contracts::Material> material;
    std::int32_t layer{};
    std::int32_t order{};
    double alpha{1};
    bool wireframe{false};
};

struct WorldDrawList3 {
    render_contracts::Camera3 camera;
    render_contracts::Lighting3 lighting;
    std::vector<PreparedDraw3> drawItems;
};

struct WorldDrawList2 {
    render_contracts::Camera2 camera;
    std::vector<PreparedDraw2> drawItems;
};

struct HudDrawList2 {
    std::vector<PreparedDraw2> drawItems;
};

using PreparedPass = std::variant<WorldDrawList3, WorldDrawList2, HudDrawList2>;

struct PreparedPasses {
    std::vector<PreparedPass> passes;
};

struct PassesConfig {
    std::optional<render_contracts::Camera3> camera3;
    std::optional<render_contracts::Camera2> camera2;
    bool hudEnabled{true};
    bool world2DFirst{};
    render_contracts::Lighting3 lighting;
    objectlib::TessellationOptions tessellation;
};

class RenderPreparation {
public:
    template <typename DrawList>
    using Result = std::expected<DrawList, objectlib::GeometryError>;

    [[nodiscard]] static Result<WorldDrawList3> prepareWorld3D(Scene& scene, render_contracts::Camera3 camera,
                                                               render_contracts::Lighting3 lighting = {},
                                                               objectlib::TessellationOptions options = {},
                                                               double aspectRatio = 1.0) {
        auto spatial = scene.refreshSpatialState();
        if (!spatial) {
            return std::unexpected(spatial.error());
        }
        WorldDrawList3 result{std::move(camera), std::move(lighting), {}};
        const auto worldToView = std::visit([](const auto& value) { return value.worldToView; }, result.camera);
        const auto view = scene.view();
        for (const auto& instance : view.instances) {
            const auto* transform = std::get_if<math::Transform3>(&instance.worldTransform);
            if (instance.renderSpace != RenderSpace::World || !instance.isVisible || transform == nullptr ||
                !instance.worldBounds.has_value()) {
                continue;
            }
            const auto& bounds = std::get<objectlib::Aabb3>(*instance.worldBounds);
            if (!isVisible(bounds, worldToView, result.camera, aspectRatio)) {
                continue;
            }
            auto geometry = scene.tessellation(instance.id, options);
            if (!geometry) {
                return std::unexpected(geometry.error());
            }
            result.drawItems.push_back({std::move(*geometry), *transform, instance.material, instance.wireframe});
        }
        return result;
    }

    [[nodiscard]] static Result<WorldDrawList2> prepareWorld2D(Scene& scene, render_contracts::Camera2 camera,
                                                               objectlib::TessellationOptions options = {}) {
        auto spatial = scene.refreshSpatialState();
        if (!spatial) {
            return std::unexpected(spatial.error());
        }
        WorldDrawList2 result{camera, {}};
        const auto view = scene.view();
        for (const auto& instance : view.instances) {
            const auto* transform = std::get_if<math::Transform2>(&instance.worldTransform);
            if (instance.renderSpace != RenderSpace::World || !instance.isVisible || transform == nullptr ||
                !instance.worldBounds.has_value()) {
                continue;
            }
            const auto& bounds = std::get<objectlib::Aabb2>(*instance.worldBounds);
            if (!isVisible(bounds, camera)) {
                continue;
            }
            auto geometry = scene.tessellation(instance.id, options);
            if (!geometry) {
                return std::unexpected(geometry.error());
            }
            const double alpha = instance.material == nullptr ? 1 : instance.material->color.alpha;
            result.drawItems.push_back({std::move(*geometry), *transform, instance.material, instance.layer,
                                        instance.order, alpha, instance.wireframe});
        }
        return result;
    }

    [[nodiscard]] static Result<HudDrawList2> prepareHud2D(Scene& scene, objectlib::TessellationOptions options = {}) {
        HudDrawList2 result;
        const auto view = scene.view();
        for (const auto& instance : view.instances) {
            const auto* transform = std::get_if<math::Transform2>(&instance.worldTransform);
            if (instance.renderSpace != RenderSpace::Hud || !instance.isVisible || transform == nullptr) {
                continue;
            }
            auto geometry = scene.tessellation(instance.id, options);
            if (!geometry) {
                return std::unexpected(geometry.error());
            }
            const double alpha = instance.material == nullptr ? 1 : instance.material->color.alpha;
            result.drawItems.push_back({std::move(*geometry), *transform, instance.material, instance.layer,
                                        instance.order, alpha, instance.wireframe});
        }
        std::stable_sort(result.drawItems.begin(), result.drawItems.end(), [](const auto& first, const auto& second) {
            return std::tie(first.layer, first.order) < std::tie(second.layer, second.order);
        });
        return result;
    }

    [[nodiscard]] static Result<PreparedPasses> preparePasses(Scene& scene, const PassesConfig& config,
                                                              double aspectRatio = 1.0) {
        PreparedPasses result;
        auto appendWorld2 = [&]() -> std::expected<void, objectlib::GeometryError> {
            if (!config.camera2) {
                return {};
            }
            auto prepared = prepareWorld2D(scene, *config.camera2, config.tessellation);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            result.passes.push_back(std::move(*prepared));
            return {};
        };
        if (config.world2DFirst) {
            auto appended = appendWorld2();
            if (!appended) {
                return std::unexpected(appended.error());
            }
        }
        if (config.camera3) {
            auto prepared = prepareWorld3D(scene, *config.camera3, config.lighting, config.tessellation, aspectRatio);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            result.passes.push_back(std::move(*prepared));
        }
        if (!config.world2DFirst) {
            auto appended = appendWorld2();
            if (!appended) {
                return std::unexpected(appended.error());
            }
        }
        if (config.hudEnabled) {
            auto prepared = prepareHud2D(scene, config.tessellation);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            result.passes.push_back(std::move(*prepared));
        }
        return result;
    }

    [[nodiscard]] static Result<render_contracts::RenderSnapshot>
    prepareSnapshot(Scene& scene, const PassesConfig& config, std::size_t imageWidth, std::size_t imageHeight) {
        const double aspectRatio = imageHeight == 0 ? 1.0 : static_cast<double>(imageWidth) / imageHeight;
        auto prepared = preparePasses(scene, config, aspectRatio);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        render_contracts::RenderSnapshot result;
        result.imageWidth = imageWidth;
        result.imageHeight = imageHeight;
        result.materials.push_back(std::make_shared<const render_contracts::Material>());
        for (auto& pass : prepared->passes) {
            auto appended = appendSnapshotPass(pass, result);
            if (!appended) {
                return std::unexpected(appended.error());
            }
        }
        return result;
    }

private:
    [[nodiscard]] static render_contracts::MaterialIndex
    materialIndex(const std::shared_ptr<const render_contracts::Material>& material,
                  render_contracts::RenderSnapshot& snapshot) {
        if (material == nullptr) {
            return 0;
        }
        const auto found = std::find(snapshot.materials.begin(), snapshot.materials.end(), material);
        if (found != snapshot.materials.end()) {
            return static_cast<std::size_t>(found - snapshot.materials.begin());
        }
        snapshot.materials.push_back(material);
        return snapshot.materials.size() - 1;
    }

    [[nodiscard]] static std::expected<void, objectlib::GeometryError>
    appendSnapshotPass(PreparedPass& pass, render_contracts::RenderSnapshot& snapshot) {
        return std::visit(
            [&](auto& source) -> std::expected<void, objectlib::GeometryError> {
                using PassType = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<PassType, WorldDrawList3>) {
                    render_contracts::World3DPass target{source.camera, source.lighting, {}};
                    for (const auto& draw : source.drawItems) {
                        const auto* mesh = std::get_if<objectlib::TriangleMesh3>(draw.geometry.get());
                        if (mesh == nullptr) {
                            return std::unexpected(
                                objectlib::GeometryError{objectlib::GeometryErrorCode::UnsupportedOperation});
                        }
                        const auto found =
                            std::find_if(snapshot.resources3.begin(), snapshot.resources3.end(),
                                         [&](const auto& resource) { return resource.owner == draw.geometry; });
                        const auto index = found == snapshot.resources3.end()
                                               ? snapshot.resources3.size()
                                               : static_cast<std::size_t>(found - snapshot.resources3.begin());
                        if (found == snapshot.resources3.end()) {
                            render_contracts::MeshResource3 resource;
                            resource.owner = draw.geometry;
                            resource.positions = mesh->positions();
                            const auto triangles = mesh->triangles();
                            resource.triangles = std::as_bytes(triangles);
                            snapshot.resources3.push_back(resource);
                        }
                        target.drawItems.push_back(
                            {index, draw.localToWorld, materialIndex(draw.material, snapshot), true, draw.wireframe});
                    }
                    snapshot.passes.push_back(std::move(target));
                } else {
                    std::vector<render_contracts::DrawItem2> items;
                    for (const auto& draw : source.drawItems) {
                        const auto* mesh = std::get_if<objectlib::TriangleMesh2>(draw.geometry.get());
                        if (mesh == nullptr) {
                            return std::unexpected(
                                objectlib::GeometryError{objectlib::GeometryErrorCode::UnsupportedOperation});
                        }
                        const auto found =
                            std::find_if(snapshot.resources2.begin(), snapshot.resources2.end(),
                                         [&](const auto& resource) { return resource.owner == draw.geometry; });
                        const auto index = found == snapshot.resources2.end()
                                               ? snapshot.resources2.size()
                                               : static_cast<std::size_t>(found - snapshot.resources2.begin());
                        if (found == snapshot.resources2.end()) {
                            render_contracts::MeshResource2 resource;
                            resource.owner = draw.geometry;
                            resource.positions = mesh->positions();
                            const auto triangles = mesh->triangles();
                            resource.triangles = std::as_bytes(triangles);
                            snapshot.resources2.push_back(resource);
                        }
                        items.push_back({index, draw.localToWorld, materialIndex(draw.material, snapshot), draw.layer,
                                         draw.order, draw.wireframe});
                    }
                    if constexpr (std::is_same_v<PassType, WorldDrawList2>) {
                        snapshot.passes.push_back(render_contracts::World2DPass{source.camera, std::move(items)});
                    } else {
                        snapshot.passes.push_back(render_contracts::Hud2DPass{std::move(items)});
                    }
                }
                return {};
            },
            pass);
    }

    template <std::size_t dimension>
    [[nodiscard]] static objectlib::Aabb<dimension> transformBounds(const objectlib::Aabb<dimension>& bounds,
                                                                    const math::Transform<dimension>& transform) {
        constexpr std::size_t cornerCount = std::size_t{1} << dimension;
        std::array<math::Point<dimension>, cornerCount> corners;
        for (std::size_t corner = 0; corner < cornerCount; ++corner) {
            for (std::size_t axis = 0; axis < dimension; ++axis) {
                corners[corner][axis] =
                    (corner & (std::size_t{1} << axis)) == 0 ? bounds.min()[axis] : bounds.max()[axis];
            }
            corners[corner] = math::transformPoint(transform, corners[corner]);
        }
        math::Point<dimension> min = corners.front();
        math::Point<dimension> max = min;
        for (const auto& corner : corners) {
            for (std::size_t axis = 0; axis < dimension; ++axis) {
                min[axis] = std::min(min[axis], corner[axis]);
                max[axis] = std::max(max[axis], corner[axis]);
            }
        }
        return *objectlib::Aabb<dimension>::create({min, max});
    }

    [[nodiscard]] static bool intersects(const objectlib::Aabb2& bounds, double width, double height) {
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            return false;
        }
        return bounds.max()[0] >= -width / 2 && bounds.min()[0] <= width / 2 && bounds.max()[1] >= -height / 2 &&
               bounds.min()[1] <= height / 2;
    }

    [[nodiscard]] static bool isVisible(const objectlib::Aabb2& worldBounds, const render_contracts::Camera2& camera) {
        return intersects(transformBounds(worldBounds, camera.worldToView), camera.width, camera.height);
    }

    [[nodiscard]] static bool isVisible(const objectlib::Aabb3& worldBounds, const math::Transform3& worldToView,
                                        const render_contracts::Camera3& camera, double aspectRatio = 1.0) {
        const auto bounds = transformBounds(worldBounds, worldToView);
        return std::visit(
            [&](const auto& value) {
                using CameraType = std::decay_t<decltype(value)>;
                if (!std::isfinite(value.nearPlane) || !std::isfinite(value.farPlane) || value.nearPlane < 0 ||
                    value.farPlane <= value.nearPlane) {
                    return false;
                }
                if (bounds.max()[2] < value.nearPlane || bounds.min()[2] > value.farPlane) {
                    return false;
                }
                if constexpr (std::is_same_v<CameraType, render_contracts::OrthographicCamera3>) {
                    if (!std::isfinite(value.width) || !std::isfinite(value.height) || value.width <= 0 ||
                        value.height <= 0) {
                        return false;
                    }
                    return bounds.max()[0] >= -value.width / 2 && bounds.min()[0] <= value.width / 2 &&
                           bounds.max()[1] >= -value.height / 2 && bounds.min()[1] <= value.height / 2;
                } else {
                    if (!std::isfinite(value.verticalFieldOfView) || value.verticalFieldOfView <= 0 ||
                        value.verticalFieldOfView >= std::numbers::pi || value.nearPlane <= 0 ||
                        !std::isfinite(aspectRatio) || aspectRatio <= 0) {
                        return false;
                    }
                    // Самое широкое сечение пересечения AABB с диапазоном глубины камеры.
                    const double farthestDepth = std::min(bounds.max()[2], value.farPlane);
                    const double halfHeight = farthestDepth * std::tan(value.verticalFieldOfView / 2);
                    const double halfWidth = halfHeight * aspectRatio;
                    return bounds.max()[0] >= -halfWidth && bounds.min()[0] <= halfWidth &&
                           bounds.max()[1] >= -halfHeight && bounds.min()[1] <= halfHeight;
                }
            },
            camera);
    }

    [[nodiscard]] static bool isVisible(const objectlib::Aabb3& worldBounds, const math::Transform3& worldToView,
                                        const render_contracts::PerspectiveCamera3& camera) {
        return isVisible(worldBounds, worldToView, render_contracts::Camera3{camera});
    }
};

} // namespace huynity::core
