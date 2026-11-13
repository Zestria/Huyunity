#include "huynity/adapters/window/headless_window.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace window = huynity::adapters::window;
namespace image = huynity::image;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

namespace {

// Compile-time checks
static_assert(std::is_base_of_v<window::WindowAdapter, window::HeadlessWindow>,
              "HeadlessWindow must inherit from WindowAdapter");
static_assert(!std::is_abstract_v<window::HeadlessWindow>, "HeadlessWindow must be a concrete class");

std::unique_ptr<image::ImageBuffer> createTestBuffer(std::size_t width, std::size_t height, std::byte fillValue = {}) {
    auto result = image::ImageBuffer::create(image::ImageBufferDesc{
        .width = width,
        .height = height,
    });
    if (!result) {
        throw std::runtime_error("Failed to create ImageBuffer for test");
    }
    if (fillValue != std::byte{}) {
        for (auto& b : result->bytes()) {
            b = fillValue;
        }
    }
    return std::make_unique<image::ImageBuffer>(std::move(*result));
}

void testCreationAndDefaultState() {
    window::HeadlessWindow defaultWindow;
    CHECK(defaultWindow.isOpen());
    CHECK(defaultWindow.size() == window::WindowSize{.width = 800, .height = 600});
    CHECK(defaultWindow.title() == "Huynity Headless");
    CHECK(defaultWindow.presentedCount() == 0);
    CHECK(defaultWindow.rejectedPresents() == 0);
    CHECK(defaultWindow.lastPresentedFrame() == nullptr);
    CHECK(defaultWindow.lastPresented() == nullptr);
    CHECK(defaultWindow.queuedEventCount() == 0);

    const window::HeadlessWindow customWindow(window::WindowSize{.width = 1280, .height = 720}, "Custom App");
    CHECK(customWindow.isOpen());
    CHECK(customWindow.size() == window::WindowSize{.width = 1280, .height = 720});
    CHECK(customWindow.title() == "Custom App");
}

void testTitleModification() {
    window::HeadlessWindow win;
    CHECK(win.title() == "Huynity Headless");

    win.setTitle("Huynity Editor");
    CHECK(win.title() == "Huynity Editor");

    win.setTitle("");
    CHECK(win.title().empty());
}

void testCloseLifecycle() {
    window::HeadlessWindow win;
    CHECK(win.isOpen());

    win.close();
    CHECK(!win.isOpen());

    // Subsequent close() call remains idempotent
    win.close();
    CHECK(!win.isOpen());
}

void testResizeGeneratesEvent() {
    window::HeadlessWindow win(window::WindowSize{.width = 800, .height = 600});
    CHECK(win.size() == window::WindowSize{.width = 800, .height = 600});
    CHECK(win.queuedEventCount() == 0);

    win.resize(window::WindowSize{.width = 1024, .height = 768});
    CHECK(win.size() == window::WindowSize{.width = 1024, .height = 768});
    CHECK(win.queuedEventCount() == 1);

    std::vector<window::InputEvent> events;
    win.pollEvents(events);
    CHECK(events.size() == 1);
    CHECK(win.queuedEventCount() == 0);

    CHECK(std::holds_alternative<window::WindowResizedEvent>(events[0]));
    const auto& resized = std::get<window::WindowResizedEvent>(events[0]);
    CHECK(resized.width == 1024);
    CHECK(resized.height == 768);

    // Test resize overload with width, height
    win.resize(1920, 1080);
    CHECK(win.size() == window::WindowSize{.width = 1920, .height = 1080});
    events.clear();
    win.pollEvents(events);
    CHECK(events.size() == 1);
    CHECK(std::get<window::WindowResizedEvent>(events[0]).width == 1920);
    CHECK(std::get<window::WindowResizedEvent>(events[0]).height == 1080);
}

void testPushEventAndPollEventsOrder() {
    window::HeadlessWindow win;

    win.pushEvent(window::KeyPressedEvent{.key = window::KeyCode::W, .isRepeat = false});
    win.pushEvent(window::MouseMovedEvent{.x = 100.0, .y = 200.0, .deltaX = 5.0, .deltaY = -2.0});
    win.pushEvent(window::MouseButtonPressedEvent{.button = window::MouseButton::Left, .x = 100.0, .y = 200.0});
    win.pushEvent(window::KeyReleasedEvent{.key = window::KeyCode::W});

    CHECK(win.queuedEventCount() == 4);

    std::vector<window::InputEvent> polled;
    win.pollEvents(polled);

    CHECK(polled.size() == 4);
    CHECK(win.queuedEventCount() == 0);

    CHECK(std::holds_alternative<window::KeyPressedEvent>(polled[0]));
    CHECK(std::get<window::KeyPressedEvent>(polled[0]).key == window::KeyCode::W);

    CHECK(std::holds_alternative<window::MouseMovedEvent>(polled[1]));
    CHECK(std::get<window::MouseMovedEvent>(polled[1]).deltaX == 5.0);

    CHECK(std::holds_alternative<window::MouseButtonPressedEvent>(polled[2]));
    CHECK(std::get<window::MouseButtonPressedEvent>(polled[2]).button == window::MouseButton::Left);

    CHECK(std::holds_alternative<window::KeyReleasedEvent>(polled[3]));
    CHECK(std::get<window::KeyReleasedEvent>(polled[3]).key == window::KeyCode::W);

    // Repeated poll should produce 0 new events
    std::vector<window::InputEvent> emptyBatch;
    win.pollEvents(emptyBatch);
    CHECK(emptyBatch.empty());
}

void testPushEventsSpan() {
    window::HeadlessWindow win;

    const std::array<window::InputEvent, 3> batch{
        window::KeyPressedEvent{.key = window::KeyCode::A},
        window::KeyPressedEvent{.key = window::KeyCode::B},
        window::WindowCloseRequestedEvent{},
    };

    win.pushEvents(batch);
    CHECK(win.queuedEventCount() == 3);

    std::vector<window::InputEvent> polled;
    win.pollEvents(polled);
    CHECK(polled.size() == 3);
    CHECK(win.queuedEventCount() == 0);
    CHECK(std::holds_alternative<window::WindowCloseRequestedEvent>(polled[2]));
}

void testPresentMatchingDimensionsSuccess() {
    window::HeadlessWindow win(window::WindowSize{.width = 800, .height = 600});

    auto buffer = createTestBuffer(800, 600, std::byte{0x7F});
    auto* originalPtr = buffer.get();

    auto returned = win.present(std::move(buffer));

    // Ownership returned synchronously
    CHECK(returned != nullptr);
    CHECK(returned.get() == originalPtr);
    CHECK(win.presentedCount() == 1);
    CHECK(win.rejectedPresents() == 0);

    // Saved frame inspection
    const auto* presented = win.lastPresentedFrame();
    CHECK(presented != nullptr);
    CHECK(win.lastPresented() == presented);
    CHECK(presented->width() == 800);
    CHECK(presented->height() == 600);
    CHECK(presented->bytes().size() == 800 * 600 * 4);
    CHECK(presented->bytes()[0] == std::byte{0x7F});

    // Second present with another pattern
    auto buffer2 = createTestBuffer(800, 600, std::byte{0xAA});
    auto returned2 = win.present(std::move(buffer2));
    CHECK(returned2 != nullptr);
    CHECK(win.presentedCount() == 2);
    CHECK(win.rejectedPresents() == 0);
    CHECK(win.lastPresentedFrame()->bytes()[0] == std::byte{0xAA});
}

void testPresentMismatchedDimensionsRejected() {
    window::HeadlessWindow win(window::WindowSize{.width = 800, .height = 600});

    // Mismatched width and height
    auto buffer1 = createTestBuffer(640, 480);
    auto* raw1 = buffer1.get();
    auto ret1 = win.present(std::move(buffer1));
    CHECK(ret1.get() == raw1);
    CHECK(win.rejectedPresents() == 1);
    CHECK(win.presentedCount() == 0);
    CHECK(win.lastPresentedFrame() == nullptr);

    // Mismatched width only
    auto buffer2 = createTestBuffer(1024, 600);
    auto* raw2 = buffer2.get();
    auto ret2 = win.present(std::move(buffer2));
    CHECK(ret2.get() == raw2);
    CHECK(win.rejectedPresents() == 2);
    CHECK(win.presentedCount() == 0);

    // Mismatched height only
    auto buffer3 = createTestBuffer(800, 700);
    auto* raw3 = buffer3.get();
    auto ret3 = win.present(std::move(buffer3));
    CHECK(ret3.get() == raw3);
    CHECK(win.rejectedPresents() == 3);
    CHECK(win.presentedCount() == 0);
    CHECK(win.lastPresentedFrame() == nullptr);
}

void testPresentAfterCloseRejected() {
    window::HeadlessWindow win(window::WindowSize{.width = 800, .height = 600});

    // First present while open succeeds
    auto buffer1 = createTestBuffer(800, 600, std::byte{0x11});
    auto ret1 = win.present(std::move(buffer1));
    CHECK(ret1 != nullptr);
    CHECK(win.presentedCount() == 1);
    CHECK(win.rejectedPresents() == 0);
    CHECK(win.lastPresentedFrame() != nullptr);

    win.close();
    CHECK(!win.isOpen());

    // Subsequent present while closed is rejected
    auto buffer2 = createTestBuffer(800, 600, std::byte{0x22});
    auto* raw2 = buffer2.get();
    auto ret2 = win.present(std::move(buffer2));
    CHECK(ret2.get() == raw2);
    CHECK(win.presentedCount() == 1);
    CHECK(win.rejectedPresents() == 1);
    // Last presented frame still retains the previous successful frame
    CHECK(win.lastPresentedFrame()->bytes()[0] == std::byte{0x11});
}

void testPresentNullBuffer() {
    window::HeadlessWindow win;
    auto ret = win.present(nullptr);
    CHECK(ret == nullptr);
}

void testResizeThenPresent() {
    window::HeadlessWindow win(window::WindowSize{.width = 800, .height = 600});

    win.resize(window::WindowSize{.width = 1280, .height = 720});

    // Old size buffer should be rejected
    auto oldBuffer = createTestBuffer(800, 600);
    auto retOld = win.present(std::move(oldBuffer));
    CHECK(retOld != nullptr);
    CHECK(win.rejectedPresents() == 1);
    CHECK(win.presentedCount() == 0);

    // New size buffer should be accepted
    auto newBuffer = createTestBuffer(1280, 720, std::byte{0x33});
    auto retNew = win.present(std::move(newBuffer));
    CHECK(retNew != nullptr);
    CHECK(win.rejectedPresents() == 1);
    CHECK(win.presentedCount() == 1);
    CHECK(win.lastPresentedFrame()->width() == 1280);
    CHECK(win.lastPresentedFrame()->height() == 720);
    CHECK(win.lastPresentedFrame()->bytes()[0] == std::byte{0x33});
}

void testPolymorphicWindowAdapterUsage() {
    auto win = std::make_unique<window::HeadlessWindow>(window::WindowSize{.width = 400, .height = 300}, "Poly Window");
    window::WindowAdapter* adapter = win.get();

    CHECK(adapter->isOpen());
    CHECK(adapter->size() == window::WindowSize{.width = 400, .height = 300});

    adapter->setTitle("New Poly Window");
    CHECK(win->title() == "New Poly Window");

    auto buffer = createTestBuffer(400, 300);
    auto returned = adapter->present(std::move(buffer));
    CHECK(returned != nullptr);
    CHECK(win->presentedCount() == 1);

    adapter->close();
    CHECK(!adapter->isOpen());
}

void testInjectedResizeUpdatesSizeBeforePresentation() {
    window::HeadlessWindow win;
    win.pushEvent(window::WindowResizedEvent{.width = 320, .height = 200});
    const std::array<window::InputEvent, 2> batch{
        window::MouseMovedEvent{.x = 12, .y = 34},
        window::WindowResizedEvent{.width = 160, .height = 100},
    };
    win.pushEvents(batch);
    std::vector<window::InputEvent> events;
    win.pollEvents(events);
    CHECK(events.size() == 3);
    CHECK(win.size() == window::WindowSize{160, 100});
    auto returned = win.present(createTestBuffer(160, 100));
    CHECK(returned != nullptr);
    CHECK(win.presentedCount() == 1);
    CHECK(win.rejectedPresents() == 0);

    win.pushEvent(window::WindowResizedEvent{});
    events.clear();
    win.pollEvents(events);
    CHECK(win.size() == window::WindowSize{});
}

} // namespace

int main() {
    try {
        testCreationAndDefaultState();
        testTitleModification();
        testCloseLifecycle();
        testResizeGeneratesEvent();
        testPushEventAndPollEventsOrder();
        testPushEventsSpan();
        testPresentMatchingDimensionsSuccess();
        testPresentMismatchedDimensionsRejected();
        testPresentAfterCloseRejected();
        testPresentNullBuffer();
        testResizeThenPresent();
        testPolymorphicWindowAdapterUsage();
        testInjectedResizeUpdatesSizeBeforePresentation();

        std::cout << "headless_window_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
