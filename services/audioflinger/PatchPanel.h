/*
**
** Copyright 2014, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef INCLUDING_FROM_AUDIOFLINGER_H
    #error This header file should only be included from AudioFlinger.h
#endif

class PatchPanel : public RefBase {
public:

    class Patch;

    PatchPanel(const sp<AudioFlinger>& audioFlinger);
    virtual ~PatchPanel();

    /* List connected audio ports and their attributes */
    status_t listAudioPorts(unsigned int *num_ports,
                                    struct audio_port *ports);

    /* Get supported attributes for a given audio port */
    status_t getAudioPort(struct audio_port *port);

    /* Create a patch between several source and sink ports */
    status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle);

    /* Release a patch */
    status_t releaseAudioPatch(audio_patch_handle_t handle);

    /* List connected audio devices and they attributes */
    status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches);

    /* Set audio port configuration */
    status_t setAudioPortConfig(const struct audio_port_config *config);

    status_t createPatchConnections(Patch *patch,
                                    const struct audio_patch *audioPatch);
    void clearPatchConnections(Patch *patch);

    class Patch {
    public:
        Patch(const struct audio_patch *patch) :
            mAudioPatch(*patch), mHandle(AUDIO_PATCH_HANDLE_NONE),
            mHalHandle(AUDIO_PATCH_HANDLE_NONE), mRecordPatchHandle(AUDIO_PATCH_HANDLE_NONE),
            mPlaybackPatchHandle(AUDIO_PATCH_HANDLE_NONE) {}
        ~Patch() {}

        struct audio_patch              mAudioPatch;
        audio_patch_handle_t            mHandle;
        audio_patch_handle_t            mHalHandle;
        sp<PlaybackThread>              mPlaybackThread;
        sp<PlaybackThread::PatchTrack>  mPatchTrack;
        sp<RecordThread>                mRecordThread;
        sp<RecordThread::PatchRecord>   mPatchRecord;
        audio_patch_handle_t            mRecordPatchHandle;
        audio_patch_handle_t            mPlaybackPatchHandle;

    };

private:
    const wp<AudioFlinger>      mAudioFlinger;
    SortedVector <Patch *>      mPatches;
};
