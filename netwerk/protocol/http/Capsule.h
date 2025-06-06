/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_capsule_h
#define mozilla_net_capsule_h

#include "mozilla/Variant.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::net {

class CapsuleEncoder;

enum class CapsuleType : uint64_t {
  CLOSE_WEBTRANSPORT_SESSION = 0x2843,
  DRAIN_WEBTRANSPORT_SESSION = 0x78AE,
  PADDING = 0x190b4d38,
  WT_RESET_STREAM = 0x190B4D39,
  WT_STOP_SENDING = 0x190B4D3A,
  WT_STREAM = 0x190B4D3B,
  WT_STREAM_FIN = 0x190B4D3C,
  WT_MAX_DATA = 0x190B4D3D,
  WT_MAX_STREAM_DATA = 0x190B4D3E,
  WT_MAX_STREAMS_BIDI = 0x190B4D3F,
  WT_MAX_STREAMS_UNIDI = 0x190B4D40,
  WT_DATA_BLOCKED = 0x190B4D41,
  WT_STREAM_DATA_BLOCKED = 0x190B4D42,
  WT_STREAMS_BLOCKED_BIDI = 0x190B4D43,
  WT_STREAMS_BLOCKED_UNIDI = 0x190B4D44,
  DATAGRAM = 0x00,
};

struct UnknownCapsule {
  uint64_t mType;
  nsTArray<uint8_t> mData;

  CapsuleType Type() const { return static_cast<CapsuleType>(mType); }
};

struct CloseWebTransportSessionCapsule {
  uint32_t mStatus{0};
  nsCString mReason;

  CapsuleType Type() const { return CapsuleType::CLOSE_WEBTRANSPORT_SESSION; }
};

struct WebTransportMaxDataCapsule {
  uint64_t mMaxDataSize{0};

  CapsuleType Type() const { return CapsuleType::WT_MAX_DATA; }
};

struct WebTransportStreamDataCapsule {
  uint64_t mID{0};
  bool mFin{false};
  nsTArray<uint8_t> mData;

  CapsuleType Type() const {
    return mFin ? CapsuleType::WT_STREAM_FIN : CapsuleType::WT_STREAM;
  }
};

struct WebTransportStreamsBlockedCapsule {
  uint64_t mLimit = 0;
  bool mBidi = true;

  CapsuleType Type() const {
    return mBidi ? CapsuleType::WT_STREAMS_BLOCKED_BIDI
                 : CapsuleType::WT_STREAMS_BLOCKED_UNIDI;
  }
};

struct WebTransportMaxStreamsCapsule {
  uint64_t mLimit = 0;
  bool mBidi = true;

  CapsuleType Type() const {
    return mBidi ? CapsuleType::WT_MAX_STREAMS_BIDI
                 : CapsuleType::WT_MAX_STREAMS_UNIDI;
  }
};

struct WebTransportStreamDataBlockedCapsule {
  uint64_t mLimit = 0;
  uint64_t mID{0};

  CapsuleType Type() const { return CapsuleType::WT_STREAM_DATA_BLOCKED; }
};

struct WebTransportMaxStreamDataCapsule {
  uint64_t mLimit = 0;
  uint64_t mID{0};

  CapsuleType Type() const { return CapsuleType::WT_MAX_STREAM_DATA; }
};

struct WebTransportDataBlockedCapsule {
  uint64_t mLimit = 0;
  uint64_t mID{0};

  CapsuleType Type() const { return CapsuleType::WT_DATA_BLOCKED; }
};

struct WebTransportStopSendingCapsule {
  uint64_t mErrorCode = 0;
  uint64_t mID{0};

  CapsuleType Type() const { return CapsuleType::WT_STOP_SENDING; }
};

struct WebTransportResetStreamCapsule {
  uint64_t mErrorCode = 0;
  uint64_t mReliableSize{0};
  uint64_t mID{0};

  CapsuleType Type() const { return CapsuleType::WT_RESET_STREAM; }
};

struct WebTransportDatagramCapsule {
  nsTArray<uint8_t> mPayload;

  CapsuleType Type() const { return CapsuleType::DATAGRAM; }
};

using CapsuleValue = mozilla::Variant<
    UnknownCapsule, CloseWebTransportSessionCapsule, WebTransportMaxDataCapsule,
    WebTransportStreamDataCapsule, WebTransportStreamsBlockedCapsule,
    WebTransportMaxStreamsCapsule, WebTransportStreamDataBlockedCapsule,
    WebTransportMaxStreamDataCapsule, WebTransportDataBlockedCapsule,
    WebTransportStopSendingCapsule, WebTransportResetStreamCapsule,
    WebTransportDatagramCapsule>;

class Capsule final {
 public:
  static Capsule Unknown(uint64_t aType, nsTArray<uint8_t>&& aData);
  static Capsule CloseWebTransportSession(uint32_t aStatus,
                                          const nsACString& aReason);
  static Capsule WebTransportMaxData(uint64_t aValue);
  static Capsule WebTransportStreamData(uint64_t aID, bool aFin,
                                        nsTArray<uint8_t>&& aData);
  static Capsule WebTransportStreamsBlocked(uint64_t aLimit, bool aBidi);
  static Capsule WebTransportMaxStreams(uint64_t aLimit, bool aBidi);
  static Capsule WebTransportStreamDataBlocked(uint64_t aLimit, uint64_t aID);
  static Capsule WebTransportMaxStreamData(uint64_t aLimit, uint64_t aID);
  static Capsule WebTransportDataBlocked(uint64_t aLimit);
  static Capsule WebTransportStopSending(uint64_t aError, uint64_t aID);
  static Capsule WebTransportResetStream(uint64_t aError, uint64_t aSize,
                                         uint64_t aID);
  static Capsule WebTransportDatagram(nsTArray<uint8_t>&& aData);

  CapsuleType Type() const;

  UnknownCapsule& GetUnknownCapsule() { return mCapsule.as<UnknownCapsule>(); }
  const UnknownCapsule& GetUnknownCapsule() const {
    return mCapsule.as<UnknownCapsule>();
  }
  CloseWebTransportSessionCapsule& GetCloseWebTransportSessionCapsule() {
    return mCapsule.as<CloseWebTransportSessionCapsule>();
  }
  const CloseWebTransportSessionCapsule& GetCloseWebTransportSessionCapsule()
      const {
    return mCapsule.as<CloseWebTransportSessionCapsule>();
  }
  WebTransportMaxDataCapsule& GetWebTransportMaxDataCapsule() {
    return mCapsule.as<WebTransportMaxDataCapsule>();
  }
  const WebTransportMaxDataCapsule& GetWebTransportMaxDataCapsule() const {
    return mCapsule.as<WebTransportMaxDataCapsule>();
  }
  WebTransportStreamDataCapsule& GetWebTransportStreamDataCapsule() {
    return mCapsule.as<WebTransportStreamDataCapsule>();
  }
  const WebTransportStreamDataCapsule& GetWebTransportStreamDataCapsule()
      const {
    return mCapsule.as<WebTransportStreamDataCapsule>();
  }
  WebTransportStreamsBlockedCapsule& GetWebTransportStreamsBlockedCapsule() {
    return mCapsule.as<WebTransportStreamsBlockedCapsule>();
  }
  const WebTransportStreamsBlockedCapsule&
  GetWebTransportStreamsBlockedCapsule() const {
    return mCapsule.as<WebTransportStreamsBlockedCapsule>();
  }
  WebTransportMaxStreamsCapsule& GetWebTransportMaxStreamsCapsule() {
    return mCapsule.as<WebTransportMaxStreamsCapsule>();
  }
  const WebTransportMaxStreamsCapsule& GetWebTransportMaxStreamsCapsule()
      const {
    return mCapsule.as<WebTransportMaxStreamsCapsule>();
  }
  WebTransportStreamDataBlockedCapsule&
  GetWebTransportStreamDataBlockedCapsule() {
    return mCapsule.as<WebTransportStreamDataBlockedCapsule>();
  }
  const WebTransportStreamDataBlockedCapsule&
  GetWebTransportStreamDataBlockedCapsule() const {
    return mCapsule.as<WebTransportStreamDataBlockedCapsule>();
  }
  WebTransportMaxStreamDataCapsule& GetWebTransportMaxStreamDataCapsule() {
    return mCapsule.as<WebTransportMaxStreamDataCapsule>();
  }
  const WebTransportMaxStreamDataCapsule& GetWebTransportMaxStreamDataCapsule()
      const {
    return mCapsule.as<WebTransportMaxStreamDataCapsule>();
  }
  WebTransportDataBlockedCapsule& GetWebTransportDataBlockedCapsule() {
    return mCapsule.as<WebTransportDataBlockedCapsule>();
  }
  const WebTransportDataBlockedCapsule& GetWebTransportDataBlockedCapsule()
      const {
    return mCapsule.as<WebTransportDataBlockedCapsule>();
  }
  WebTransportStopSendingCapsule& GetWebTransportStopSendingCapsule() {
    return mCapsule.as<WebTransportStopSendingCapsule>();
  }
  const WebTransportStopSendingCapsule& GetWebTransportStopSendingCapsule()
      const {
    return mCapsule.as<WebTransportStopSendingCapsule>();
  }
  WebTransportResetStreamCapsule& GetWebTransportResetStreamCapsule() {
    return mCapsule.as<WebTransportResetStreamCapsule>();
  }
  const WebTransportResetStreamCapsule& GetWebTransportResetStreamCapsule()
      const {
    return mCapsule.as<WebTransportResetStreamCapsule>();
  }
  WebTransportDatagramCapsule& GetWebTransportDatagramCapsule() {
    return mCapsule.as<WebTransportDatagramCapsule>();
  }
  const WebTransportDatagramCapsule& GetWebTransportDatagramCapsule() const {
    return mCapsule.as<WebTransportDatagramCapsule>();
  }

  template <typename CapsuleStruct>
  explicit Capsule(CapsuleStruct&& aCapsule) : mCapsule(std::move(aCapsule)) {}

  static void LogBuffer(const uint8_t* aBuffer, uint32_t aLength);

 private:
  Capsule() = default;

  friend class CapsuleEncoder;
  CapsuleValue mCapsule = AsVariant(UnknownCapsule());
};

};  // namespace mozilla::net

#endif
