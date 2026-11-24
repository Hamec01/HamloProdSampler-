# HPSAMPLER & HPDG (DRUMENGINE) — Preflight Audit (`AUDIT.md`)

**Дата:** 24 ноября 2026  
**Рабочее пространство HPSAMPLER:** `E:\hamloprodsampler`  
**Репозиторий HPDG:** `C:\DRUMENGINE`

---

## 1. Фактическое состояние окружения и сборочной среды

- **ОС:** Windows (x64)
- **CMake:** `C:\Program Files\CMake\bin\cmake.exe`
- **Генератор / компилятор (из `C:\DRUMENGINE\build\CMakeCache.txt`):** `Visual Studio 18 2026` (MSVC x64)
- **Git:** `C:\Program Files\Git\cmd\git.exe`
- **JUCE:** `8.0.12` расположен локально в `C:\DRUMENGINE\JUCE`. HPSAMPLER ссылается на него через кешируемую переменную `HPSAMPLER_JUCE_DIR` (по умолчанию `C:/DRUMENGINE/JUCE`), без дублирования дерева JUCE.
- **Текущее состояние `E:\hamloprodsampler`:** чистый каталог с `HPSAMPLER_MASTER_SPEC_FOR_CODEX_RU_v1_1.md`. Создаётся новый самостоятельный проект `HPSAMPLER` + общая библиотека контрактов `HPBridgeCore`.

---

## 2. Фактическая структура `C:\DRUMENGINE` (HPDG Beta 9)

### 2.1. Плагин и форматы
- Продукт: `HPDG` (`0.1.0-beta.9`), `COMPANY_NAME "HamloProd"`, `PLUGIN_CODE BBG1`, форматы `Standalone` и `VST3`.
- Главный процессор: `bbg::BoomBapGeneratorAudioProcessor` (`Source/Plugin/PluginProcessor.h/.cpp`).
- Внутреннее разрешение времени: `kPpq = 960` тиков на четверть (полностью совпадает с целевым 960 PPQ в HPSAMPLER).

### 2.2. Существующий межплагинный протокол (`HamloLink`)
В `C:\DRUMENGINE\Source\Services\` уже существует рабочий односторонний IPC-механизм `hamlo::link`:
- `HamloLinkProtocol.h/.cpp` — memory-mapped файл `%LOCALAPPDATA%\HamloProd\Link\hpdg-<instanceId>.slot` с lock-free seqlock (`sequence`, `heartbeatMs`, `kAliveMs = 2000`, `kPpq = 960`, `bpm`, `genre`, `substyle`, `swingPercent`, `keyRoot`, `scaleMode`, `bars`, до 1024 событий `Event` по 11 дорожкам `HiHat..Sub808`).
- `HamloLinkPublisher.h/.cpp` — таймер 4 Гц на message thread (никогда не вызывается в audio thread), публикует снимок паттерна HPDG.

**Вывод для интеграции:**
- Для направления **HPDG → HPSAMPLER (Drum-First)** HPSAMPLER уже на P0 может читать реальные живые слоты `hamlo::link` через адаптер `HamloLinkReader` в `HPBridgeCore` и преобразовывать их в валидированный `hp.drum-guide.v1` **без единой правки в HPDG**!
- Для полноценного двустороннего handshake (`Connect to HPDG` ↔ `Connect to HPSAMPLER`) и направления **HPSAMPLER → HPDG** вводится единый контракт `HPBridgeCore` (`hp.sample-context.v1` + `hp.drum-guide.v1`), начинающийся на P0 с `FileSnapshotTransport` + `HamloLinkReader` и расширяемый до двустороннего `LocalIPCTransport` на P1.

### 2.3. Реальные точки интеграции в HPDG (для P1 адаптера)
- `bbg::SampleAwareGenerationContext` (`Source/Analysis/SampleAwareGenerationContext.h`) — готовая структура входного музыкального контекста генераторов HPDG (`AudioFeatureMap`, `LaneEvidenceMap`, `GenerationHints`, `SampleHarmony`, `MusicalEvidenceSnapshot`, `SamplePlaybackTimeline`, `guideKickPeaks`, `guideBoomBapPeaks`).
- `bbg::PatternProject` (`Source/Core/PatternProject.h`) — модель паттерна, `Lock` дорожек и ревизии.
- `Source/UI/MainHeaderComponent.h/.cpp` — верхняя панель интерфейса HPDG, куда в P1 можно добавить кнопку `Connect to HPSAMPLER` в существующей дизайн-системе (`SketchLookAndFeel` / `BlackSteel`), не ломая раскладку.
- **На P0 код `C:\DRUMENGINE` не модифицируется**, чтобы сохранить зелёный билд HPDG Beta 9 нетронутым до этапа P1.

---

## 3. Что можно переиспользовать из `C:\DRUMENGINE` (и что строго исключено)

| Модуль в `C:\DRUMENGINE` | Статус для HPSAMPLER | Примечание |
|---|---|---|
| `Source/Analysis/SampleLabelReader.h/.cpp` | **Переиспользовать (адаптировать)** | Чистый парсер BPM/Key из имени файла и WAV `acid` chunk (0% NN). |
| `Source/Analysis/STFTAnalyzer.h` | **Переиспользовать (адаптировать)** | Классический Hann STFT, spectral flux, low/mid/high energy, spectral flatness. |
| `Source/Analysis/OnsetDetector.h` | **Переиспользовать (адаптировать)** | Классический multiband onset detector с адаптивным порогом. |
| `Source/Analysis/SampleHarmonyAnalyzer.cpp` (часть Chroma + Albrecht-Shanahan / Krumhansl-Kessler) | **Переиспользовать (только классический путь)** | 12-bin chroma + корреляция с профилями major/minor, уверенность и отказ на шуме/перкуссии. |
| `Source/Analysis/BasicPitch/*` | **СТРОГО ИСКЛЮЧЕНО** | Содержит веса нейросети Basic Pitch. Запрещено Требованием #2 (без нейронных сетей). |
| `Source/Audio/LofiColor.h/.cpp` (уравнения HamloMag tube + HamloComp vari-mu) | **Переиспользовать (адаптировать в `FX Lite`)** | Лёгкий zero-latency, zero-alloc DSP-код без вложенного VST. |
| `Source/Services/HamloLinkProtocol.h/.cpp` | **Переиспользовать (read-only адаптер)** | Чтение живых инстансов HPDG в `HPBridgeCore::HamloLinkReader`. |
| `Assets/Images/Logo.png` | **Доступен** | Фирменный логотип HamloProd присутствует в `C:\DRUMENGINE\Assets\Images\Logo.png`. |

---

## 4. Риски и ограничения этапа P0

1. **Независимый Time-Stretch (Signalsmith Stretch):** не входит в стандартную поставку JUCE и отсутствует в `C:\DRUMENGINE`. В P0 реализуется качественный Repitch/Vintage (интерполяция Эрмита) + базовая архитектура `SyncMode::Off / Repitch / Stretch` (с фазо-осознанным/гранулярным fallback или предупреждением до подключения Signalsmith Stretch в P1).
2. **Двусторонний Live IPC:** на P0 `Connect to HPDG` честно поддерживает:
   - импорт/экспорт `hp.sample-context.v1` и `hp.drum-guide.v1` через `FileSnapshotTransport`;
   - живое обнаружение и чтение `HPDGDrumGuide` из работающего HPDG через `HamloLinkReader` (статус `Guide Linked (Read-Only)` / `File Snapshot`, никогда не подделывает двусторонний `Connected` без handshake).
3. **Сохранность HPDG:** исходники в `C:\DRUMENGINE` на этапе P0 остаются read-only.
