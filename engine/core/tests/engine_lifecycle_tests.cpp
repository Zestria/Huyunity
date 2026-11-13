#include "huynity/adapters/window/headless_window.h"
#include "huynity/engine.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

void testDeterministicStep() {
    constexpr double fixedDt = 0.02; // 50 Hz
    huynity::EngineConfig config{
        .fixedTimeStep = fixedDt,
        .forceHeadless = true,
    };

    // 1. Проверка случая dt < fixedTimeStep: тик не должен срабатывать
    {
        huynity::Engine engine{config};
        std::vector<double> updateDts;
        std::vector<double> frameAlphas;

        engine.onUpdate([&](double dt) { updateDts.push_back(dt); });
        engine.onFrame([&](double alpha) { frameAlphas.push_back(alpha); });

        engine.step(0.01); // 0.01 < 0.02

        CHECK(updateDts.empty());
        CHECK(frameAlphas.size() == 1);
        CHECK(std::abs(frameAlphas[0] - 0.5) < 1e-6); // 0.01 / 0.02 = 0.5
    }

    // 2. Проверка случая dt = fixedTimeStep * 2.5: срабатывает ровно 2 тика, alpha = 0.5
    {
        huynity::Engine engine{config};
        std::vector<double> updateDts;
        std::vector<double> frameAlphas;

        engine.onUpdate([&](double dt) { updateDts.push_back(dt); });
        engine.onFrame([&](double alpha) { frameAlphas.push_back(alpha); });

        engine.step(fixedDt * 2.5); // 0.05

        CHECK(updateDts.size() == 2);
        CHECK(std::abs(updateDts[0] - fixedDt) < 1e-6);
        CHECK(std::abs(updateDts[1] - fixedDt) < 1e-6);
        CHECK(frameAlphas.size() == 1);
        CHECK(std::abs(frameAlphas[0] - 0.5) < 1e-6);
    }
}

void testInputEventDelivery() {
    huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
    auto* headless = dynamic_cast<huynity::adapters::window::HeadlessWindow*>(&engine.window());
    CHECK(headless != nullptr);

    std::vector<huynity::adapters::window::InputEvent> receivedEvents;
    engine.onEvent([&](const huynity::adapters::window::InputEvent& event) { receivedEvents.push_back(event); });

    CHECK(!engine.input().isKeyPressed(huynity::adapters::window::KeyCode::Space));

    // Помещаем события нажатия клавиши и перемещения мыши
    headless->pushEvent(huynity::adapters::window::KeyPressedEvent{.key = huynity::adapters::window::KeyCode::Space});
    headless->pushEvent(
        huynity::adapters::window::MouseMovedEvent{.x = 120.0, .y = 240.0, .deltaX = 12.0, .deltaY = 24.0});

    engine.step(0.016);

    // Проверяем доставку в коллбек onEvent
    CHECK(receivedEvents.size() == 2);

    // Проверяем отражение в синхронном InputState
    CHECK(engine.input().isKeyPressed(huynity::adapters::window::KeyCode::Space));
    CHECK(engine.input().mouseX() == 120.0);
    CHECK(engine.input().mouseY() == 240.0);
    // Дельты мыши должны сброситься в конце step()
    CHECK(engine.input().mouseDeltaX() == 0.0);
    CHECK(engine.input().mouseDeltaY() == 0.0);

    // Помещаем событие отпускания клавиши
    headless->pushEvent(huynity::adapters::window::KeyReleasedEvent{.key = huynity::adapters::window::KeyCode::Space});
    engine.step(0.016);

    CHECK(receivedEvents.size() == 3);
    CHECK(!engine.input().isKeyPressed(huynity::adapters::window::KeyCode::Space));
}

void testWindowResizedEvent() {
    huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
    auto* headless = dynamic_cast<huynity::adapters::window::HeadlessWindow*>(&engine.window());
    CHECK(headless != nullptr);

    CHECK(engine.pipeline().surface().size().width == 800);
    CHECK(engine.pipeline().surface().size().height == 600);

    // Изменение размера через pushEvent в окно
    headless->pushEvent(huynity::adapters::window::WindowResizedEvent{.width = 1280, .height = 720});
    engine.step(0.016);

    CHECK(engine.pipeline().surface().size().width == 1280);
    CHECK(engine.pipeline().surface().size().height == 720);
}

void testWindowCloseRequestedAndStop() {
    // 1. Проверка step(dt) при WindowCloseRequestedEvent
    {
        huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
        auto* headless = dynamic_cast<huynity::adapters::window::HeadlessWindow*>(&engine.window());
        CHECK(headless != nullptr);
        CHECK(engine.window().isOpen());

        headless->pushEvent(huynity::adapters::window::WindowCloseRequestedEvent{});
        engine.step(0.016);

        CHECK(!engine.isRunning());
        CHECK(!engine.window().isOpen());
    }

    // 2. Проверка завершения run() по WindowCloseRequestedEvent
    {
        huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
        auto* headless = dynamic_cast<huynity::adapters::window::HeadlessWindow*>(&engine.window());
        CHECK(headless != nullptr);

        headless->pushEvent(huynity::adapters::window::WindowCloseRequestedEvent{});
        engine.run();

        CHECK(!engine.isRunning());
        CHECK(!engine.window().isOpen());
    }

    // 3. Проверка явного вызова engine.stop() из коллбека onFrame
    {
        huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
        std::size_t frameCount = 0;
        engine.onFrame([&](double) {
            CHECK(engine.isRunning());
            ++frameCount;
            if (frameCount >= 3) {
                engine.stop();
                CHECK(!engine.isRunning());
            }
        });

        CHECK(!engine.isRunning());
        engine.run();
        CHECK(!engine.isRunning());
        CHECK(!engine.window().isOpen());
        CHECK(frameCount == 3);
    }
}

void testPresentationRendering() {
    huynity::Engine engine{huynity::EngineConfig{.forceHeadless = true}};
    auto* headless = dynamic_cast<huynity::adapters::window::HeadlessWindow*>(&engine.window());
    CHECK(headless != nullptr);

    const auto countBefore = headless->presentedCount();
    engine.step(0.016);

    CHECK(headless->presentedCount() == countBefore + 1);
    CHECK(headless->lastPresentedFrame() != nullptr);
    CHECK(headless->lastPresentedFrame()->width() == 800);
    CHECK(headless->lastPresentedFrame()->height() == 600);
}

void testStopPreventsFurtherCallbacks() {
    for (int stopPhase = 0; stopPhase < 3; ++stopPhase) {
        huynity::Engine engine({.windowSize = {16, 16}, .fixedTimeStep = 0.01, .forceHeadless = true});
        auto& headless = dynamic_cast<huynity::adapters::window::HeadlessWindow&>(engine.window());
        int events = 0;
        int updates = 0;
        int frames = 0;
        engine.onEvent([&](const auto&) {
            ++events;
            if (stopPhase == 0) {
                engine.stop();
            }
        });
        engine.onUpdate([&](double) {
            ++updates;
            if (stopPhase == 1) {
                engine.stop();
            }
        });
        engine.onFrame([&](double) { ++frames; });
        if (stopPhase == 0) {
            headless.pushEvent(huynity::adapters::window::KeyPressedEvent{});
            headless.pushEvent(huynity::adapters::window::KeyReleasedEvent{});
        } else if (stopPhase == 2) {
            headless.pushEvent(huynity::adapters::window::WindowCloseRequestedEvent{});
        }
        engine.step(0.05);
        CHECK(events == 1 || stopPhase == 1);
        CHECK(updates == (stopPhase == 1 ? 1 : 0));
        CHECK(frames == 0);
        CHECK(headless.presentedCount() == 0);
        const int previousEvents = events;
        engine.step(0.05);
        CHECK(events == previousEvents);
        CHECK(updates == (stopPhase == 1 ? 1 : 0));
        CHECK(frames == 0);
    }
}

void testRunResetsStateAfterCallbackException() {
    huynity::Engine engine({.windowSize = {16, 16}, .forceHeadless = true});
    engine.onFrame([](double) { throw std::runtime_error("callback failed"); });
    bool hasThrown = false;
    try {
        engine.run();
    } catch (const std::runtime_error&) {
        hasThrown = true;
    }
    CHECK(hasThrown);
    CHECK(!engine.isRunning());
    CHECK(engine.window().isOpen());
    engine.onFrame([&](double) { engine.stop(); });
    engine.run();
    CHECK(!engine.isRunning());
}

} // namespace

int main() {
    try {
        testDeterministicStep();
        testInputEventDelivery();
        testWindowResizedEvent();
        testWindowCloseRequestedAndStop();
        testPresentationRendering();
        testStopPreventsFurtherCallbacks();
        testRunResetsStateAfterCallbackException();
        std::cout << "engine_lifecycle_tests: OK\n";
    } catch (const std::exception& error) {
        std::cerr << "Engine lifecycle test failed: " << error.what() << '\n';
        return 1;
    }
}
