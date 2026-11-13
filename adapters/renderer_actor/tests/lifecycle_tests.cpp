#include "huynity/adapters/renderer_actor/fake_renderer_adapter.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace runtime = huynity::runtime;
namespace adapter = huynity::adapters::renderer_actor;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__);    \
        }                                                                                                              \
    } while (false)

runtime::RenderFrame frame(std::uint64_t id, const std::shared_ptr<const huynity::render_contracts::Material>& marker) {
    auto snapshot = std::make_unique<huynity::render_contracts::RenderSnapshot>();
    snapshot->materials.push_back(marker);
    return {id, std::move(snapshot), nullptr};
}

void downWithoutFrameFailedStopsWaitingAndRetainsResources() {
    runtime::CompletionChannel channel;
    adapter::FakeRendererAdapter adapter{channel};
    runtime::FrameTracker tracker;
    auto marker = std::make_shared<const huynity::render_contracts::Material>();
    std::weak_ptr<const huynity::render_contracts::Material> weak = marker;
    auto submitted = frame(1, marker);
    CHECK(tracker.submit(adapter, submitted) == runtime::SubmitResult::Accepted);

    adapter.down();
    CHECK(tracker.rendererDown(channel));
    CHECK(tracker.isRendererDown());
    CHECK(!tracker.hasActiveFrame());
    CHECK(adapter.isBusy());
    CHECK(!weak.expired());
    CHECK(adapter.releaseAfterStop() == std::nullopt);
    CHECK(!weak.expired());

    auto rejected = frame(2, marker);
    CHECK(tracker.submit(adapter, rejected) == runtime::SubmitResult::Rejected);
    CHECK(rejected.snapshot != nullptr);
    adapter.confirmStopped();
    // Остановка после RendererDown не ожидает Stopped от умершего исполнителя:
    // подтверждение прекращения исполнения приходит через адаптер.
    tracker.confirmStopped();
    CHECK(tracker.isStopped());
    auto released = adapter.releaseAfterStop();
    CHECK(released.has_value());
    CHECK(released->frameId == 1);
    CHECK(released->snapshot != nullptr);
    CHECK(!adapter.isBusy());
    CHECK(!adapter.releaseAfterStop().has_value());
    released.reset();
    rejected = {};
    marker.reset();
    CHECK(weak.expired());
}

void timeoutAloneDoesNotReleaseFrameResources() {
    runtime::CompletionChannel channel;
    adapter::FakeRendererAdapter adapter{channel};
    runtime::FrameTracker tracker;
    auto marker = std::make_shared<const huynity::render_contracts::Material>();
    std::weak_ptr<const huynity::render_contracts::Material> weak = marker;
    auto submitted = frame(7, marker);
    const auto submittedAt = runtime::FrameTracker::Clock::now();
    const auto limit = std::chrono::milliseconds{50};
    CHECK(tracker.submit(adapter, submitted, submittedAt) == runtime::SubmitResult::Accepted);
    CHECK(!tracker.isFrameOverdue(limit, submittedAt + limit / 2));

    // Таймаут — только диагностика: задание остаётся активным, ресурсы у исполнителя.
    CHECK(tracker.isFrameOverdue(limit, submittedAt + limit * 2));
    CHECK(tracker.hasActiveFrame());
    CHECK(adapter.isBusy());
    CHECK(!weak.expired());

    // Позднее штатное завершение по-прежнему коррелируется и принимается.
    CHECK(adapter.completeReady());
    CHECK(tracker.complete(channel) == runtime::CompletionResult::Ready);
    CHECK(!tracker.isFrameOverdue(limit, submittedAt + limit * 4));
    marker.reset();
}

void stopProducesStoppedBeforeResourceRelease() {
    runtime::CompletionChannel channel;
    adapter::FakeRendererAdapter adapter{channel};
    runtime::FrameTracker tracker;
    auto marker = std::make_shared<const huynity::render_contracts::Material>();
    auto submitted = frame(3, marker);
    CHECK(tracker.submit(adapter, submitted) == runtime::SubmitResult::Accepted);
    CHECK(adapter.completeReady());
    CHECK(tracker.complete(channel) == runtime::CompletionResult::Ready);
    tracker.requestStop();
    adapter.requestStop();
    adapter.confirmStopped();
    CHECK(adapter.isStopped());
    tracker.confirmStopped();
    CHECK(tracker.isStopped());
    auto next = frame(4, marker);
    CHECK(tracker.submit(adapter, next) == runtime::SubmitResult::Rejected);
}

void closeWithActiveFrameDoesNotDiscardIt() {
    runtime::CompletionChannel channel;
    adapter::FakeRendererAdapter adapter{channel};
    runtime::FrameTracker tracker;
    auto marker = std::make_shared<const huynity::render_contracts::Material>();
    std::weak_ptr<const huynity::render_contracts::Material> weak = marker;
    auto submitted = frame(5, marker);
    CHECK(tracker.submit(adapter, submitted) == runtime::SubmitResult::Accepted);
    tracker.requestStop();
    adapter.requestStop();
    CHECK(adapter.isBusy());
    CHECK(!weak.expired());
    auto next = frame(6, marker);
    CHECK(tracker.submit(adapter, next) == runtime::SubmitResult::Rejected);
    CHECK(adapter.completeFailed("closed"));
    CHECK(tracker.complete(channel) == runtime::CompletionResult::Failed);
    adapter.confirmStopped();
    CHECK(adapter.releaseAfterStop() == std::nullopt);
}

int main() {
    try {
        downWithoutFrameFailedStopsWaitingAndRetainsResources();
        timeoutAloneDoesNotReleaseFrameResources();
        stopProducesStoppedBeforeResourceRelease();
        closeWithActiveFrameDoesNotDiscardIt();
        std::cout << "lifecycle: OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
