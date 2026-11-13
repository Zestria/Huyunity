#include "huynity/renderer/software_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace render = huynity::render_contracts;
namespace image = huynity::image;
namespace math = huynity::math;
namespace renderer = huynity::renderer;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

struct Rgba {
    int red;
    int green;
    int blue;
    int alpha;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

constexpr Rgba BLACK{0, 0, 0, 255};
constexpr Rgba RED{255, 0, 0, 255};
constexpr Rgba BLUE{0, 0, 255, 255};

struct TestMesh2 {
    std::vector<math::Point2> positions;
    std::vector<render::TriangleIndices> triangles;
    std::shared_ptr<const void> owner = std::make_shared<const int>(0);
};

render::Color color(double red, double green, double blue, double alpha = 1) {
    return {red, green, blue, alpha};
}

std::shared_ptr<const render::Material> material(double red, double green, double blue, double alpha = 1) {
    return std::make_shared<const render::Material>(color(red, green, blue, alpha));
}

TestMesh2 triangle(double x0, double y0, double x1, double y1, double x2, double y2) {
    return {{{x0, y0}, {x1, y1}, {x2, y2}}, {{0, 1, 2}}, {}};
}

image::ImageBuffer makeFramebuffer(std::size_t width, std::size_t height) {
    auto created = image::ImageBuffer::create({width, height});
    if (!created) {
        throw std::runtime_error("failed to create framebuffer");
    }
    return std::move(*created);
}

Rgba pixel(const image::ImageBuffer& framebuffer, std::size_t x, std::size_t y) {
    const auto bytes = framebuffer.bytes();
    const std::size_t offset = y * framebuffer.stride() + x * 4;
    return {std::to_integer<int>(bytes[offset]), std::to_integer<int>(bytes[offset + 1]),
            std::to_integer<int>(bytes[offset + 2]), std::to_integer<int>(bytes[offset + 3])};
}

render::RenderSnapshot makeSnapshot(std::size_t width, std::size_t height) {
    render::RenderSnapshot snapshot;
    snapshot.imageWidth = width;
    snapshot.imageHeight = height;
    snapshot.clearColor = color(0, 0, 0);
    snapshot.materials = {material(1, 0, 0), material(0, 1, 0), material(0, 0, 1)};
    return snapshot;
}

void addResource(render::RenderSnapshot& snapshot, const TestMesh2& mesh) {
    render::MeshResource2 resource;
    resource.owner = mesh.owner;
    resource.positions = mesh.positions;
    resource.triangles = std::as_bytes(std::span{mesh.triangles});
    snapshot.resources2.push_back(resource);
}

render::World2DPass world2DPass() {
    render::World2DPass pass;
    pass.camera = render::Camera2{math::Transform2{}, 4, 4};
    return pass;
}

void world2DUsesOwnCameraIndependentOf3D() {
    auto snapshot = makeSnapshot(64, 64);
    auto mesh = triangle(-1, -1, 0, 1, 1, -1);
    addResource(snapshot, mesh);
    auto pass = world2DPass();
    pass.drawItems.push_back({0, math::Transform2{}, 0, 0, 0});
    snapshot.passes.push_back(pass);

    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, framebuffer).has_value());
    CHECK(pixel(framebuffer, 32, 32) == RED);
    CHECK(pixel(framebuffer, 0, 0) == BLACK);

    // независимая 2D-камера: смещение камеры двигает проекцию мирового содержимого
    auto& camera = std::get<render::World2DPass>(snapshot.passes.back()).camera;
    camera.worldToView = math::translation(math::Vector2{-1, 0});
    auto shifted = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, shifted).has_value());
    CHECK(pixel(shifted, 32, 32) == BLACK);
    CHECK(pixel(shifted, 16, 32) == RED);
}

void hudUsesScreenPixelsAndIgnoresWorldCameras() {
    auto snapshot = makeSnapshot(64, 64);
    auto worldMesh = triangle(-1, -1, 0, 1, 1, -1);
    auto hudMesh = triangle(4, 4, 28, 4, 4, 28); // экранные пиксели, y вниз
    addResource(snapshot, worldMesh);
    addResource(snapshot, hudMesh);
    auto world = world2DPass();
    world.drawItems.push_back({0, math::Transform2{}, 0, 0, 0});
    snapshot.passes.push_back(world);
    render::Hud2DPass hud;
    hud.drawItems.push_back({1, math::Transform2{}, 1, 0, 0});
    snapshot.passes.push_back(hud);

    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, framebuffer).has_value());
    CHECK(pixel(framebuffer, 8, 8) == Rgba{0, 255, 0, 255});
    CHECK(pixel(framebuffer, 32, 32) == RED);
    CHECK(pixel(framebuffer, 56, 56) == BLACK);

    // смещение 2D-камеры мира не влияет на экранный HUD
    auto& camera = std::get<render::World2DPass>(snapshot.passes[0]).camera;
    camera.worldToView = math::translation(math::Vector2{-100, 0});
    auto moved = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, moved).has_value());
    CHECK(pixel(moved, 8, 8) == Rgba{0, 255, 0, 255});
    CHECK(pixel(moved, 32, 32) == BLACK); // мировое содержимое ушло из вида
}

void layerAndOrderControlDrawOrderRegardlessOfSubmission() {
    auto snapshot = makeSnapshot(64, 64);
    auto mesh = triangle(8, 8, 56, 8, 8, 56); // один mesh, два экземпляра
    addResource(snapshot, mesh);

    const auto verify = [&](std::int32_t redLayer, std::int32_t redOrder, std::int32_t blueLayer,
                            std::int32_t blueOrder, const Rgba& expected) {
        for (const bool blueFirst : {false, true}) {
            auto current = snapshot;
            render::Hud2DPass hud;
            const render::DrawItem2 red{0, math::Transform2{}, 0, redLayer, redOrder};
            const render::DrawItem2 blue{0, math::Transform2{}, 2, blueLayer, blueOrder};
            if (blueFirst) {
                hud.drawItems = {blue, red};
            } else {
                hud.drawItems = {red, blue};
            }
            current.passes.push_back(hud);
            auto framebuffer = makeFramebuffer(64, 64);
            CHECK(renderer::render(current, framebuffer).has_value());
            CHECK(pixel(framebuffer, 32, 16) == expected);
        }
    };

    verify(0, 0, 0, 1, BLUE); // больший order сверху
    verify(0, 1, 0, 0, RED);  // меньший order сверху
    verify(1, 0, 0, 5, RED);  // layer важнее order
}

void alphaBlendingCompositesSourceOver() {
    auto snapshot = makeSnapshot(64, 64);
    auto mesh = triangle(8, 8, 56, 8, 8, 56);
    addResource(snapshot, mesh);
    snapshot.materials.push_back(material(0, 0, 1, 0.5));
    render::Hud2DPass hud;
    hud.drawItems = {
        {0, math::Transform2{}, 0, 0, 0}, // непрозрачный красный
        {0, math::Transform2{}, 3, 0, 1}, // полупрозрачный синий поверх
    };
    snapshot.passes.push_back(hud);

    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, framebuffer).has_value());
    CHECK(pixel(framebuffer, 32, 16) == Rgba{128, 0, 128, 255}); // source-over: 0.5 синего + 0.5 красного
}

void passOrderControlsMixedComposition() {
    auto snapshot = makeSnapshot(64, 64);
    auto mesh3 = [] {
        struct Storage {
            std::vector<math::Point3> positions;
            std::vector<render::TriangleIndices> triangles;
            std::shared_ptr<const void> owner = std::make_shared<const int>(0);
        };
        return Storage{{{-1, -1, 10}, {0, 1, 10}, {1, -1, 10}}, {{0, 1, 2}}, {}};
    }();
    auto hudMesh = triangle(8, 8, 56, 8, 8, 56);
    render::MeshResource3 resource3;
    resource3.owner = mesh3.owner;
    resource3.positions = mesh3.positions;
    resource3.triangles = std::as_bytes(std::span{mesh3.triangles});
    snapshot.resources3.push_back(resource3);
    addResource(snapshot, hudMesh);

    render::World3DPass world3;
    world3.camera = render::OrthographicCamera3{math::Transform3{}, 4, 4, 0.1, 100};
    world3.lighting = {{render::Color{1, 1, 1, 1}, 1}, {}};
    world3.drawItems.push_back({0, math::Transform3{}, 0, true});
    render::Hud2DPass hud;
    hud.drawItems.push_back({0, math::Transform2{}, 2, 0, 0});

    // HUD после 3D: экранный слой перекрывает мир
    auto hudOnTop = snapshot;
    hudOnTop.passes = {world3, hud};
    auto first = makeFramebuffer(64, 64);
    CHECK(renderer::render(hudOnTop, first).has_value());
    CHECK(pixel(first, 30, 30) == BLUE);

    // HUD до 3D: мировой проход перекрывает его по правилу порядка passes
    auto worldOnTop = snapshot;
    worldOnTop.passes = {hud, world3};
    auto second = makeFramebuffer(64, 64);
    CHECK(renderer::render(worldOnTop, second).has_value());
    CHECK(pixel(second, 30, 30) == RED);
}

void invalid2DIndicesAreErrors() {
    auto snapshot = makeSnapshot(64, 64);
    auto mesh = triangle(8, 8, 56, 8, 8, 56);
    addResource(snapshot, mesh);
    render::Hud2DPass hud;
    hud.drawItems.push_back({7, math::Transform2{}, 0, 0, 0});
    snapshot.passes.push_back(hud);
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(snapshot, framebuffer).error() == renderer::RenderError::InvalidResource);

    std::get<render::Hud2DPass>(snapshot.passes.back()).drawItems.back() = {0, math::Transform2{}, 9, 0, 0};
    CHECK(renderer::render(snapshot, framebuffer).error() == renderer::RenderError::InvalidMaterial);
}

void adjacentTrianglesBlendSharedEdgesOnlyOnce() {
    const std::array<math::Point2, 4> points{{{{0, 0}}, {{4, 0}}, {{4, 4}}, {{0, 4}}}};
    auto texture = image::ImageBuffer::create({1, 1});
    CHECK(texture.has_value());
    std::ranges::fill(texture->bytes(), std::byte{255});
    const render::Color tint{1, 0, 0, 0.5};
    for (int mode = 0; mode < 3; ++mode) {
        for (const bool reversed : {false, true}) {
            for (const bool reverseOrder : {false, true}) {
                auto framebuffer = makeFramebuffer(4, 4);
                renderer::detail::clear(framebuffer, {0, 0, 0, 1});
                std::array<std::array<std::size_t, 3>, 2> triangles{{{0, 1, 2}, {0, 2, 3}}};
                if (reverseOrder) {
                    std::swap(triangles[0], triangles[1]);
                }
                for (auto indices : triangles) {
                    if (reversed) {
                        std::swap(indices[0], indices[2]);
                    }
                    const auto& first = points[indices[0]];
                    const auto& second = points[indices[1]];
                    const auto& third = points[indices[2]];
                    if (mode == 0) {
                        renderer::detail::rasterizeTriangle2D({first[0], second[0], third[0]},
                                                              {first[1], second[1], third[1]}, tint, framebuffer);
                    } else if (mode == 1) {
                        renderer::rasterizeTriangleGouraud2D(first, second, third, tint, tint, tint, framebuffer);
                    } else {
                        renderer::rasterizeTriangleTextured2D(first, second, third, {}, {}, {}, *texture, tint,
                                                              framebuffer);
                    }
                }
                for (std::size_t y = 0; y < 4; ++y) {
                    for (std::size_t x = 0; x < 4; ++x) {
                        CHECK(pixel(framebuffer, x, y) == Rgba{128, 0, 0, 255});
                    }
                }
            }
        }
    }
}

int main() {
    try {
        world2DUsesOwnCameraIndependentOf3D();
        hudUsesScreenPixelsAndIgnoresWorldCameras();
        layerAndOrderControlDrawOrderRegardlessOfSubmission();
        alphaBlendingCompositesSourceOver();
        passOrderControlsMixedComposition();
        invalid2DIndicesAreErrors();
        adjacentTrianglesBlendSharedEdgesOnlyOnce();
        std::cout << "composition: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
