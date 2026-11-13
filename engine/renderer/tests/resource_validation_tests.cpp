#include "huynity/renderer/software_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = huynity::image;
namespace math = huynity::math;
namespace render = huynity::render_contracts;
namespace renderer = huynity::renderer;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

namespace {

struct Fixture {
    std::array<math::Point3, 3> positions3{{{{-1, -1, 2}}, {{0, 1, 2}}, {{1, -1, 2}}}};
    std::array<math::Point2, 3> positions2{{{{0, 0}}, {{4, 0}}, {{0, 4}}}};
    std::array<render::TriangleIndices, 2> triangles{{{0, 1, 2}, {0, 2, 1}}};
    std::shared_ptr<const void> owner = std::make_shared<const int>(0);

    render::MeshResource3 resource3() const {
        return {owner, positions3, std::as_bytes(std::span{triangles}).first(sizeof(render::TriangleIndices))};
    }

    render::RenderSnapshot snapshot(bool is3D, std::span<const std::byte> indices) const {
        render::RenderSnapshot result;
        result.imageWidth = result.imageHeight = 4;
        result.materials.push_back(std::make_shared<const render::Material>());
        if (is3D) {
            result.resources3.push_back({owner, positions3, indices});
            render::World3DPass pass;
            pass.camera = render::OrthographicCamera3{.width = 4, .height = 4, .nearPlane = 0.1, .farPlane = 10};
            pass.lighting.ambient.intensity = 1;
            pass.drawItems.push_back({});
            result.passes.push_back(pass);
        } else {
            result.resources2.push_back({owner, positions2, indices});
            render::Hud2DPass pass;
            pass.drawItems.push_back({});
            result.passes.push_back(pass);
        }
        return result;
    }
};

void nullMaterialsReturnAnError() {
    const Fixture fixture;
    auto framebuffer = image::ImageBuffer::create({4, 4});
    CHECK(framebuffer.has_value());
    for (const bool is3D : {false, true}) {
        auto snapshot = fixture.snapshot(is3D, fixture.resource3().triangles);
        snapshot.materials[0].reset();
        auto result = renderer::render(snapshot, *framebuffer);
        CHECK(!result.has_value());
        CHECK(result.error() == renderer::RenderError::InvalidMaterial);
    }
}

void truncatedTrianglesReturnAnError() {
    const Fixture fixture;
    auto framebuffer = image::ImageBuffer::create({4, 4});
    CHECK(framebuffer.has_value());
    const auto bytes = fixture.resource3().triangles.first(sizeof(render::TriangleIndices) - 1);
    for (const bool is3D : {false, true}) {
        const auto snapshot = fixture.snapshot(is3D, bytes);
        auto result = renderer::render(snapshot, *framebuffer);
        CHECK(!result.has_value());
        CHECK(result.error() == renderer::RenderError::InvalidResource);
    }
    auto resource = fixture.resource3();
    resource.triangles = bytes;
    renderer::ShadingDataCache cache;
    CHECK(cache.faceNormals(resource).error() == renderer::RenderError::InvalidResource);
    CHECK(cache.vertexNormals(resource).error() == renderer::RenderError::InvalidResource);
}

void unalignedTriangleBytesAreSupported() {
    const Fixture fixture;
    alignas(render::TriangleIndices) std::array<std::byte, sizeof(render::TriangleIndices) + 1> storage{};
    const auto source = fixture.resource3().triangles;
    std::memcpy(storage.data() + 1, source.data(), source.size());
    const auto bytes = std::span<const std::byte>{storage}.subspan(1);
    auto framebuffer = image::ImageBuffer::create({4, 4});
    CHECK(framebuffer.has_value());
    for (const bool is3D : {false, true}) {
        auto snapshot = fixture.snapshot(is3D, bytes);
        CHECK(renderer::render(snapshot, *framebuffer).has_value());
        if (is3D) {
            auto& pass = std::get<render::World3DPass>(snapshot.passes[0]);
            pass.drawItems[0].shading = render::ShadingModel::Gouraud;
            CHECK(renderer::render(snapshot, *framebuffer).has_value());
        }
    }
}

void faceNormalKeyIncludesTopologyAndVertexCount() {
    const Fixture fixture;
    renderer::ShadingDataCache cache;
    auto resource = fixture.resource3();
    auto initial = cache.faceNormals(resource);
    CHECK(initial.has_value());
    CHECK((**initial)[0][2] == -1);
    resource.triangles = std::as_bytes(std::span{fixture.triangles}).last(sizeof(render::TriangleIndices));
    auto reversed = cache.faceNormals(resource);
    CHECK(reversed.has_value());
    CHECK((**reversed)[0][2] == 1);
    CHECK(cache.misses() == 2);
    CHECK(cache.faceNormals(resource).has_value());
    CHECK(cache.hits() == 1);
    resource.positions = resource.positions.first(2);
    auto shortened = cache.faceNormals(resource);
    CHECK(!shortened.has_value());
    CHECK(shortened.error() == renderer::RenderError::InvalidResource);
}

void clearSupportsUnalignedStrideAndPreservesPadding() {
    auto framebuffer = image::ImageBuffer::create({.width = 1, .height = 3, .stride = 5});
    CHECK(framebuffer.has_value());
    std::ranges::fill(framebuffer->bytes(), std::byte{0x5A});
    render::RenderSnapshot snapshot;
    snapshot.imageWidth = 1;
    snapshot.imageHeight = 3;
    snapshot.clearColor = {1, 0.5, 0, 1};
    CHECK(renderer::render(snapshot, *framebuffer).has_value());
    for (std::size_t y = 0; y < 3; ++y) {
        const auto row = framebuffer->row(y);
        CHECK(row[0] == std::byte{255});
        CHECK(row[1] == std::byte{128});
        CHECK(row[2] == std::byte{0});
        CHECK(row[3] == std::byte{255});
        CHECK(row[4] == std::byte{0x5A});
    }
}

void shadingCacheDoesNotRetainRemovedResources() {
    renderer::ShadingDataCache cache;
    std::weak_ptr<const void> owner;
    {
        const Fixture fixture;
        owner = fixture.owner;
        CHECK(cache.faceNormals(fixture.resource3()).has_value());
        CHECK(cache.vertexNormals(fixture.resource3()).has_value());
        CHECK(cache.faceNormals(fixture.resource3()).has_value());
        CHECK(cache.hits() == 1);
    }
    CHECK(owner.expired());
    const Fixture replacement;
    CHECK(cache.faceNormals(replacement.resource3()).has_value());
    CHECK(cache.vertexNormals(replacement.resource3()).has_value());
    CHECK(cache.misses() == 4);
}

} // namespace

int main(int argc, char* argv[]) {
    const std::array<std::pair<std::string_view, void (*)()>, 6> cases{{
        {"null_material", nullMaterialsReturnAnError},
        {"truncated_triangles", truncatedTrianglesReturnAnError},
        {"unaligned_triangles", unalignedTriangleBytesAreSupported},
        {"cache_identity", faceNormalKeyIncludesTopologyAndVertexCount},
        {"clear_stride", clearSupportsUnalignedStrideAndPreservesPadding},
        {"cache_lifetime", shadingCacheDoesNotRetainRemovedResources},
    }};
    for (const auto& [name, test] : cases) {
        if (argc == 2 && name == argv[1]) {
            try {
                test();
                std::cout << name << ": OK\n";
                return 0;
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                return 1;
            }
        }
    }
    std::cerr << "Expected a resource validation case name\n";
    return 1;
}
