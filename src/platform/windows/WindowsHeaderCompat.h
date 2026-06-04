#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#ifndef STRICT
#define STRICT 1
#endif

#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef _UNICODE
#define _UNICODE 1
#endif

#ifndef UNICODE
#define UNICODE 1
#endif

// This file is force-included for every MSVC translation unit, including
// Qt-generated rcc/moc sources. Keep Windows SDK headers in a stable order:
// windows.h provides the base SDK typedefs, and WIN32_LEAN_AND_MEAN prevents
// it from pulling in legacy winsock.h before winsock2.h.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <iphlpapi.h>

#endif
