# Render Preparation: версии, кеши и время жизни

## Версии и точечная инвалидация

`SceneInstance` хранит неизменяемую Geometry, её `geometryVersion`, отдельные revisions для
local/world transform, bounds, material и visibility, а также независимые dirty-флаги.
Общий номер версии не инвалидирует все производные данные.

- Изменение local transform обновляет world transform поддерева и помечает world bounds;
  Geometry и tessellation остаются прежними.
- `setGeometry` публикует новый immutable Geometry resource, увеличивает версию и помечает
  bounds/tessellation. Прежний ресурс остаётся действительным у существующих владельцев.
- `setFiniteRepresentation` задаёт отдельную конечную Geometry для Line/Ray/Plane и
  инвалидирует bounds/tessellation без замены исходной геометрии. После `setGeometry` старое
  представление сбрасывается.
- Material, visibility и render properties влияют на draw lists, но не на локальную
  tessellation.

`refreshSpatialState()` вычисляет local bounds только при отсутствии кеша или несовпадении
`cachedBoundsGeometryVersion`. World bounds получаются преобразованием углов кешированного
AABB, без повторного обхода вершин. `recomputeSpatialState()` выполняет полный пересчёт и
используется тестами для сравнения с инкрементальным результатом.

## Tessellation cache и качество

`TessellationCache` синхронно вызывает Object Library на потоке Core. Ключ состоит из identity
immutable Geometry, `geometryVersion`, `TessellationOptions::maxDeviation` и
`maxPrimitiveCount`. Поэтому экземпляры одной Geometry с одинаковым качеством разделяют
`shared_ptr<const TessellatedGeometry>`. Движение не меняет ключ и не вызывает повторную
тесселяцию; изменение любого входа создаёт отдельный результат.

`GeometryError` не добавляет частичный результат в кеш и не изменяет Geometry или регистрацию
объектов. `prepareSnapshot` возвращает ошибку вместо частичного снимка. Счётчики
`tessellationCalculations()` и `tessellationCacheHits()` позволяют тестам отличать reuse от
нового вычисления.

Camera-dependent LOD сейчас не реализован. Если он понадобится, следует выбирать дискретные
наборы `TessellationOptions`, которые уже входят в ключ кеша. Непрерывный пересчёт
`maxDeviation` от расстояния до камеры не является текущим поведением.

## Снимок и владение ресурсами

`prepareSnapshot` заново строит полное состояние активных World3D/World2D/Hud2D passes и не
зависит от предыдущего кадра. Мировые passes используют camera transforms и world bounds для
culling; HUD использует screen-space transforms и сортируется по `(layer, order)`.

Для перспективной камеры `prepareSnapshot` передаёт в culling соотношение ширины и высоты
изображения. При прямом вызове `prepareWorld3D` или `preparePasses` оно задаётся последним
параметром `aspectRatio` (по умолчанию 1). Проверка AABB использует наиболее широкое сечение
в пределах near/far, чтобы не отбрасывать объекты, частично пересекающие область видимости.

Mesh и материалы дедуплицируются в таблицах снимка, draw items содержат их индексы. Каждый
`MeshResource` хранит `shared_ptr<const void>` владельца tessellation и read-only spans на
positions и bytes индексов. Поэтому массивы не копируются и остаются доступными после удаления
исходного объекта, пока жив снимок. Bytes индексов декодируются через `memcpy` в тип
`TriangleIndices`; не создаётся некорректный flat pointer с арифметикой между подобъектами.

Снимок не содержит изменяемых ссылок на хранилище Core. Последующие команды изменяют только
сцену и следующие снимки; уже подготовленный снимок сохраняет прежнее согласованное состояние.

## Проверки и будущая фоновая работа

Тесты `core.spatial`, `core.tessellation_cache`, `core.draw_lists`, `core.hud`, `core.snapshot`
и `core.finite_representation` покрывают описанные правила, включая reuse, ошибки, порядок
passes и lifetime после удаления объекта.

Фоновая тесселяция не реализована и не является обязательной. При будущем добавлении рабочий
поток должен получать стабильный immutable вход, а результат с Geometry version и полным
quality key сможет опубликовать только Core после проверки актуальности.
