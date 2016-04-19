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

#ifndef ANDROID_AUDIOPOLICYEFFECTS_H
#define ANDROID_AUDIOPOLICYEFFECTS_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cutils/misc.h>
#include <media/AudioEffect.h>
#include <system/audio.h>
#include <hardware/audio_effect.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>

#include <media/stagefright/foundation/ADebug.h>

namespace android {

class AudioPolicyService;

// ----------------------------------------------------------------------------

// AudioPolicyEffects class
// This class will manage all effects attached to input and output streams in
// AudioPolicyService as configured in audio_effects.conf.
class AudioPolicyEffects : public RefBase
{

public:

    // The constructor will parse audio_effects.conf
    // First it will look whether vendor specific file exists,
    // otherwise it will parse the system default file.
	         AudioPolicyEffects(AudioPolicyService *audioPolicyService);
    virtual ~AudioPolicyEffects();

    // NOTE: methods on AudioPolicyEffects should never be called with the AudioPolicyService
    // main mutex (mLock) held as they will indirectly call back into AudioPolicyService when
    // managing audio effects.

    // Return a list of effect descriptors for default input effects
    // associated with audioSession
    status_t queryDefaultInputEffects(int audioSession,
                             effect_descriptor_t *descriptors,
                             uint32_t *count);

    // Add all input effects associated with this input
    // Effects are attached depending on the audio_source_t
    status_t addInputEffects(audio_io_handle_t input,
                             audio_source_t inputSource,
                             int audioSession);

    // Add all input effects associated to this input
    status_t releaseInputEffects(audio_io_handle_t input);


    // Return a list of effect descriptors for default output effects
    // associated with audioSession
    status_t queryDefaultOutputSessionEffects(int audioSession,
                             effect_descriptor_t *descriptors,
                             uint32_t *count);

    // Add all output effects associated to this output
    // Effects are attached depending on the audio_stream_type_t
    status_t addOutputSessionEffects(audio_io_handle_t output,
                             audio_stream_type_t stream,
                             int audioSession);

    // release all output effects associated with this output stream and audiosession
    status_t releaseOutputSessionEffects(audio_io_handle_t output,
                             audio_stream_type_t stream,
                             int audioSession);

    status_t doAddOutputSessionEffects(audio_io_handle_t output,
                             audio_stream_type_t stream,
                             int audioSession,
                             audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE,
                             audio_channel_mask_t channelMask = 0, uid_t uid = 0);

private:

    // class to store the description of an effects and its parameters
    // as defined in audio_effects.conf
    class EffectDesc {
    public:
        EffectDesc(const char *name, const effect_uuid_t& uuid) :
                        mName(strdup(name)),
                        mUuid(uuid) { }
        EffectDesc(const EffectDesc& orig) :
                        mName(strdup(orig.mName)),
                        mUuid(orig.mUuid) {
                            // deep copy mParams
                            for (size_t k = 0; k < orig.mParams.size(); k++) {
                                effect_param_t *origParam = orig.mParams[k];
                                // psize and vsize are rounded up to an int boundary for allocation
                                size_t origSize = sizeof(effect_param_t) +
                                                  ((origParam->psize + 3) & ~3) +
                                                  ((origParam->vsize + 3) & ~3);
                                effect_param_t *dupParam = (effect_param_t *) malloc(origSize);
                                CHECK(dupParam != NULL);
                                memcpy(dupParam, origParam, origSize);
                                // This works because the param buffer allocation is also done by
                                // multiples of 4 bytes originally. In theory we should memcpy only
                                // the actual param size, that is without rounding vsize.
                                mParams.add(dupParam);
                            }
                        }
        /*virtual*/ ~EffectDesc() {
            free(mName);
            for (size_t k = 0; k < mParams.size(); k++) {
                free(mParams[k]);
            }
        }
        char *mName;
        effect_uuid_t mUuid;
        Vector <effect_param_t *> mParams;
    };

    // class to store voctor of EffectDesc
    class EffectDescVector {
    public:
        EffectDescVector() {}
        /*virtual*/ ~EffectDescVector() {
            for (size_t j = 0; j < mEffects.size(); j++) {
                delete mEffects[j];
            }
        }
        Vector <EffectDesc *> mEffects;
    };

    // class to store voctor of AudioEffects
    class EffectVector {
    public:
        EffectVector(int session) : mSessionId(session), mRefCount(0) {}
        /*virtual*/ ~EffectVector() {}

        // Enable or disable all effects in effect vector
        void setProcessorEnabled(bool enabled);

        const int mSessionId;
        // AudioPolicyManager keeps mLock, no need for lock on reference count here
        int mRefCount;
        Vector< sp<AudioEffect> >mEffects;
    };


    static const char * const kInputSourceNames[AUDIO_SOURCE_CNT -1];
    static audio_source_t inputSourceNameToEnum(const char *name);

    static const char *kStreamNames[AUDIO_STREAM_PUBLIC_CNT+1]; //+1 required as streams start from -1
    audio_stream_type_t streamNameToEnum(const char *name);

    // Parse audio_effects.conf
    status_t loadAudioEffectConfig(const char *path);

    // Load all effects descriptors in configuration file
    status_t loadEffects(cnode *root, Vector <EffectDesc *>& effects);
    EffectDesc *loadEffect(cnode *root);

    // Load all automatic effect configurations
    status_t loadInputEffectConfigurations(cnode *root, const Vector <EffectDesc *>& effects);
    status_t loadStreamEffectConfigurations(cnode *root, const Vector <EffectDesc *>& effects);
    EffectDescVector *loadEffectConfig(cnode *root, const Vector <EffectDesc *>& effects);

    // Load all automatic effect parameters
    void loadEffectParameters(cnode *root, Vector <effect_param_t *>& params);
    effect_param_t *loadEffectParameter(cnode *root);
    size_t readParamValue(cnode *node,
                          char *param,
                          size_t *curSize,
                          size_t *totSize);
    size_t growParamSize(char *param,
                         size_t size,
                         size_t *curSize,
                         size_t *totSize);

    // protects access to mInputSources, mInputs, mOutputStreams, mOutputSessions
    Mutex mLock;
    // Automatic input effects are configured per audio_source_t
    KeyedVector< audio_source_t, EffectDescVector* > mInputSources;
    // Automatic input effects are unique for audio_io_handle_t
    KeyedVector< audio_io_handle_t, EffectVector* > mInputs;

    // Automatic output effects are organized per audio_stream_type_t
    KeyedVector< audio_stream_type_t, EffectDescVector* > mOutputStreams;
    // Automatic output effects are unique for audiosession ID
    KeyedVector< int32_t, EffectVector* > mOutputSessions;

    AudioPolicyService *mAudioPolicyService;
};

}; // namespace android

#endif // ANDROID_AUDIOPOLICYEFFECTS_H
