/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_UniFFIRust_h
#define mozilla_UniFFIRust_h

#include <stdint.h>

namespace mozilla::uniffi {

// Low-level Rust structs for UniFFI

// RustCallStatus.code values
constexpr int8_t RUST_CALL_SUCCESS = 0;
constexpr int8_t RUST_CALL_ERROR = 1;
constexpr int8_t RUST_CALL_INTERNAL_ERROR = 2;

// Future continutation code values
constexpr int8_t UNIFFI_FUTURE_READY = 0;
constexpr int8_t UNIFFI_FUTURE_MAYBE_READY = 1;

// Return values for callback interfaces (See
// https://github.com/mozilla/uniffi-rs/blob/main/uniffi_core/src/ffi/foreigncallbacks.rs
// for details)
constexpr int8_t CALLBACK_INTERFACE_SUCCESS = 0;
constexpr int8_t CALLBACK_INTERFACE_ERROR = 1;
constexpr int8_t CALLBACK_INTERFACE_UNEXPECTED_ERROR = 2;

// structs/functions from UniFFI
extern "C" {
struct RustBuffer {
  uint64_t capacity;
  uint64_t len;
  uint8_t* data;
};

struct ForeignBytes {
  int32_t len;
  uint8_t* data;
};

struct RustCallStatus {
  int8_t code;
  RustBuffer error_buf;
};

typedef int (*ForeignCallback)(uint64_t handle, uint32_t method,
                               const uint8_t* argsData, int32_t argsLen,
                               RustBuffer* buf_ptr);

typedef void (*FutureCallback)(uint64_t futureHandle, int8_t code);
// Poll a future.
//
// If the future is ready, then `futureCallback` will be called with
// `UNIFFI_FUTURE_READY` If the future is pending, then `futureCallback` will be
// called with `UNIFFI_FUTURE_MAYBE_READY` when the Rust waker is invoked.
typedef void (*PollFutureFn)(uint64_t futureHandle, FutureCallback callback,
                             uint64_t callback_data);
// Free a future
typedef void (*FreeFutureFn)(uint64_t futureHandle);

RustBuffer uniffi_rustbuffer_alloc(uint64_t size, RustCallStatus* call_status);
void uniffi_rustbuffer_free(RustBuffer buf, RustCallStatus* call_status);
}

}  // namespace mozilla::uniffi

#endif /* mozilla_UniFFIRust_h */
