/*
 * Copyright (C) 2011 NXP Software
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

#define LOG_NDEBUG 1
#define LOG_TAG "VideoEditorBGAudioProcessing"
#include <utils/Log.h>
#include "VideoEditorBGAudioProcessing.h"

namespace android {

VideoEditorBGAudioProcessing ::VideoEditorBGAudioProcessing() {

    LOGV("VideoEditorBGAudioProcessing:: Construct  VideoEditorBGAudioProcessing ");

    VideoEditorBGAudioProcessing::mAudVolArrIndex = 0;
    VideoEditorBGAudioProcessing::mDoDucking = 0;
    VideoEditorBGAudioProcessing::mDucking_enable = 0;
    VideoEditorBGAudioProcessing::mDucking_lowVolume = 0;
    VideoEditorBGAudioProcessing::mDucking_threshold = 0;
    VideoEditorBGAudioProcessing::mDuckingFactor = 0;

    VideoEditorBGAudioProcessing::mBTVolLevel = 0;
    VideoEditorBGAudioProcessing::mPTVolLevel = 0;

    VideoEditorBGAudioProcessing::mIsSSRCneeded = 0;
    VideoEditorBGAudioProcessing::mChannelConversion = 0;

    VideoEditorBGAudioProcessing::mBTFormat = MONO_16_BIT;

    VideoEditorBGAudioProcessing::mInSampleRate = 8000;
    VideoEditorBGAudioProcessing::mOutSampleRate = 16000;
    VideoEditorBGAudioProcessing::mPTChannelCount = 2;
    VideoEditorBGAudioProcessing::mBTChannelCount = 1;
}

M4OSA_Int32 VideoEditorBGAudioProcessing::veProcessAudioMixNDuck(
        void *pPTBuffer, void *pBTBuffer, void *pOutBuffer) {

    M4AM_Buffer16* pPrimaryTrack   = (M4AM_Buffer16*)pPTBuffer;
    M4AM_Buffer16* pBackgroundTrack = (M4AM_Buffer16*)pBTBuffer;
    M4AM_Buffer16* pMixedOutBuffer  = (M4AM_Buffer16*)pOutBuffer;

    LOGV("VideoEditorBGAudioProcessing::lvProcessAudioMixNDuck \
     pPTBuffer 0x%x pBTBuffer 0x%x pOutBuffer 0x%x", pPTBuffer,
      pBTBuffer, pOutBuffer);

    M4OSA_ERR result = M4NO_ERROR;
    M4OSA_Int16 *pBTMdata1;
    M4OSA_Int16 *pPTMdata2;
    M4OSA_UInt32 uiPCMsize;

    // Ducking variable
    M4OSA_UInt16 loopIndex = 0;
    M4OSA_Int16 *pPCM16Sample = M4OSA_NULL;
    M4OSA_Int32 peakDbValue = 0;
    M4OSA_Int32 previousDbValue = 0;
    M4OSA_UInt32 i;

    // Output size if same as PT size
    pMixedOutBuffer->m_bufferSize = pPrimaryTrack->m_bufferSize;

    // Before mixing, we need to have only PT as out buffer
    M4OSA_memcpy((M4OSA_MemAddr8)pMixedOutBuffer->m_dataAddress,
     (M4OSA_MemAddr8)pPrimaryTrack->m_dataAddress, pMixedOutBuffer->m_bufferSize);

    // Initially contains the input primary track
    pPTMdata2 = (M4OSA_Int16*)pMixedOutBuffer->m_dataAddress;
    // Contains BG track processed data(like channel conversion etc..
    pBTMdata1 = (M4OSA_Int16*) pBackgroundTrack->m_dataAddress;

    // Since we need to give sample count and not buffer size
    uiPCMsize = pMixedOutBuffer->m_bufferSize/2 ;

    if((this->mDucking_enable) && (this->mPTVolLevel != 0.0)) {
        // LOGI("VideoEditorBGAudioProcessing:: In Ducking analysis ");
        loopIndex = 0;
        peakDbValue = 0;
        previousDbValue = peakDbValue;

        pPCM16Sample = (M4OSA_Int16*)pPrimaryTrack->m_dataAddress;

        while( loopIndex < pPrimaryTrack->m_bufferSize/sizeof(M4OSA_Int16))
        {
            if (pPCM16Sample[loopIndex] >= 0){
                peakDbValue = previousDbValue > pPCM16Sample[loopIndex] ?
                 previousDbValue : pPCM16Sample[loopIndex];
                previousDbValue = peakDbValue;
            }else{
                peakDbValue = previousDbValue > -pPCM16Sample[loopIndex] ?
                 previousDbValue: -pPCM16Sample[loopIndex];
                previousDbValue = peakDbValue;
            }
            loopIndex++;
        }

        this->mAudioVolumeArray[this->mAudVolArrIndex] =
         getDecibelSound(peakDbValue);

        LOGV("VideoEditorBGAudioProcessing:: getDecibelSound %d",
         this->mAudioVolumeArray[this->mAudVolArrIndex]);

        // WINDOW_SIZE is 10 by default
        // Check for threshold is done after 10 cycles
        if ( this->mAudVolArrIndex >= WINDOW_SIZE -1) {
            this->mDoDucking = isThresholdBreached(this->mAudioVolumeArray,
             this->mAudVolArrIndex,this->mDucking_threshold );

            this->mAudVolArrIndex = 0;
        } else {
            this->mAudVolArrIndex++;
        }

        //
        // Below logic controls the mixing weightage
        // for Background and Primary Tracks
        // for the duration of window under analysis,
        // to give fade-out for Background and fade-in for primary
        // Current fading factor is distributed in equal range over
        // the defined window size.
        // For a window size = 25
        // (500 ms (window under analysis) / 20 ms (sample duration))
        //

        if(this->mDoDucking){
            if ( this->mDuckingFactor > this->mDucking_lowVolume) {
                // FADE OUT BG Track
                // Increment ducking factor in total steps in factor
                // of low volume steps to reach low volume level
                this->mDuckingFactor -= (this->mDucking_lowVolume);
            }
            else {
                this->mDuckingFactor = this->mDucking_lowVolume;
            }
        } else {
            if ( this->mDuckingFactor < 1.0 ){
                // FADE IN BG Track
                // Increment ducking factor in total steps of
                // low volume factor to reach orig.volume level
                this->mDuckingFactor += (this->mDucking_lowVolume);
            }
            else{
                this->mDuckingFactor = 1.0;
            }
        }
    } // end if - mDucking_enable


    // Mixing Logic

    LOGV("VideoEditorBGAudioProcessing:: Out of Ducking analysis uiPCMsize\
     %d %f %f", this->mDoDucking, this->mDuckingFactor,this->mBTVolLevel);

    while(uiPCMsize-->0) {

        M4OSA_Int32 temp;
        // Set vol factor for BT and PT
        *pBTMdata1 = (M4OSA_Int16)(*pBTMdata1*this->mBTVolLevel);
        *pPTMdata2 = (M4OSA_Int16)(*pPTMdata2*this->mPTVolLevel);

        // Mix the two samples
        if ( this->mDoDucking) {

            // Duck the BG track to ducking factor value before mixing
            *pBTMdata1 = (M4OSA_Int16)((*pBTMdata1)*(this->mDuckingFactor));

            // mix as normal case
            *pBTMdata1 = (M4OSA_Int16)(*pBTMdata1 /2 + *pPTMdata2 /2);
        }
        else {

            *pBTMdata1 = (M4OSA_Int16)((*pBTMdata1)*(this->mDuckingFactor));
            *pBTMdata1 = (M4OSA_Int16)(*pBTMdata1 /2 + *pPTMdata2 /2);
        }

        if ( *pBTMdata1 < 0) {
            temp = -(*pBTMdata1) * 2; // bring to original Amplitude level

            if ( temp > 32767) {
                *pBTMdata1 = -32766; // less then max allowed value
            }
            else{
                *pBTMdata1 = (M4OSA_Int16)(-temp);
            }
        }
        else {
            temp = (*pBTMdata1) * 2; // bring to original Amplitude level
            if ( temp > 32768) {
                *pBTMdata1 = 32767; // less than max allowed value
            }
            else {
                *pBTMdata1 = (M4OSA_Int16)temp;
            }
        }

        pBTMdata1++;
        pPTMdata2++;
    }
    //LOGV("VideoEditorBGAudioProcessing:: Copy final out ");
    M4OSA_memcpy((M4OSA_MemAddr8)pMixedOutBuffer->m_dataAddress,
     (M4OSA_MemAddr8)pBackgroundTrack->m_dataAddress,
      pBackgroundTrack->m_bufferSize);

    LOGV("VideoEditorBGAudioProcessing::lvProcessAudioMixNDuck EXIT");
    return result;
}

VideoEditorBGAudioProcessing:: ~VideoEditorBGAudioProcessing() {

 //free(VideoEditorBGAudioProcessing:: pTempBuffer);
}

M4OSA_Int32 VideoEditorBGAudioProcessing::calculateOutResampleBufSize() {

    M4OSA_Int32 bufSize =0;

    // This already takes care of channel count in mBTBuffer.m_bufferSize
    bufSize = (this->mOutSampleRate/this->mInSampleRate)*this->mBTBuffer.m_bufferSize;

    return bufSize;
}

void VideoEditorBGAudioProcessing ::veSetAudioProcessingParams(
        veAudMixSettings gInputParams) {

    LOGV("VideoEditorBGAudioProcessing:: ENTER lvSetAudioProcessingParams ");
    this->mDucking_enable       = gInputParams.lvInDucking_enable;
    this->mDucking_lowVolume    = gInputParams.lvInDucking_lowVolume;
    this->mDucking_threshold    = gInputParams.lvInDucking_threshold;

    this->mPTVolLevel           = gInputParams.lvPTVolLevel;
    this->mBTVolLevel           = gInputParams.lvBTVolLevel ;

    this->mBTChannelCount       = gInputParams.lvBTChannelCount;
    this->mPTChannelCount       = gInputParams.lvPTChannelCount;

    this->mBTFormat             = gInputParams.lvBTFormat;

    this->mInSampleRate         = gInputParams.lvInSampleRate;
    this->mOutSampleRate        = gInputParams.lvOutSampleRate;

    this->mAudVolArrIndex       = 0;
    this->mDoDucking            = 0;
    this->mDuckingFactor        = 1.0; // default

    LOGV("VideoEditorBGAudioProcessing::  ducking_enable 0x%x \
     ducking_lowVolume %f  ducking_threshold %d  fPTVolLevel %f BTVolLevel %f",
     this->mDucking_enable, this->mDucking_lowVolume, this->mDucking_threshold,
     this->mPTVolLevel, this->mPTVolLevel);

    // Following logc decides if SSRC support is needed for this mixing
    if ( gInputParams.lvInSampleRate != gInputParams.lvOutSampleRate){
        this->mIsSSRCneeded      = 1;
    }else{
        this->mIsSSRCneeded      = 0;
    }
    if( gInputParams.lvBTChannelCount != gInputParams.lvPTChannelCount){
        if (gInputParams.lvBTChannelCount == 2){
            this->mChannelConversion   = 1; // convert to MONO
        }else{
            this->mChannelConversion   = 2; // Convert to STEREO
        }
    }else{
        this->mChannelConversion   = 0;
    }
    LOGV("VideoEditorBGAudioProcessing:: EXIT veSetAudioProcessingParams ");
}


M4OSA_Int32 VideoEditorBGAudioProcessing:: getDecibelSound(M4OSA_UInt32 value) {

    int dbSound = 1;

    if (value == 0) return 0;

    if (value > 0x4000 && value <= 0x8000) // 32768
        dbSound = 90;
    else if (value > 0x2000 && value <= 0x4000) // 16384
        dbSound = 84;
    else if (value > 0x1000 && value <= 0x2000) // 8192
        dbSound = 78;
    else if (value > 0x0800 && value <= 0x1000) // 4028
        dbSound = 72;
    else if (value > 0x0400 && value <= 0x0800) // 2048
        dbSound = 66;
    else if (value > 0x0200 && value <= 0x0400) // 1024
        dbSound = 60;
    else if (value > 0x0100 && value <= 0x0200) // 512
        dbSound = 54;
    else if (value > 0x0080 && value <= 0x0100) // 256
        dbSound = 48;
    else if (value > 0x0040 && value <= 0x0080) // 128
        dbSound = 42;
    else if (value > 0x0020 && value <= 0x0040) // 64
        dbSound = 36;
    else if (value > 0x0010 && value <= 0x0020) // 32
        dbSound = 30;
    else if (value > 0x0008 && value <= 0x0010) //16
        dbSound = 24;
    else if (value > 0x0007 && value <= 0x0008) //8
        dbSound = 24;
    else if (value > 0x0003 && value <= 0x0007) // 4
        dbSound = 18;
    else if (value > 0x0001 && value <= 0x0003) //2
        dbSound = 12;
    else if (value > 0x000 && value <= 0x0001) // 1
        dbSound = 6;
    else
        dbSound = 0;

    return dbSound;
}

M4OSA_Bool VideoEditorBGAudioProcessing:: isThresholdBreached(
        M4OSA_Int32* averageValue, M4OSA_Int32 storeCount,
         M4OSA_Int32 thresholdValue) {

    M4OSA_Bool result = 0;
    int i;
    int finalValue = 0;

    for (i=0; i< storeCount;i++)
        finalValue += averageValue[i];

    finalValue = finalValue/storeCount;

    //printf ("<%d > \t  ", finalValue);

    if (finalValue > thresholdValue)
        result = M4OSA_TRUE;
    else
        result = M4OSA_FALSE;

    return result;
}

}//namespace android
