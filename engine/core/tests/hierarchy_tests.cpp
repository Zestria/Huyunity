#include "huynity/core/scene.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace core = huynity::core;
namespace math = huynity::math;
namespace objects = huynity::objectlib;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

core::ObjectId createObject(core::Scene& scene) {
    auto result = scene.createObject<objects::ObjectType::Point3>({.coordinates = {1, 2, 3}});
    CHECK(result.has_value());
    return *result;
}

void parentTransformsAndLocalPreservation() {
    core::Scene scene;
    const auto parent = createObject(scene);
    const auto child = createObject(scene);
    const auto grandchild = createObject(scene);
    CHECK(scene.setParent(child, parent).has_value());
    CHECK(scene.setParent(grandchild, child).has_value());
    CHECK(scene.setLocalTransform(parent, math::translation(math::Vector3{{10, 0, 0}})).has_value());
    CHECK(scene.setLocalTransform(child, math::translation(math::Vector3{{0, 2, 0}})).has_value());
    CHECK(scene.setLocalTransform(grandchild, math::translation(math::Vector3{{0, 0, 3}})).has_value());

    const auto childLocal = std::get<math::Transform3>(scene.find(child)->localTransform);
    CHECK(scene.find(child)->parent == parent);
    CHECK(std::get<math::Transform3>(scene.find(child)->worldTransform).translation(0) == 10);
    CHECK(std::get<math::Transform3>(scene.find(child)->worldTransform).translation(1) == 2);
    CHECK(std::get<math::Transform3>(scene.find(grandchild)->worldTransform).translation(0) == 10);
    CHECK(std::get<math::Transform3>(scene.find(grandchild)->worldTransform).translation(1) == 2);
    CHECK(std::get<math::Transform3>(scene.find(grandchild)->worldTransform).translation(2) == 3);
    CHECK(std::get<math::Transform3>(scene.find(child)->localTransform) == childLocal);

    CHECK(scene.setLocalTransform(parent, math::translation(math::Vector3{{-5, 1, 0}})).has_value());
    CHECK(std::get<math::Transform3>(scene.find(grandchild)->worldTransform).translation(0) == -5);
    CHECK(std::get<math::Transform3>(scene.find(grandchild)->worldTransform).translation(1) == 3);
    CHECK(std::get<math::Transform3>(scene.find(child)->localTransform) == childLocal);
}

void cyclesAreRejectedWithoutMutation() {
    core::Scene scene;
    const auto parent = createObject(scene);
    const auto child = createObject(scene);
    CHECK(scene.setParent(child, parent).has_value());
    const auto before = scene.find(parent)->worldTransform;
    auto self = scene.setParent(parent, parent);
    CHECK(!self.has_value() && self.error() == core::SceneError::CyclicHierarchy);
    auto cycle = scene.setParent(parent, child);
    CHECK(!cycle.has_value() && cycle.error() == core::SceneError::CyclicHierarchy);
    CHECK(scene.find(child)->parent == parent);
    CHECK(!scene.find(parent)->parent.has_value());
    CHECK(scene.find(parent)->worldTransform == before);
}

void subtreeRemovalInvalidatesEveryId() {
    core::Scene scene;
    const auto root = createObject(scene);
    const auto child = createObject(scene);
    const auto grandchild = createObject(scene);
    const auto unrelated = createObject(scene);
    CHECK(scene.setParent(child, root).has_value());
    CHECK(scene.setParent(grandchild, child).has_value());
    CHECK(scene.removeSubtree(root).has_value());
    CHECK(scene.size() == 1);
    CHECK(scene.find(root) == nullptr);
    CHECK(scene.find(child) == nullptr);
    CHECK(scene.find(grandchild) == nullptr);
    CHECK(scene.find(unrelated) != nullptr);
    CHECK(!scene.removeSubtree(child).has_value());
    CHECK(scene.find(unrelated) != nullptr);
}

core::ObjectId createObject2(core::Scene& scene) {
    auto result = scene.createObject<objects::ObjectType::Point2>({.coordinates = {1, 2}});
    CHECK(result.has_value());
    return *result;
}

void hierarchy2dAndReparenting() {
    core::Scene scene;
    const auto first = createObject2(scene);
    const auto second = createObject2(scene);
    const auto child = createObject2(scene);
    const auto grandchild = createObject2(scene);
    CHECK(scene.setParent(child, first).has_value());
    CHECK(scene.setParent(grandchild, child).has_value());
    CHECK(scene.setLocalTransform(first, math::translation(math::Vector2{{10, 0}})).has_value());
    CHECK(scene.setLocalTransform(child, math::translation(math::Vector2{{0, 2}})).has_value());
    CHECK(scene.setLocalTransform(grandchild, math::translation(math::Vector2{{0, 0}})).has_value());
    CHECK(std::get<math::Transform2>(scene.find(grandchild)->worldTransform).translation(0) == 10);
    CHECK(std::get<math::Transform2>(scene.find(grandchild)->worldTransform).translation(1) == 2);

    const auto childLocal = scene.find(child)->localTransform;
    const auto grandchildLocal = scene.find(grandchild)->localTransform;
    CHECK(scene.setLocalTransform(second, math::translation(math::Vector2{{-3, 5}})).has_value());
    CHECK(scene.setParent(child, second).has_value());
    CHECK(scene.find(first)->children.empty());
    CHECK(scene.find(second)->children.size() == 1);
    CHECK(scene.find(child)->localTransform == childLocal);
    CHECK(scene.find(grandchild)->localTransform == grandchildLocal);
    CHECK(std::get<math::Transform2>(scene.find(grandchild)->worldTransform).translation(0) == -3);
    CHECK(scene.setParent(child, std::nullopt).has_value());
    CHECK(scene.find(second)->children.empty());
    CHECK(!scene.find(child)->parent.has_value());
    CHECK(scene.find(child)->worldTransform == childLocal);
    CHECK(scene.find(grandchild)->worldTransform == childLocal);
}

void nonRootRemovalAndSlotReuse() {
    core::Scene scene;
    const auto root = createObject(scene);
    const auto child = createObject(scene);
    const auto grandchild = createObject(scene);
    CHECK(scene.setParent(child, root).has_value());
    CHECK(scene.setParent(grandchild, child).has_value());
    CHECK(scene.removeSubtree(child).has_value());
    CHECK(scene.find(root) != nullptr && scene.find(root)->children.empty());
    CHECK(scene.find(child) == nullptr && scene.find(grandchild) == nullptr);
    const auto replacement = createObject(scene);
    CHECK(replacement.slot == child.slot);
    const auto grandchildReplacement = createObject(scene);
    CHECK(grandchildReplacement.slot == grandchild.slot);
    CHECK(scene.find(child) == nullptr && scene.find(grandchild) == nullptr);
}

void detachedInstancesRecomputeWorldState() {
    core::Scene scene;
    for (const bool is2D : {true, false}) {
        const auto parent = is2D ? createObject2(scene) : createObject(scene);
        const auto child = is2D ? createObject2(scene) : createObject(scene);
        const core::Transform parentTransform = is2D ? core::Transform{math::translation(math::Vector2{{10, 0}})}
                                                     : core::Transform{math::translation(math::Vector3{{10, 0, 0}})};
        CHECK(scene.setLocalTransform(parent, parentTransform).has_value());
        CHECK(scene.setParent(child, parent).has_value());
        CHECK(scene.refreshSpatialState().has_value());

        const auto copy = scene.createInstance(child);
        CHECK(copy.has_value());
        CHECK(!scene.find(*copy)->parent.has_value());
        CHECK(scene.find(*copy)->children.empty());
        CHECK(scene.find(*copy)->geometry == scene.find(child)->geometry);
        CHECK(scene.find(*copy)->worldTransform == scene.find(*copy)->localTransform);
        CHECK(scene.refreshSpatialState().has_value());
        const auto& bounds = *scene.find(*copy)->worldBounds;
        std::visit([](const auto& value) { CHECK(value.min()[0] == 1); }, bounds);
        const core::Transform movedParent = is2D ? core::Transform{math::translation(math::Vector2{{20, 0}})}
                                                 : core::Transform{math::translation(math::Vector3{{20, 0, 0}})};
        CHECK(scene.setLocalTransform(parent, movedParent).has_value());
        CHECK(scene.find(*copy)->worldTransform == scene.find(*copy)->localTransform);
    }
}

int main() {
    try {
        parentTransformsAndLocalPreservation();
        cyclesAreRejectedWithoutMutation();
        subtreeRemovalInvalidatesEveryId();
        hierarchy2dAndReparenting();
        nonRootRemovalAndSlotReuse();
        detachedInstancesRecomputeWorldState();
        std::cout << "hierarchy: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
