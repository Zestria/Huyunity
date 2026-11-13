#include "huynity/adapters/window/headless_window.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace huynity::adapters::window {

HeadlessWindow::HeadlessWindow(WindowSize size, std::string_view title)
    : m_size{size}, m_title{title}, m_isOpen{true} {}

bool HeadlessWindow::isOpen() const noexcept {
    return m_isOpen;
}

void HeadlessWindow::close() noexcept {
    m_isOpen = false;
}

WindowSize HeadlessWindow::size() const noexcept {
    return m_size;
}

void HeadlessWindow::setTitle(std::string_view title) {
    m_title = title;
}

void HeadlessWindow::resize(WindowSize newSize) {
    m_size = newSize;
    m_eventQueue.push_back(WindowResizedEvent{.width = newSize.width, .height = newSize.height});
}

void HeadlessWindow::resize(std::uint32_t width, std::uint32_t height) {
    resize(WindowSize{.width = width, .height = height});
}

void HeadlessWindow::pushEvent(InputEvent event) {
    m_eventQueue.push_back(std::move(event));
}

void HeadlessWindow::pushEvents(std::span<const InputEvent> events) {
    m_eventQueue.insert(m_eventQueue.end(), events.begin(), events.end());
}

void HeadlessWindow::pollEvents(std::vector<InputEvent>& outEvents) {
    for (const auto& event : m_eventQueue) {
        if (const auto* resized = std::get_if<WindowResizedEvent>(&event)) {
            m_size = {.width = resized->width, .height = resized->height};
        }
    }
    outEvents.insert(outEvents.end(), std::make_move_iterator(m_eventQueue.begin()),
                     std::make_move_iterator(m_eventQueue.end()));
    m_eventQueue.clear();
}

std::unique_ptr<image::ImageBuffer> HeadlessWindow::present(std::unique_ptr<image::ImageBuffer> framebuffer) {
    if (!m_isOpen) {
        ++m_rejectedPresents;
        return framebuffer;
    }
    if (!framebuffer) {
        return nullptr;
    }
    if (framebuffer->width() != m_size.width || framebuffer->height() != m_size.height) {
        ++m_rejectedPresents;
        return framebuffer;
    }

    auto copyResult = image::ImageBuffer::create(image::ImageBufferDesc{
        .width = framebuffer->width(),
        .height = framebuffer->height(),
        .format = framebuffer->format(),
        .stride = framebuffer->stride(),
    });
    if (copyResult.has_value()) {
        std::ranges::copy(framebuffer->bytes(), copyResult->bytes().begin());
        m_lastPresentedFrame = std::make_unique<image::ImageBuffer>(std::move(*copyResult));
    }

    ++m_presentedCount;
    return framebuffer;
}

const std::string& HeadlessWindow::title() const noexcept {
    return m_title;
}

std::size_t HeadlessWindow::presentedCount() const noexcept {
    return m_presentedCount;
}

std::size_t HeadlessWindow::rejectedPresents() const noexcept {
    return m_rejectedPresents;
}

const image::ImageBuffer* HeadlessWindow::lastPresentedFrame() const noexcept {
    return m_lastPresentedFrame.get();
}

const image::ImageBuffer* HeadlessWindow::lastPresented() const noexcept {
    return lastPresentedFrame();
}

std::size_t HeadlessWindow::queuedEventCount() const noexcept {
    return m_eventQueue.size();
}

} // namespace huynity::adapters::window
