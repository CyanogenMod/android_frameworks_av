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

#include "M4OSA_Error.h"
#include "M4OSA_Types.h"
#include "M4OSA_Memory.h"
#include "M4OSA_Export.h"
#include "M4OSA_CoreID.h"

namespace android{

#define WINDOW_SIZE 10

enum veAudioFormat {MONO_16_BIT, STEREO_16_BIT};


typedef struct {
    M4OSA_UInt16*   m_dataAddress; // Android SRC needs a Int16 pointer
    M4OSA_UInt32    m_bufferSize;
} M4AM_Buffer16;    // Structure contains Int16_t pointer

// Following struct will be used by app to supply the PT and BT properties
// along with ducking values
typedef struct {
    M4OSA_Int32 lvInSampleRate; // Sampling audio freq (8000,16000 or more )
    M4OSA_Int32 lvOutSampleRate; //Sampling audio freq (8000,16000 or more )
    veAudioFormat lvBTFormat;

    M4OSA_Int32 lvInDucking_threshold;
    M4OSA_Float lvInDucking_lowVolume;
    M4OSA_Bool lvInDucking_enable;
    M4OSA_Float lvPTVolLevel;
    M4OSA_Float lvBTVolLevel;
    M4OSA_Int32 lvBTChannelCount;
    M4OSA_Int32 lvPTChannelCount;
} veAudMixSettings;

// This class is defined to get SF SRC access
class VideoEditorBGAudioProcessing {
public:
    VideoEditorBGAudioProcessing();
    void veSetAudioProcessingParams(const veAudMixSettings& mixParams);

    M4OSA_Int32 veProcessAudioMixNDuck(
                    void* primaryTrackBuffer,
                    void* backgroundTrackBuffer,
                    void* mixedOutputBuffer);

    ~VideoEditorBGAudioProcessing();

private:
    M4OSA_Int32 mInSampleRate;
    M4OSA_Int32 mOutSampleRate;
    veAudioFormat mBTFormat;

    M4OSA_Bool mIsSSRCneeded;
    M4OSA_Int32 mBTChannelCount;
    M4OSA_Int32 mPTChannelCount;
    M4OSA_UInt8 mChannelConversion;

    M4OSA_UInt32 mDucking_threshold;
    M4OSA_Float mDucking_lowVolume;
    M4OSA_Float mDuckingFactor ;
    M4OSA_Bool mDucking_enable;
    M4OSA_Int32 mAudioVolumeArray[WINDOW_SIZE];
    M4OSA_Int32 mAudVolArrIndex;
    M4OSA_Bool mDoDucking;
    M4OSA_Float mPTVolLevel;
    M4OSA_Float mBTVolLevel;

    M4AM_Buffer16 mBTBuffer;

    M4OSA_Int32 getDecibelSound(M4OSA_UInt32 value);
    M4OSA_Bool  isThresholdBreached(M4OSA_Int32* averageValue,
                    M4OSA_Int32 storeCount, M4OSA_Int32 thresholdValue);

    // This returns the size of buffer which needs to allocated
    // before resampling is called
    M4OSA_Int32 calculateOutResampleBufSize();
};
} // namespace android
