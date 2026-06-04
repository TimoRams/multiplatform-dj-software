#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>

class DjEngine;

struct Flx10LedMapping
{
    std::array<uint8_t, 4> deckNoteStatus = { 0x90, 0x91, 0x92, 0x93 };
    std::array<uint8_t, 4> hotcueStatus = { 0x97, 0x99, 0x9B, 0x9D };
    std::array<uint8_t, 4> hotcueShiftStatus = { 0x98, 0x9A, 0x9C, 0x9E };
    std::array<uint8_t, 4> vuStatus = { 0xB0, 0xB1, 0xB2, 0xB3 };

    uint8_t playNote = 0x0B;
    uint8_t cueNote = 0x0C;
    uint8_t loopInNote = 0x10;
    uint8_t loopOutNote = 0x11;
    uint8_t loop4BeatNote = 0x12;
    uint8_t loopReloopNote = 0x14;
    uint8_t tempoResetNote = 0x41;
    uint8_t beatSyncNote = 0x58;
    uint8_t keySyncNote = 0x65;
    uint8_t vuControl = 0x02;

    uint8_t padBlue = 0x01;
    uint8_t padCyan = 0x11;
    uint8_t padGreen = 0x15;
    uint8_t padYellow = 0x1D;
    uint8_t padOrange = 0x25;
    uint8_t padRed = 0x29;
    uint8_t padPink = 0x35;
    uint8_t padMagenta = 0x39;
    uint8_t padPurple = 0x3D;
    uint8_t padWhite = 0x7F;
};

class Flx10LedController : public QObject
{
    Q_OBJECT

public:
    using MidiSender = std::function<bool(uint8_t status, uint8_t data1, uint8_t data2, const QString& type)>;

    explicit Flx10LedController(QObject* parent = nullptr);

    void setMidiSender(MidiSender sender);
    void setDecks(DjEngine* deckA, DjEngine* deckB);
    void setMapping(const Flx10LedMapping& mapping);
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void start();
    void stop();

    void clearAll();
    void refreshAll();
    void refreshDeck(int deck);
    void refreshDeckLeds(int deck);
    void refreshHotcuePads(int deck);

    void sendDeckLed(int deck, uint8_t note, bool enabled);
    void sendHotcuePadLed(int deck, int pad, uint8_t color);
    void sendVuMeter(int channel, float level);

    void sendPaletteTest();
    void testRawLedOutput();

private:
    uint8_t deckNoteStatus(int deck) const;
    uint8_t hotcueStatusForDeck(int deck) const;
    uint8_t hotcueShiftStatusForDeck(int deck) const;
    uint8_t vuStatusForDeck(int deck) const;
    uint8_t hotcueColorValue(const QString& color) const;

    bool sendMidiShort(uint8_t status, uint8_t data1, uint8_t data2, const QString& type);
    DjEngine* deckEngine(int deck) const;
    bool deckHasBlinkingHotcue(int deck) const;
    void updateVuMeters();
    void updateBlinkPhase();
    void scheduleRawTestStep(int delayMs, const std::function<void()>& step);

    MidiSender m_sender;
    DjEngine* m_decks[4] = { nullptr, nullptr, nullptr, nullptr };
    Flx10LedMapping m_mapping;
    QTimer m_vuTimer;
    QTimer m_blinkTimer;
    std::array<uint8_t, 4> m_lastVuValues = { 0xFF, 0xFF, 0xFF, 0xFF };
    bool m_enabled = false;
    bool m_blinkPhase = false;
};
