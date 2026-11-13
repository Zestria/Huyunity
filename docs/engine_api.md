# Высокоуровневый Engine API и создание игр

Высокоуровневый фасад `huynity::Engine` (заголовочный файл `<huynity/engine.h>`) предоставляет единую точку входа для создания 2D и 3D игр, интерактивных графических приложений и быстрого предварительного просмотра геометрических моделей.

Компонент объединяет подсистемы движка в связную и простую в использовании модель:
- **Управление сценой:** `huynity::core::Scene` (сущности, иерархия, компоненты, пространственное состояние);
- **Оконная подсистема и ввод:** `huynity::adapters::window::WindowAdapter` и `InputState`;
- **Фиксированная симуляция:** `huynity::runtime::FixedStepUpdate` (детерминированный тик логики);
- **Конвейер кадров:** `huynity::runtime::FramePipeline` (двойная буферизация кадровых буферов);
- **Программный рендерер:** `huynity::renderer::SoftwareRenderer` (3D, 2D, HUD, освещение, растеризация).

```text
┌────────────────────────────────────────────────────────────────────────┐
│                          huynity::Engine                               │
│                                                                        │
│  ┌──────────────┐  ┌──────────────────┐  ┌──────────────────────────┐  │
│  │ core::Scene  │  │   InputState     │  │  FixedStepUpdate (60 Hz) │  │
│  └──────┬───────┘  └────────┬─────────┘  └────────────┬─────────────┘  │
│         │                   │                         │                │
│  ┌──────▼───────────────────▼─────────────────────────▼─────────────┐  │
│  │                Игровой цикл: step(dt) / run()                    │  │
│  └──────────────────────────┬───────────────────────────────────────┘  │
│                             │ RenderPreparation                        │
│  ┌──────────────────────────▼─────────┐  ┌──────────────────────────┐  │
│  │     FramePipeline (1+1 буфер)      │──▶   SoftwareRenderer       │  │
│  └──────────────────────────┬─────────┘  └──────────────────────────┘  │
│                             │ present()                                │
│  ┌──────────────────────────▼───────────────────────────────────────┐  │
│  │             adapters::window::WindowAdapter                      │  │
│  │             (Native Window / Headless Window)                    │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Конфигурация: `EngineConfig`

Конфигурация передаётся в конструктор `Engine` и задаёт параметры окна, симуляции и окружения:

```cpp
struct EngineConfig {
    adapters::window::WindowSize windowSize{800, 600}; ///< Размеры клиентской области окна (ширина x высота).
    std::string windowTitle{"Huynity Game"};           ///< Текст заголовка системного окна.
    double fixedTimeStep{1.0 / 60.0};                  ///< Фиксированный шаг времени симуляции (по умолчанию 60 Гц).
    bool forceHeadless{false};                         ///< Принудительный режим без экрана (для CI и автотестов).
};
```

### Пример инициализации

```cpp
#include "huynity/engine.h"

int main() {
    huynity::Engine engine({
        .windowSize = {1280, 720},
        .windowTitle = "My Super Game",
        .fixedTimeStep = 1.0 / 60.0,
        .forceHeadless = false,
    });

    // ... настройка сцены и запуск
}
```

---

## Создание объектов в сцене

Фасад предоставляет синхронные фабричные методы создания сущностей в активной сцене движка:

### 1. Строго типизированный метод `engine.create<ObjectType::...>()`

Позволяет создавать параметрические и составные фигуры Object Library в одну строку с проверкой типов на этапе компиляции:

```cpp
template <objectlib::ObjectType kind>
    requires objectlib::SupportedObject<kind>
core::ObjectId create(const objectlib::TypedParams<kind>& params,
                      core::Mobility mobility = core::Mobility::Static);
```

- **Параметры:**
  - `kind` — перечисление `objectlib::ObjectType` (например, `Box3`, `Sphere3`, `TriangleMesh3`, `Rectangle2`, `Circle2` и др.);
  - `params` — агрегатная структура параметров фигуры (`objectlib::TypedParams<kind>`);
  - `mobility` — класс подвижности сущности (`Static`, `Stationary`, `Dynamic`).
- **Возвращаемое значение:** уникальный идентификатор сущности `core::ObjectId`.
- **Исключения:** выбрасывает `std::runtime_error`, если параметры не прошли валидацию инвариантов геометрии.

```cpp
using namespace huynity;

// Создание динамического 3D-куба
const core::ObjectId cubeId = engine.create<objectlib::ObjectType::Box3>(
    {.size = {2.0, 2.0, 2.0}},
    core::Mobility::Dynamic
);

// Создание статической 3D-сферы
const core::ObjectId sphereId = engine.create<objectlib::ObjectType::Sphere3>(
    {.radius = 1.5},
    core::Mobility::Static
);

// Создание 2D-прямоугольника
const core::ObjectId rectId = engine.create<objectlib::ObjectType::Rectangle2>(
    {.width = 100.0, .height = 50.0},
    core::Mobility::Dynamic
);
```

### 2. Создание по спецификации `engine.create(ObjectSpec)`

Позволяет создавать объекты по динамической спецификации `objectlib::ObjectSpec` (например, при загрузке уровней из файлов конфигурации или скриптов):

```cpp
core::ObjectId create(const objectlib::ObjectSpec& spec,
                      core::Mobility mobility = core::Mobility::Static);
```

```cpp
objectlib::ObjectSpec spec{
    .type = objectlib::ObjectType::Box3,
    .params = objectlib::Box3Params{.size = {1.0, 1.0, 1.0}},
};
const auto objId = engine.create(spec, core::Mobility::Dynamic);
```

---

## Игровой жизненный цикл и симуляция

Жизненный цикл движка построен на разделении фиксированного тика логики/физики и переменного шага отрисовки кадров с интерполяцией.

### Обратные вызовы (коллбеки)

1. **`onUpdate(UpdateCallback callback)` — шаг фиксированной симуляции:**
   - Вызывается с детерминированным шагом времени `fixedDt` (равным `config.fixedTimeStep`).
   - Если между кадрами прошло больше времени, чем один тик, `onUpdate` вызывается последовательно несколько раз (до 5 тиков за кадр для защиты от лавинообразного падения производительности).
   - Используется для физики, игровой логики, перемещения сущностей и проверки правил игры.

2. **`onFrame(FrameCallback callback)` — подготовка кадра:**
   - Вызывается перед каждым рендерингом кадра.
   - Принимает параметр `alpha` (в диапазоне `[0.0, 1.0)`) — долю времени, прошедшую с момента последнего тика:
     $$\alpha = \frac{\text{remainder}}{\text{fixedTimeStep}}$$
   - Используется для плавной визуальной интерполяции положения камеры и объектов между фиксированными тиками.

3. **`onEvent(EventCallback callback)` — системные события:**
   - Вызывается для каждого входящего системного события `adapters::window::InputEvent` (нажатия клавиш, движение мыши, ресайз окна, запрос на закрытие).
   - События закрытия окна (`WindowCloseRequestedEvent`) и изменения размера (`WindowResizedEvent`) обрабатываются фасадом автоматически.

### Синхронный опрос клавиатуры и мыши: `engine.input()`

Для быстрого доступа к состоянию периферии внутри `onUpdate` фасад предоставляет константную ссылку на `adapters::window::InputState`:

```cpp
const auto& input = engine.input();

// Проверка удержания клавиши
if (input.isKeyPressed(adapters::window::KeyCode::KeyW)) {
    // Движение вперёд
}

// Проверка одиночного нажатия в текущем кадре
if (input.isKeyJustPressed(adapters::window::KeyCode::Space)) {
    // Прыжок
}

// Проверка кнопок мыши
if (input.isMouseButtonPressed(adapters::window::MouseButton::Left)) {
    // Стрельба / выделение
}

// Координаты курсора и смещение
const auto [mouseX, mouseY] = input.mousePosition();
const auto [deltaX, deltaY] = input.mouseDelta();
```

### Управление выполнением

- **`step(double dt)`** — выполняет одну полную итерацию движка: опрос системных событий окна, продвижение симуляции `FixedStepUpdate` (с вызовами `onUpdate`), вызов `onFrame`, сборку кадра `RenderPreparation`, программный рендеринг в буфер и вывод в окно через `present()`. Метод незаменим для детерминированного покадрового тестирования без системного ожидания.
- **`run()`** — запускает главный непрерывный цикл приложения. Внутри используется таймер `std::chrono::steady_clock` с ограничением максимального шага кадра (0.1 с) для исключения скачков времени при отладке. Цикл продолжается, пока окно открыто и не вызван `stop()`.
- **`stop()`** — останавливает главный цикл и закрывает окно приложения.

После `stop()` или события закрытия текущий `step()` прекращает следующие вызовы обработчиков
и показ кадра. Уже выполняющийся обработчик завершается обычным образом. Вызов `step()`
для закрытого окна ничего не выполняет. Если обработчик выбрасывает исключение из `run()`,
исключение передаётся вызывающему коду, а `isRunning()` сбрасывается в `false`.

---

## Режим быстрого просмотра (Quick Preview)

Режим Quick Preview позволяет мгновенно отобразить любую геометрическую фигуру или существующий объект сцены без ручной настройки проекции, освещения, кадровых буферов и оконных портов.

### 1. `Engine::preview(geometry, config)`

Статический метод, открывающий окно и отображающий переданную геометрию:

```cpp
#include "huynity/engine.h"
#include "huynity/objectlib/shapes/sphere.h"

int main() {
    huynity::objectlib::Sphere3 sphere({0.0, 0.0, 0.0}, 2.5);

    // Отображает сферу в окне 800x600 с автоматическим светом и камерой
    huynity::Engine::preview(sphere);
    return 0;
}
```

### 2. `engine.show(objectId)`

Метод экземпляра, центрирующий камеру на уже существующем в сцене объекте:

```cpp
const auto cube = engine.create<objectlib::ObjectType::Box3>({.size = {3.0, 3.0, 3.0}});
engine.show(cube); // автоматическая подгонка камеры под куб
```

### Алгоритм автоматической подгонки камеры по габаритам AABB

Алгоритм гарантирует 100% попадание объекта в область видимости окна с сохранением пропорций:

1. **Вычисление габаритов:** запрашиваются мировые границы объекта `worldBounds` (или вычисляются через `objectlib::bounds`).
2. **Автоматический материал:** если у объекта не назначен материал, ему автоматически присваивается приятный базовый материал (голубой оттенок RGBA `{0.2, 0.6, 0.9, 1.0}`).
3. **Для 3D-геометрии (`Aabb3`):**
   - Вычисляется центр $C = (C_x, C_y, C_z)$ и размеры $(S_x, S_y, S_z)$.
   - Определяется максимальный размер $D_{\max} = \max(S_x, S_y, S_z)$.
   - С учётом коэффициента запаса `margin = 1.5` и соотношения сторон окна $\text{aspect} = W / H$ настраиваются видимые размеры ортографической камеры.
   - Дистанция камеры устанавливается равной $Z_{\text{cam}} = C_z - \max(20.0, D_{\max} \cdot 5.0)$, а плоскости отсечения охватывают всю глубину фигуры: $\text{nearPlane} = 0.1$, $\text{farPlane} = \text{targetDepth} + \max(50.0, D_{\max} \cdot 10.0)$.
   - Настраивается стандартное освещение `Lighting3`: рассеянный фоновый свет `ambient` (белый, интенсивность 0.3) и направленный источник `directional` под углом $(1, 1, 1)$ с интенсивностью 0.7.
4. **Для 2D-геометрии (`Aabb2`):**
   - Центр камеры совмещается с центром фигуры $(C_x, C_y)$.
   - Габариты 2D-камеры масштабируются с запасом `margin = 1.25` и выравниваются по пропорциям окна.
5. **Отрисовка первого кадра:** вызывается `step(0.0)` для немедленной визуализации.

---

## Управление смешанной сценой: 3D, 2D и HUD

`Engine` поддерживает рендеринг многослойных сцен, комбинирующих пространственный 3D-мир, плоский 2D-мир и экранный интерфейс (HUD).

```text
       Вариант 1 (по умолчанию):              Вариант 2 (setWorld2DFirst(true)):
┌───────────────────────────────────────┐  ┌───────────────────────────────────────┐
│        HUD (экранный интерфейс)       │  │        HUD (экранный интерфейс)       │
├───────────────────────────────────────┤  ├───────────────────────────────────────┤
│        2D World (спрайты, UI)         │  │        3D World (меши, освещение)     │
├───────────────────────────────────────┤  ├───────────────────────────────────────┤
│        3D World (меши, освещение)     │  │        2D World (фоны, тайлы)         │
├───────────────────────────────────────┤  ├───────────────────────────────────────┤
│        Clear Color (очистка)          │  │        Clear Color (очистка)          │
└───────────────────────────────────────┘  └───────────────────────────────────────┘
```

### Методы настройки слоёв

| Метод | Описание |
| :--- | :--- |
| `setCamera(Camera3 camera)` | Устанавливает активную 3D-камеру (перспективную или ортографическую). |
| `clearCamera3()` | Отключает рендеринг 3D-прохода. |
| `setCamera(Camera2 camera)` | Устанавливает активную 2D-камеру мирового пространства. |
| `clearCamera2()` | Отключает рендеринг 2D-прохода. |
| `setLighting(Lighting3 lighting)` | Задаёт рассеянное и направленное освещение для 3D-прохода. |
| `setWorld2DFirst(bool world2DFirst)` | Если `true`, сначала отрисовывается 2D-мир, затем поверх него — 3D-мир. По умолчанию `false` (3D, затем 2D). |
| `setHudEnabled(bool enabled)` | Включает или выключает проход отрисовки экранных элементов HUD. По умолчанию включён (`true`). |
| `setClearColor(Color color)` | Задаёт цвет очистки кадрового буфера в формате RGBA (от 0.0 до 1.0). |

---

## Полный пример: игра «Hello, Cube»

Минимальная интерактивная 3D-игра с вращающимся кубом и управлением клавишами укладывается всего в 20 строк кода:

```cpp
#include "huynity/engine.h"
#include "huynity/math/transform.h"

int main() {
    huynity::Engine engine({.windowTitle = "Huynity: Hello, Cube"});

    const auto cube = engine.create<huynity::objectlib::ObjectType::Box3>(
        {.size = {3.0, 3.0, 3.0}}, huynity::core::Mobility::Dynamic);
    engine.show(cube);

    double angle = 0.0;
    engine.onUpdate([&](double dt) {
        if (engine.input().isKeyPressed(huynity::adapters::window::KeyCode::Escape)) {
            engine.stop();
        }
        const double speed = engine.input().isKeyPressed(huynity::adapters::window::KeyCode::Space) ? 3.0 : 1.0;
        angle += dt * speed;
        (void)engine.scene().setLocalTransform(
            cube, huynity::math::compose(huynity::math::rotation(huynity::math::RotationAxis::X, angle * 0.5),
                                         huynity::math::rotation(huynity::math::RotationAxis::Y, angle)));
    });

    engine.run();
    return 0;
}
```

---

## Справочник ключевых методов `huynity::Engine`

| Метод / Сигнатура | Назначение |
| :--- | :--- |
| `explicit Engine(const EngineConfig& = {})` | Создаёт движок, инициализирует окно, пайплайн кадров и рендерер. |
| `template <ObjectType kind> ObjectId create(...)` | Создаёт 2D/3D сущность в активной сцене по типизированным параметрам. |
| `ObjectId create(const ObjectSpec&, Mobility)` | Создаёт сущность по динамической спецификации `ObjectSpec`. |
| `void onUpdate(UpdateCallback)` | Регистрирует функцию фиксированного шага физики и логики (`fixedDt`). |
| `void onFrame(FrameCallback)` | Регистрирует функцию подготовки кадра с коэффициентом интерполяции `alpha`. |
| `void onEvent(EventCallback)` | Регистрирует функцию обработки системных событий окна и ввода. |
| `const InputState& input() const noexcept` | Возвращает объект быстрого синхронного опроса клавиатуры и мыши. |
| `void step(double dt)` | Выполняет ровно одну полную итерацию кадра с дельтой `dt` (удобно для тестов). |
| `void run()` | Запускает главный непрерывный игровой цикл приложения. |
| `void stop() noexcept` | Сигнализирует о завершении цикла и закрывает окно. |
| `void show(ObjectId)` | Автоматически настраивает камеру и свет по габаритам AABB объекта. |
| `static void preview(const Geometry&, ...)` | Мгновенно показывает геометрию в отдельном окне. |
| `core::Scene& scene() noexcept` | Предоставляет прямой доступ к низкоуровневой сцене Core. |
| `renderer::SoftwareRenderer& renderer() noexcept`| Предоставляет доступ к программному рендереру. |
| `runtime::FramePipeline& pipeline() noexcept` | Предоставляет доступ к конвейеру кадровых буферов. |
| `adapters::window::WindowAdapter& window() noexcept` | Предоставляет доступ к оконному адаптеру. |
