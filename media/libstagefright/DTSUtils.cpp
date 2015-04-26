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

#define LOG_NDEBUG 0
#define LOG_TAG "DTSUtils"
#include <utils/Log.h>

#include "include/DTSUtils.h"

#include <OMX_Audio.h>
#include "include/OMX_Audio_DTS.h"

namespace android {

status_t DTSUtils::setupDecoder(sp<IOMX> omx, IOMX::node_id node, int32_t sampleRate)
{
    ALOGV("(DTS) +setupDecoder()");

    status_t result;
    OMX_AUDIO_PARAM_DTSDECTYPE myDtsDecParam;

    // initialize myDtsDecParam
    memset(&myDtsDecParam, 0, sizeof(myDtsDecParam));
    InitOMXParams(&myDtsDecParam);

    // get the current params
    result = omx->getParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioDTSDec, &myDtsDecParam, sizeof(myDtsDecParam));
    ALOGV("(DTS)     -> omx->getParameter() :  node = %d  nSpkrOut = %d  result = 0x%x", (int)node, (int)myDtsDecParam.nSpkrOut, (int)result);

    if (result == OK)
    {
        // Set 7.1 channel speaker mask for M8: 0x84B (2123) == C-LR-LFE1-LssRss-LsrRsr
        // M6 will internally set speaker mask to 0xF (15) == C-LR-LsRs-LFE1
        myDtsDecParam.nSpkrOut = OMX_AUDIO_DTS_SPKROUTTYPE(  OMX_AUDIO_DTSSPKROUT_MASK_C
                                                           | OMX_AUDIO_DTSSPKROUT_MASK_LR
                                                           | OMX_AUDIO_DTSSPKROUT_MASK_LFE1
                                                           | OMX_AUDIO_DTSSPKROUT_MASK_LssRss
                                                           | OMX_AUDIO_DTSSPKROUT_MASK_LsrRsr
                                                           );
        // call Android's OMX setParameter wrapper
        ALOGV("(DTS)     -> Attempting to set multichannel speaker mask : nSpkrOut = 0x%x (%d)", (int)myDtsDecParam.nSpkrOut, (int)myDtsDecParam.nSpkrOut);
        result = omx->setParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioDTSDec, &myDtsDecParam, sizeof(myDtsDecParam));
        ALOGV("(DTS)     -> omx->setParameter() :  node = %d  result = 0x%x", (int)node, (int)result);

        if (result == OK)
        {
            // make sure the param got set
            result = omx->getParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioDTSDec, &myDtsDecParam, sizeof(myDtsDecParam));
            ALOGV("(DTS)     -> node->getParameter() :  node = %d  nSpkrOut = 0x%x (%d)  result = 0x%x", (int)node, (int)myDtsDecParam.nSpkrOut, (int)myDtsDecParam.nSpkrOut, (int)result);
        }
    }

    ALOGV("(DTS) -setupDecoder() : nSpkrOut result = 0x%x", (int)result);

    // initialize myPcmParam
    OMX_AUDIO_PARAM_PCMMODETYPE myPcmParam;
    memset(&myPcmParam, 0, sizeof(myPcmParam));
    InitOMXParams(&myPcmParam);
    myPcmParam.nPortIndex = 1;

    result = omx->getParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioPcm, &myPcmParam, sizeof(myPcmParam));
    ALOGV("(DTS)     -> omx->getParameter() :  node = %d  nSamplingRate = %d  result = 0x%x", (int)node, (int)myPcmParam.nSamplingRate, (int)result);

    if (result == OK)
    {
        myPcmParam.nSamplingRate = sampleRate;
        // call Android's OMX setParameter wrapper
        ALOGV("(DTS)     -> Attempting to set sampling rate : nSamplingRate = 0x%x (%d)", (int)myPcmParam.nSamplingRate, (int)myPcmParam.nSamplingRate);
        result = omx->setParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioPcm, &myPcmParam, sizeof(myPcmParam));
        ALOGV("(DTS)     -> omx->setParameter() :  node = %d  result = 0x%x", (int)node, (int)result);

        if (result == OK)
        {
            // make sure the param got set
            result = omx->getParameter(node, (OMX_INDEXTYPE)OMX_IndexParamAudioPcm, &myPcmParam, sizeof(myPcmParam));
            ALOGV("(DTS)     -> node->getParameter() :  node = %d  nSamplingRate = 0x%x (%d)  result = 0x%x", (int)node, (int)myPcmParam.nSamplingRate, (int)myPcmParam.nSamplingRate, (int)result);
        }
    }

    ALOGV("(DTS) -setupDecoder() : nSamplingRate result = 0x%x", (int)result);

    return result;
}

} // namespace android
