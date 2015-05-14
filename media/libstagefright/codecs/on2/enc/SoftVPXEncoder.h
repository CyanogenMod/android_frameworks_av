/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SOFT_VPX_ENCODER_H_

#define SOFT_VPX_ENCODER_H_

#include "SoftVideoEncoderOMXComponent.h"

#include <OMX_VideoExt.h>
#include <OMX_IndexExt.h>

#include <hardware/gralloc.h>

#include "vpx/vpx_encoder.h"
#include "vpx/vpx_codec.h"
#include "vpx/vp8cx.h"

namespace android {

// Exposes a vpx encoder as an OMX Component
//
// Boilerplate for callback bindings are taken care
// by the base class SimpleSoftOMXComponent and its
// parent SoftOMXComponent.
//
// Only following encoder settings are available
//    - target bitrate
//    - rate control (constant / variable)
//    - frame rate
//    - error resilience
//    - token partitioning
//    - reconstruction & loop filters (g_profile)
//
// Only following color formats are recognized
//    - YUV420Planar
//    - YUV420SemiPlanar
//    - AndroidOpaque
//
// Following settings are not configurable by the client
//    - encoding deadline is realtime
//    - multithreaded encoding utilizes a number of threads equal
// to online cpu's available
//    - the algorithm interface for encoder is vp8
//    - fractional bits of frame rate is discarded
//    - OMX timestamps are in microseconds, therefore
// encoder timebase is fixed to 1/1000000

struct SoftVPXEncoder : public SoftVideoEncoderOMXComponent {
    SoftVPXEncoder(const char *name,
                   const OMX_CALLBACKTYPE *callbacks,
                   OMX_PTR appData,
                   OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftVPXEncoder();

    // Returns current values for requested OMX
    // parameters
    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR param);

    // Validates, extracts and stores relevant OMX
    // parameters
    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR param);

    virtual OMX_ERRORTYPE setConfig(
            OMX_INDEXTYPE index, const OMX_PTR params);

    // OMX callback when buffers available
    // Note that both an input and output buffer
    // is expected to be available to carry out
    // encoding of the frame
    virtual void onQueueFilled(OMX_U32 portIndex);

private:
    enum TemporalReferences {
        // For 1 layer case: reference all (last, golden, and alt ref), but only
        // update last.
        kTemporalUpdateLastRefAll = 12,
        // First base layer frame for 3 temporal layers, which updates last and
        // golden with alt ref dependency.
        kTemporalUpdateLastAndGoldenRefAltRef = 11,
        // First enhancement layer with alt ref dependency.
        kTemporalUpdateGoldenRefAltRef = 10,
        // First enhancement layer with alt ref dependency.
        kTemporalUpdateGoldenWithoutDependencyRefAltRef = 9,
        // Base layer with alt ref dependency.
        kTemporalUpdateLastRefAltRef = 8,
        // Highest enhacement layer without dependency on golden with alt ref
        // dependency.
        kTemporalUpdateNoneNoRefGoldenRefAltRef = 7,
        // Second layer and last frame in cycle, for 2 layers.
        kTemporalUpdateNoneNoRefAltref = 6,
        // Highest enhancement layer.
        kTemporalUpdateNone = 5,
        // Second enhancement layer.
        kTemporalUpdateAltref = 4,
        // Second enhancement layer without dependency on previous frames in
        // the second enhancement layer.
        kTemporalUpdateAltrefWithoutDependency = 3,
        // First enhancement layer.
        kTemporalUpdateGolden = 2,
        // First enhancement layer without dependency on previous frames in
        // the first enhancement layer.
        kTemporalUpdateGoldenWithoutDependency = 1,
        // Base layer.
        kTemporalUpdateLast = 0,
    };
    enum {
        kMaxTemporalPattern = 8
    };

    // number of buffers allocated per port
    static const uint32_t kNumBuffers = 4;

    // OMX port indexes that refer to input and
    // output ports respectively
    static const uint32_t kInputPortIndex = 0;
    static const uint32_t kOutputPortIndex = 1;

    // Byte-alignment required for buffers
    static const uint32_t kInputBufferAlignment = 1;
    static const uint32_t kOutputBufferAlignment = 2;

    // Max value supported for DCT partitions
    static const uint32_t kMaxDCTPartitions = 3;

    // Number of supported input color formats
    static const uint32_t kNumberOfSupportedColorFormats = 3;

    // vpx specific opaque data structure that
    // stores encoder state
    vpx_codec_ctx_t* mCodecContext;

    // vpx specific data structure that
    // stores encoder configuration
    vpx_codec_enc_cfg_t* mCodecConfiguration;

    // vpx specific read-only data structure
    // that specifies algorithm interface (e.g. vp8)
    vpx_codec_iface_t* mCodecInterface;

    // If a request for a change it bitrate has been received.
    bool mBitrateUpdated;

    // Bitrate control mode, either constant or variable
    vpx_rc_mode mBitrateControlMode;

    // vp8 specific configuration parameter
    // that enables token partitioning of
    // the stream into substreams
    int32_t mDCTPartitions;

    // Parameter that denotes whether error resilience
    // is enabled in encoder
    OMX_BOOL mErrorResilience;

    // Encoder profile corresponding to OMX level parameter
    //
    // The inconsistency in the naming is caused by
    // OMX spec referring vpx profiles (g_profile)
    // as "levels" whereas using the name "profile" for
    // something else.
    OMX_VIDEO_VP8LEVELTYPE mLevel;

    // Key frame interval in frames
    uint32_t mKeyFrameInterval;

    // Minimum (best quality) quantizer
    uint32_t mMinQuantizer;

    // Maximum (worst quality) quantizer
    uint32_t mMaxQuantizer;

    // Number of coding temporal layers to be used.
    size_t mTemporalLayers;

    // Temporal layer bitrare ratio in percentage
    uint32_t mTemporalLayerBitrateRatio[OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS];

    // Temporal pattern type
    OMX_VIDEO_ANDROID_VPXTEMPORALLAYERPATTERNTYPE mTemporalPatternType;

    // Temporal pattern length
    size_t mTemporalPatternLength;

    // Temporal pattern current index
    size_t mTemporalPatternIdx;

    // Frame type temporal pattern
    TemporalReferences mTemporalPattern[kMaxTemporalPattern];

    // Last input buffer timestamp
    OMX_TICKS mLastTimestamp;

    // Conversion buffer is needed to convert semi
    // planar yuv420 to planar format
    // It is only allocated if input format is
    // indeed YUV420SemiPlanar.
    uint8_t* mConversionBuffer;

    bool mKeyFrameRequested;

    // Initializes vpx encoder with available settings.
    status_t initEncoder();

    // Releases vpx encoder instance, with it's associated
    // data structures.
    //
    // Unless called earlier, this is handled by the
    // dtor.
    status_t releaseEncoder();

    // Get current encode flags
    vpx_enc_frame_flags_t getEncodeFlags();

    // Updates bitrate to reflect port settings.
    OMX_ERRORTYPE internalSetBitrateParams(
        const OMX_VIDEO_PARAM_BITRATETYPE* bitrate);

    // Handles vp8 specific parameters.
    OMX_ERRORTYPE internalSetVp8Params(
        const OMX_VIDEO_PARAM_VP8TYPE* vp8Params);

    // Handles Android vp8 specific parameters.
    OMX_ERRORTYPE internalSetAndroidVp8Params(
        const OMX_VIDEO_PARAM_ANDROID_VP8ENCODERTYPE* vp8AndroidParams);

    DISALLOW_EVIL_CONSTRUCTORS(SoftVPXEncoder);
};

}  // namespace android

#endif  // SOFT_VPX_ENCODER_H_
