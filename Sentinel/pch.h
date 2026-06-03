#pragma once

// These three defines MUST appear before any Windows headers.
// Without WIN32_LEAN_AND_MEAN, windows.h pulls in the old winsock.h which
// collides with winsock2.h. NOMINMAX prevents windows.h from clobbering
// std::min/std::max. SECURITY_WIN32 is required by schannel.h / sspi.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif