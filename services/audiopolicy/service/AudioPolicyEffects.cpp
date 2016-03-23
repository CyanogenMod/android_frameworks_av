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

#define LOG_TAG "AudioPolicyEffects"
//#define LOG_NDEBUG 0

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cutils/misc.h>
#include <media/AudioEffect.h>
#include <system/audio.h>
#include <hardware/audio_effect.h>
#include <audio_effects/audio_effects_conf.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
#include <cutils/config_utils.h>
#include "AudioPolicyService.h"
#include "AudioPolicyEffects.h"
#include "ServiceUtilities.h"

namespace android {

// ----------------------------------------------------------------------------
// AudioPolicyEffects Implementation
// ----------------------------------------------------------------------------

AudioPolicyEffects::AudioPolicyEffects(AudioPolicyService *audioPolicyService) :
    mAudioPolicyService(audioPolicyService)
{
    // load automatic audio effect modules
    if (access(AUDIO_EFFECT_VENDOR_CONFIG_FILE2, R_OK) == 0) {
        loadAudioEffectConfig(AUDIO_EFFECT_VENDOR_CONFIG_FILE2);
    } else if (access(AUDIO_EFFECT_VENDOR_CONFIG_FILE, R_OK) == 0) {
        loadAudioEffectConfig(AUDIO_EFFECT_VENDOR_CONFIG_FILE);
    } else if (access(AUDIO_EFFECT_DEFAULT_CONFIG_FILE, R_OK) == 0) {
        loadAudioEffectConfig(AUDIO_EFFECT_DEFAULT_CONFIG_FILE);
    }
}


AudioPolicyEffects::~AudioPolicyEffects()
{
    size_t i = 0;
    // release audio input processing resources
    for (i = 0; i < mInputSources.size(); i++) {
        delete mInputSources.valueAt(i);
    }
    mInputSources.clear();

    for (i = 0; i < mInputs.size(); i++) {
        mInputs.valueAt(i)->mEffects.clear();
        delete mInputs.valueAt(i);
    }
    mInputs.clear();

    // release audio output processing resources
    for (i = 0; i < mOutputStreams.size(); i++) {
        delete mOutputStreams.valueAt(i);
    }
    mOutputStreams.clear();

    for (i = 0; i < mOutputSessions.size(); i++) {
        mOutputSessions.valueAt(i)->mEffects.clear();
        delete mOutputSessions.valueAt(i);
    }
    mOutputSessions.clear();
}


status_t AudioPolicyEffects::addInputEffects(audio_io_handle_t input,
                             audio_source_t inputSource,
                             int audioSession)
{
    status_t status = NO_ERROR;

    // create audio pre processors according to input source
    audio_source_t aliasSource = (inputSource == AUDIO_SOURCE_HOTWORD) ?
                                    AUDIO_SOURCE_VOICE_RECOGNITION : inputSource;

    Mutex::Autolock _l(mLock);
    ssize_t index = mInputSources.indexOfKey(aliasSource);
    if (index < 0) {
        ALOGV("addInputEffects(): no processing needs to be attached to this source");
        return status;
    }
    ssize_t idx = mInputs.indexOfKey(input);
    EffectVector *inputDesc;
    if (idx < 0) {
        inputDesc = new EffectVector(audioSession);
        mInputs.add(input, inputDesc);
    } else {
        // EffectVector is existing and we just need to increase ref count
        inputDesc = mInputs.valueAt(idx);
    }
    inputDesc->mRefCount++;

    ALOGV("addInputEffects(): input: %d, refCount: %d", input, inputDesc->mRefCount);
    if (inputDesc->mRefCount == 1) {
        Vector <EffectDesc *> effects = mInputSources.valueAt(index)->mEffects;
        for (size_t i = 0; i < effects.size(); i++) {
            EffectDesc *effect = effects[i];
            sp<AudioEffect> fx = new AudioEffect(NULL, String16("android"), &effect->mUuid, -1, 0,
                                                 0, audioSession, input);
            status_t status = fx->initCheck();
            if (status != NO_ERROR && status != ALREADY_EXISTS) {
                ALOGW("addInputEffects(): failed to create Fx %s on source %d",
                      effect->mName, (int32_t)aliasSource);
                // fx goes out of scope and strong ref on AudioEffect is released
                continue;
            }
            for (size_t j = 0; j < effect->mParams.size(); j++) {
                fx->setParameter(effect->mParams[j]);
            }
            ALOGV("addInputEffects(): added Fx %s on source: %d",
                  effect->mName, (int32_t)aliasSource);
            inputDesc->mEffects.add(fx);
        }
        inputDesc->setProcessorEnabled(true);
    }
    return status;
}


status_t AudioPolicyEffects::releaseInputEffects(audio_io_handle_t input)
{
    status_t status = NO_ERROR;

    Mutex::Autolock _l(mLock);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        return status;
    }
    EffectVector *inputDesc = mInputs.valueAt(index);
    inputDesc->mRefCount--;
    ALOGV("releaseInputEffects(): input: %d, refCount: %d", input, inputDesc->mRefCount);
    if (inputDesc->mRefCount == 0) {
        inputDesc->setProcessorEnabled(false);
        delete inputDesc;
        mInputs.removeItemsAt(index);
        ALOGV("releaseInputEffects(): all effects released");
    }
    return status;
}

status_t AudioPolicyEffects::queryDefaultInputEffects(int audioSession,
                                                      effect_descriptor_t *descriptors,
                                                      uint32_t *count)
{
    status_t status = NO_ERROR;

    Mutex::Autolock _l(mLock);
    size_t index;
    for (index = 0; index < mInputs.size(); index++) {
        if (mInputs.valueAt(index)->mSessionId == audioSession) {
            break;
        }
    }
    if (index == mInputs.size()) {
        *count = 0;
        return BAD_VALUE;
    }
    Vector< sp<AudioEffect> > effects = mInputs.valueAt(index)->mEffects;

    for (size_t i = 0; i < effects.size(); i++) {
        effect_descriptor_t desc = effects[i]->descriptor();
        if (i < *count) {
            descriptors[i] = desc;
        }
    }
    if (effects.size() > *count) {
        status = NO_MEMORY;
    }
    *count = effects.size();
    return status;
}


status_t AudioPolicyEffects::queryDefaultOutputSessionEffects(int audioSession,
                         effect_descriptor_t *descriptors,
                         uint32_t *count)
{
    status_t status = NO_ERROR;

    Mutex::Autolock _l(mLock);
    size_t index;
    for (index = 0; index < mOutputSessions.size(); index++) {
        if (mOutputSessions.valueAt(index)->mSessionId == audioSession) {
            break;
        }
    }
    if (index == mOutputSessions.size()) {
        *count = 0;
        return BAD_VALUE;
    }
    Vector< sp<AudioEffect> > effects = mOutputSessions.valueAt(index)->mEffects;

    for (size_t i = 0; i < effects.size(); i++) {
        effect_descriptor_t desc = effects[i]->descriptor();
        if (i < *count) {
            descriptors[i] = desc;
        }
    }
    if (effects.size() > *count) {
        status = NO_MEMORY;
    }
    *count = effects.size();
    return status;
}


status_t AudioPolicyEffects::addOutputSessionEffects(audio_io_handle_t output,
                         audio_stream_type_t stream,
                         int audioSession)
{
    status_t status = NO_ERROR;

    Mutex::Autolock _l(mLock);
    // create audio processors according to stream
    // FIXME: should we have specific post processing settings for internal streams?
    // default to media for now.
    if (stream >= AUDIO_STREAM_PUBLIC_CNT) {
        stream = AUDIO_STREAM_MUSIC;
    }
    ssize_t index = mOutputStreams.indexOfKey(stream);
    if (index < 0) {
        ALOGV("addOutputSessionEffects(): no output processing needed for this stream");
        return NO_ERROR;
    }

    ssize_t idx = mOutputSessions.indexOfKey(audioSession);
    EffectVector *procDesc;
    if (idx < 0) {
        procDesc = new EffectVector(audioSession);
        mOutputSessions.add(audioSession, procDesc);

        mAudioPolicyService->onOutputSessionEffectsUpdate(stream, audioSession, true);
    } else {
        // EffectVector is existing and we just need to increase ref count
        procDesc = mOutputSessions.valueAt(idx);
    }
    procDesc->mRefCount++;

    ALOGV("addOutputSessionEffects(): session: %d, refCount: %d",
          audioSession, procDesc->mRefCount);
    if (procDesc->mRefCount == 1) {
        Vector <EffectDesc *> effects = mOutputStreams.valueAt(index)->mEffects;
        for (size_t i = 0; i < effects.size(); i++) {
            EffectDesc *effect = effects[i];
            sp<AudioEffect> fx = new AudioEffect(NULL, String16("android"), &effect->mUuid, 0, 0, 0,
                                                 audioSession, output);
            status_t status = fx->initCheck();
            if (status != NO_ERROR && status != ALREADY_EXISTS) {
                ALOGE("addOutputSessionEffects(): failed to create Fx  %s on session %d",
                      effect->mName, audioSession);
                // fx goes out of scope and strong ref on AudioEffect is released
                continue;
            }
            ALOGV("addOutputSessionEffects(): added Fx %s on session: %d for stream: %d",
                  effect->mName, audioSession, (int32_t)stream);
            procDesc->mEffects.add(fx);
        }

        procDesc->setProcessorEnabled(true);
    }
    return status;
}

status_t AudioPolicyEffects::releaseOutputSessionEffects(audio_io_handle_t output,
                         audio_stream_type_t stream,
                         int audioSession)
{
    status_t status = NO_ERROR;
    (void) output; // argument not used for now
    (void) stream; // argument not used for now

    Mutex::Autolock _l(mLock);
    ssize_t index = mOutputSessions.indexOfKey(audioSession);
    if (index < 0) {
        ALOGV("releaseOutputSessionEffects: no output processing was attached to this stream");
        return NO_ERROR;
    }

    EffectVector *procDesc = mOutputSessions.valueAt(index);

    // just in case it already has a death wish
    if (procDesc->mRefCount == 0) {
        return NO_ERROR;
    }

    procDesc->mRefCount--;
    ALOGV("releaseOutputSessionEffects(): session: %d, refCount: %d",
          audioSession, procDesc->mRefCount);

    if (procDesc->mRefCount == 0) {
        mAudioPolicyService->releaseOutputSessionEffectsDelayed(
                output, stream, audioSession, 10000);
    }

    return status;
}

status_t AudioPolicyEffects::doReleaseOutputSessionEffects(audio_io_handle_t output,
                         audio_stream_type_t stream,
                         int audioSession)
{
    status_t status = NO_ERROR;
    (void) output; // argument not used for now

    Mutex::Autolock _l(mLock);
    ssize_t index = mOutputSessions.indexOfKey(audioSession);
    if (index < 0) {
        ALOGV("doReleaseOutputSessionEffects: no output processing was attached to this stream");
        return NO_ERROR;
    }

    EffectVector *procDesc = mOutputSessions.valueAt(index);
    ALOGV("doReleaseOutputSessionEffects(): session: %d, refCount: %d",
          audioSession, procDesc->mRefCount);

    if (procDesc->mRefCount == 0) {
        procDesc->setProcessorEnabled(false);
        procDesc->mEffects.clear();
        delete procDesc;
        mOutputSessions.removeItemsAt(index);
        mAudioPolicyService->onOutputSessionEffectsUpdate(stream, audioSession, false);
        ALOGV("doReleaseOutputSessionEffects(): output processing released from session: %d",
              audioSession);
    }
    return status;
}


void AudioPolicyEffects::EffectVector::setProcessorEnabled(bool enabled)
{
    for (size_t i = 0; i < mEffects.size(); i++) {
        mEffects.itemAt(i)->setEnabled(enabled);
    }
}


// ----------------------------------------------------------------------------
// Audio processing configuration
// ----------------------------------------------------------------------------

/*static*/ const char * const AudioPolicyEffects::kInputSourceNames[AUDIO_SOURCE_CNT -1] = {
    MIC_SRC_TAG,
    VOICE_UL_SRC_TAG,
    VOICE_DL_SRC_TAG,
    VOICE_CALL_SRC_TAG,
    CAMCORDER_SRC_TAG,
    VOICE_REC_SRC_TAG,
    VOICE_COMM_SRC_TAG
};

// returns the audio_source_t enum corresponding to the input source name or
// AUDIO_SOURCE_CNT is no match found
/*static*/ audio_source_t AudioPolicyEffects::inputSourceNameToEnum(const char *name)
{
    int i;
    for (i = AUDIO_SOURCE_MIC; i < AUDIO_SOURCE_CNT; i++) {
        if (strcmp(name, kInputSourceNames[i - AUDIO_SOURCE_MIC]) == 0) {
            ALOGV("inputSourceNameToEnum found source %s %d", name, i);
            break;
        }
    }
    return (audio_source_t)i;
}

const char *AudioPolicyEffects::kStreamNames[AUDIO_STREAM_PUBLIC_CNT+1] = {
    AUDIO_STREAM_DEFAULT_TAG,
    AUDIO_STREAM_VOICE_CALL_TAG,
    AUDIO_STREAM_SYSTEM_TAG,
    AUDIO_STREAM_RING_TAG,
    AUDIO_STREAM_MUSIC_TAG,
    AUDIO_STREAM_ALARM_TAG,
    AUDIO_STREAM_NOTIFICATION_TAG,
    AUDIO_STREAM_BLUETOOTH_SCO_TAG,
    AUDIO_STREAM_ENFORCED_AUDIBLE_TAG,
    AUDIO_STREAM_DTMF_TAG,
    AUDIO_STREAM_TTS_TAG
};

// returns the audio_stream_t enum corresponding to the output stream name or
// AUDIO_STREAM_PUBLIC_CNT is no match found
audio_stream_type_t AudioPolicyEffects::streamNameToEnum(const char *name)
{
    int i;
    for (i = AUDIO_STREAM_DEFAULT; i < AUDIO_STREAM_PUBLIC_CNT; i++) {
        if (strcmp(name, kStreamNames[i - AUDIO_STREAM_DEFAULT]) == 0) {
            ALOGV("streamNameToEnum found stream %s %d", name, i);
            break;
        }
    }
    return (audio_stream_type_t)i;
}

// ----------------------------------------------------------------------------
// Audio Effect Config parser
// ----------------------------------------------------------------------------

size_t AudioPolicyEffects::growParamSize(char *param,
                                         size_t size,
                                         size_t *curSize,
                                         size_t *totSize)
{
    // *curSize is at least sizeof(effect_param_t) + 2 * sizeof(int)
    size_t pos = ((*curSize - 1 ) / size + 1) * size;

    if (pos + size > *totSize) {
        while (pos + size > *totSize) {
            *totSize += ((*totSize + 7) / 8) * 4;
        }
        param = (char *)realloc(param, *totSize);
    }
    *curSize = pos + size;
    return pos;
}

size_t AudioPolicyEffects::readParamValue(cnode *node,
                                          char *param,
                                          size_t *curSize,
                                          size_t *totSize)
{
    if (strncmp(node->name, SHORT_TAG, sizeof(SHORT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(short), curSize, totSize);
        *(short *)((char *)param + pos) = (short)atoi(node->value);
        ALOGV("readParamValue() reading short %d", *(short *)((char *)param + pos));
        return sizeof(short);
    } else if (strncmp(node->name, INT_TAG, sizeof(INT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(int), curSize, totSize);
        *(int *)((char *)param + pos) = atoi(node->value);
        ALOGV("readParamValue() reading int %d", *(int *)((char *)param + pos));
        return sizeof(int);
    } else if (strncmp(node->name, FLOAT_TAG, sizeof(FLOAT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(float), curSize, totSize);
        *(float *)((char *)param + pos) = (float)atof(node->value);
        ALOGV("readParamValue() reading float %f",*(float *)((char *)param + pos));
        return sizeof(float);
    } else if (strncmp(node->name, BOOL_TAG, sizeof(BOOL_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(bool), curSize, totSize);
        if (strncmp(node->value, "false", strlen("false") + 1) == 0) {
            *(bool *)((char *)param + pos) = false;
        } else {
            *(bool *)((char *)param + pos) = true;
        }
        ALOGV("readParamValue() reading bool %s",*(bool *)((char *)param + pos) ? "true" : "false");
        return sizeof(bool);
    } else if (strncmp(node->name, STRING_TAG, sizeof(STRING_TAG) + 1) == 0) {
        size_t len = strnlen(node->value, EFFECT_STRING_LEN_MAX);
        if (*curSize + len + 1 > *totSize) {
            *totSize = *curSize + len + 1;
            param = (char *)realloc(param, *totSize);
        }
        strncpy(param + *curSize, node->value, len);
        *curSize += len;
        param[*curSize] = '\0';
        ALOGV("readParamValue() reading string %s", param + *curSize - len);
        return len;
    }
    ALOGW("readParamValue() unknown param type %s", node->name);
    return 0;
}

effect_param_t *AudioPolicyEffects::loadEffectParameter(cnode *root)
{
    cnode *param;
    cnode *value;
    size_t curSize = sizeof(effect_param_t);
    size_t totSize = sizeof(effect_param_t) + 2 * sizeof(int);
    effect_param_t *fx_param = (effect_param_t *)malloc(totSize);
    CHECK(fx_param != NULL);

    param = config_find(root, PARAM_TAG);
    value = config_find(root, VALUE_TAG);
    if (param == NULL && value == NULL) {
        // try to parse simple parameter form {int int}
        param = root->first_child;
        if (param != NULL) {
            // Note: that a pair of random strings is read as 0 0
            int *ptr = (int *)fx_param->data;
            int *ptr2 = (int *)((char *)param + sizeof(effect_param_t));
            ALOGW("loadEffectParameter() ptr %p ptr2 %p", ptr, ptr2);
            *ptr++ = atoi(param->name);
            *ptr = atoi(param->value);
            fx_param->psize = sizeof(int);
            fx_param->vsize = sizeof(int);
            return fx_param;
        }
    }
    if (param == NULL || value == NULL) {
        ALOGW("loadEffectParameter() invalid parameter description %s", root->name);
        goto error;
    }

    fx_param->psize = 0;
    param = param->first_child;
    while (param) {
        ALOGV("loadEffectParameter() reading param of type %s", param->name);
        size_t size = readParamValue(param, (char *)fx_param, &curSize, &totSize);
        if (size == 0) {
            goto error;
        }
        fx_param->psize += size;
        param = param->next;
    }

    // align start of value field on 32 bit boundary
    curSize = ((curSize - 1 ) / sizeof(int) + 1) * sizeof(int);

    fx_param->vsize = 0;
    value = value->first_child;
    while (value) {
        ALOGV("loadEffectParameter() reading value of type %s", value->name);
        size_t size = readParamValue(value, (char *)fx_param, &curSize, &totSize);
        if (size == 0) {
            goto error;
        }
        fx_param->vsize += size;
        value = value->next;
    }

    return fx_param;

error:
    delete fx_param;
    return NULL;
}

void AudioPolicyEffects::loadEffectParameters(cnode *root, Vector <effect_param_t *>& params)
{
    cnode *node = root->first_child;
    while (node) {
        ALOGV("loadEffectParameters() loading param %s", node->name);
        effect_param_t *param = loadEffectParameter(node);
        if (param == NULL) {
            node = node->next;
            continue;
        }
        params.add(param);
        node = node->next;
    }
}


AudioPolicyEffects::EffectDescVector *AudioPolicyEffects::loadEffectConfig(
                                                            cnode *root,
                                                            const Vector <EffectDesc *>& effects)
{
    cnode *node = root->first_child;
    if (node == NULL) {
        ALOGW("loadInputSource() empty element %s", root->name);
        return NULL;
    }
    EffectDescVector *desc = new EffectDescVector();
    while (node) {
        size_t i;
        for (i = 0; i < effects.size(); i++) {
            if (strncmp(effects[i]->mName, node->name, EFFECT_STRING_LEN_MAX) == 0) {
                ALOGV("loadEffectConfig() found effect %s in list", node->name);
                break;
            }
        }
        if (i == effects.size()) {
            ALOGV("loadEffectConfig() effect %s not in list", node->name);
            node = node->next;
            continue;
        }
        EffectDesc *effect = new EffectDesc(*effects[i]);   // deep copy
        loadEffectParameters(node, effect->mParams);
        ALOGV("loadEffectConfig() adding effect %s uuid %08x",
              effect->mName, effect->mUuid.timeLow);
        desc->mEffects.add(effect);
        node = node->next;
    }
    if (desc->mEffects.size() == 0) {
        ALOGW("loadEffectConfig() no valid effects found in config %s", root->name);
        delete desc;
        return NULL;
    }
    return desc;
}

status_t AudioPolicyEffects::loadInputEffectConfigurations(cnode *root,
                                                           const Vector <EffectDesc *>& effects)
{
    cnode *node = config_find(root, PREPROCESSING_TAG);
    if (node == NULL) {
        return -ENOENT;
    }
    node = node->first_child;
    while (node) {
        audio_source_t source = inputSourceNameToEnum(node->name);
        if (source == AUDIO_SOURCE_CNT) {
            ALOGW("loadInputSources() invalid input source %s", node->name);
            node = node->next;
            continue;
        }
        ALOGV("loadInputSources() loading input source %s", node->name);
        EffectDescVector *desc = loadEffectConfig(node, effects);
        if (desc == NULL) {
            node = node->next;
            continue;
        }
        mInputSources.add(source, desc);
        node = node->next;
    }
    return NO_ERROR;
}

status_t AudioPolicyEffects::loadStreamEffectConfigurations(cnode *root,
                                                            const Vector <EffectDesc *>& effects)
{
    cnode *node = config_find(root, OUTPUT_SESSION_PROCESSING_TAG);
    if (node == NULL) {
        return -ENOENT;
    }
    node = node->first_child;
    while (node) {
        audio_stream_type_t stream = streamNameToEnum(node->name);
        if (stream == AUDIO_STREAM_PUBLIC_CNT) {
            ALOGW("loadStreamEffectConfigurations() invalid output stream %s", node->name);
            node = node->next;
            continue;
        }
        ALOGV("loadStreamEffectConfigurations() loading output stream %s", node->name);
        EffectDescVector *desc = loadEffectConfig(node, effects);
        if (desc == NULL) {
            node = node->next;
            continue;
        }
        mOutputStreams.add(stream, desc);
        node = node->next;
    }
    return NO_ERROR;
}

AudioPolicyEffects::EffectDesc *AudioPolicyEffects::loadEffect(cnode *root)
{
    cnode *node = config_find(root, UUID_TAG);
    if (node == NULL) {
        return NULL;
    }
    effect_uuid_t uuid;
    if (AudioEffect::stringToGuid(node->value, &uuid) != NO_ERROR) {
        ALOGW("loadEffect() invalid uuid %s", node->value);
        return NULL;
    }
    return new EffectDesc(root->name, uuid);
}

status_t AudioPolicyEffects::loadEffects(cnode *root, Vector <EffectDesc *>& effects)
{
    cnode *node = config_find(root, EFFECTS_TAG);
    if (node == NULL) {
        return -ENOENT;
    }
    node = node->first_child;
    while (node) {
        ALOGV("loadEffects() loading effect %s", node->name);
        EffectDesc *effect = loadEffect(node);
        if (effect == NULL) {
            node = node->next;
            continue;
        }
        effects.add(effect);
        node = node->next;
    }
    return NO_ERROR;
}

status_t AudioPolicyEffects::loadAudioEffectConfig(const char *path)
{
    cnode *root;
    char *data;

    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    config_load(root, data);

    Vector <EffectDesc *> effects;
    loadEffects(root, effects);
    loadInputEffectConfigurations(root, effects);
    loadStreamEffectConfigurations(root, effects);

    for (size_t i = 0; i < effects.size(); i++) {
        delete effects[i];
    }

    config_free(root);
    free(root);
    free(data);

    return NO_ERROR;
}


}; // namespace android
