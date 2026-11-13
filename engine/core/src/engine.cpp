#include "huynity/engine.h"

#include "huynity/adapters/window/headless_window.h"
#include "huynity/adapters/window/window_factory.h"
#include "huynity/core/render_preparation.h"
#include "huynity/math/transform.h"
#include "huynity/objectlib/operations/bounds.h"
#include "huynity/objectlib/params/point_params.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

bool is2DGeometry(const huynity::objectlib::Geometry& geometry) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            return std::disjunction_v<
                std::is_same<T, huynity::objectlib::Point2>, std::is_same<T, huynity::objectlib::Segment2>,
                std::is_same<T, huynity::objectlib::Line2>, std::is_same<T, huynity::objectlib::Ray2>,
                std::is_same<T, huynity::objectlib::Triangle2>, std::is_same<T, huynity::objectlib::Polyline2>,
                std::is_same<T, huynity::objectlib::Aabb2>, std::is_same<T, huynity::objectlib::TriangleMesh2>,
                std::is_same<T, huynity::objectlib::Polygon2>, std::is_same<T, huynity::objectlib::Rectangle2>,
                std::is_same<T, huynity::objectlib::Circle2>, std::is_same<T, huynity::objectlib::Disk2>>;
        },
        geometry);
}

} // namespace

namespace huynity {

Engine::Engine(const EngineConfig& config) : m_config{config}, m_fixedUpdate{m_config.fixedTimeStep, 5} {
    adapters::window::WindowConfig winConfig{
        .size = m_config.windowSize,
        .title = m_config.windowTitle,
        .forceHeadless = m_config.forceHeadless,
    };
    m_window = adapters::window::createWindow(winConfig);
    if (!m_window) {
        throw std::runtime_error("Failed to create window adapter");
    }
    const auto sz = m_window->size();
    m_pipeline.resize(runtime::SurfaceSize{.width = sz.width, .height = sz.height});
}

Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

void Engine::setCamera(render_contracts::Camera3 camera) {
    m_camera3 = std::move(camera);
}

void Engine::setCamera(render_contracts::Camera2 camera) {
    m_camera2 = std::move(camera);
}

void Engine::clearCamera3() noexcept {
    m_camera3.reset();
}

void Engine::clearCamera2() noexcept {
    m_camera2.reset();
}

void Engine::setLighting(render_contracts::Lighting3 lighting) {
    m_lighting = std::move(lighting);
}

void Engine::setWorld2DFirst(bool world2DFirst) noexcept {
    m_world2DFirst = world2DFirst;
}

bool Engine::isWorld2DFirst() const noexcept {
    return m_world2DFirst;
}

void Engine::setHudEnabled(bool enabled) noexcept {
    m_hudEnabled = enabled;
}

bool Engine::isHudEnabled() const noexcept {
    return m_hudEnabled;
}

void Engine::setClearColor(render_contracts::Color color) noexcept {
    m_clearColor = color;
}

render_contracts::Color Engine::clearColor() const noexcept {
    return m_clearColor;
}

core::ObjectId Engine::create(const objectlib::ObjectSpec& spec, core::Mobility mobility) {
    auto result = m_scene.createObject(spec);
    if (!result) {
        throw std::runtime_error("Failed to create object from ObjectSpec in scene");
    }
    const auto id = *result;
    (void)m_scene.setMobility(id, mobility);
    return id;
}

void Engine::onUpdate(UpdateCallback callback) {
    m_onUpdate = std::move(callback);
}

void Engine::onFrame(FrameCallback callback) {
    m_onFrame = std::move(callback);
}

void Engine::onEvent(EventCallback callback) {
    m_onEvent = std::move(callback);
}

void Engine::step(double dt) {
    if (!m_window->isOpen()) {
        return;
    }
    std::vector<adapters::window::InputEvent> events;
    m_window->pollEvents(events);

    for (const auto& event : events) {
        if (m_onEvent) {
            m_onEvent(event);
        }
        m_inputState.processEvent(event);
        std::visit(
            [this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, adapters::window::WindowCloseRequestedEvent>) {
                    stop();
                } else if constexpr (std::is_same_v<T, adapters::window::WindowResizedEvent>) {
                    m_pipeline.resize(runtime::SurfaceSize{.width = e.width, .height = e.height});
                    if (e.height > 0) {
                        const double aspect = static_cast<double>(e.width) / static_cast<double>(e.height);
                        if (m_camera3.has_value()) {
                            if (auto* ortho = std::get_if<render_contracts::OrthographicCamera3>(&*m_camera3)) {
                                ortho->width = ortho->height * aspect;
                            }
                        }
                        if (m_camera2.has_value()) {
                            m_camera2->width = m_camera2->height * aspect;
                        }
                    }
                }
            },
            event);
        if (!m_window->isOpen()) {
            m_inputState.resetDeltas();
            return;
        }
    }

    m_fixedUpdate.advance(dt, [this](double fixedDt) {
        if (m_window->isOpen() && m_onUpdate) {
            m_onUpdate(fixedDt);
        }
    });

    if (!m_window->isOpen()) {
        m_inputState.resetDeltas();
        return;
    }

    if (m_onFrame) {
        const double alpha =
            m_fixedUpdate.fixedStep() > 0.0 ? (m_fixedUpdate.remainder() / m_fixedUpdate.fixedStep()) : 0.0;
        m_onFrame(alpha);
    }

    if (m_window->isOpen()) {
        const auto winSize = m_window->size();
        if (winSize.width > 0 && winSize.height > 0) {
            core::PassesConfig passesConfig{
                .camera3 = m_camera3,
                .camera2 = m_camera2,
                .hudEnabled = m_hudEnabled,
                .world2DFirst = m_world2DFirst,
                .lighting = m_lighting,
                .tessellation = {},
            };
            auto snapshotResult =
                core::RenderPreparation::prepareSnapshot(m_scene, passesConfig, winSize.width, winSize.height);
            if (snapshotResult.has_value()) {
                snapshotResult->clearColor = m_clearColor;
                auto framebuffer = m_pipeline.acquireFramebuffer();
                if (framebuffer) {
                    if (m_renderer.render(*snapshotResult, *framebuffer).has_value()) {
                        framebuffer = m_window->present(std::move(framebuffer));
                    }
                    m_pipeline.releaseFromPresentation(std::move(framebuffer));
                }
            }
        }
    }

    m_inputState.resetDeltas();
}

void Engine::run() {
    m_isRunning = true;
    auto previousTime = std::chrono::steady_clock::now();
    constexpr double maxDt = 0.1;
    const std::chrono::duration<double> targetFrameTime{1.0 / 60.0};
    try {
        while (m_isRunning && m_window->isOpen()) {
            const auto frameStart = std::chrono::steady_clock::now();
            const std::chrono::duration<double> elapsed = frameStart - previousTime;
            previousTime = frameStart;
            const double dt = std::clamp(elapsed.count(), 0.0, maxDt);
            step(dt);

            const auto frameEnd = std::chrono::steady_clock::now();
            const auto frameDuration = frameEnd - frameStart;
            if (frameDuration < targetFrameTime) {
                std::this_thread::sleep_for(targetFrameTime - frameDuration);
            }
        }
    } catch (...) {
        m_isRunning = false;
        throw;
    }
    m_isRunning = false;
}

void Engine::stop() noexcept {
    m_isRunning = false;
    if (m_window) {
        m_window->close();
    }
}

bool Engine::isRunning() const noexcept {
    return m_isRunning;
}

void Engine::show(core::ObjectId id) {
    const auto* instance = m_scene.find(id);
    if (instance == nullptr) {
        throw std::runtime_error("Object not found in scene");
    }

    (void)m_scene.refreshSpatialState();
    instance = m_scene.find(id);
    if (instance == nullptr) {
        throw std::runtime_error("Object not found in scene");
    }

    std::optional<objectlib::BoundsGeometry> bounds;
    if (instance->worldBounds.has_value()) {
        bounds = instance->worldBounds;
    } else {
        const auto& geom = instance->finiteRepresentation ? instance->finiteRepresentation : instance->geometry;
        if (geom) {
            auto calc = objectlib::bounds(*geom);
            if (calc.has_value()) {
                bounds = *calc;
            }
        }
    }

    if (!bounds.has_value()) {
        throw std::runtime_error("Unable to compute bounds for object");
    }

    if (instance->material == nullptr) {
        (void)m_scene.setMaterial(
            id, std::make_shared<const render_contracts::Material>(render_contracts::Color{0.2, 0.6, 0.9, 1.0}));
    }

    const auto winSize = m_window ? m_window->size() : m_config.windowSize;
    const double winW = static_cast<double>(winSize.width);
    const double winH = static_cast<double>(winSize.height);
    const double aspect = (winW > 0.0 && winH > 0.0) ? (winW / winH) : (4.0 / 3.0);

    if (std::holds_alternative<objectlib::Aabb3>(*bounds)) {
        const auto& aabb = std::get<objectlib::Aabb3>(*bounds);
        const auto minPt = aabb.min();
        const auto maxPt = aabb.max();
        const double centerX = (minPt[0] + maxPt[0]) * 0.5;
        const double centerY = (minPt[1] + maxPt[1]) * 0.5;
        const double centerZ = (minPt[2] + maxPt[2]) * 0.5;
        const double sizeX = maxPt[0] - minPt[0];
        const double sizeY = maxPt[1] - minPt[1];
        const double sizeZ = maxPt[2] - minPt[2];
        double dMax = std::max({sizeX, sizeY, sizeZ});
        if (dMax <= 0.0) {
            dMax = 1.0;
        }

        constexpr double margin = 1.5;
        const double extentX = sizeX > 0.0 ? sizeX : dMax;
        const double extentY = sizeY > 0.0 ? sizeY : dMax;
        double camW = extentX * margin;
        double camH = extentY * margin;
        if (aspect >= 1.0) {
            if (camW < camH * aspect) {
                camW = camH * aspect;
            } else {
                camH = camW / aspect;
            }
        } else {
            if (camH < camW / aspect) {
                camH = camW / aspect;
            } else {
                camW = camH * aspect;
            }
        }

        const double targetDepth = std::max(20.0, dMax * 5.0);
        const double nearPlane = 0.1;
        const double farPlane = targetDepth + std::max(50.0, dMax * 10.0);

        render_contracts::OrthographicCamera3 cam3{
            .worldToView = math::translation(math::Vector3{{-centerX, -centerY, targetDepth - centerZ}}),
            .width = camW,
            .height = camH,
            .nearPlane = nearPlane,
            .farPlane = farPlane,
        };
        setCamera(std::move(cam3));
        clearCamera2();

        render_contracts::Lighting3 defaultLighting{
            .ambient =
                {
                    .color = render_contracts::Color{1.0, 1.0, 1.0, 1.0},
                    .intensity = 0.3,
                },
            .directional =
                {
                    {
                        .direction = math::Vector3{{1.0, 1.0, 1.0}},
                        .color = render_contracts::Color{1.0, 1.0, 1.0, 1.0},
                        .intensity = 0.7,
                    },
                },
        };
        setLighting(std::move(defaultLighting));
    } else {
        const auto& aabb = std::get<objectlib::Aabb2>(*bounds);
        const auto minPt = aabb.min();
        const auto maxPt = aabb.max();
        const double centerX = (minPt[0] + maxPt[0]) * 0.5;
        const double centerY = (minPt[1] + maxPt[1]) * 0.5;
        const double sizeX = maxPt[0] - minPt[0];
        const double sizeY = maxPt[1] - minPt[1];
        const double extentX = sizeX > 0.0 ? sizeX : 1.0;
        const double extentY = sizeY > 0.0 ? sizeY : 1.0;

        constexpr double margin = 1.25;
        double camW = extentX * margin;
        double camH = extentY * margin;
        if (aspect >= 1.0) {
            if (camW < camH * aspect) {
                camW = camH * aspect;
            } else {
                camH = camW / aspect;
            }
        } else {
            if (camH < camW / aspect) {
                camH = camW / aspect;
            } else {
                camW = camH * aspect;
            }
        }

        render_contracts::Camera2 cam2{
            .worldToView = math::translation(math::Vector2{{-centerX, -centerY}}),
            .width = camW,
            .height = camH,
        };
        setCamera(std::move(cam2));
        clearCamera3();
    }

    step(0.0);
}

void Engine::preview(const objectlib::Geometry& geometry, const EngineConfig& config) {
    Engine engine(config);

    const bool is2D = is2DGeometry(geometry);
    core::ObjectId id{};
    if (is2D) {
        auto res = engine.scene().createObject<objectlib::ObjectType::Point2>(objectlib::Point2Params{});
        if (!res) {
            throw std::runtime_error("Failed to create preview object in scene");
        }
        id = *res;
    } else {
        auto res = engine.scene().createObject<objectlib::ObjectType::Point3>(objectlib::Point3Params{});
        if (!res) {
            throw std::runtime_error("Failed to create preview object in scene");
        }
        id = *res;
    }

    auto setRes = engine.scene().setGeometry(id, geometry);
    if (!setRes) {
        throw std::runtime_error("Failed to set geometry for preview object");
    }

    engine.show(id);

    auto* headless = dynamic_cast<adapters::window::HeadlessWindow*>(&engine.window());
    if (!engine.config().forceHeadless && headless == nullptr && adapters::window::hasNativeWindowSupport()) {
        engine.run();
    }
}

} // namespace huynity
