/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_SandboxLogging_h
#define mozilla_SandboxLogging_h

// This header defines the SANDBOX_LOG macro used in the Linux
// sandboxing code.  It uses Android logging on Android and writes to
// stderr otherwise.  Android logging has severity levels; currently
// only "error" severity is exposed here, and this isn't marked when
// writing to stderr.
//
// The format strings are processed by Chromium SafeSPrintf, which
// doesn't accept size modifiers or %u because it uses C++11 variadic
// templates to obtain the actual argument types; all decimal integer
// formatting uses %d.  See safe_sprintf.h for more details.

// Build SafeSPrintf without assertions to avoid a dependency on
// Chromium logging.  This doesn't affect safety; it just means that
// type mismatches (pointer vs. integer) always result in unexpanded
// %-directives instead of crashing.  See also the moz.build files,
// which apply NDEBUG to the .cc file.
#ifndef NDEBUG
#  define NDEBUG 1
#  include "base/strings/safe_sprintf.h"
#  undef NDEBUG
#else
#  include "base/strings/safe_sprintf.h"
#endif

#include <errno.h>

#include "SandboxProfiler.h"

namespace mozilla {
// Logs the formatted string (marked with "error" severity, if supported).
void SandboxLogError(const char* aMessage);

// Writes into aBuf the identifier for an error number (e.g., "EINVAL"
// rather than "Invalid argument"); may fall back to "error N" (with
// the number) for unhandled errors.
//
// Bounds are handled like snprintf: the return value is the length
// the string would have (not counting the null terminator) ignoring
// buffer size, and the string written into the buffer may be
// truncated to fit but is always null terminated.
ssize_t GetLibcErrorName(char* aBuf, size_t aSize, int aErr);
}  // namespace mozilla

#define SANDBOX_LOG_LEN 256

// Formats a log message and logs it (with "error" severity, if supported).
//
// Note that SafeSPrintf doesn't accept size modifiers or %u; all
// decimal integers are %d, because it uses C++11 variadic templates
// to use the actual argument type.
#define SANDBOX_LOG(fmt, args...)                              \
  do {                                                         \
    char _sandboxLogBuf[SANDBOX_LOG_LEN];                      \
    ::base::strings::SafeSPrintf(_sandboxLogBuf, fmt, ##args); \
    ::mozilla::SandboxLogError(_sandboxLogBuf);                \
    ::mozilla::SandboxProfiler::ReportLog(_sandboxLogBuf);     \
  } while (0)

#define SANDBOX_LOG_WITH_ERROR(errnum, fmt, args...)                       \
  do {                                                                     \
    char _sandboxLogBuf[SANDBOX_LOG_LEN];                                  \
    ssize_t _sandboxLogOff =                                               \
        ::base::strings::SafeSPrintf(_sandboxLogBuf, fmt ": ", ##args);    \
    if (static_cast<size_t>(_sandboxLogOff) < sizeof(_sandboxLogBuf)) {    \
      ::mozilla::GetLibcErrorName(_sandboxLogBuf + _sandboxLogOff,         \
                                  sizeof(_sandboxLogBuf) - _sandboxLogOff, \
                                  errnum);                                 \
    }                                                                      \
    ::mozilla::SandboxLogError(_sandboxLogBuf);                            \
    ::mozilla::SandboxProfiler::ReportLog(_sandboxLogBuf);                 \
  } while (0)

#define SANDBOX_LOG_ERRNO(fmt, args...) \
  SANDBOX_LOG_WITH_ERROR(errno, fmt, ##args)

#endif  // mozilla_SandboxLogging_h
