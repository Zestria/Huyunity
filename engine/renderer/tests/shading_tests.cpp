#include "huynity/renderer/software_renderer.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <numbers>
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

struct TestMesh {
    std::vector<math::Point3> positions;
    std::vector<render::TriangleIndices> triangles;
    std::shared_ptr<const void> owner = std::make_shared<const int>(0);
};

render::Color color(double red, double green, double blue) {
    return {red, green, blue, 1};
}

std::shared_ptr<const render::Material> material(double red, double green, double blue) {
    return std::make_shared<const render::Material>(color(red, green, blue));
}

// Лицевая грань по соглашению рендерера: нормаль cross(b - a, c - a) смотрит в -z.
TestMesh frontTriangle(double extent, double z) {
    return {{{-extent, -extent, z}, {0, extent, z}, {extent, -extent, z}}, {{0, 1, 2}}};
}

image::ImageBuffer makeFramebuffer(std::size_t width, std::size_t height) {
    auto created = image::ImageBuffer::create({width, height});
    if (!created) {
        throw std::runtime_error("failed to create framebuffer");
    }
    return std::move(*created);
}

struct Fixture {
    render::RenderSnapshot snapshot;
    TestMesh mesh;
};

// Сцена с одним красным треугольником перед ортографической камерой и заданным светом.
Fixture makeFixture(render::Lighting3 lighting) {
    Fixture fixture;
    fixture.snapshot.imageWidth = 64;
    fixture.snapshot.imageHeight = 64;
    fixture.snapshot.clearColor = color(0, 0, 0);
    fixture.snapshot.materials = {material(1, 0, 0)};
    fixture.mesh = frontTriangle(1, 10);
    render::MeshResource3 resource;
    resource.owner = fixture.mesh.owner;
    resource.positions = fixture.mesh.positions;
    resource.triangles = std::as_bytes(std::span{fixture.mesh.triangles});
    fixture.snapshot.resources3.push_back(resource);
    render::World3DPass pass;
    pass.camera = render::OrthographicCamera3{math::Transform3{}, 4, 4, 0.1, 100};
    pass.lighting = std::move(lighting);
    pass.drawItems.push_back({0, math::Transform3{}, 0, true});
    fixture.snapshot.passes.push_back(std::move(pass));
    return fixture;
}

render::DirectionalLight directional(math::Vector3 direction, double intensity,
                                     render::Color lightColor = color(1, 1, 1)) {
    return {direction, lightColor, intensity};
}

Rgba pixel(const image::ImageBuffer& framebuffer, std::size_t x, std::size_t y) {
    const auto bytes = framebuffer.bytes();
    const std::size_t offset = y * framebuffer.stride() + x * 4;
    return {std::to_integer<int>(bytes[offset]), std::to_integer<int>(bytes[offset + 1]),
            std::to_integer<int>(bytes[offset + 2]), std::to_integer<int>(bytes[offset + 3])};
}

Rgba centerPixel(const image::ImageBuffer& framebuffer) {
    return pixel(framebuffer, framebuffer.width() / 2, framebuffer.height() / 2);
}

void ambientLightScalesMaterialColor() {
    auto dim = makeFixture({{color(1, 1, 1), 0.5}, {}});
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(dim.snapshot, framebuffer).has_value());
    CHECK(centerPixel(framebuffer) == Rgba{128, 0, 0, 255});

    auto full = makeFixture({{color(1, 1, 1), 1}, {}});
    auto bright = makeFramebuffer(64, 64);
    CHECK(renderer::render(full.snapshot, bright).has_value());
    CHECK(centerPixel(bright) == Rgba{255, 0, 0, 255});
}

void directionalLightAddsDiffuseByNormal() {
    // свет распространяется в +z (от камеры вглубь): лицевая грань смотрит навстречу
    auto facing = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, 1}, 1)}});
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(facing.snapshot, framebuffer).has_value());
    CHECK(centerPixel(framebuffer) == Rgba{255, 0, 0, 255});

    // свет из глубины сцены к камере: лицевая грань не освещена
    auto behind = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, -1}, 1)}});
    auto dark = makeFramebuffer(64, 64);
    CHECK(renderer::render(behind.snapshot, dark).has_value());
    CHECK(centerPixel(dark) == Rgba{0, 0, 0, 255});
}

void perpendicularDirectionalAddsNothing() {
    auto fixture = makeFixture({{color(1, 1, 1), 0}, {directional({1, 0, 0}, 1)}});
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(fixture.snapshot, framebuffer).has_value());
    CHECK(centerPixel(framebuffer) == Rgba{0, 0, 0, 255});
}

void coloredLightMultipliesMaterialChannels() {
    auto fixture = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, 1}, 1, color(0, 0.5, 1))}});
    fixture.snapshot.materials = {material(1, 1, 1)};
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(fixture.snapshot, framebuffer).has_value());
    CHECK(centerPixel(framebuffer) == Rgba{0, 128, 255, 255});
}

void itemRotationTurnsNormalAwayFromLight() {
    // поворот на 180° вокруг Y с переносом сохраняет глубину; камера видит обратную
    // сторону грани. Кешированная нормаль переносится преобразованием экземпляра,
    // поэтому видимая сторона освещается светом, идущим со стороны камеры
    const auto flippedTransform = math::compose(math::translation(math::Vector3{0, 0, 20}),
                                                math::rotation(math::RotationAxis::Y, std::numbers::pi));
    auto rotated = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, 1}, 1)}});
    auto& item = std::get<render::World3DPass>(rotated.snapshot.passes.back()).drawItems.back();
    item.localToWorld = flippedTransform;
    item.backfaceCulling = false;
    auto lit = makeFramebuffer(64, 64);
    CHECK(renderer::render(rotated.snapshot, lit).has_value());
    CHECK(centerPixel(lit) == Rgba{255, 0, 0, 255});

    // свет из глубины сцены видимую обратную сторону не освещает
    auto flipped = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, -1}, 1)}});
    auto& flippedItem = std::get<render::World3DPass>(flipped.snapshot.passes.back()).drawItems.back();
    flippedItem.localToWorld = flippedTransform;
    flippedItem.backfaceCulling = false;
    auto dark = makeFramebuffer(64, 64);
    CHECK(renderer::render(flipped.snapshot, dark).has_value());
    CHECK(centerPixel(dark) == Rgba{0, 0, 0, 255});
}

void culledOffBackfaceIsShadedWithFlippedNormal() {
    auto fixture = makeFixture({{color(1, 1, 1), 0}, {directional({0, 0, 1}, 1)}});
    fixture.mesh.triangles = {{0, 2, 1}};
    fixture.snapshot.resources3.back().triangles = std::as_bytes(std::span{fixture.mesh.triangles});
    auto& item = std::get<render::World3DPass>(fixture.snapshot.passes.back()).drawItems.back();
    item.backfaceCulling = false;
    auto framebuffer = makeFramebuffer(64, 64);
    CHECK(renderer::render(fixture.snapshot, framebuffer).has_value());
    CHECK(centerPixel(framebuffer) == Rgba{255, 0, 0, 255});
}

void shadingCacheReusesNormalsForUnchangedGeometry() {
    auto fixture = makeFixture({{color(1, 1, 1), 1}, {}});
    renderer::SoftwareRenderer renderer;

    auto first = makeFramebuffer(64, 64);
    CHECK(renderer.render(fixture.snapshot, first).has_value());
    CHECK(renderer.shadingCacheMisses() == 1);
    CHECK(renderer.shadingCacheHits() == 0);

    auto second = makeFramebuffer(64, 64);
    CHECK(renderer.render(fixture.snapshot, second).has_value());
    CHECK(renderer.shadingCacheMisses() == 1);
    CHECK(renderer.shadingCacheHits() == 1);

    // неизменная геометрия даёт идентичный кадр
    const auto firstBytes = first.bytes();
    const auto secondBytes = second.bytes();
    CHECK(firstBytes.size() == secondBytes.size());
    CHECK(std::equal(firstBytes.begin(), firstBytes.end(), secondBytes.begin()));

    // движение экземпляра не инвалидирует кеш: нормали ресурса те же
    auto& item = std::get<render::World3DPass>(fixture.snapshot.passes.back()).drawItems.back();
    item.localToWorld = math::translation(math::Vector3{0.5, 0, 0});
    auto moved = makeFramebuffer(64, 64);
    CHECK(renderer.render(fixture.snapshot, moved).has_value());
    CHECK(renderer.shadingCacheMisses() == 1);
    CHECK(renderer.shadingCacheHits() == 2);

    // новая версия ресурса (другой владелец и данные) вычисляет нормали заново
    TestMesh changed = frontTriangle(1, 10);
    fixture.mesh = std::move(changed);
    render::MeshResource3 resource;
    resource.owner = fixture.mesh.owner;
    resource.positions = fixture.mesh.positions;
    resource.triangles = std::as_bytes(std::span{fixture.mesh.triangles});
    fixture.snapshot.resources3.back() = resource;
    auto updated = makeFramebuffer(64, 64);
    CHECK(renderer.render(fixture.snapshot, updated).has_value());
    CHECK(renderer.shadingCacheMisses() == 2);
}

int main() {
    try {
        ambientLightScalesMaterialColor();
        directionalLightAddsDiffuseByNormal();
        perpendicularDirectionalAddsNothing();
        coloredLightMultipliesMaterialChannels();
        itemRotationTurnsNormalAwayFromLight();
        culledOffBackfaceIsShadedWithFlippedNormal();
        shadingCacheReusesNormalsForUnchangedGeometry();
        std::cout << "shading: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
