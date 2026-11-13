#include "huynity/core/render_preparation.h"

#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace core = huynity::core;
namespace math = huynity::math;
namespace objects = huynity::objectlib;
namespace render = huynity::render_contracts;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

render::OrthographicCamera3 camera3() {
    return {.width = 10, .height = 10, .nearPlane = 0, .farPlane = 100};
}

render::Camera2 camera2() {
    return {.width = 10, .height = 10};
}

void world3dContainsAllVisibleStateAfterCommands() {
    core::Scene scene;
    auto visible = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    auto hidden = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    auto outside = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    CHECK(visible.has_value() && hidden.has_value() && outside.has_value());
    auto material = std::make_shared<const render::Material>(render::Material{.color = {.red = 0.5}});
    const std::vector<core::CommandRequest> commands{
        {1, core::SetMaterialCommand{*visible, material}},
        {2, core::SetVisibilityCommand{*hidden, false}},
        {3, core::SetLocalTransformCommand{*outside, math::translation(math::Vector3{{20, 0, 1}})}},
    };
    CHECK(scene.apply(commands).size() == 3);

    auto prepared = core::RenderPreparation::prepareWorld3D(scene, camera3());
    CHECK(prepared.has_value());
    CHECK(prepared->drawItems.size() == 1);
    CHECK(prepared->drawItems[0].material == material);
    CHECK(prepared->drawItems[0].geometry != nullptr);
    CHECK(prepared->drawItems[0].localToWorld.translation(0) == 0);
}

void world3dDoesNotDependOnPreviousFrame() {
    core::Scene scene;
    auto first = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    auto second = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    CHECK(first.has_value() && second.has_value());
    auto initial = core::RenderPreparation::prepareWorld3D(scene, camera3());
    CHECK(initial.has_value() && initial->drawItems.size() == 2);

    CHECK(scene.setVisibility(*first, false).has_value());
    CHECK(scene.setLocalTransform(*second, math::translation(math::Vector3{{1, 2, 3}})).has_value());
    auto next = core::RenderPreparation::prepareWorld3D(scene, camera3());
    CHECK(next.has_value() && next->drawItems.size() == 1);
    CHECK(next->drawItems[0].localToWorld.translation(0) == 1);
    CHECK(next->drawItems[0].localToWorld.translation(1) == 2);
    CHECK(next->drawItems[0].localToWorld.translation(2) == 3);
}

void world2dUsesIndependentCameraAndVisibility() {
    core::Scene scene;
    auto visible = scene.createObject<objects::ObjectType::Rectangle2>({.size = {{2, 2}}});
    auto hidden = scene.createObject<objects::ObjectType::Rectangle2>({.size = {{2, 2}}});
    auto outside = scene.createObject<objects::ObjectType::Rectangle2>({.size = {{2, 2}}});
    CHECK(visible.has_value() && hidden.has_value() && outside.has_value());
    CHECK(scene.setVisibility(*hidden, false).has_value());
    CHECK(scene.setLocalTransform(*outside, math::translation(math::Vector2{{20, 0}})).has_value());

    auto prepared = core::RenderPreparation::prepareWorld2D(scene, camera2());
    CHECK(prepared.has_value());
    CHECK(prepared->drawItems.size() == 1);
    CHECK(prepared->drawItems[0].geometry != nullptr);
    CHECK(prepared->drawItems[0].localToWorld.translation(0) == 0);
}

void perspectiveCullsDepthAndHorizontalBounds() {
    core::Scene scene;
    auto visible = scene.createObject<objects::ObjectType::Sphere3>({.radius = 1});
    auto behind = scene.createObject<objects::ObjectType::Sphere3>({.radius = 1});
    CHECK(visible.has_value() && behind.has_value());
    CHECK(scene.setLocalTransform(*visible, math::translation(math::Vector3{{0, 0, 5}})).has_value());
    CHECK(scene.setLocalTransform(*behind, math::translation(math::Vector3{{0, 0, -5}})).has_value());
    render::PerspectiveCamera3 camera{.verticalFieldOfView = 1, .nearPlane = 0.1, .farPlane = 10};
    auto prepared = core::RenderPreparation::prepareWorld3D(scene, camera);
    CHECK(prepared.has_value());
    CHECK(prepared->drawItems.size() == 1);
    CHECK(prepared->drawItems[0].localToWorld.translation(2) == 5);
}

void cameraTransformsAffectBoth3dCullingModes() {
    core::Scene scene;
    auto object = scene.createObject<objects::ObjectType::Box3>({.size = {{1, 1, 1}}});
    CHECK(object.has_value());
    auto orthographic = camera3();
    orthographic.worldToView = math::translation(math::Vector3{{-20, 0, 0}});
    auto orthographicResult = core::RenderPreparation::prepareWorld3D(scene, orthographic);
    CHECK(orthographicResult.has_value());
    CHECK(orthographicResult->drawItems.empty());

    CHECK(scene.setLocalTransform(*object, math::translation(math::Vector3{{0, 0, 5}})).has_value());
    render::PerspectiveCamera3 perspective{.worldToView = math::translation(math::Vector3{{-20, 0, 0}}),
                                           .verticalFieldOfView = 1,
                                           .nearPlane = 0.1,
                                           .farPlane = 10};
    auto perspectiveResult = core::RenderPreparation::prepareWorld3D(scene, perspective);
    CHECK(perspectiveResult.has_value());
    CHECK(perspectiveResult->drawItems.empty());
}

void perspectiveCullingUsesViewportAndFarthestDepth() {
    core::Scene scene;
    const auto id = scene.createObject<objects::ObjectType::Box3>({.size = {{2, 2, 2}}});
    CHECK(id.has_value());
    CHECK(scene.setLocalTransform(*id, math::translation(math::Vector3{{15, 0, 10}})).has_value());
    core::PassesConfig config;
    config.camera3 =
        render::PerspectiveCamera3{.verticalFieldOfView = std::numbers::pi / 2, .nearPlane = 0.1, .farPlane = 100};
    config.hudEnabled = false;
    auto wide = core::RenderPreparation::prepareSnapshot(scene, config, 200, 100);
    CHECK(wide.has_value());
    CHECK(std::get<render::World3DPass>(wide->passes[0]).drawItems.size() == 1);
    auto square = core::RenderPreparation::prepareSnapshot(scene, config, 100, 100);
    CHECK(square.has_value());
    CHECK(std::get<render::World3DPass>(square->passes[0]).drawItems.empty());

    const auto deepBox = objects::Box3::create({.size = {{2, 2, 8}}});
    CHECK(deepBox.has_value());
    CHECK(scene.setGeometry(*id, objects::Geometry{*deepBox}).has_value());
    CHECK(scene.setLocalTransform(*id, math::translation(math::Vector3{{6, 0, 6}})).has_value());
    auto deep = core::RenderPreparation::prepareSnapshot(scene, config, 100, 100);
    CHECK(deep.has_value());
    CHECK(std::get<render::World3DPass>(deep->passes[0]).drawItems.size() == 1);
}

int main() {
    try {
        world3dContainsAllVisibleStateAfterCommands();
        world3dDoesNotDependOnPreviousFrame();
        world2dUsesIndependentCameraAndVisibility();
        perspectiveCullsDepthAndHorizontalBounds();
        cameraTransformsAffectBoth3dCullingModes();
        perspectiveCullingUsesViewportAndFarthestDepth();
        std::cout << "draw_lists: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
