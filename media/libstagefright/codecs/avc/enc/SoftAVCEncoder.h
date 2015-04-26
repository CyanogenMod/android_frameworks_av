/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef SOFT_AVC_ENCODER_H_
#define SOFT_AVC_ENCODER_H_

#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/foundation/ABase.h>
#include <utils/Vector.h>

#include "avcenc_api.h"
#include "SoftVideoEncoderOMXComponent.h"

namespace android {

struct MediaBuffer;

struct SoftAVCEncoder : public MediaBufferObserver,
                        public SoftVideoEncoderOMXComponent {
    SoftAVCEncoder(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

    // Override SimpleSoftOMXComponent methods
    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);

    // Implement MediaBufferObserver
    virtual void signalBufferReturned(MediaBuffer *buffer);


    // Callbacks required by PV's encoder
    int32_t allocOutputBuffers(unsigned int sizeInMbs, unsigned int numBuffers);
    void    unbindOutputBuffer(int32_t index);
    int32_t bindOutputBuffer(int32_t index, uint8_t **yuv);

protected:
    virtual ~SoftAVCEncoder();

private:
    enum {
        kNumBuffers = 2,
    };

    // OMX input buffer's timestamp and flags
    typedef struct {
        int64_t mTimeUs;
        int32_t mFlags;
    } InputBufferInfo;

    int32_t  mIDRFrameRefreshIntervalInSec;
    AVCProfile mAVCEncProfile;
    AVCLevel   mAVCEncLevel;

    int64_t  mNumInputFrames;
    int64_t  mPrevTimestampUs;
    bool     mStarted;
    bool     mSpsPpsHeaderReceived;
    bool     mReadyForNextFrame;
    bool     mSawInputEOS;
    bool     mSignalledError;
    bool     mIsIDRFrame;

    tagAVCHandle          *mHandle;
    tagAVCEncParam        *mEncParams;
    uint8_t               *mInputFrameData;
    uint32_t              *mSliceGroup;
    Vector<MediaBuffer *> mOutputBuffers;
    Vector<InputBufferInfo> mInputBufferInfoVec;

    OMX_ERRORTYPE initEncParams();
    OMX_ERRORTYPE initEncoder();
    OMX_ERRORTYPE releaseEncoder();
    void releaseOutputBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(SoftAVCEncoder);
};

}  // namespace android

#endif  // SOFT_AVC_ENCODER_H_
