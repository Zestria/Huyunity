# Адаптер окна и подсистема ввода

Компонент `adapters/window` предоставляет платформонезависимый слой управления системным окном, опроса событий ввода и отображения готовых кадровых буферов программного рендерера Huynity.

## Архитектура и границы зависимостей

Оконный адаптер спроектирован по принципу инверсии зависимостей (Dependency Inversion):
- Пространство имён: `huynity::adapters::window`.
- CMake-цели: `huynity_window` и псевдоним `huynity::window`.
- Зависимости: строго ограниченные — компонент зависит только от `huynity::runtime` и `huynity::image`.
- Отсутствие связей с подсистемами `core`, `renderer` и `math`: адаптер не знает о структуре сцены, 3D-сетках, материалах или алгоритмах растеризации. Он оперирует исключительно готовым буфером пикселей `image::ImageBuffer` и интерфейсом порта показа `runtime::PresentationPort`.

```text
┌────────────────────────────────────────────────────────┐
│                   Runtime Main Loop                    │
└───────────────────────────┬────────────────────────────┘
                            │ presentation.present(framebuffer)
                            ▼
┌────────────────────────────────────────────────────────┐
│               runtime::PresentationPort                 │
└───────────────────────────▲────────────────────────────┘
                            │ implements
┌───────────────────────────┴────────────────────────────┐
│              window::WindowAdapter                     │
├────────────────────────────┬───────────────────────────┤
│       HeadlessWindow       │       NativeWindow        │
│   (in-memory, CI/tests)    │      (optional SDL3)      │
└────────────────────────────┴───────────────────────────┘
```

## Интерфейс WindowAdapter и PresentationPort

Базовый абстрактный класс `WindowAdapter` наследует интерфейс `runtime::PresentationPort` и расширяет его методами жизненного цикла системного окна и опроса событий:

```cpp
class WindowAdapter : public runtime::PresentationPort {
public:
    ~WindowAdapter() override = default;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual WindowSize size() const noexcept = 0;
    virtual void setTitle(std::string_view title) = 0;

    virtual void pollEvents(std::vector<InputEvent>& outEvents) = 0;

    // runtime::PresentationPort
    [[nodiscard]] virtual std::unique_ptr<image::ImageBuffer>
    present(std::unique_ptr<image::ImageBuffer> framebuffer) override = 0;
};
```

### Контракт показа кадра `present()`

Метод `present()` принимает уникальное владение `std::unique_ptr<image::ImageBuffer>`:
1. **Синхронный возврат**: буфер выводится на экран (копированием/блитом пикселей в экранный контекст) и синхронно возвращается вызывающей стороне.
2. **Переиспользование без аллокаций**: возвращённый буфер немедленно поступает в `runtime::FramebufferPool` для отрисовки следующих кадров.
3. **Контроль размеров**: если размеры буфера не совпадают с текущим `size()` окна (например, из-за произошедшего изменения размера клиентской области), кадр отклоняется без вывода и возвращается вызывающему, а счётчик отклонённых кадров инкрементируется.

## Оконные бэкенды: HeadlessWindow и NativeWindow

Выбор и создание оконного адаптера осуществляется через фабричную функцию:

```cpp
std::unique_ptr<WindowAdapter> createWindow(const WindowConfig& config = {});
```

Конфигурация `WindowConfig` задаёт начальные параметры:
- `size` — размеры клиентской области окна (по умолчанию 800x600 пикселей);
- `title` — заголовок окна (по умолчанию "Huynity Window");
- `forceHeadless` — принудительный выбор headless-режима даже при наличии графической платформы.

### HeadlessWindow

`HeadlessWindow` — детерминированная реализация в оперативной памяти:
- Не требует графического сервера, X11, Wayland или физического дисплея.
- Поддерживает программную инъекцию событий через методы `pushEvent(InputEvent)` и `pushEvents(std::span<const InputEvent>)`.
- При опросе внедрённого `WindowResizedEvent` обновляет собственный `size()` до последнего
  размера из очереди, чтобы следующий кадр и поверхность Runtime использовали одинаковые габариты.
- Позволяет изменять габариты окна через `resize(width, height)`, автоматически генерируя `WindowResizedEvent`.
- Сохраняет копию последнего успешно отображённого буфера (`lastPresentedFrame()`), что позволяет модульным и интеграционным тестам верифицировать контрольные пиксели отрисованного кадра.
- Ведёт статистику `presentedCount()` и `rejectedPresents()`.

### NativeWindow (SDL3)

При наличии SDL3 адаптер подключает нативный оконный бэкенд:
- В `adapters/window/CMakeLists.txt` выполняется тихий поиск `find_package(SDL3 QUIET)`. На Fedora пакет разработки устанавливается командой `sudo dnf install SDL3-devel`.
- При успешном обнаружении задаётся определение компилятора `HUYNITY_HAS_NATIVE_WINDOW`, а `hasNativeWindowSupport()` возвращает `true`.
- `NativeWindow` создаёт SDL-окно с изменяемым размером, показывает RGBA-кадр через streaming-текстуру и учитывает `stride` исходного `ImageBuffer`.
- Размеры поверхности и координаты ввода выражаются в физических пикселях. Для HiDPI используются drawable pixel size и пересчёт логических координат мыши.
- Если SDL3 не найден, сборка продолжается только с `HeadlessWindow`. Если видеоподсистема или окно не может быть создано, фабрика выводит причину в stderr и возвращает `HeadlessWindow`.
- Определение `HUYNITY_HAS_NATIVE_WINDOW` сообщает о наличии бэкенда в сборке, но не гарантирует доступность графического дисплея. Для гарантированного режима без окна используйте `forceHeadless = true`.

## Подсистема событий ввода

Подсистема ввода разделена на поток дискретных событий и синхронное агрегированное состояние.

### Дискретные события: `InputEvent`

Тип `InputEvent` представляет собой `std::variant` следующих структур:
- `WindowResizedEvent` — изменение ширины и высоты клиентской области окна (`width`, `height`).
- `WindowCloseRequestedEvent` — запрос пользователя на закрытие окна (кнопка закрытия, комбинация закрытия окна).
- `KeyPressedEvent` — нажатие клавиши клавиатуры (`key: KeyCode`, признак повтора `isRepeat`).
- `KeyReleasedEvent` — отпускание клавиши клавиатуры (`key: KeyCode`).
- `MouseMovedEvent` — перемещение курсора мыши (`x`, `y`, а также относительные дельты `deltaX`, `deltaY`).
- `MouseButtonPressedEvent` — нажатие кнопки мыши (`button: MouseButton`, координаты `x`, `y`).
- `MouseButtonReleasedEvent` — отпускание кнопки мыши (`button: MouseButton`, координаты `x`, `y`).
- `MouseScrolledEvent` — прокрутка колёсика мыши (`deltaX`, `deltaY`).

### Коды клавиш и кнопок

- `KeyCode`: строго типизированное перечисление (`enum class KeyCode : std::uint16_t`), включающее буквы (`A`–`Z`), цифры (`Num0`–`Num9`), функциональные клавиши (`F1`–`F12`), навигационные клавиши (`Space`, `Escape`, `Enter`, `Left`, `Right`, `Up`, `Down`, `Home`, `End` и др.) и модификаторы (`LeftShift`, `LeftControl`, `LeftAlt` и др.).
- `MouseButton`: перечисление кнопок мыши (`Left`, `Right`, `Middle`, `Button4`, `Button5`).

### Агрегированное состояние: `InputState`

Структура `InputState` накапливает данные событий и предоставляет быстрый константный доступ $O(1)$ для фазы обновления логики сцены (`onUpdate`):

```cpp
struct InputState {
    [[nodiscard]] bool isKeyPressed(KeyCode key) const noexcept;
    [[nodiscard]] bool isMouseButtonPressed(MouseButton button) const noexcept;
    [[nodiscard]] double mouseX() const noexcept;
    [[nodiscard]] double mouseY() const noexcept;
    [[nodiscard]] double mouseDeltaX() const noexcept;
    [[nodiscard]] double mouseDeltaY() const noexcept;
    [[nodiscard]] double scrollDelta() const noexcept;

    void resetDeltas() noexcept;
    void processEvent(const InputEvent& event) noexcept;
};
```

Перед опросом событий в каждом кадре вызывается `resetDeltas()`, сбрасывающий покадровые смещения мыши и колеса прокрутки, при этом удерживаемые клавиши и кнопки остаются зажатыми.

## Обработка Resize и синхронизация поколений с FramePipeline

В архитектуре асинхронного конвейера рендеринга Huynity размер окна может измениться в момент, когда кадр уже отправлен на отрисовку:

1. **Монотонное поколение**: класс `runtime::SurfaceState` содержит целочисленный счётчик `generation()`. Счётчик увеличивается строго при действительном изменении ширины или высоты поверхности (`pipeline.resize(newSize)`).
2. **Привязка кадра к поколению**: при вызове `pipeline.submit(port, frame)` кадр запоминает текущее поколение поверхности.
3. **Отбрасывание устаревших (stale) кадров**: если во время рендеринга пришло событие `WindowResizedEvent`, поколение поверхности увеличивается. При обработке завершения `pipeline.complete(channel)` конвейер обнаруживает несовпадение поколений и присваивает завершению статус `FrameOutcome::Stale`.
4. **Безопасность показа**: метод `pipeline.handleCompletedFrame(completed, window)` проверяет статус завершения. Кадр со статусом `Stale` не передаётся в `window.present()`, предотвращая вывод кадра неверного разрешения на изменённую поверхность. Буфер старого разрешения возвращается в пул и пересоздаётся под новые размеры.

## Пример использования и запуск Demo

Пример `examples/window_demo/main.cpp` демонстрирует интеграцию всех подсистем:
- Открытие окна 64x64 через `window::createWindow`.
- Создание 3D-сцены: куб с динамической мобильностью и материалом, направленный и рассеянный свет.
- Создание 2D-элемента интерфейса (HUD): прямоугольный индикатор в экранном пространстве.
- Главный цикл с `runtime::FixedStepUpdate` (вращение куба с шагом $\Delta t = 1/60$ с).
- Подготовка снимка через `core::RenderPreparation::prepareSnapshot`.
- Растеризация сцены через `renderer::SoftwareRenderer`.
- Вывод через `runtime::FramePipeline` и `window->present()`.

### Сборка

Для сборки оконного адаптера и интерактивного примера необходимо включить опции `HUYNITY_BUILD_ADAPTERS` и `HUYNITY_BUILD_EXAMPLES`:

```sh
cmake --preset ninja -DHUYNITY_BUILD_ADAPTERS=ON -DHUYNITY_BUILD_EXAMPLES=ON
cmake --build build --target huynity_window_demo
```

### Запуск

1. **Автономный демонстрационный режим (10 шагов с программной эмуляцией ввода и resize)**:
   ```sh
   ./build/examples/window_demo/huynity_window_demo
   ```
   В этом режиме программа выполняет 10 шагов анимации, тестирует нажатие клавиш Space, клик мыши, изменение размера окна до 80x80, перехватывает `WindowCloseRequestedEvent` и корректно завершается с кодом возврата `0`.

2. **Непрерывный интерактивный режим**:
   ```sh
   ./build/examples/window_demo/huynity_window_demo --continuous
   ```
   В непрерывном режиме цикл работает до закрытия окна или нажатия клавиши `Escape`.


## Интерактивные примеры

Соберите примеры с нативным оконным адаптером:

```sh
cmake --preset ninja -DHUYNITY_BUILD_ADAPTERS=ON -DHUYNITY_BUILD_EXAMPLES=ON
cmake --build "$PWD/build" --target huynity_hello_cube huynity_game_scene --parallel
```

Запустите вращающийся куб или деревню с мельницей:

```sh
./build/examples/hello_cube/huynity_hello_cube --continuous
./build/examples/game_scene/huynity_game_scene --continuous
```

В `hello_cube` клавиша `Space` ускоряет вращение, а `Escape` закрывает окно. В `game_scene` используются:
- `A/D` или `Left/Right` — поворот камеры;
- `W/S` или `Up/Down` — приближение и отдаление;
- `Space` — ускорение мельницы;
- `Escape` — закрытие окна.

Без `--continuous` оба примера принудительно используют `HeadlessWindow`, выполняют 10 шагов и завершаются. Этот режим предназначен для CI и сохранения кадра через `--screenshot <path.ppm>`, а не для интерактивного показа.
