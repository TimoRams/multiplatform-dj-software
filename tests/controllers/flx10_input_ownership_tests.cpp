#include "controllers/flx10/Flx10InputOwnership.h"

#include <iostream>

namespace {

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

using namespace flx10;

bool testDecode()
{
    bool ok = true;
    auto pcA = decodeInputSelect(0x90, 0x55, 0x7F);
    ok &= require(pcA && pcA->channel == 0 && pcA->position == InputSelectorPosition::PcA,
                  "0x55 = 7F on channel 1 selects PC-A");
    auto line = decodeInputSelect(0x92, 0x56, 0x7F);
    ok &= require(line && line->channel == 2 && line->position == InputSelectorPosition::Line,
                  "0x56 = 7F on channel 3 selects LINE");
    auto pcB = decodeInputSelect(0x93, 0x57, 0x7F);
    ok &= require(pcB && pcB->channel == 3 && pcB->position == InputSelectorPosition::PcB,
                  "0x57 = 7F on channel 4 selects PC-B");
    ok &= require(!decodeInputSelect(0x90, 0x55, 0x00), "0x00 companions carry no position");
    ok &= require(!decodeInputSelect(0x94, 0x55, 0x7F), "channel 5 is not a deck channel");
    ok &= require(!decodeInputSelect(0xB0, 0x55, 0x7F), "CC is not a selector report");
    ok &= require(!decodeInputSelect(0x90, 0x54, 0x7F), "other notes are ignored");
    return ok;
}

bool testResolution()
{
    bool ok = true;
    for (auto position : {InputSelectorPosition::PcA, InputSelectorPosition::PcB,
                          InputSelectorPosition::Unknown}) {
        ok &= require(resolveInputOwnership(position, HostUsbPort::Unknown)
                          == InputOwnership::Unknown,
                      "an unknown USB port never resolves a PC position");
    }
    ok &= require(resolveInputOwnership(InputSelectorPosition::Line, HostUsbPort::Unknown)
                      == InputOwnership::External,
                  "LINE is external even when the USB port is unknown");
    ok &= require(resolveInputOwnership(InputSelectorPosition::PcA, HostUsbPort::A)
                      == InputOwnership::Local, "A host on PC-A is local");
    ok &= require(resolveInputOwnership(InputSelectorPosition::PcB, HostUsbPort::A)
                      == InputOwnership::External, "A host on PC-B is external");
    ok &= require(resolveInputOwnership(InputSelectorPosition::PcB, HostUsbPort::B)
                      == InputOwnership::Local, "B host on PC-B is local");
    ok &= require(resolveInputOwnership(InputSelectorPosition::PcA, HostUsbPort::B)
                      == InputOwnership::External, "B host on PC-A is external");
    ok &= require(resolveInputOwnership(InputSelectorPosition::Unknown, HostUsbPort::A)
                      == InputOwnership::Unknown, "unreported selector stays unknown");
    return ok;
}

bool testTracker()
{
    bool ok = true;
    InputOwnershipTracker tracker;
    ok &= require(tracker.ownership(0) == InputOwnership::Unknown, "starts unknown");
    tracker.setPosition(1, InputSelectorPosition::PcB);
    ok &= require(tracker.ownership(1) == InputOwnership::Unknown, "needs a configured port");
    tracker.setHostPort(hostUsbPortFromSetting(1));
    ok &= require(tracker.ownership(1) == InputOwnership::External, "port change re-resolves");
    ok &= require(tracker.ownership(0) == InputOwnership::Unknown, "channels are independent");
    tracker.reset();
    ok &= require(tracker.ownership(1) == InputOwnership::Unknown, "reset forgets positions");
    ok &= require(tracker.hostPort() == HostUsbPort::A, "reset keeps the configured port");
    ok &= require(tracker.ownership(9) == InputOwnership::Unknown, "out of range is unknown");
    ok &= require(hostUsbPortFromSetting(7) == HostUsbPort::Unknown, "bad setting is unknown");
    return ok;
}

} // namespace

int main()
{
    const bool ok = testDecode() & testResolution() & testTracker();
    return ok ? 0 : 1;
}
