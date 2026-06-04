#include "Flx10LedController.h"

#include "DjEngine.h"

#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace {
constexpr uint8_t kLedOff = 0x00;
constexpr uint8_t kLedOn = 0x7F;
int clamp7(int value)
{
    return std::max(0, std::min(127, value));
}

double hueDistance(double a, double b)
{
    const double distance = std::abs(a - b);
    return std::min(distance, 360.0 - distance);
}
}

Flx10LedController::Flx10LedController(QObject* parent)
    : QObject(parent)
{
    m_vuTimer.setInterval(33);
    connect(&m_vuTimer, &QTimer::timeout,
            this, &Flx10LedController::updateVuMeters);

    m_blinkTimer.setInterval(500);
    connect(&m_blinkTimer, &QTimer::timeout,
            this, &Flx10LedController::updateBlinkPhase);
}

void Flx10LedController::setMidiSender(MidiSender sender)
{
    m_sender = std::move(sender);
}

void Flx10LedController::setDecks(DjEngine* deckA, DjEngine* deckB)
{
    m_decks[0] = deckA;
    m_decks[1] = deckB;
    m_decks[2] = nullptr;
    m_decks[3] = nullptr;
}

void Flx10LedController::setMapping(const Flx10LedMapping& mapping)
{
    m_mapping = mapping;
    if (m_enabled)
        refreshAll();
}

void Flx10LedController::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    if (m_enabled)
        start();
    else
        stop();
}

void Flx10LedController::start()
{
    if (!m_enabled)
        return;

    clearAll();
    refreshAll();
    if (!m_vuTimer.isActive())
        m_vuTimer.start();
    if (!m_blinkTimer.isActive())
        m_blinkTimer.start();
}

void Flx10LedController::stop()
{
    if (m_vuTimer.isActive())
        m_vuTimer.stop();
    if (m_blinkTimer.isActive())
        m_blinkTimer.stop();
}

void Flx10LedController::clearAll()
{
    for (int deck = 1; deck <= 4; ++deck) {
        sendDeckLed(deck, m_mapping.playNote, false);
        sendDeckLed(deck, m_mapping.cueNote, false);
        sendDeckLed(deck, m_mapping.loopInNote, false);
        sendDeckLed(deck, m_mapping.loopOutNote, false);
        sendDeckLed(deck, m_mapping.loop4BeatNote, false);
        sendDeckLed(deck, m_mapping.loopReloopNote, false);
        sendDeckLed(deck, m_mapping.tempoResetNote, false);
        sendDeckLed(deck, m_mapping.beatSyncNote, false);
        sendDeckLed(deck, m_mapping.keySyncNote, false);

        for (int pad = 1; pad <= 8; ++pad)
            sendHotcuePadLed(deck, pad, kLedOff);

        m_lastVuValues[static_cast<size_t>(deck - 1)] = 0xFF;
        sendVuMeter(deck, 0.0f);
    }
}

void Flx10LedController::refreshAll()
{
    if (!m_enabled)
        return;

    for (int deck = 1; deck <= 4; ++deck)
        refreshDeck(deck);
}

void Flx10LedController::refreshDeck(int deck)
{
    if (!m_enabled)
        return;

    refreshDeckLeds(deck);
    refreshHotcuePads(deck);
}

void Flx10LedController::refreshDeckLeds(int deck)
{
    if (!m_enabled)
        return;

    DjEngine* engine = deckEngine(deck);
    if (!engine) {
        sendDeckLed(deck, m_mapping.playNote, false);
        sendDeckLed(deck, m_mapping.cueNote, false);
        sendDeckLed(deck, m_mapping.loopInNote, false);
        sendDeckLed(deck, m_mapping.loopOutNote, false);
        sendDeckLed(deck, m_mapping.loop4BeatNote, false);
        sendDeckLed(deck, m_mapping.loopReloopNote, false);
        sendDeckLed(deck, m_mapping.tempoResetNote, false);
        sendDeckLed(deck, m_mapping.beatSyncNote, false);
        sendDeckLed(deck, m_mapping.keySyncNote, false);
        return;
    }

    sendDeckLed(deck, m_mapping.playNote, engine->isPlaying());
    sendDeckLed(deck, m_mapping.cueNote, engine->cueEnabled());
    sendDeckLed(deck, m_mapping.loopInNote, engine->loopInSet());
    sendDeckLed(deck, m_mapping.loopOutNote, engine->loopActive());
    sendDeckLed(deck, m_mapping.loop4BeatNote, engine->loopActive());
    sendDeckLed(deck, m_mapping.loopReloopNote, engine->loopActive());
    sendDeckLed(deck, m_mapping.tempoResetNote, qFuzzyIsNull(engine->getTempoPercent()));
    sendDeckLed(deck, m_mapping.beatSyncNote, engine->syncEnabled());
    sendDeckLed(deck, m_mapping.keySyncNote, engine->keylock());
}

void Flx10LedController::refreshHotcuePads(int deck)
{
    if (!m_enabled)
        return;

    DjEngine* engine = deckEngine(deck);
    const QVariantList cues = engine ? engine->hotCues() : QVariantList();

    for (int pad = 1; pad <= 8; ++pad) {
        bool exists = false;
        bool isLoop = false;
        uint8_t color = kLedOff;

        const int index = pad - 1;
        if (index < cues.size()) {
            const QVariantMap cue = cues.at(index).toMap();
            exists = cue.value(QStringLiteral("set")).toBool();
            isLoop = cue.value(QStringLiteral("isLoop")).toBool()
                || cue.value(QStringLiteral("type")).toString().compare(QStringLiteral("loop"), Qt::CaseInsensitive) == 0;

            if (!isLoop) {
                const QString label = cue.value(QStringLiteral("label")).toString().trimmed().toLower();
                isLoop = label.contains(QStringLiteral("loop")) || label.contains(QStringLiteral("reloop"));
            }

            color = hotcueColorValue(cue.value(QStringLiteral("color")).toString());
        }

        const uint8_t value = !exists ? kLedOff : (isLoop && !m_blinkPhase ? kLedOff : color);
        sendHotcuePadLed(deck, pad, value);
    }
}

void Flx10LedController::sendDeckLed(int deck, uint8_t note, bool enabled)
{
    sendMidiShort(deckNoteStatus(deck), note, enabled ? kLedOn : kLedOff, QStringLiteral("note-led"));
}

void Flx10LedController::sendHotcuePadLed(int deck, int pad, uint8_t color)
{
    const uint8_t note = static_cast<uint8_t>(std::clamp(pad, 1, 8) - 1);
    const uint8_t value = color & 0x7F;
    sendMidiShort(hotcueStatusForDeck(deck), note, value, QStringLiteral("pad-led"));
    sendMidiShort(hotcueShiftStatusForDeck(deck), note, value, QStringLiteral("pad-led"));
}

void Flx10LedController::sendVuMeter(int channel, float level)
{
    const float clamped = std::clamp(level, 0.0f, 1.0f);
    const int index = std::clamp(channel, 1, 4) - 1;
    const uint8_t status = vuStatusForDeck(channel);
    const uint8_t value = static_cast<uint8_t>(std::round(clamped * 127.0f));
    if (m_lastVuValues[static_cast<size_t>(index)] == value)
        return;

    m_lastVuValues[static_cast<size_t>(index)] = value;
    sendMidiShort(status, m_mapping.vuControl, value, QStringLiteral("vu-meter"));
}

void Flx10LedController::sendPaletteTest()
{
    for (int deck = 1; deck <= 4; ++deck) {
        for (int pad = 0; pad < 8; ++pad)
            sendMidiShort(hotcueStatusForDeck(deck), static_cast<uint8_t>(pad),
                          static_cast<uint8_t>(3 + pad * 4), QStringLiteral("pad-palette-test"));
    }
}

void Flx10LedController::testRawLedOutput()
{
    const bool vuWasActive = m_vuTimer.isActive();
    const bool blinkWasActive = m_blinkTimer.isActive();
    m_vuTimer.stop();
    m_blinkTimer.stop();

    int delay = 0;
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x90, 0x0B, 0x7F, QStringLiteral("raw-test")); });
    delay += 500;
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x90, 0x0B, 0x00, QStringLiteral("raw-test")); });
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x90, 0x0C, 0x7F, QStringLiteral("raw-test")); });
    delay += 500;
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x90, 0x0C, 0x00, QStringLiteral("raw-test")); });
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x97, 0x00, 0x29, QStringLiteral("raw-test")); });
    delay += 500;
    scheduleRawTestStep(delay, [this] { sendMidiShort(0x97, 0x00, 0x00, QStringLiteral("raw-test")); });

    for (int value = 0; value <= 127; value += 8) {
        scheduleRawTestStep(delay, [this, value] {
            sendMidiShort(0xB0, 0x02, static_cast<uint8_t>(value), QStringLiteral("raw-test"));
        });
        delay += 20;
    }

    scheduleRawTestStep(delay, [this, vuWasActive, blinkWasActive] {
        sendMidiShort(0xB0, 0x02, 0x00, QStringLiteral("raw-test"));
        if (m_enabled) {
            if (vuWasActive && !m_vuTimer.isActive())
                m_vuTimer.start();
            if (blinkWasActive && !m_blinkTimer.isActive())
                m_blinkTimer.start();
            refreshAll();
        }
    });
}

uint8_t Flx10LedController::deckNoteStatus(int deck) const
{
    return m_mapping.deckNoteStatus[static_cast<size_t>(std::clamp(deck, 1, 4) - 1)];
}

uint8_t Flx10LedController::hotcueStatusForDeck(int deck) const
{
    return m_mapping.hotcueStatus[static_cast<size_t>(std::clamp(deck, 1, 4) - 1)];
}

uint8_t Flx10LedController::hotcueShiftStatusForDeck(int deck) const
{
    return m_mapping.hotcueShiftStatus[static_cast<size_t>(std::clamp(deck, 1, 4) - 1)];
}

uint8_t Flx10LedController::vuStatusForDeck(int deck) const
{
    return m_mapping.vuStatus[static_cast<size_t>(std::clamp(deck, 1, 4) - 1)];
}

uint8_t Flx10LedController::hotcueColorValue(const QString& color) const
{
    struct PaletteValue { int value; double hue; };
    const PaletteValue palette[] = {
        { m_mapping.padRed, 0.0 },
        { m_mapping.padOrange, 30.0 },
        { m_mapping.padYellow, 55.0 },
        { m_mapping.padGreen, 120.0 },
        { m_mapping.padCyan, 180.0 },
        { m_mapping.padBlue, 235.0 },
        { m_mapping.padPurple, 280.0 },
        { m_mapping.padPink, 325.0 },
        { m_mapping.padMagenta, 305.0 },
    };

    QString hex = color.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex.remove(0, 1);

    if (hex.size() == 3) {
        QString expanded;
        expanded.reserve(6);
        for (const QChar c : hex) {
            expanded.append(c);
            expanded.append(c);
        }
        hex = expanded;
    }

    bool ok = false;
    const int rgb = hex.toInt(&ok, 16);
    if (!ok || hex.size() != 6)
        return m_mapping.padRed;

    const double r = static_cast<double>((rgb >> 16) & 0xff) / 255.0;
    const double g = static_cast<double>((rgb >> 8) & 0xff) / 255.0;
    const double b = static_cast<double>(rgb & 0xff) / 255.0;
    const double maxC = std::max({ r, g, b });
    const double minC = std::min({ r, g, b });
    const double delta = maxC - minC;
    if (maxC <= 0.0 || delta <= 0.0001)
        return m_mapping.padWhite;

    const double saturation = delta / maxC;
    if (saturation < 0.18)
        return m_mapping.padWhite;

    double hue = 0.0;
    if (maxC == r)
        hue = 60.0 * std::fmod(((g - b) / delta), 6.0);
    else if (maxC == g)
        hue = 60.0 * (((b - r) / delta) + 2.0);
    else
        hue = 60.0 * (((r - g) / delta) + 4.0);
    if (hue < 0.0)
        hue += 360.0;

    int bestValue = m_mapping.padRed;
    double bestDistance = 361.0;
    for (const auto& entry : palette) {
        const double distance = hueDistance(hue, entry.hue);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestValue = entry.value;
        }
    }

    return static_cast<uint8_t>(bestValue);
}

bool Flx10LedController::sendMidiShort(uint8_t status, uint8_t data1, uint8_t data2, const QString& type)
{
    if (!m_sender)
        return false;
    return m_sender(status, static_cast<uint8_t>(clamp7(data1)), static_cast<uint8_t>(clamp7(data2)), type);
}

DjEngine* Flx10LedController::deckEngine(int deck) const
{
    const int index = std::clamp(deck, 1, 4) - 1;
    return m_decks[index];
}

bool Flx10LedController::deckHasBlinkingHotcue(int deck) const
{
    DjEngine* engine = deckEngine(deck);
    if (!engine)
        return false;

    const QVariantList cues = engine->hotCues();
    for (const QVariant& item : cues) {
        const QVariantMap cue = item.toMap();
        if (!cue.value(QStringLiteral("set")).toBool())
            continue;

        if (cue.value(QStringLiteral("isLoop")).toBool())
            return true;

        if (cue.value(QStringLiteral("type")).toString().compare(QStringLiteral("loop"), Qt::CaseInsensitive) == 0)
            return true;

        const QString label = cue.value(QStringLiteral("label")).toString().trimmed().toLower();
        if (label.contains(QStringLiteral("loop")) || label.contains(QStringLiteral("reloop")))
            return true;
    }

    return false;
}

void Flx10LedController::updateVuMeters()
{
    if (!m_enabled)
        return;

    auto deckVu = [](DjEngine* engine) -> float
    {
        if (!engine)
            return 0.0f;
        return std::clamp(std::max(engine->vuLevelL(), engine->vuLevelR()), 0.0f, 1.0f);
    };

    sendVuMeter(1, deckVu(m_decks[0]));
    sendVuMeter(2, deckVu(m_decks[1]));
    sendVuMeter(3, deckVu(m_decks[2]));
    sendVuMeter(4, deckVu(m_decks[3]));
}

void Flx10LedController::updateBlinkPhase()
{
    if (!m_enabled)
        return;

    m_blinkPhase = !m_blinkPhase;
    for (int deck = 1; deck <= 4; ++deck) {
        if (deckHasBlinkingHotcue(deck))
            refreshHotcuePads(deck);
    }
}

void Flx10LedController::scheduleRawTestStep(int delayMs, const std::function<void()>& step)
{
    QTimer::singleShot(delayMs, this, [step] { step(); });
}
