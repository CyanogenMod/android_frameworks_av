/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef SOFT_HEVC_H_

#define SOFT_HEVC_H_

#include "SoftVideoDecoderOMXComponent.h"
#include <sys/time.h>

namespace android {

#define ivd_aligned_malloc(alignment, size) memalign(alignment, size)
#define ivd_aligned_free(buf) free(buf)

/** Number of entries in the time-stamp array */
#define MAX_TIME_STAMPS 64

/** Maximum number of cores supported by the codec */
#define CODEC_MAX_NUM_CORES 4

#define CODEC_MAX_WIDTH     1920

#define CODEC_MAX_HEIGHT    1088

/** Input buffer size */
#define INPUT_BUF_SIZE (1024 * 1024)

#define MIN(a, b) ((a) < (b)) ? (a) : (b)

/** Used to remove warnings about unused parameters */
#define UNUSED(x) ((void)(x))

/** Get time */
#define GETTIME(a, b) gettimeofday(a, b);

/** Compute difference between start and end */
#define TIME_DIFF(start, end, diff) \
    diff = ((end.tv_sec - start.tv_sec) * 1000000) + \
            (end.tv_usec - start.tv_usec);

struct SoftHEVC: public SoftVideoDecoderOMXComponent {
    SoftHEVC(const char *name, const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData, OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftHEVC();

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onReset();
private:
    // Number of input and output buffers
    enum {
        kNumBuffers = 8
    };

    iv_obj_t *mCodecCtx;         // Codec context
    iv_mem_rec_t *mMemRecords;   // Memory records requested by the codec
    size_t mNumMemRecords;       // Number of memory records requested by the codec

    uint32_t mNewWidth;          // New width after change in resolution
    uint32_t mNewHeight;         // New height after change in resolution
    uint32_t mInitWidth;         // Width used during codec creation
    uint32_t mInitHeight;        // Height used during codec creation
    size_t mStride;              // Stride to be used for display buffers

    size_t mNumCores;            // Number of cores to be uesd by the codec

    struct timeval mTimeStart;   // Time at the start of decode()
    struct timeval mTimeEnd;     // Time at the end of decode()

    // Internal buffer to be used to flush out the buffers from decoder
    uint8_t *mFlushOutBuffer;

    // Status of entries in the timestamp array
    bool mTimeStampsValid[MAX_TIME_STAMPS];

    // Timestamp array - Since codec does not take 64 bit timestamps,
    // they are maintained in the plugin
    OMX_S64 mTimeStamps[MAX_TIME_STAMPS];

    OMX_COLOR_FORMATTYPE mOmxColorFormat;    // OMX Color format
    IV_COLOR_FORMAT_T mIvColorFormat;        // Ittiam Color format

    bool mIsInFlush;        // codec is flush mode
    bool mReceivedEOS;      // EOS is receieved on input port
    bool mIsAdapting;       // plugin in middle of change in resolution

    status_t initDecoder();
    status_t deInitDecoder();
    status_t setFlushMode();
    status_t setParams(size_t stride);
    void logVersion();
    status_t setNumCores();
    status_t resetDecoder();
    status_t resetPlugin();
    status_t reInitDecoder();

    void setDecodeArgs(ivd_video_decode_ip_t *ps_dec_ip,
        ivd_video_decode_op_t *ps_dec_op,
        OMX_BUFFERHEADERTYPE *inHeader,
        OMX_BUFFERHEADERTYPE *outHeader,
        size_t sizeY,
        size_t timeStampIx);

    DISALLOW_EVIL_CONSTRUCTORS (SoftHEVC);
};

} // namespace android

#endif  // SOFT_HEVC_H_
