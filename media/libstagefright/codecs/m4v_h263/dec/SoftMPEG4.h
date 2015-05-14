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

#ifndef SOFT_MPEG4_H_

#define SOFT_MPEG4_H_

#include "SoftVideoDecoderOMXComponent.h"

struct tagvideoDecControls;

namespace android {

struct SoftMPEG4 : public SoftVideoDecoderOMXComponent {
    SoftMPEG4(const char *name,
            const char *componentRole,
            OMX_VIDEO_CODINGTYPE codingType,
            const CodecProfileLevel *profileLevels,
            size_t numProfileLevels,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftMPEG4();

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onReset();

private:
    enum {
        kNumInputBuffers  = 4,
        kNumOutputBuffers = 2,
    };

    enum {
        MODE_MPEG4,
        MODE_H263,
    } mMode;

    tagvideoDecControls *mHandle;

    size_t mInputBufferCount;

    bool mSignalledError;
    bool mInitialized;
    bool mFramesConfigured;

    int32_t mNumSamplesOutput;
    int32_t mPvTime;
    KeyedVector<int32_t, OMX_TICKS> mPvToOmxTimeMap;

    status_t initDecoder();

    virtual void updatePortDefinitions(bool updateCrop = true, bool updateInputSize = false);
    bool handlePortSettingsChange();

    DISALLOW_EVIL_CONSTRUCTORS(SoftMPEG4);
};

}  // namespace android

#endif  // SOFT_MPEG4_H_


