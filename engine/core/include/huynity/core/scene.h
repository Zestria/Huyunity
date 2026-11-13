#pragma once

#include "huynity/core/object_store.h"
#include "huynity/core/tessellation_cache.h"
#include "huynity/math/transform.h"
#include "huynity/objectlib/create_object.h"
#include "huynity/objectlib/operations/bounds.h"
#include "huynity/render_contracts/render_snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace huynity::core {

using Transform = std::variant<math::Transform2, math::Transform3>;

enum class Mobility {
    Static,
    Dynamic,
};

enum class RenderSpace {
    World,
    Hud,
};

struct InstanceRevisions {
    std::uint64_t localTransform{};
    std::uint64_t worldTransform{};
    std::uint64_t bounds{};
    std::uint64_t material{};
    std::uint64_t visibility{};
};

struct DirtyFlags {
    bool worldTransform{true};
    bool bounds{true};
    bool material{true};
    bool visibility{true};
    bool tessellation{};
};

struct SceneInstance {
    std::shared_ptr<const objectlib::Geometry> geometry;
    std::shared_ptr<const objectlib::Geometry> finiteRepresentation;
    std::uint64_t geometryVersion{};
    Transform localTransform{math::Transform3{}};
    Transform worldTransform{math::Transform3{}};
    Mobility mobility{Mobility::Dynamic};
    bool isVisible{true};
    RenderSpace renderSpace{RenderSpace::World};
    std::int32_t layer{};
    std::int32_t order{};
    bool wireframe{false};
    std::shared_ptr<const render_contracts::Material> material;
    InstanceRevisions revisions;
    DirtyFlags dirty;
    std::optional<objectlib::BoundsGeometry> localBounds;
    std::optional<objectlib::BoundsGeometry> worldBounds;
    std::uint64_t cachedBoundsGeometryVersion{};
    std::optional<ObjectId> parent;
    std::vector<ObjectId> children;
};

enum class SceneError {
    InvalidObjectId,
    CyclicHierarchy,
    TransformDimensionMismatch,
    InvalidFiniteRepresentation,
};

struct CreateObjectCommand {
    objectlib::ObjectSpec spec;
};

struct CreateInstanceCommand {
    ObjectId source;
};

struct SetParentCommand {
    ObjectId child;
    std::optional<ObjectId> parent;
};

struct SetLocalTransformCommand {
    ObjectId id;
    Transform transform;
};

struct SetGeometryCommand {
    ObjectId id;
    objectlib::Geometry geometry;
};

struct SetMobilityCommand {
    ObjectId id;
    Mobility mobility;
};

struct SetVisibilityCommand {
    ObjectId id;
    bool isVisible;
};

struct SetMaterialCommand {
    ObjectId id;
    std::shared_ptr<const render_contracts::Material> material;
};

struct SetRenderPropertiesCommand {
    ObjectId id;
    RenderSpace renderSpace;
    std::int32_t layer;
    std::int32_t order;
    bool wireframe{false};
};

struct RemoveSubtreeCommand {
    ObjectId root;
};

using Command = std::variant<CreateObjectCommand, CreateInstanceCommand, SetParentCommand, SetLocalTransformCommand,
                             SetGeometryCommand, SetMobilityCommand, SetVisibilityCommand, SetMaterialCommand,
                             SetRenderPropertiesCommand, RemoveSubtreeCommand>;

struct CommandRequest {
    std::uint64_t requestId;
    Command command;
};

using CommandError = std::variant<objectlib::CreateError, SceneError>;

struct CommandResult {
    std::uint64_t requestId;
    std::expected<void, CommandError> status;
    std::optional<ObjectId> createdObject;
};

struct SceneInstanceView {
    ObjectId id;
    std::shared_ptr<const objectlib::Geometry> geometry;
    std::uint64_t geometryVersion;
    Transform localTransform;
    Transform worldTransform;
    std::optional<objectlib::BoundsGeometry> worldBounds;
    Mobility mobility;
    bool isVisible;
    RenderSpace renderSpace;
    std::int32_t layer;
    std::int32_t order;
    bool wireframe{false};
    std::shared_ptr<const render_contracts::Material> material;
    std::optional<ObjectId> parent;
    std::vector<ObjectId> children;
};

struct SceneView {
    std::vector<SceneInstanceView> instances;
};

class Scene {
public:
    using CreateResult = std::expected<ObjectId, objectlib::CreateError>;
    using Result = std::expected<void, SceneError>;
    using SpatialResult = std::expected<void, objectlib::GeometryError>;

    template <objectlib::ObjectType kind>
        requires objectlib::SupportedObject<kind>
    [[nodiscard]] CreateResult createObject(objectlib::ParamsFor<kind> params) {
        auto geometry = objectlib::createObject<kind>(std::move(params));
        if (!geometry) {
            return std::unexpected(geometry.error());
        }
        return registerGeometry(objectlib::Geometry{std::move(*geometry)});
    }

    [[nodiscard]] CreateResult createObject(objectlib::ObjectSpec spec) {
        auto geometry = objectlib::createObject(std::move(spec));
        if (!geometry) {
            return std::unexpected(geometry.error());
        }
        return registerGeometry(std::move(*geometry));
    }

    [[nodiscard]] std::expected<ObjectId, SceneError> createInstance(ObjectId source) {
        const auto* sourceInstance = m_instances.find(source);
        if (sourceInstance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        auto instance = *sourceInstance;
        instance.parent.reset();
        instance.children.clear();
        const auto id = m_instances.insert(std::move(instance));
        updateWorld(id);
        return id;
    }

    Result setParent(ObjectId child, std::optional<ObjectId> parent) {
        auto* childInstance = m_instances.find(child);
        if (childInstance == nullptr || (parent && !m_instances.contains(*parent))) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (parent && (child == *parent || isDescendant(*parent, child))) {
            return std::unexpected(SceneError::CyclicHierarchy);
        }
        if (parent) {
            auto* parentInstance = m_instances.find(*parent);
            if (childInstance->localTransform.index() != parentInstance->localTransform.index()) {
                return std::unexpected(SceneError::TransformDimensionMismatch);
            }
            parentInstance->children.reserve(parentInstance->children.size() + 1);
        }

        detachFromParent(*childInstance);
        childInstance->parent = parent;
        if (parent) {
            m_instances.find(*parent)->children.push_back(child);
        }
        updateWorld(child);
        return {};
    }

    Result setLocalTransform(ObjectId id, const Transform& transform) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (instance->localTransform.index() != transform.index()) {
            return std::unexpected(SceneError::TransformDimensionMismatch);
        }
        if (instance->localTransform == transform) {
            return {};
        }
        instance->localTransform = transform;
        ++instance->revisions.localTransform;
        updateWorld(id);
        return {};
    }

    Result setGeometry(ObjectId id, objectlib::Geometry geometry) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        const bool is2D = std::visit(
            [](const auto& value) {
                using GeometryType = std::decay_t<decltype(value)>;
                return is2DGeometry<GeometryType>;
            },
            geometry);
        if ((instance->localTransform.index() == 0) != is2D) {
            return std::unexpected(SceneError::TransformDimensionMismatch);
        }
        instance->geometry = std::make_shared<const objectlib::Geometry>(std::move(geometry));
        instance->finiteRepresentation.reset();
        ++instance->geometryVersion;
        ++instance->revisions.bounds;
        instance->dirty.bounds = true;
        instance->dirty.tessellation = true;
        return {};
    }

    Result setFiniteRepresentation(ObjectId id, objectlib::Geometry geometry) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        const bool is2D = std::visit(
            [](const auto& value) {
                using GeometryType = std::decay_t<decltype(value)>;
                return is2DGeometry<GeometryType>;
            },
            geometry);
        if ((instance->localTransform.index() == 0) != is2D) {
            return std::unexpected(SceneError::TransformDimensionMismatch);
        }
        if (!objectlib::bounds(geometry)) {
            return std::unexpected(SceneError::InvalidFiniteRepresentation);
        }
        instance->finiteRepresentation = std::make_shared<const objectlib::Geometry>(std::move(geometry));
        instance->localBounds.reset();
        instance->cachedBoundsGeometryVersion = 0;
        instance->dirty.bounds = true;
        instance->dirty.tessellation = true;
        return {};
    }

    Result setMobility(ObjectId id, Mobility mobility) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        instance->mobility = mobility;
        return {};
    }

    Result setVisibility(ObjectId id, bool isVisible) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (instance->isVisible == isVisible) {
            return {};
        }
        instance->isVisible = isVisible;
        ++instance->revisions.visibility;
        instance->dirty.visibility = true;
        return {};
    }

    Result setRenderProperties(ObjectId id, RenderSpace renderSpace, std::int32_t layer, std::int32_t order,
                               bool wireframe = false) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (instance->renderSpace == renderSpace && instance->layer == layer && instance->order == order &&
            instance->wireframe == wireframe) {
            return {};
        }
        instance->renderSpace = renderSpace;
        instance->layer = layer;
        instance->order = order;
        instance->wireframe = wireframe;
        ++instance->revisions.visibility;
        instance->dirty.visibility = true;
        return {};
    }

    Result setWireframe(ObjectId id, bool wireframe) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (instance->wireframe == wireframe) {
            return {};
        }
        instance->wireframe = wireframe;
        ++instance->revisions.visibility;
        instance->dirty.visibility = true;
        return {};
    }

    Result setMaterial(ObjectId id, std::shared_ptr<const render_contracts::Material> material) {
        auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        if (instance->material == material) {
            return {};
        }
        instance->material = std::move(material);
        ++instance->revisions.material;
        instance->dirty.material = true;
        return {};
    }

    Result removeSubtree(ObjectId root) {
        if (!m_instances.contains(root)) {
            return std::unexpected(SceneError::InvalidObjectId);
        }
        std::vector<ObjectId> removal{root};
        for (std::size_t i = 0; i < removal.size(); ++i) {
            const auto* instance = m_instances.find(removal[i]);
            removal.insert(removal.end(), instance->children.begin(), instance->children.end());
        }
        m_instances.reserveRemoval(removal.size());
        detachFromParent(*m_instances.find(root));
        for (auto it = removal.rbegin(); it != removal.rend(); ++it) {
            m_instances.remove(*it);
        }
        return {};
    }

    [[nodiscard]] std::vector<CommandResult> apply(std::span<const CommandRequest> requests) {
        std::vector<CommandResult> results;
        results.reserve(requests.size());
        for (const auto& request : requests) {
            results.push_back(applyOne(request));
        }
        return results;
    }

    [[nodiscard]] SceneView view() const {
        SceneView result;
        result.instances.reserve(m_instances.size());
        for (std::size_t index = 0; index < m_instances.size(); ++index) {
            const auto& instance = m_instances.values()[index];
            result.instances.push_back({m_instances.ids()[index], instance.geometry, instance.geometryVersion,
                                        instance.localTransform, instance.worldTransform, instance.worldBounds,
                                        instance.mobility, instance.isVisible, instance.renderSpace, instance.layer,
                                        instance.order, instance.wireframe, instance.material, instance.parent,
                                        instance.children});
        }
        return result;
    }

    [[nodiscard]] SpatialResult refreshSpatialState() {
        for (ObjectId id : m_instances.ids()) {
            const auto* instance = m_instances.find(id);
            if (instance->isVisible && instance->dirty.bounds) {
                auto result = updateBounds(id);
                if (!result) {
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    }

    [[nodiscard]] SpatialResult recomputeSpatialState() {
        for (auto& instance : m_instances.values()) {
            instance.localBounds.reset();
            instance.worldBounds.reset();
            instance.cachedBoundsGeometryVersion = 0;
            instance.dirty.bounds = true;
        }
        return refreshSpatialState();
    }

    [[nodiscard]] TessellationCache::Result tessellation(ObjectId id, objectlib::TessellationOptions options = {}) {
        const auto* instance = m_instances.find(id);
        if (instance == nullptr) {
            return std::unexpected(objectlib::GeometryError{objectlib::GeometryErrorCode::InvalidGeometry});
        }
        const auto& geometry = instance->finiteRepresentation ? instance->finiteRepresentation : instance->geometry;
        auto result = m_tessellationCache.get(geometry, instance->geometryVersion, options);
        if (result) {
            m_instances.find(id)->dirty.tessellation = false;
        }
        return result;
    }

    [[nodiscard]] std::size_t tessellationCalculations() const noexcept { return m_tessellationCache.calculations(); }
    [[nodiscard]] std::size_t tessellationCacheHits() const noexcept { return m_tessellationCache.cacheHits(); }
    [[nodiscard]] std::size_t localBoundsCalculations() const noexcept { return m_localBoundsCalculations; }
    [[nodiscard]] const SceneInstance* find(ObjectId id) const noexcept { return m_instances.find(id); }
    [[nodiscard]] std::size_t size() const noexcept { return m_instances.size(); }

private:
    [[nodiscard]] CommandResult applyOne(const CommandRequest& request) {
        CommandResult result{request.requestId, {}, {}};
        std::visit(
            [&](const auto& command) {
                using CommandType = std::decay_t<decltype(command)>;
                if constexpr (std::is_same_v<CommandType, CreateObjectCommand>) {
                    auto created = createObject(command.spec);
                    if (created) {
                        result.createdObject = *created;
                    } else {
                        result.status = std::unexpected(CommandError{created.error()});
                    }
                } else if constexpr (std::is_same_v<CommandType, CreateInstanceCommand>) {
                    auto created = createInstance(command.source);
                    if (created) {
                        result.createdObject = *created;
                    } else {
                        result.status = std::unexpected(CommandError{created.error()});
                    }
                } else {
                    Result operation;
                    if constexpr (std::is_same_v<CommandType, SetParentCommand>) {
                        operation = setParent(command.child, command.parent);
                    } else if constexpr (std::is_same_v<CommandType, SetLocalTransformCommand>) {
                        operation = setLocalTransform(command.id, command.transform);
                    } else if constexpr (std::is_same_v<CommandType, SetGeometryCommand>) {
                        operation = setGeometry(command.id, command.geometry);
                    } else if constexpr (std::is_same_v<CommandType, SetMobilityCommand>) {
                        operation = setMobility(command.id, command.mobility);
                    } else if constexpr (std::is_same_v<CommandType, SetVisibilityCommand>) {
                        operation = setVisibility(command.id, command.isVisible);
                    } else if constexpr (std::is_same_v<CommandType, SetMaterialCommand>) {
                        operation = setMaterial(command.id, command.material);
                    } else if constexpr (std::is_same_v<CommandType, SetRenderPropertiesCommand>) {
                        operation = setRenderProperties(command.id, command.renderSpace, command.layer, command.order,
                                                        command.wireframe);
                    } else {
                        operation = removeSubtree(command.root);
                    }
                    if (!operation) {
                        result.status = std::unexpected(CommandError{operation.error()});
                    }
                }
            },
            request.command);
        return result;
    }

    template <typename GeometryType>
    static constexpr bool is2DGeometry = std::disjunction_v<
        std::is_same<GeometryType, objectlib::Point2>, std::is_same<GeometryType, objectlib::Segment2>,
        std::is_same<GeometryType, objectlib::Line2>, std::is_same<GeometryType, objectlib::Ray2>,
        std::is_same<GeometryType, objectlib::Triangle2>, std::is_same<GeometryType, objectlib::Polyline2>,
        std::is_same<GeometryType, objectlib::Aabb2>, std::is_same<GeometryType, objectlib::TriangleMesh2>,
        std::is_same<GeometryType, objectlib::Polygon2>, std::is_same<GeometryType, objectlib::Rectangle2>,
        std::is_same<GeometryType, objectlib::Circle2>, std::is_same<GeometryType, objectlib::Disk2>>;

    [[nodiscard]] ObjectId registerGeometry(objectlib::Geometry geometry) {
        const Transform initialTransform = std::visit(
            [](const auto& value) -> Transform {
                using GeometryType = std::decay_t<decltype(value)>;
                if constexpr (is2DGeometry<GeometryType>) {
                    return math::Transform2{};
                } else {
                    return math::Transform3{};
                }
            },
            geometry);
        auto resource = std::make_shared<const objectlib::Geometry>(std::move(geometry));
        SceneInstance instance;
        instance.geometry = std::move(resource);
        instance.geometryVersion = 1;
        instance.localTransform = initialTransform;
        instance.worldTransform = initialTransform;
        return m_instances.insert(std::move(instance));
    }

    [[nodiscard]] bool isDescendant(ObjectId candidate, ObjectId ancestor) const {
        const auto* instance = m_instances.find(candidate);
        while (instance != nullptr && instance->parent) {
            if (*instance->parent == ancestor) {
                return true;
            }
            instance = m_instances.find(*instance->parent);
        }
        return false;
    }

    void detachFromParent(SceneInstance& instance) {
        if (!instance.parent) {
            return;
        }
        auto& siblings = m_instances.find(*instance.parent)->children;
        const auto it =
            std::find(siblings.begin(), siblings.end(), m_instances.ids()[&instance - m_instances.values().data()]);
        if (it != siblings.end()) {
            siblings.erase(it);
        }
        instance.parent.reset();
    }

    template <std::size_t dimension>
    [[nodiscard]] static objectlib::BoundsResult<dimension>
    transformBounds(const objectlib::Aabb<dimension>& local, const math::Transform<dimension>& transform) {
        constexpr std::size_t cornerCount = std::size_t{1} << dimension;
        std::array<math::Point<dimension>, cornerCount> corners;
        for (std::size_t corner = 0; corner < cornerCount; ++corner) {
            for (std::size_t axis = 0; axis < dimension; ++axis) {
                corners[corner][axis] =
                    (corner & (std::size_t{1} << axis)) == 0 ? local.min()[axis] : local.max()[axis];
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
        auto result = objectlib::Aabb<dimension>::create({min, max});
        if (!result) {
            return std::unexpected(objectlib::GeometryError{objectlib::GeometryErrorCode::NumericalFailure});
        }
        return std::move(*result);
    }

    [[nodiscard]] SpatialResult updateBounds(ObjectId id) {
        auto* instance = m_instances.find(id);
        if (!instance->localBounds || instance->cachedBoundsGeometryVersion != instance->geometryVersion) {
            const auto& geometry = instance->finiteRepresentation ? instance->finiteRepresentation : instance->geometry;
            auto calculated = objectlib::bounds(*geometry);
            if (!calculated) {
                return std::unexpected(calculated.error());
            }
            instance->localBounds = std::move(*calculated);
            instance->cachedBoundsGeometryVersion = instance->geometryVersion;
            ++m_localBoundsCalculations;
        }
        auto world = std::visit(
            [](const auto& local,
               const auto& transform) -> std::expected<objectlib::BoundsGeometry, objectlib::GeometryError> {
                using BoundsType = std::decay_t<decltype(local)>;
                using TransformType = std::decay_t<decltype(transform)>;
                if constexpr (std::is_same_v<BoundsType, objectlib::Aabb2> &&
                              std::is_same_v<TransformType, math::Transform2>) {
                    auto transformed = transformBounds(local, transform);
                    if (!transformed) {
                        return std::unexpected(transformed.error());
                    }
                    return objectlib::BoundsGeometry{std::move(*transformed)};
                } else if constexpr (std::is_same_v<BoundsType, objectlib::Aabb3> &&
                                     std::is_same_v<TransformType, math::Transform3>) {
                    auto transformed = transformBounds(local, transform);
                    if (!transformed) {
                        return std::unexpected(transformed.error());
                    }
                    return objectlib::BoundsGeometry{std::move(*transformed)};
                } else {
                    return std::unexpected(objectlib::GeometryError{objectlib::GeometryErrorCode::InvalidGeometry});
                }
            },
            *instance->localBounds, instance->worldTransform);
        if (!world) {
            return std::unexpected(world.error());
        }
        instance->worldBounds = std::move(*world);
        instance->dirty.bounds = false;
        return {};
    }

    void updateWorld(ObjectId id) {
        auto* instance = m_instances.find(id);
        if (instance->parent) {
            const auto* parent = m_instances.find(*instance->parent);
            if (const auto* local = std::get_if<math::Transform2>(&instance->localTransform)) {
                instance->worldTransform = math::compose(std::get<math::Transform2>(parent->worldTransform), *local);
            } else {
                const auto& local3 = std::get<math::Transform3>(instance->localTransform);
                instance->worldTransform = math::compose(std::get<math::Transform3>(parent->worldTransform), local3);
            }
        } else {
            instance->worldTransform = instance->localTransform;
        }
        ++instance->revisions.worldTransform;
        ++instance->revisions.bounds;
        instance->dirty.worldTransform = false;
        instance->dirty.bounds = true;
        for (ObjectId child : instance->children) {
            updateWorld(child);
        }
    }

    ObjectStore<SceneInstance> m_instances;
    TessellationCache m_tessellationCache;
    std::size_t m_localBoundsCalculations{};
};

} // namespace huynity::core
