#include "huynity/renderer/software_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
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
constexpr Rgba WHITE{255, 255, 255, 255};

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

render::Color color(double red, double green, double blue, double alpha = 1.0) {
    return {red, green, blue, alpha};
}

// Проверка 3D-растеризации Гуро: контрастные цвета вершин, цвета вблизи вершин и цвет в центре.
void testGouraud3DInterpolation() {
    constexpr std::size_t width = 120;
    constexpr std::size_t height = 120;
    auto framebuffer = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer, color(0, 0, 0));
    std::vector<double> depth(width * height, std::numeric_limits<double>::infinity());

    // Треугольник с экранными вершинами: V0 = (60.5, 20.5), V1 = (10.5, 110.5), V2 = (110.5, 110.5).
    // Центроид: ((60.5 + 10.5 + 110.5) / 3, (20.5 + 110.5 + 110.5) / 3) = (60.5, 80.5) -> пиксель (60, 80).
    const renderer::detail::ProjectedVertex v0{1.0 / 120.0, 79.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v1{-99.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v2{101.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};

    const auto c0 = color(1, 0, 0); // Красный
    const auto c1 = color(0, 1, 0); // Зелёный
    const auto c2 = color(0, 0, 1); // Синий

    renderer::rasterizeTriangleGouraud(v0, v1, v2, c0, c1, c2, framebuffer, depth);

    // 1. Проверяем пиксели вблизи вершин
    // Вблизи V0 (верхняя вершина, красный): пиксель (60, 21)
    const auto nearV0 = pixel(framebuffer, 60, 21);
    CHECK(nearV0.red >= 245);
    CHECK(nearV0.green <= 5);
    CHECK(nearV0.blue <= 5);
    CHECK(nearV0.alpha == 255);

    // Вблизи V1 (левая нижняя вершина, зелёный): пиксель (11, 110)
    const auto nearV1 = pixel(framebuffer, 11, 110);
    CHECK(nearV1.green >= 245);
    CHECK(nearV1.red <= 5);
    CHECK(nearV1.blue <= 5);
    CHECK(nearV1.alpha == 255);

    // Вблизи V2 (правая нижняя вершина, синий): пиксель (109, 110)
    const auto nearV2 = pixel(framebuffer, 109, 110);
    CHECK(nearV2.blue >= 245);
    CHECK(nearV2.red <= 5);
    CHECK(nearV2.green <= 5);
    CHECK(nearV2.alpha == 255);

    // 2. Проверяем центр треугольника: центроид строго попадает на центр пикселя (60, 80).
    // При w0 = w1 = w2 = 1/3: R = 1/3, G = 1/3, B = 1/3 -> в байтах 255 * (1/3) = 85.
    const auto center = pixel(framebuffer, 60, 80);
    CHECK(std::abs(center.red - 85) <= 1);
    CHECK(std::abs(center.green - 85) <= 1);
    CHECK(std::abs(center.blue - 85) <= 1);
    CHECK(center.alpha == 255);

    // Проверяем относительные доли каналов (R ≈ 1/3, G ≈ 1/3, B ≈ 1/3)
    const double rNorm = static_cast<double>(center.red) / 255.0;
    const double gNorm = static_cast<double>(center.green) / 255.0;
    const double bNorm = static_cast<double>(center.blue) / 255.0;
    constexpr double third = 1.0 / 3.0;
    CHECK(std::abs(rNorm - third) < 0.01);
    CHECK(std::abs(gNorm - third) < 0.01);
    CHECK(std::abs(bNorm - third) < 0.01);

    // Проверяем глубину в центре
    CHECK(std::abs(depth[80 * width + 60] - 10.0) < 1e-6);
}

// Проверка монотонности градиента вдоль медианы и вдоль рёбер (отсутствие скачков).
void testGradientMonotonicity() {
    constexpr std::size_t width = 120;
    constexpr std::size_t height = 120;
    auto framebuffer = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer, color(0, 0, 0));
    std::vector<double> depth(width * height, std::numeric_limits<double>::infinity());

    const renderer::detail::ProjectedVertex v0{1.0 / 120.0, 79.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v1{-99.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v2{101.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};

    const auto c0 = color(1, 0, 0); // Красный
    const auto c1 = color(0, 1, 0); // Зелёный
    const auto c2 = color(0, 0, 1); // Синий

    renderer::rasterizeTriangleGouraud(v0, v1, v2, c0, c1, c2, framebuffer, depth);

    // 1. Монотонность вдоль медианы x = 60 от y = 21 до y = 108:
    // Сверху вниз красный монотонно убывает, зелёный и синий монотонно возрастают.
    for (std::size_t y = 21; y < 108; ++y) {
        const auto pCurrent = pixel(framebuffer, 60, y);
        const auto pNext = pixel(framebuffer, 60, y + 1);

        // Красный монотонно не возрастает (убывает или равен из-за дискретизации)
        CHECK(pNext.red <= pCurrent.red);
        // Зелёный и синий монотонно не убывают
        CHECK(pNext.green >= pCurrent.green);
        CHECK(pNext.blue >= pCurrent.blue);

        // Симметрия относительно оси x = 60.5
        CHECK(std::abs(pCurrent.green - pCurrent.blue) <= 1);

        // Отсутствие резких перепадов между соседними пикселями (плавный градиент)
        CHECK(std::abs(pNext.red - pCurrent.red) <= 4);
        CHECK(std::abs(pNext.green - pCurrent.green) <= 3);
        CHECK(std::abs(pNext.blue - pCurrent.blue) <= 3);
    }

    // 2. Монотонность вдоль нижнего ребра y = 110 от x = 12 до x = 108:
    // Слева направо (от зелёной V1 к синей V2) зелёный монотонно убывает, синий возрастает, красный близок к 0.
    for (std::size_t x = 12; x < 108; ++x) {
        const auto pCurrent = pixel(framebuffer, x, 110);
        const auto pNext = pixel(framebuffer, x + 1, 110);

        CHECK(pNext.green <= pCurrent.green);
        CHECK(pNext.blue >= pCurrent.blue);
        CHECK(pCurrent.red <= 2);

        // Плавность перехода цвета вдоль ребра
        CHECK(std::abs(pNext.green - pCurrent.green) <= 4);
        CHECK(std::abs(pNext.blue - pCurrent.blue) <= 4);
    }
}

// Проверка теста глубины Z-буфера при растеризации Гуро.
void testZBufferDepthOcclusion() {
    constexpr std::size_t width = 120;
    constexpr std::size_t height = 120;
    auto framebuffer = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer, color(0, 0, 0));
    std::vector<double> depth(width * height, std::numeric_limits<double>::infinity());

    // Треугольник 1 на глубине z = 10.0
    const renderer::detail::ProjectedVertex v0{1.0 / 120.0, 79.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v1{-99.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};
    const renderer::detail::ProjectedVertex v2{101.0 / 120.0, -101.0 / 120.0, 1.0, 10.0};

    renderer::rasterizeTriangleGouraud(v0, v1, v2, color(1, 0, 0), color(0, 1, 0), color(0, 0, 1), framebuffer, depth);

    const auto center1 = pixel(framebuffer, 60, 80);
    CHECK(std::abs(center1.red - 85) <= 1);
    CHECK(std::abs(depth[80 * width + 60] - 10.0) < 1e-6);

    // Треугольник 2 на большей глубине z = 15.0 (позади первого) с белым цветом во всех вершинах.
    // Не должен перекрыть первый треугольник.
    const renderer::detail::ProjectedVertex vFar0{1.0 / 120.0, 79.0 / 120.0, 1.0, 15.0};
    const renderer::detail::ProjectedVertex vFar1{-99.0 / 120.0, -101.0 / 120.0, 1.0, 15.0};
    const renderer::detail::ProjectedVertex vFar2{101.0 / 120.0, -101.0 / 120.0, 1.0, 15.0};

    renderer::rasterizeTriangleGouraud(vFar0, vFar1, vFar2, color(1, 1, 1), color(1, 1, 1), color(1, 1, 1), framebuffer,
                                       depth);

    const auto centerAfterFar = pixel(framebuffer, 60, 80);
    CHECK(centerAfterFar == center1); // Не изменился
    CHECK(std::abs(depth[80 * width + 60] - 10.0) < 1e-6);

    // Треугольник 3 на меньшей глубине z = 5.0 (впереди первого) с белым цветом.
    // Должен перекрыть первый треугольник и обновить Z-буфер.
    const renderer::detail::ProjectedVertex vNear0{1.0 / 120.0, 79.0 / 120.0, 1.0, 5.0};
    const renderer::detail::ProjectedVertex vNear1{-99.0 / 120.0, -101.0 / 120.0, 1.0, 5.0};
    const renderer::detail::ProjectedVertex vNear2{101.0 / 120.0, -101.0 / 120.0, 1.0, 5.0};

    renderer::rasterizeTriangleGouraud(vNear0, vNear1, vNear2, color(1, 1, 1), color(1, 1, 1), color(1, 1, 1),
                                       framebuffer, depth);

    const auto centerAfterNear = pixel(framebuffer, 60, 80);
    CHECK(centerAfterNear == WHITE);
    CHECK(std::abs(depth[80 * width + 60] - 5.0) < 1e-6);
}

// Проверка 2D-растеризации Гуро с alpha blending.
void testGouraud2DRasterization() {
    constexpr std::size_t width = 120;
    constexpr std::size_t height = 120;
    auto framebuffer = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer, color(0, 0, 0));

    const std::array<double, 3> screenX{60.5, 10.5, 110.5};
    const std::array<double, 3> screenY{20.5, 110.5, 110.5};

    const auto c0 = color(1, 0, 0);
    const auto c1 = color(0, 1, 0);
    const auto c2 = color(0, 0, 1);

    renderer::rasterizeTriangleGouraud2D(screenX, screenY, c0, c1, c2, framebuffer);

    // Проверяем внутренние пиксели около вершин: нижнее ребро исключено правилом top-left.
    const auto nearV0 = pixel(framebuffer, 60, 21);
    CHECK(nearV0.red >= 245);
    CHECK(nearV0.green <= 5);
    CHECK(nearV0.blue <= 5);

    const auto nearV1 = pixel(framebuffer, 11, 109);
    CHECK(nearV1.green >= 245);
    CHECK(nearV1.red <= 5);
    CHECK(nearV1.blue <= 5);

    const auto nearV2 = pixel(framebuffer, 109, 109);
    CHECK(nearV2.blue >= 245);
    CHECK(nearV2.red <= 5);
    CHECK(nearV2.green <= 5);
    CHECK(pixel(framebuffer, 60, 110) == BLACK);

    // Центр треугольника
    const auto center = pixel(framebuffer, 60, 80);
    CHECK(std::abs(center.red - 85) <= 1);
    CHECK(std::abs(center.green - 85) <= 1);
    CHECK(std::abs(center.blue - 85) <= 1);
    CHECK(center.alpha == 255);

    // Перегрузка с Point2
    auto framebuffer2 = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer2, color(0, 0, 0));
    renderer::rasterizeTriangleGouraud2D(math::Point2{60.5, 20.5}, math::Point2{10.5, 110.5},
                                         math::Point2{110.5, 110.5}, c0, c1, c2, framebuffer2);
    const auto center2 = pixel(framebuffer2, 60, 80);
    CHECK(center2 == center);
}

// Проверка 2D-смешивания (alpha blending) при полупрозрачных цветах вершин.
void testGouraud2DAlphaBlending() {
    constexpr std::size_t width = 120;
    constexpr std::size_t height = 120;
    auto framebuffer = makeFramebuffer(width, height);
    // Заполняем белым фоном
    renderer::detail::clear(framebuffer, color(1, 1, 1, 1));

    const std::array<double, 3> screenX{60.5, 10.5, 110.5};
    const std::array<double, 3> screenY{20.5, 110.5, 110.5};

    // Цвета с alpha = 0.5
    const auto c0 = color(1, 0, 0, 0.5);
    const auto c1 = color(0, 1, 0, 0.5);
    const auto c2 = color(0, 0, 1, 0.5);

    renderer::rasterizeTriangleGouraud2D(screenX, screenY, c0, c1, c2, framebuffer);

    // В центре (60, 80): цвет треугольника = (1/3, 1/3, 1/3, 0.5)
    // Смешивание с белым фоном: 0.5 * (1/3) + 0.5 * 1.0 = 2/3 ≈ 0.6667
    // В байтах: 255 * (2/3) = 170
    const auto center = pixel(framebuffer, 60, 80);
    CHECK(std::abs(center.red - 170) <= 2);
    CHECK(std::abs(center.green - 170) <= 2);
    CHECK(std::abs(center.blue - 170) <= 2);
    CHECK(center.alpha == 255);
}

// Проверка граничных и вырожденных случаев.
void testDegenerateAndEdgeCases() {
    constexpr std::size_t width = 64;
    constexpr std::size_t height = 64;
    auto framebuffer = makeFramebuffer(width, height);
    renderer::detail::clear(framebuffer, color(0, 0, 0));
    std::vector<double> depth(width * height, std::numeric_limits<double>::infinity());

    // 1. Вырожденный треугольник (три точки на одной прямой, площадь = 0)
    const renderer::detail::ProjectedVertex d0{0.0, 0.0, 1.0, 5.0};
    const renderer::detail::ProjectedVertex d1{0.2, 0.2, 1.0, 5.0};
    const renderer::detail::ProjectedVertex d2{0.4, 0.4, 1.0, 5.0};
    renderer::rasterizeTriangleGouraud(d0, d1, d2, color(1, 0, 0), color(0, 1, 0), color(0, 0, 1), framebuffer, depth);
    // Ни один пиксель не должен быть закрашен
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            CHECK(pixel(framebuffer, x, y) == BLACK);
            CHECK(depth[y * width + x] == std::numeric_limits<double>::infinity());
        }
    }

    // 2. Треугольник полностью за пределами экрана
    const renderer::detail::ProjectedVertex off0{5.0, 5.0, 1.0, 5.0};
    const renderer::detail::ProjectedVertex off1{6.0, 5.0, 1.0, 5.0};
    const renderer::detail::ProjectedVertex off2{5.0, 6.0, 1.0, 5.0};
    renderer::rasterizeTriangleGouraud(off0, off1, off2, color(1, 0, 0), color(0, 1, 0), color(0, 0, 1), framebuffer,
                                       depth);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            CHECK(pixel(framebuffer, x, y) == BLACK);
        }
    }

    // 3. Вызов с пустым depth vector не должен приводить к падению
    auto smallFb = makeFramebuffer(1, 1);
    std::vector<double> emptyDepth;
    renderer::rasterizeTriangleGouraud(d0, d1, d2, color(1, 0, 0), color(0, 1, 0), color(0, 0, 1), smallFb, emptyDepth);
}

int main() {
    try {
        testGouraud3DInterpolation();
        testGradientMonotonicity();
        testZBufferDepthOcclusion();
        testGouraud2DRasterization();
        testGouraud2DAlphaBlending();
        testDegenerateAndEdgeCases();
        std::cout << "All Gouraud shading tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
}
