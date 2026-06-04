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

// Keep this header macro-only. Pulling Windows SDK headers into every Qt/JUCE
// generated translation unit can break winsock type declarations on MSVC.

#endif
