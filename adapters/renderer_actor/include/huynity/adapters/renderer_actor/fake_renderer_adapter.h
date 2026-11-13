#pragma once

#include "huynity/runtime/frame_completions.h"

#include <optional>
#include <string>
#include <utility>

namespace huynity::adapters::renderer_actor {

class FakeRendererAdapter final : public runtime::RendererPort {
public:
    FakeRendererAdapter() = default;
    explicit FakeRendererAdapter(runtime::CompletionChannel& completions) : m_completions{&completions} {}

    [[nodiscard]] runtime::SubmitResult trySubmit(runtime::RenderFrame& frame) override {
        if (m_active || m_isDown || m_stopRequested || (m_completions && !m_completions->reserve())) {
            return runtime::SubmitResult::Rejected;
        }
        m_active.emplace(std::move(frame));
        return runtime::SubmitResult::Accepted;
    }

    [[nodiscard]] runtime::RenderFrame complete() {
        auto frame = std::move(*m_active);
        m_active.reset();
        return frame;
    }

    [[nodiscard]] bool completeReady() {
        if (!m_active || !m_completions) {
            return false;
        }
        const auto id = m_active->frameId;
        auto frame = std::move(*m_active);
        if (!m_completions->publish(runtime::FrameReady{id, std::move(frame)})) {
            return false;
        }
        m_active.reset();
        return true;
    }

    [[nodiscard]] bool completeFailed(std::string error) {
        if (!m_active || !m_completions) {
            return false;
        }
        const auto id = m_active->frameId;
        auto frame = std::move(*m_active);
        if (!m_completions->publish(runtime::FrameFailed{id, std::move(frame), std::move(error)})) {
            return false;
        }
        m_active.reset();
        return true;
    }

    void down() {
        m_isDown = true;
        if (m_completions) {
            m_completions->markRendererDown();
        }
    }

    void requestStop() noexcept { m_stopRequested = true; }

    void confirmStopped() noexcept {
        if (m_stopRequested || m_isDown) {
            m_isStopped = true;
        }
    }

    [[nodiscard]] std::optional<runtime::RenderFrame> releaseAfterStop() {
        if (!m_isStopped) {
            return std::nullopt;
        }
        return std::exchange(m_active, std::nullopt);
    }

    [[nodiscard]] bool isBusy() const noexcept { return m_active.has_value(); }
    [[nodiscard]] bool isStopped() const noexcept { return m_isStopped; }
    [[nodiscard]] std::uint64_t activeFrameId() const { return m_active->frameId; }

private:
    std::optional<runtime::RenderFrame> m_active;
    runtime::CompletionChannel* m_completions{};
    bool m_isDown{};
    bool m_stopRequested{};
    bool m_isStopped{};
};

} // namespace huynity::adapters::renderer_actor
