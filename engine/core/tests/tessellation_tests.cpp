#include "huynity/core/scene.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace core = huynity::core;
namespace math = huynity::math;
namespace objects = huynity::objectlib;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

core::ObjectId createSphere(core::Scene& scene, double radius = 2) {
    auto result = scene.createObject<objects::ObjectType::Sphere3>({.radius = radius});
    CHECK(result.has_value());
    return *result;
}

void reuseAcrossMovementAndSharedInstances() {
    core::Scene scene;
    const auto first = createSphere(scene);
    const auto second = scene.createInstance(first);
    CHECK(second.has_value());
    auto initial = scene.tessellation(first, {.maxDeviation = 0.05, .maxPrimitiveCount = 10000});
    CHECK(initial.has_value());
    CHECK(scene.tessellationCalculations() == 1);

    CHECK(scene.setLocalTransform(first, math::translation(math::Vector3{{4, 5, 6}})).has_value());
    auto moved = scene.tessellation(first, {.maxDeviation = 0.05, .maxPrimitiveCount = 10000});
    auto shared = scene.tessellation(*second, {.maxDeviation = 0.05, .maxPrimitiveCount = 10000});
    CHECK(moved.has_value() && shared.has_value());
    CHECK(*moved == *initial);
    CHECK(*shared == *initial);
    CHECK(scene.tessellationCalculations() == 1);
    CHECK(scene.tessellationCacheHits() == 2);
}

void qualityKeyIncludesEveryOption() {
    core::Scene scene;
    const auto id = createSphere(scene);
    auto initial = scene.tessellation(id, {.maxDeviation = 0.1, .maxPrimitiveCount = 10000});
    auto deviation = scene.tessellation(id, {.maxDeviation = 0.05, .maxPrimitiveCount = 10000});
    auto primitiveLimit = scene.tessellation(id, {.maxDeviation = 0.1, .maxPrimitiveCount = 9000});
    CHECK(initial.has_value() && deviation.has_value() && primitiveLimit.has_value());
    CHECK(*deviation != *initial);
    CHECK(*primitiveLimit != *initial);
    CHECK(scene.tessellationCalculations() == 3);
    auto reused = scene.tessellation(id, {.maxDeviation = 0.05, .maxPrimitiveCount = 10000});
    CHECK(reused.has_value() && *reused == *deviation);
    CHECK(scene.tessellationCalculations() == 3);
}

void geometryChangeInvalidatesOnlyNewVersion() {
    core::Scene scene;
    const auto id = createSphere(scene);
    auto initial = scene.tessellation(id);
    CHECK(initial.has_value());
    auto replacement = objects::createObject<objects::ObjectType::Sphere3>({.radius = 3});
    CHECK(replacement.has_value());
    CHECK(scene.setGeometry(id, objects::Geometry{std::move(*replacement)}).has_value());
    CHECK(scene.find(id)->dirty.tessellation);
    auto changed = scene.tessellation(id);
    CHECK(changed.has_value());
    CHECK(*changed != *initial);
    CHECK(scene.tessellationCalculations() == 2);
    CHECK(!scene.find(id)->dirty.tessellation);
}

void geometryErrorLeavesSceneAndCacheConsistent() {
    core::Scene scene;
    const auto id = createSphere(scene);
    auto initial = scene.tessellation(id);
    CHECK(initial.has_value());
    const auto geometry = scene.find(id)->geometry;
    const auto version = scene.find(id)->geometryVersion;
    const auto calculations = scene.tessellationCalculations();

    auto failed = scene.tessellation(id, {.maxDeviation = 0.01, .maxPrimitiveCount = 0});
    CHECK(!failed.has_value());
    CHECK(scene.size() == 1);
    CHECK(scene.find(id)->geometry == geometry);
    CHECK(scene.find(id)->geometryVersion == version);
    CHECK(scene.find(id)->dirty.tessellation == false);
    CHECK(scene.tessellationCalculations() == calculations + 1);

    auto valid = scene.tessellation(id);
    CHECK(valid.has_value());
    CHECK(*valid == *initial);
    CHECK(scene.tessellationCalculations() == calculations + 1);
}

void cacheReleasesUnusedGeometry() {
    core::Scene scene;
    const auto id = createSphere(scene);
    std::weak_ptr<const objects::Geometry> original = scene.find(id)->geometry;
    std::weak_ptr<const objects::TessellatedGeometry> oldMesh;
    {
        auto mesh = scene.tessellation(id);
        CHECK(mesh.has_value());
        oldMesh = *mesh;
    }
    CHECK(scene.removeSubtree(id).has_value());
    CHECK(original.expired());
    const auto replacement = createSphere(scene);
    CHECK(scene.tessellation(replacement).has_value());
    CHECK(oldMesh.expired());
}

int main() {
    try {
        reuseAcrossMovementAndSharedInstances();
        qualityKeyIncludesEveryOption();
        geometryChangeInvalidatesOnlyNewVersion();
        geometryErrorLeavesSceneAndCacheConsistent();
        cacheReleasesUnusedGeometry();
        std::cout << "tessellation_cache: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
