/*
 * Copyright (C) 2011 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftVPX"
#include <utils/Log.h>

#include "SoftVPX.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>


namespace android {

SoftVPX::SoftVPX(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SoftVideoDecoderOMXComponent(
            name, componentRole, codingType,
            NULL /* profileLevels */, 0 /* numProfileLevels */,
            320 /* width */, 240 /* height */, callbacks, appData, component),
      mMode(codingType == OMX_VIDEO_CodingVP8 ? MODE_VP8 : MODE_VP9),
      mCtx(NULL),
      mImg(NULL) {
    // arbitrary from avc/hevc as vpx does not specify a min compression ratio
    const size_t kMinCompressionRatio = mMode == MODE_VP8 ? 2 : 4;
    const char *mime = mMode == MODE_VP8 ? MEDIA_MIMETYPE_VIDEO_VP8 : MEDIA_MIMETYPE_VIDEO_VP9;
    const size_t kMaxOutputBufferSize = 2048 * 2048 * 3 / 2;
    initPorts(
            kNumBuffers, kMaxOutputBufferSize / kMinCompressionRatio /* inputBufferSize */,
            kNumBuffers, mime, kMinCompressionRatio);
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftVPX::~SoftVPX() {
    vpx_codec_destroy((vpx_codec_ctx_t *)mCtx);
    delete (vpx_codec_ctx_t *)mCtx;
    mCtx = NULL;
}

static int GetCPUCoreCount() {
    int cpuCoreCount = 1;
#if defined(_SC_NPROCESSORS_ONLN)
    cpuCoreCount = sysconf(_SC_NPROCESSORS_ONLN);
#else
    // _SC_NPROC_ONLN must be defined...
    cpuCoreCount = sysconf(_SC_NPROC_ONLN);
#endif
    CHECK(cpuCoreCount >= 1);
    ALOGV("Number of CPU cores: %d", cpuCoreCount);
    return cpuCoreCount;
}

status_t SoftVPX::initDecoder() {
    mCtx = new vpx_codec_ctx_t;
    vpx_codec_err_t vpx_err;
    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof(vpx_codec_dec_cfg_t));
    cfg.threads = GetCPUCoreCount();
    if ((vpx_err = vpx_codec_dec_init(
                (vpx_codec_ctx_t *)mCtx,
                 mMode == MODE_VP8 ? &vpx_codec_vp8_dx_algo : &vpx_codec_vp9_dx_algo,
                 &cfg, 0))) {
        ALOGE("on2 decoder failed to initialize. (%d)", vpx_err);
        return UNKNOWN_ERROR;
    }

    return OK;
}

void SoftVPX::onQueueFilled(OMX_U32 /* portIndex */) {
    if (mOutputPortSettingsChange != NONE) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);
    bool EOSseen = false;

    while (!inQueue.empty() && !outQueue.empty()) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            EOSseen = true;
            if (inHeader->nFilledLen == 0) {
                inQueue.erase(inQueue.begin());
                inInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inHeader);

                outHeader->nFilledLen = 0;
                outHeader->nFlags = OMX_BUFFERFLAG_EOS;

                outQueue.erase(outQueue.begin());
                outInfo->mOwnedByUs = false;
                notifyFillBufferDone(outHeader);
                return;
            }
        }

        if (mImg == NULL) {
            if (vpx_codec_decode(
                        (vpx_codec_ctx_t *)mCtx,
                        inHeader->pBuffer + inHeader->nOffset,
                        inHeader->nFilledLen,
                        NULL,
                        0)) {
                ALOGE("on2 decoder failed to decode frame.");

                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }
            vpx_codec_iter_t iter = NULL;
            mImg = vpx_codec_get_frame((vpx_codec_ctx_t *)mCtx, &iter);
        }

        if (mImg != NULL) {
            CHECK_EQ(mImg->fmt, IMG_FMT_I420);

            uint32_t width = mImg->d_w;
            uint32_t height = mImg->d_h;
            bool portWillReset = false;
            handlePortSettingsChange(&portWillReset, width, height);
            if (portWillReset) {
                return;
            }

            outHeader->nOffset = 0;
            outHeader->nFilledLen = (width * height * 3) / 2;
            outHeader->nFlags = EOSseen ? OMX_BUFFERFLAG_EOS : 0;
            outHeader->nTimeStamp = inHeader->nTimeStamp;

            uint8_t *dst = outHeader->pBuffer;
            const uint8_t *srcY = (const uint8_t *)mImg->planes[PLANE_Y];
            const uint8_t *srcU = (const uint8_t *)mImg->planes[PLANE_U];
            const uint8_t *srcV = (const uint8_t *)mImg->planes[PLANE_V];
            size_t srcYStride = mImg->stride[PLANE_Y];
            size_t srcUStride = mImg->stride[PLANE_U];
            size_t srcVStride = mImg->stride[PLANE_V];
            copyYV12FrameToOutputBuffer(dst, srcY, srcU, srcV, srcYStride, srcUStride, srcVStride);

            mImg = NULL;
            outInfo->mOwnedByUs = false;
            outQueue.erase(outQueue.begin());
            outInfo = NULL;
            notifyFillBufferDone(outHeader);
            outHeader = NULL;
        }

        inInfo->mOwnedByUs = false;
        inQueue.erase(inQueue.begin());
        inInfo = NULL;
        notifyEmptyBufferDone(inHeader);
        inHeader = NULL;
    }
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    if (!strcmp(name, "OMX.google.vp8.decoder")) {
        return new android::SoftVPX(
                name, "video_decoder.vp8", OMX_VIDEO_CodingVP8,
                callbacks, appData, component);
    } else if (!strcmp(name, "OMX.google.vp9.decoder")) {
        return new android::SoftVPX(
                name, "video_decoder.vp9", OMX_VIDEO_CodingVP9,
                callbacks, appData, component);
    } else {
        CHECK(!"Unknown component");
    }
    return NULL;
}
