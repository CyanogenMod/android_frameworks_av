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

#define LOG_TAG "EffectProxy"
//#define LOG_NDEBUG 0

#include <cutils/log.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <EffectProxy.h>
#include <utils/threads.h>
#include <media/EffectsFactoryApi.h>

namespace android {
// This is a dummy proxy descriptor just to return to Factory during the initial
// GetDescriptor call. Later in the factory, it is replaced with the
// SW sub effect descriptor
const effect_descriptor_t gProxyDescriptor = {
        EFFECT_UUID_INITIALIZER, // type
        EFFECT_UUID_INITIALIZER, // uuid
        EFFECT_CONTROL_API_VERSION, //version of effect control API
        (EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST |
         EFFECT_FLAG_VOLUME_CTRL), // effect capability flags
        0, // CPU load
        1, // Data memory
        "Proxy", //effect name
        "AOSP", //implementor name
};


static const effect_descriptor_t *const gDescriptors[] =
{
    &gProxyDescriptor,
};

static inline bool isGetterCmd(uint32_t cmdCode)
{
    switch (cmdCode) {
    case EFFECT_CMD_GET_PARAM:
    case EFFECT_CMD_GET_CONFIG:
    case EFFECT_CMD_GET_CONFIG_REVERSE:
    case EFFECT_CMD_GET_FEATURE_SUPPORTED_CONFIGS:
    case EFFECT_CMD_GET_FEATURE_CONFIG:
        return true;
    default:
        return false;
    }
}


int EffectProxyCreate(const effect_uuid_t *uuid,
                            int32_t             sessionId,
                            int32_t             ioId,
                           effect_handle_t  *pHandle) {

    effect_descriptor_t* desc;
    EffectContext* pContext;
    if (pHandle == NULL || uuid == NULL) {
        ALOGE("EffectProxyCreate() called with NULL pointer");
        return -EINVAL;
    }
    ALOGV("EffectProxyCreate start..");
    pContext = new EffectContext;
    pContext->sessionId = sessionId;
    pContext->ioId = ioId;
    pContext->uuid = *uuid;
    pContext->common_itfe = &gEffectInterface;
    // The sub effects will be created in effect_command when the first command
    // for the effect is received
    pContext->eHandle[SUB_FX_HOST] = pContext->eHandle[SUB_FX_OFFLOAD] = NULL;

    // Get the HW and SW sub effect descriptors from the effects factory
    desc = new effect_descriptor_t[SUB_FX_COUNT];
    pContext->desc = new effect_descriptor_t[SUB_FX_COUNT];
    int retValue = EffectGetSubEffects(uuid, desc,
                                sizeof(effect_descriptor_t) * SUB_FX_COUNT);
    // EffectGetSubEffects returns the number of sub-effects copied.
    if (retValue != SUB_FX_COUNT) {
       ALOGE("EffectCreate() could not get the sub effects");
       delete desc;
       delete pContext->desc;
       return -EINVAL;
    }
    // Check which is the HW descriptor and copy the descriptors
    // to the Context desc array
    // Also check if there is only one HW and one SW descriptor.
    // HW descriptor alone has the HW_TUNNEL flag.
    if ((desc[0].flags & EFFECT_FLAG_HW_ACC_TUNNEL) &&
       !(desc[1].flags & EFFECT_FLAG_HW_ACC_TUNNEL)) {
        pContext->desc[SUB_FX_OFFLOAD] = desc[0];
        pContext->desc[SUB_FX_HOST] = desc[1];
    }
    else if ((desc[1].flags & EFFECT_FLAG_HW_ACC_TUNNEL) &&
             !(desc[0].flags & EFFECT_FLAG_HW_ACC_TUNNEL)) {
        pContext->desc[SUB_FX_HOST] = desc[0];
        pContext->desc[SUB_FX_OFFLOAD] = desc[1];
    }
    delete desc;
#if (LOG_NDEBUG == 0)
    effect_uuid_t uuid_print = pContext->desc[SUB_FX_HOST].uuid;
    ALOGV("EffectCreate() UUID of HOST: %08X-%04X-%04X-%04X-%02X%02X%02X%02X"
        "%02X%02X\n",uuid_print.timeLow, uuid_print.timeMid,
        uuid_print.timeHiAndVersion, uuid_print.clockSeq, uuid_print.node[0],
        uuid_print.node[1], uuid_print.node[2], uuid_print.node[3],
        uuid_print.node[4], uuid_print.node[5]);
    ALOGV("EffectCreate() UUID of OFFLOAD: %08X-%04X-%04X-%04X-%02X%02X%02X%02X"
        "%02X%02X\n", uuid_print.timeLow, uuid_print.timeMid,
        uuid_print.timeHiAndVersion, uuid_print.clockSeq, uuid_print.node[0],
        uuid_print.node[1], uuid_print.node[2], uuid_print.node[3],
        uuid_print.node[4], uuid_print.node[5]);
#endif
    *pHandle = (effect_handle_t)pContext;
    ALOGV("EffectCreate end");
    return 0;
} //end EffectProxyCreate

int EffectProxyRelease(effect_handle_t handle) {
    EffectContext * pContext = (EffectContext *)handle;
    if (pContext == NULL) {
        ALOGV("ERROR : EffectRelease called with NULL pointer");
        return -EINVAL;
    }
    ALOGV("EffectRelease");
    delete pContext->desc;
    if (pContext->eHandle[SUB_FX_HOST])
       EffectRelease(pContext->eHandle[SUB_FX_HOST]);
    if (pContext->eHandle[SUB_FX_OFFLOAD])
       EffectRelease(pContext->eHandle[SUB_FX_OFFLOAD]);
    delete pContext;
    pContext = NULL;
    return 0;
} /*end EffectProxyRelease */

int EffectProxyGetDescriptor(const effect_uuid_t *uuid,
                                   effect_descriptor_t *pDescriptor) {
    const effect_descriptor_t *desc = NULL;

    if (pDescriptor == NULL || uuid == NULL) {
        ALOGV("EffectGetDescriptor() called with NULL pointer");
        return -EINVAL;
    }
    desc = &gProxyDescriptor;
    *pDescriptor = *desc;
    return 0;
} /* end EffectProxyGetDescriptor */

/* Effect Control Interface Implementation: Process */
int Effect_process(effect_handle_t     self,
                              audio_buffer_t         *inBuffer,
                              audio_buffer_t         *outBuffer) {

    EffectContext *pContext = (EffectContext *) self;
    int ret = 0;
    if (pContext != NULL) {
        int index = pContext->index;
        // if the index refers to HW , do not do anything. Just return.
        if (index == SUB_FX_HOST) {
            ret = (*pContext->eHandle[index])->process(pContext->eHandle[index],
                                                       inBuffer, outBuffer);
        }
    }
    return ret;
}   /* end Effect_process */

/* Effect Control Interface Implementation: Command */
int Effect_command(effect_handle_t  self,
                              uint32_t            cmdCode,
                              uint32_t            cmdSize,
                              void                *pCmdData,
                              uint32_t            *replySize,
                              void                *pReplyData) {

    EffectContext *pContext = (EffectContext *) self;
    int status = 0;
    if (pContext == NULL) {
        ALOGV("Effect_command() Proxy context is NULL");
        return -EINVAL;
    }
    if (pContext->eHandle[SUB_FX_HOST] == NULL) {
        ALOGV("Effect_command() Calling HOST EffectCreate");
        status = EffectCreate(&pContext->desc[SUB_FX_HOST].uuid,
                              pContext->sessionId, pContext->ioId,
                              &(pContext->eHandle[SUB_FX_HOST]));
        if (status != NO_ERROR || (pContext->eHandle[SUB_FX_HOST] == NULL)) {
            ALOGV("Effect_command() Error creating SW sub effect");
            return status;
        }
    }
    if (pContext->eHandle[SUB_FX_OFFLOAD] == NULL) {
        ALOGV("Effect_command() Calling OFFLOAD EffectCreate");
        status = EffectCreate(&pContext->desc[SUB_FX_OFFLOAD].uuid,
                              pContext->sessionId, pContext->ioId,
                              &(pContext->eHandle[SUB_FX_OFFLOAD]));
        if (status != NO_ERROR || (pContext->eHandle[SUB_FX_OFFLOAD] == NULL)) {
            ALOGV("Effect_command() Error creating HW effect");
            // Do not return error here as SW effect is created
            // Return error if the CMD_OFFLOAD sends the index as OFFLOAD
        }
        pContext->index = SUB_FX_HOST;
    }
    // EFFECT_CMD_OFFLOAD used to (1) send whether the thread is offload or not
    // (2) Send the ioHandle of the effectThread when the effect
    // is moved from one type of thread to another.
    // pCmdData points to a memory holding effect_offload_param_t structure
    if (cmdCode == EFFECT_CMD_OFFLOAD) {
        ALOGV("Effect_command() cmdCode = EFFECT_CMD_OFFLOAD");
        if (cmdSize == 0 || pCmdData == NULL) {
            ALOGV("effectsOffload: Effect_command: CMD_OFFLOAD has no data");
             *(int*)pReplyData = FAILED_TRANSACTION;
            return FAILED_TRANSACTION;
        }
        effect_offload_param_t* offloadParam = (effect_offload_param_t*)pCmdData;
        // Assign the effect context index based on isOffload field of the structure
        pContext->index = offloadParam->isOffload ? SUB_FX_OFFLOAD : SUB_FX_HOST;
        // if the index is HW and the HW effect is unavailable, return error
        // and reset the index to SW
        if (pContext->eHandle[pContext->index] == NULL) {
            ALOGV("Effect_command()CMD_OFFLOAD sub effect unavailable");
            *(int*)pReplyData = FAILED_TRANSACTION;
            return FAILED_TRANSACTION;
        }
        pContext->ioId = offloadParam->ioHandle;
        ALOGV("Effect_command()CMD_OFFLOAD index:%d io %d", pContext->index, pContext->ioId);
        // Update the DSP wrapper with the new ioHandle.
        // Pass the OFFLOAD command to the wrapper.
        // The DSP wrapper needs to handle this CMD
        if (pContext->eHandle[SUB_FX_OFFLOAD])
            status = (*pContext->eHandle[SUB_FX_OFFLOAD])->command(
                             pContext->eHandle[SUB_FX_OFFLOAD], cmdCode, cmdSize,
                             pCmdData, replySize, pReplyData);
        return status;
    }

    int index = pContext->index;
    if (index != SUB_FX_HOST && index != SUB_FX_OFFLOAD) {
        ALOGV("Effect_command: effect index is neither offload nor host");
        return -EINVAL;
    }

    // Getter commands are only sent to the active sub effect.
    uint32_t hostReplySize = replySize != NULL ? *replySize : 0;
    bool hostReplied = false;
    int hostStatus = 0;
    uint32_t offloadReplySize = replySize != NULL ? *replySize : 0;
    bool offloadReplied = false;
    int offloadStatus = 0;

    if (pContext->eHandle[SUB_FX_HOST] && (!isGetterCmd(cmdCode) || index == SUB_FX_HOST)) {
        hostStatus = (*pContext->eHandle[SUB_FX_HOST])->command(
                             pContext->eHandle[SUB_FX_HOST], cmdCode, cmdSize,
                             pCmdData, replySize != NULL ? &hostReplySize : NULL, pReplyData);
        hostReplied = true;
    }
    if (pContext->eHandle[SUB_FX_OFFLOAD] && (!isGetterCmd(cmdCode) || index == SUB_FX_OFFLOAD)) {
        // In case of SET CMD, when the offload stream is unavailable,
        // we will store the effect param values in the DSP effect wrapper.
        // When the offload effects get enabled, we send these values to the
        // DSP during Effect_config.
        // So,we send the params to DSP wrapper also
        offloadStatus = (*pContext->eHandle[SUB_FX_OFFLOAD])->command(
                         pContext->eHandle[SUB_FX_OFFLOAD], cmdCode, cmdSize,
                         pCmdData, replySize != NULL ? &offloadReplySize : NULL, pReplyData);
        offloadReplied = true;
    }
    // By convention the offloaded implementation reply is returned if command is processed by both
    // host and offloaded sub effects
    if (offloadReplied){
        status = offloadStatus;
        if (replySize) {
            *replySize = offloadReplySize;
        }
    } else if (hostReplied) {
        status = hostStatus;
        if (replySize) {
            *replySize = hostReplySize;
        }
    }
    return status;
}    /* end Effect_command */


/* Effect Control Interface Implementation: get_descriptor */
int Effect_getDescriptor(effect_handle_t   self,
                         effect_descriptor_t *pDescriptor) {

    EffectContext * pContext = (EffectContext *) self;
    const effect_descriptor_t *desc;

    ALOGV("Effect_getDescriptor");
    if (pContext == NULL || pDescriptor == NULL) {
        ALOGV("Effect_getDescriptor() invalid param");
        return -EINVAL;
    }
    if (pContext->desc == NULL) {
        ALOGV("Effect_getDescriptor() could not get descriptor");
        return -EINVAL;
    }
    desc = &pContext->desc[SUB_FX_HOST];
    *pDescriptor = *desc;
    pDescriptor->uuid = pContext->uuid; // Replace the uuid with the Proxy UUID
    // Also set/clear the EFFECT_FLAG_OFFLOAD_SUPPORTED flag based on the sub effects availability
    if (pContext->eHandle[SUB_FX_OFFLOAD] != NULL)
        pDescriptor->flags |= EFFECT_FLAG_OFFLOAD_SUPPORTED;
    else
        pDescriptor->flags &= ~EFFECT_FLAG_OFFLOAD_SUPPORTED;
    return 0;
} /* end Effect_getDescriptor */

} // namespace android

__attribute__ ((visibility ("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    tag : AUDIO_EFFECT_LIBRARY_TAG,
    version : EFFECT_LIBRARY_API_VERSION,
    name : "Effect Proxy",
    implementor : "AOSP",
    create_effect : android::EffectProxyCreate,
    release_effect : android::EffectProxyRelease,
    get_descriptor : android::EffectProxyGetDescriptor,
};
