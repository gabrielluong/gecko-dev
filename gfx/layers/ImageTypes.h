/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_IMAGETYPES_H
#define GFX_IMAGETYPES_H

#include <stdint.h>  // for uint32_t
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Variant.h"

namespace mozilla {

enum class ImageFormat {
  /**
   * The PLANAR_YCBCR format creates a PlanarYCbCrImage. All backends should
   * support this format, because the Ogg video decoder depends on it.
   * The maximum image width and height is 16384.
   */
  PLANAR_YCBCR,

  /**
   * The NV_IMAGE format creates a NVImage. The PLANAR_YCBCR together with this
   * complete the YUV format family.
   */
  NV_IMAGE,

  /**
   * The SHARED_RGB format creates a SharedRGBImage, which stores RGB data in
   * shared memory. Some Android hardware video decoders require this format.
   * Currently only used on Android.
   */
  SHARED_RGB,

  /**
   * The MOZ2D_SURFACE format creates a SourceSurfaceImage. All backends should
   * support this format, because video rendering sometimes requires it.
   *
   * This format is useful even though a PaintedLayer could be used.
   * It makes it easy to render a cairo surface when another Image format
   * could be used. It can also avoid copying the surface data in some
   * cases.
   */
  MOZ2D_SURFACE,

  /**
   * A MacIOSurface object.
   */
  MAC_IOSURFACE,

  /**
   * An Android SurfaceTexture ID that can be shared across threads and
   * processes.
   */
  SURFACE_TEXTURE,

  /**
   * The D3D9_RGB32_TEXTURE format creates a D3D9SurfaceImage, and wraps a
   * IDirect3DTexture9 in RGB32 layout.
   */
  D3D9_RGB32_TEXTURE,

  /**
   * An Image type carries an opaque handle once for each stream.
   * The opaque handle would be a platform specific identifier.
   */
  OVERLAY_IMAGE,

  /**
   * A share handle to a ID3D11Texture2D.
   */
  D3D11_SHARE_HANDLE_TEXTURE,

  /**
   * A wrapper of ID3D11Texture2D for zero copy hw video decoding.
   * Expected to be used in GPU process.
   */
  D3D11_TEXTURE_ZERO_COPY,

  /**
   * A wrapper around a drawable TextureClient.
   */
  TEXTURE_WRAPPER,

  /**
   * An opaque handle that refers to an Image stored in the GPU
   * process.
   */
  GPU_VIDEO,

  /**
   * The DMABUF format creates a SharedDMABUFImage, which stores YUV
   * data in DMABUF memory. Used by VAAPI decoder on Linux.
   */
  DMABUF,

  /**
   * A Wrapper of Dcomp surface handle, used by the windows media foundation
   * media engine playback.
   */
  DCOMP_SURFACE,
};

enum class StereoMode {
  MONO,
  LEFT_RIGHT,
  RIGHT_LEFT,
  BOTTOM_TOP,
  TOP_BOTTOM,
  MAX,
};

namespace layers {

using ContainerFrameID = uint32_t;
constexpr ContainerFrameID kContainerFrameID_Invalid = 0;

using ContainerProducerID = uint32_t;
constexpr ContainerProducerID kContainerProducerID_Invalid = 0;

// int64_t represents a WebRTC NTP timestamp.
using ContainerCaptureTime = Variant<Nothing, TimeStamp, int64_t>;

// int64_t represents a WebRTC Realtime timestamp.
using ContainerReceiveTime = Maybe<int64_t>;

using ContainerRtpTimestamp = Maybe<uint32_t>;

}  // namespace layers

}  // namespace mozilla

#endif
