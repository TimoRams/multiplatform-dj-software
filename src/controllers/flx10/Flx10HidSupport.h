#pragma once

#include <QtGlobal>

#if defined(Q_OS_MACOS) || (defined(BROCKDJ_HAS_LIBUSB) && defined(Q_OS_LINUX))
#define BROCKDJ_FLX10_HAS_HID 1
#endif

namespace flx10 {
inline constexpr bool kHidTransportAvailable =
#if defined(BROCKDJ_FLX10_HAS_HID)
    true;
#else
    false;
#endif
}
