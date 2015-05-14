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

#ifndef SOFT_AVC_H_

#define SOFT_AVC_H_

#include "SoftVideoDecoderOMXComponent.h"
#include <utils/KeyedVector.h>

#include "H264SwDecApi.h"
#include "basetype.h"

namespace android {

struct SoftAVC : public SoftVideoDecoderOMXComponent {
    SoftAVC(const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftAVC();

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onReset();

private:
    enum {
        kNumInputBuffers  = 8,
        kNumOutputBuffers = 2,
    };

    enum EOSStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED,
    };

    void *mHandle;

    size_t mInputBufferCount;

    uint8_t *mFirstPicture;
    int32_t mFirstPictureId;

    int32_t mPicId;  // Which output picture is for which input buffer?

    // OMX_BUFFERHEADERTYPE may be overkill, but it is convenient
    // for tracking the following fields: nFlags, nTimeStamp, etc.
    KeyedVector<int32_t, OMX_BUFFERHEADERTYPE *> mPicToHeaderMap;
    bool mHeadersDecoded;

    EOSStatus mEOSStatus;

    bool mSignalledError;

    status_t initDecoder();
    void drainAllOutputBuffers(bool eos);
    void drainOneOutputBuffer(int32_t picId, uint8_t *data);
    void saveFirstOutputBuffer(int32_t pidId, uint8_t *data);
    CropSettingsMode handleCropParams(const H264SwDecInfo& decInfo);

    DISALLOW_EVIL_CONSTRUCTORS(SoftAVC);
};

}  // namespace android

#endif  // SOFT_AVC_H_

