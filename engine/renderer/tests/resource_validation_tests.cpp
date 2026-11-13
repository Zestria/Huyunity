#include "huynity/renderer/software_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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

void borrowedResourcesRecomputeNormalsAndValidateIndices() {
    Fixture fixture;
    fixture.owner.reset();
    renderer::ShadingDataCache cache;
    std::optional<std::array<render::TriangleIndices, 1>> storage;
    storage.emplace(std::array<render::TriangleIndices, 1>{{{0, 1, 2}}});
    {
        const render::MeshResource3 resource{fixture.owner, fixture.positions3, std::as_bytes(std::span{*storage})};
        auto face = cache.faceNormals(resource);
        auto vertex = cache.vertexNormals(resource);
        CHECK(face.has_value() && vertex.has_value());
        CHECK((**face)[0][2] == -1);
        CHECK((**vertex)[0][2] == -1);
    }
    storage.reset();
    storage.emplace(std::array<render::TriangleIndices, 1>{{{0, 2, 1}}});
    {
        const render::MeshResource3 resource{fixture.owner, fixture.positions3, std::as_bytes(std::span{*storage})};
        auto face = cache.faceNormals(resource);
        auto vertex = cache.vertexNormals(resource);
        CHECK(face.has_value() && vertex.has_value());
        CHECK((**face)[0][2] == 1);
        CHECK((**vertex)[0][2] == 1);
    }
    CHECK(cache.hits() == 0);

    for (const auto shading : {render::ShadingModel::Flat, render::ShadingModel::Gouraud}) {
        renderer::SoftwareRenderer softwareRenderer;
        auto framebuffer = image::ImageBuffer::create({4, 4});
        CHECK(framebuffer.has_value());
        storage.reset();
        storage.emplace(std::array<render::TriangleIndices, 1>{{{0, 1, 2}}});
        {
            auto snapshot = fixture.snapshot(true, std::as_bytes(std::span{*storage}));
            std::get<render::World3DPass>(snapshot.passes[0]).drawItems[0].shading = shading;
            CHECK(softwareRenderer.render(snapshot, *framebuffer).has_value());
        }
        storage.reset();
        storage.emplace(std::array<render::TriangleIndices, 1>{{{0, 1, 3}}});
        auto invalid = fixture.snapshot(true, std::as_bytes(std::span{*storage}));
        std::get<render::World3DPass>(invalid.passes[0]).drawItems[0].shading = shading;
        auto result = softwareRenderer.render(invalid, *framebuffer);
        CHECK(!result.has_value());
        CHECK(result.error() == renderer::RenderError::InvalidResource);
        CHECK(!cache.faceNormals(invalid.resources3[0]).has_value());
        CHECK(!cache.vertexNormals(invalid.resources3[0]).has_value());
    }
}

void liveCacheResultsSurviveCleanupAndInsertion() {
    std::array<Fixture, 3> fixtures;
    fixtures[2].triangles[0] = {0, 2, 1};
    renderer::ShadingDataCache cache;
    for (const auto& fixture : fixtures) {
        CHECK(cache.faceNormals(fixture.resource3()).has_value());
        CHECK(cache.vertexNormals(fixture.resource3()).has_value());
    }
    const auto second = fixtures[1].resource3();
    const auto third = fixtures[2].resource3();
    const auto* face = *cache.faceNormals(second);
    const auto* vertex = *cache.vertexNormals(second);
    const auto faceValues = *face;
    const auto vertexValues = *vertex;
    fixtures[0].owner.reset();
    CHECK(cache.vertexNormals(second).has_value());
    CHECK(*cache.faceNormals(second) == face);
    CHECK(*cache.vertexNormals(second) == vertex);
    CHECK(*cache.faceNormals(third) != face);
    CHECK(*cache.vertexNormals(third) != vertex);

    std::vector<std::shared_ptr<const void>> owners;
    for (int index = 0; index < 32; ++index) {
        owners.push_back(std::make_shared<const int>(index));
        auto resource = second;
        resource.owner = owners.back();
        CHECK(cache.faceNormals(resource).has_value());
        CHECK(cache.vertexNormals(resource).has_value());
    }
    CHECK(*cache.faceNormals(second) == face);
    CHECK(*cache.vertexNormals(second) == vertex);
    CHECK(*face == faceValues);
    CHECK(*vertex == vertexValues);
}

void nonOwningAliasesReuseTemporaryResults() {
    Fixture fixture;
    fixture.owner = std::shared_ptr<const void>{std::shared_ptr<const void>{}, &fixture.positions3};
    CHECK(fixture.owner.get() != nullptr);
    CHECK(fixture.owner.use_count() == 0);
    renderer::ShadingDataCache cache;
    const std::vector<math::Vector3>* face = nullptr;
    const std::vector<math::Vector3>* vertex = nullptr;
    for (int index = 0; index < 5; ++index) {
        fixture.triangles[0] = index % 2 == 0 ? render::TriangleIndices{0, 1, 2} : render::TriangleIndices{0, 2, 1};
        auto currentFace = cache.faceNormals(fixture.resource3());
        auto currentVertex = cache.vertexNormals(fixture.resource3());
        CHECK(currentFace.has_value() && currentVertex.has_value());
        if (face != nullptr) {
            CHECK(*currentFace == face);
            CHECK(*currentVertex == vertex);
        }
        face = *currentFace;
        vertex = *currentVertex;
        const double expected = index % 2 == 0 ? -1 : 1;
        CHECK((*face)[0][2] == expected);
        CHECK((*vertex)[0][2] == expected);
    }
    CHECK(cache.hits() == 0);
    CHECK(cache.misses() == 10);

    fixture.owner = std::shared_ptr<const void>{std::make_shared<const int>(0), nullptr};
    CHECK(fixture.owner.get() == nullptr);
    CHECK(fixture.owner.use_count() != 0);
    const auto resource = fixture.resource3();
    CHECK(cache.faceNormals(resource).has_value());
    CHECK(cache.vertexNormals(resource).has_value());
    CHECK(cache.faceNormals(resource).has_value());
    CHECK(cache.vertexNormals(resource).has_value());
    CHECK(cache.hits() == 2);
    CHECK(cache.misses() == 12);
}

void invalidWorldCameraDoesNotSkipResourceValidation() {
    const Fixture fixture;
    auto framebuffer = image::ImageBuffer::create({4, 4});
    CHECK(framebuffer.has_value());
    alignas(render::TriangleIndices) std::array<std::byte, sizeof(render::TriangleIndices) + 1> storage{};
    std::memcpy(storage.data(), fixture.resource3().triangles.data(), sizeof(render::TriangleIndices));
    const std::array<math::Point2, 7> extents{{{{0, 0}},
                                               {{0, 4}},
                                               {{4, 0}},
                                               {{-4, 4}},
                                               {{std::numeric_limits<double>::quiet_NaN(), 4}},
                                               {{std::numeric_limits<double>::infinity(), 4}},
                                               {{4, 4}}}};
    for (const auto& extent : extents) {
        auto snapshot = fixture.snapshot(false, storage);
        render::World2DPass world;
        world.camera = {.width = extent[0], .height = extent[1]};
        world.drawItems.push_back({});
        snapshot.passes[0] = world;
        for (const std::size_t byteCount : {sizeof(render::TriangleIndices) - 1, sizeof(render::TriangleIndices) + 1}) {
            snapshot.resources2[0].triangles = std::span<const std::byte>{storage}.first(byteCount);
            const auto result = renderer::render(snapshot, *framebuffer);
            CHECK(!result.has_value());
            CHECK(result.error() == renderer::RenderError::InvalidResource);
        }
        snapshot.resources2[0].triangles = std::span<const std::byte>{storage}.first(sizeof(render::TriangleIndices));
        CHECK(renderer::render(snapshot, *framebuffer).has_value());
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const std::array<std::pair<std::string_view, void (*)()>, 10> cases{{
        {"null_material", nullMaterialsReturnAnError},
        {"truncated_triangles", truncatedTrianglesReturnAnError},
        {"unaligned_triangles", unalignedTriangleBytesAreSupported},
        {"cache_identity", faceNormalKeyIncludesTopologyAndVertexCount},
        {"clear_stride", clearSupportsUnalignedStrideAndPreservesPadding},
        {"cache_lifetime", shadingCacheDoesNotRetainRemovedResources},
        {"borrowed_cache", borrowedResourcesRecomputeNormalsAndValidateIndices},
        {"stable_cache_results", liveCacheResultsSurviveCleanupAndInsertion},
        {"aliasing_borrowed_cache", nonOwningAliasesReuseTemporaryResults},
        {"world2d_invalid_camera", invalidWorldCameraDoesNotSkipResourceValidation},
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
