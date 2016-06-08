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


#define LOG_TAG "AudioFlinger::PatchPanel"
//#define LOG_NDEBUG 0

#include "Configuration.h"
#include <utils/Log.h>
#include <audio_utils/primitives.h>

#include "AudioFlinger.h"
#include "ServiceUtilities.h"
#include <media/AudioParameter.h>

// ----------------------------------------------------------------------------

// Note: the following macro is used for extremely verbose logging message.  In
// order to run with ALOG_ASSERT turned on, we need to have LOG_NDEBUG set to
// 0; but one side effect of this is to turn all LOGV's as well.  Some messages
// are so verbose that we want to suppress them even when we have ALOG_ASSERT
// turned on.  Do not uncomment the #def below unless you really know what you
// are doing and want to see all of the extremely verbose messages.
//#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

namespace android {

/* List connected audio ports and their attributes */
status_t AudioFlinger::listAudioPorts(unsigned int *num_ports,
                                struct audio_port *ports)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->listAudioPorts(num_ports, ports);
    }
    return NO_INIT;
}

/* Get supported attributes for a given audio port */
status_t AudioFlinger::getAudioPort(struct audio_port *port)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->getAudioPort(port);
    }
    return NO_INIT;
}


/* Connect a patch between several source and sink ports */
status_t AudioFlinger::createAudioPatch(const struct audio_patch *patch,
                                   audio_patch_handle_t *handle)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->createAudioPatch(patch, handle);
    }
    return NO_INIT;
}

/* Disconnect a patch */
status_t AudioFlinger::releaseAudioPatch(audio_patch_handle_t handle)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->releaseAudioPatch(handle);
    }
    return NO_INIT;
}


/* List connected audio ports and they attributes */
status_t AudioFlinger::listAudioPatches(unsigned int *num_patches,
                                  struct audio_patch *patches)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->listAudioPatches(num_patches, patches);
    }
    return NO_INIT;
}

/* Set audio port configuration */
status_t AudioFlinger::setAudioPortConfig(const struct audio_port_config *config)
{
    Mutex::Autolock _l(mLock);
    if (mPatchPanel != 0) {
        return mPatchPanel->setAudioPortConfig(config);
    }
    return NO_INIT;
}


AudioFlinger::PatchPanel::PatchPanel(const sp<AudioFlinger>& audioFlinger)
                                   : mAudioFlinger(audioFlinger)
{
}

AudioFlinger::PatchPanel::~PatchPanel()
{
}

/* List connected audio ports and their attributes */
status_t AudioFlinger::PatchPanel::listAudioPorts(unsigned int *num_ports __unused,
                                struct audio_port *ports __unused)
{
    ALOGV("listAudioPorts");
    return NO_ERROR;
}

/* Get supported attributes for a given audio port */
status_t AudioFlinger::PatchPanel::getAudioPort(struct audio_port *port __unused)
{
    ALOGV("getAudioPort");
    return NO_ERROR;
}


/* Connect a patch between several source and sink ports */
status_t AudioFlinger::PatchPanel::createAudioPatch(const struct audio_patch *patch,
                                   audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;
    audio_patch_handle_t halHandle = AUDIO_PATCH_HANDLE_NONE;
    sp<AudioFlinger> audioflinger = mAudioFlinger.promote();
    if (handle == NULL || patch == NULL) {
        return BAD_VALUE;
    }
    ALOGV("createAudioPatch() num_sources %d num_sinks %d handle %d",
          patch->num_sources, patch->num_sinks, *handle);
    if (audioflinger == 0) {
        return NO_INIT;
    }

    if (patch->num_sources == 0 || patch->num_sources > AUDIO_PATCH_PORTS_MAX ||
            (patch->num_sinks == 0 && patch->num_sources != 2) ||
            patch->num_sinks > AUDIO_PATCH_PORTS_MAX) {
        return BAD_VALUE;
    }
    // limit number of sources to 1 for now or 2 sources for special cross hw module case.
    // only the audio policy manager can request a patch creation with 2 sources.
    if (patch->num_sources > 2) {
        return INVALID_OPERATION;
    }

    if (*handle != AUDIO_PATCH_HANDLE_NONE) {
        for (size_t index = 0; *handle != 0 && index < mPatches.size(); index++) {
            if (*handle == mPatches[index]->mHandle) {
                ALOGV("createAudioPatch() removing patch handle %d", *handle);
                halHandle = mPatches[index]->mHalHandle;
                Patch *removedPatch = mPatches[index];
                // free resources owned by the removed patch if applicable
                // 1) if a software patch is present, release the playback and capture threads and
                // tracks created. This will also release the corresponding audio HAL patches
                if ((removedPatch->mRecordPatchHandle
                        != AUDIO_PATCH_HANDLE_NONE) ||
                        (removedPatch->mPlaybackPatchHandle !=
                                AUDIO_PATCH_HANDLE_NONE)) {
                    clearPatchConnections(removedPatch);
                }
                // 2) if the new patch and old patch source or sink are devices from different
                // hw modules,  clear the audio HAL patches now because they will not be updated
                // by call to create_audio_patch() below which will happen on a different HW module
                if (halHandle != AUDIO_PATCH_HANDLE_NONE) {
                    audio_module_handle_t hwModule = AUDIO_MODULE_HANDLE_NONE;
                    if ((removedPatch->mAudioPatch.sources[0].type == AUDIO_PORT_TYPE_DEVICE) &&
                        ((patch->sources[0].type != AUDIO_PORT_TYPE_DEVICE) ||
                          (removedPatch->mAudioPatch.sources[0].ext.device.hw_module !=
                           patch->sources[0].ext.device.hw_module))) {
                        hwModule = removedPatch->mAudioPatch.sources[0].ext.device.hw_module;
                    } else if ((patch->num_sinks == 0) ||
                            ((removedPatch->mAudioPatch.sinks[0].type == AUDIO_PORT_TYPE_DEVICE) &&
                             ((patch->sinks[0].type != AUDIO_PORT_TYPE_DEVICE) ||
                              (removedPatch->mAudioPatch.sinks[0].ext.device.hw_module !=
                               patch->sinks[0].ext.device.hw_module)))) {
                        // Note on (patch->num_sinks == 0): this situation should not happen as
                        // these special patches are only created by the policy manager but just
                        // in case, systematically clear the HAL patch.
                        // Note that removedPatch->mAudioPatch.num_sinks cannot be 0 here because
                        // halHandle would be AUDIO_PATCH_HANDLE_NONE in this case.
                        hwModule = removedPatch->mAudioPatch.sinks[0].ext.device.hw_module;
                    }
                    if (hwModule != AUDIO_MODULE_HANDLE_NONE) {
                        ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(hwModule);
                        if (index >= 0) {
                            audio_hw_device_t *hwDevice =
                                    audioflinger->mAudioHwDevs.valueAt(index)->hwDevice();
                            hwDevice->release_audio_patch(hwDevice, halHandle);
                        }
                    }
                }
                mPatches.removeAt(index);
                delete removedPatch;
                break;
            }
        }
    }

    Patch *newPatch = new Patch(patch);

    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            audio_module_handle_t srcModule = patch->sources[0].ext.device.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(srcModule);
            if (index < 0) {
                ALOGW("createAudioPatch() bad src hw module %d", srcModule);
                status = BAD_VALUE;
                goto exit;
            }
            AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                // support only one sink if connection to a mix or across HW modules
                if ((patch->sinks[i].type == AUDIO_PORT_TYPE_MIX ||
                        patch->sinks[i].ext.mix.hw_module != srcModule) &&
                        patch->num_sinks > 1) {
                    status = INVALID_OPERATION;
                    goto exit;
                }
                // reject connection to different sink types
                if (patch->sinks[i].type != patch->sinks[0].type) {
                    ALOGW("createAudioPatch() different sink types in same patch not supported");
                    status = BAD_VALUE;
                    goto exit;
                }
            }

            // manage patches requiring a software bridge
            // - special patch request with 2 sources (reuse one existing output mix) OR
            // - Device to device AND
            //    - source HW module != destination HW module OR
            //    - audio HAL version < 3.0
            if ((patch->num_sources == 2) ||
                ((patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) &&
                 ((patch->sinks[0].ext.device.hw_module != srcModule) ||
                  (audioHwDevice->version() < AUDIO_DEVICE_API_VERSION_3_0)))) {
                if (patch->num_sources == 2) {
                    if (patch->sources[1].type != AUDIO_PORT_TYPE_MIX ||
                            (patch->num_sinks != 0 && patch->sinks[0].ext.device.hw_module !=
                                    patch->sources[1].ext.mix.hw_module)) {
                        ALOGW("createAudioPatch() invalid source combination");
                        status = INVALID_OPERATION;
                        goto exit;
                    }

                    sp<ThreadBase> thread =
                            audioflinger->checkPlaybackThread_l(patch->sources[1].ext.mix.handle);
                    newPatch->mPlaybackThread = (MixerThread *)thread.get();
                    if (thread == 0) {
                        ALOGW("createAudioPatch() cannot get playback thread");
                        status = INVALID_OPERATION;
                        goto exit;
                    }
                } else {
                    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                    audio_devices_t device = patch->sinks[0].ext.device.type;
                    String8 address = String8(patch->sinks[0].ext.device.address);
                    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
                    newPatch->mPlaybackThread = audioflinger->openOutput_l(
                                                             patch->sinks[0].ext.device.hw_module,
                                                             &output,
                                                             &config,
                                                             device,
                                                             address,
                                                             AUDIO_OUTPUT_FLAG_NONE);
                    ALOGV("audioflinger->openOutput_l() returned %p",
                                          newPatch->mPlaybackThread.get());
                    if (newPatch->mPlaybackThread == 0) {
                        status = NO_MEMORY;
                        goto exit;
                    }
                }
                audio_devices_t device = patch->sources[0].ext.device.type;
                String8 address = String8(patch->sources[0].ext.device.address);
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                // open input stream with source device audio properties if provided or
                // default to peer output stream properties otherwise.
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
                    config.sample_rate = patch->sources[0].sample_rate;
                } else {
                    config.sample_rate = newPatch->mPlaybackThread->sampleRate();
                }
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
                    config.channel_mask = patch->sources[0].channel_mask;
                } else {
                    config.channel_mask =
                        audio_channel_in_mask_from_count(newPatch->mPlaybackThread->channelCount());
                }
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_FORMAT) {
                    config.format = patch->sources[0].format;
                } else {
                    config.format = newPatch->mPlaybackThread->format();
                }
                audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
                newPatch->mRecordThread = audioflinger->openInput_l(srcModule,
                                                                    &input,
                                                                    &config,
                                                                    device,
                                                                    address,
                                                                    AUDIO_SOURCE_MIC,
                                                                    AUDIO_INPUT_FLAG_NONE);
                ALOGV("audioflinger->openInput_l() returned %p inChannelMask %08x",
                      newPatch->mRecordThread.get(), config.channel_mask);
                if (newPatch->mRecordThread == 0) {
                    status = NO_MEMORY;
                    goto exit;
                }
                status = createPatchConnections(newPatch, patch);
                if (status != NO_ERROR) {
                    goto exit;
                }
            } else {
                if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                    sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                              patch->sinks[0].ext.mix.handle);
                    if (thread == 0) {
                        ALOGW("createAudioPatch() bad capture I/O handle %d",
                                                              patch->sinks[0].ext.mix.handle);
                        status = BAD_VALUE;
                        goto exit;
                    }
                    status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
                } else {
                    if (audioHwDevice->version() < AUDIO_DEVICE_API_VERSION_3_0) {
                        status = INVALID_OPERATION;
                        goto exit;
                    }

                    audio_hw_device_t *hwDevice = audioHwDevice->hwDevice();
                    status = hwDevice->create_audio_patch(hwDevice,
                                                           patch->num_sources,
                                                           patch->sources,
                                                           patch->num_sinks,
                                                           patch->sinks,
                                                           &halHandle);
                }
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            audio_module_handle_t srcModule =  patch->sources[0].ext.mix.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(srcModule);
            if (index < 0) {
                ALOGW("createAudioPatch() bad src hw module %d", srcModule);
                status = BAD_VALUE;
                goto exit;
            }
            // limit to connections between devices and output streams
            audio_devices_t type = AUDIO_DEVICE_NONE;
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                    ALOGW("createAudioPatch() invalid sink type %d for mix source",
                          patch->sinks[i].type);
                    status = BAD_VALUE;
                    goto exit;
                }
                // limit to connections between sinks and sources on same HW module
                if (patch->sinks[i].ext.device.hw_module != srcModule) {
                    status = BAD_VALUE;
                    goto exit;
                }
                type |= patch->sinks[i].ext.device.type;
            }
            sp<ThreadBase> thread =
                            audioflinger->checkPlaybackThread_l(patch->sources[0].ext.mix.handle);
            if (thread == 0) {
                ALOGW("createAudioPatch() bad playback I/O handle %d",
                          patch->sources[0].ext.mix.handle);
                status = BAD_VALUE;
                goto exit;
            }
            if (thread == audioflinger->primaryPlaybackThread_l()) {
                AudioParameter param = AudioParameter();
                param.addInt(String8(AUDIO_PARAMETER_STREAM_ROUTING), (int)type);

                audioflinger->broacastParametersToRecordThreads_l(param.toString());
            }

            status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
        } break;
        default:
            status = BAD_VALUE;
            goto exit;
    }
exit:
    ALOGV("createAudioPatch() status %d", status);
    if (status == NO_ERROR) {
        *handle = (audio_patch_handle_t) audioflinger->nextUniqueId(AUDIO_UNIQUE_ID_USE_PATCH);
        newPatch->mHandle = *handle;
        newPatch->mHalHandle = halHandle;
        mPatches.add(newPatch);
        ALOGV("createAudioPatch() added new patch handle %d halHandle %d", *handle, halHandle);
    } else {
        clearPatchConnections(newPatch);
        delete newPatch;
    }
    return status;
}

status_t AudioFlinger::PatchPanel::createPatchConnections(Patch *patch,
                                                          const struct audio_patch *audioPatch)
{
    // create patch from source device to record thread input
    struct audio_patch subPatch;
    subPatch.num_sources = 1;
    subPatch.sources[0] = audioPatch->sources[0];
    subPatch.num_sinks = 1;

    patch->mRecordThread->getAudioPortConfig(&subPatch.sinks[0]);
    subPatch.sinks[0].ext.mix.usecase.source = AUDIO_SOURCE_MIC;

    status_t status = createAudioPatch(&subPatch, &patch->mRecordPatchHandle);
    if (status != NO_ERROR) {
        patch->mRecordPatchHandle = AUDIO_PATCH_HANDLE_NONE;
        return status;
    }

    // create patch from playback thread output to sink device
    if (audioPatch->num_sinks != 0) {
        patch->mPlaybackThread->getAudioPortConfig(&subPatch.sources[0]);
        subPatch.sinks[0] = audioPatch->sinks[0];
        status = createAudioPatch(&subPatch, &patch->mPlaybackPatchHandle);
        if (status != NO_ERROR) {
            patch->mPlaybackPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            return status;
        }
    } else {
        patch->mPlaybackPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    }

    // use a pseudo LCM between input and output framecount
    size_t playbackFrameCount = patch->mPlaybackThread->frameCount();
    int playbackShift = __builtin_ctz(playbackFrameCount);
    size_t recordFramecount = patch->mRecordThread->frameCount();
    int shift = __builtin_ctz(recordFramecount);
    if (playbackShift < shift) {
        shift = playbackShift;
    }
    size_t frameCount = (playbackFrameCount * recordFramecount) >> shift;
    ALOGV("createPatchConnections() playframeCount %zu recordFramecount %zu frameCount %zu",
          playbackFrameCount, recordFramecount, frameCount);

    // create a special record track to capture from record thread
    uint32_t channelCount = patch->mPlaybackThread->channelCount();
    audio_channel_mask_t inChannelMask = audio_channel_in_mask_from_count(channelCount);
    audio_channel_mask_t outChannelMask = patch->mPlaybackThread->channelMask();
    uint32_t sampleRate = patch->mPlaybackThread->sampleRate();
    audio_format_t format = patch->mPlaybackThread->format();

    patch->mPatchRecord = new RecordThread::PatchRecord(
                                             patch->mRecordThread.get(),
                                             sampleRate,
                                             inChannelMask,
                                             format,
                                             frameCount,
                                             NULL,
                                             AUDIO_INPUT_FLAG_NONE);
    if (patch->mPatchRecord == 0) {
        return NO_MEMORY;
    }
    status = patch->mPatchRecord->initCheck();
    if (status != NO_ERROR) {
        return status;
    }
    patch->mRecordThread->addPatchRecord(patch->mPatchRecord);

    // create a special playback track to render to playback thread.
    // this track is given the same buffer as the PatchRecord buffer
    patch->mPatchTrack = new PlaybackThread::PatchTrack(
                                           patch->mPlaybackThread.get(),
                                           audioPatch->sources[1].ext.mix.usecase.stream,
                                           sampleRate,
                                           outChannelMask,
                                           format,
                                           frameCount,
                                           patch->mPatchRecord->buffer(),
                                           AUDIO_OUTPUT_FLAG_NONE);
    if (patch->mPatchTrack == 0) {
        return NO_MEMORY;
    }
    status = patch->mPatchTrack->initCheck();
    if (status != NO_ERROR) {
        return status;
    }
    patch->mPlaybackThread->addPatchTrack(patch->mPatchTrack);

    // tie playback and record tracks together
    patch->mPatchRecord->setPeerProxy(patch->mPatchTrack.get());
    patch->mPatchTrack->setPeerProxy(patch->mPatchRecord.get());

    // start capture and playback
    patch->mPatchRecord->start(AudioSystem::SYNC_EVENT_NONE, AUDIO_SESSION_NONE);
    patch->mPatchTrack->start();

    return status;
}

void AudioFlinger::PatchPanel::clearPatchConnections(Patch *patch)
{
    sp<AudioFlinger> audioflinger = mAudioFlinger.promote();
    if (audioflinger == 0) {
        return;
    }

    ALOGV("clearPatchConnections() patch->mRecordPatchHandle %d patch->mPlaybackPatchHandle %d",
          patch->mRecordPatchHandle, patch->mPlaybackPatchHandle);

    if (patch->mPatchRecord != 0) {
        patch->mPatchRecord->stop();
    }
    if (patch->mPatchTrack != 0) {
        patch->mPatchTrack->stop();
    }
    if (patch->mRecordPatchHandle != AUDIO_PATCH_HANDLE_NONE) {
        releaseAudioPatch(patch->mRecordPatchHandle);
        patch->mRecordPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    }
    if (patch->mPlaybackPatchHandle != AUDIO_PATCH_HANDLE_NONE) {
        releaseAudioPatch(patch->mPlaybackPatchHandle);
        patch->mPlaybackPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    }
    if (patch->mRecordThread != 0) {
        if (patch->mPatchRecord != 0) {
            patch->mRecordThread->deletePatchRecord(patch->mPatchRecord);
        }
        audioflinger->closeInputInternal_l(patch->mRecordThread);
    }
    if (patch->mPlaybackThread != 0) {
        if (patch->mPatchTrack != 0) {
            patch->mPlaybackThread->deletePatchTrack(patch->mPatchTrack);
        }
        // if num sources == 2 we are reusing an existing playback thread so we do not close it
        if (patch->mAudioPatch.num_sources != 2) {
            audioflinger->closeOutputInternal_l(patch->mPlaybackThread);
        }
    }
    if (patch->mRecordThread != 0) {
        if (patch->mPatchRecord != 0) {
            patch->mPatchRecord.clear();
        }
        patch->mRecordThread.clear();
    }
    if (patch->mPlaybackThread != 0) {
        if (patch->mPatchTrack != 0) {
            patch->mPatchTrack.clear();
        }
        patch->mPlaybackThread.clear();
    }

}

/* Disconnect a patch */
status_t AudioFlinger::PatchPanel::releaseAudioPatch(audio_patch_handle_t handle)
{
    ALOGV("releaseAudioPatch handle %d", handle);
    status_t status = NO_ERROR;
    size_t index;

    sp<AudioFlinger> audioflinger = mAudioFlinger.promote();
    if (audioflinger == 0) {
        return NO_INIT;
    }

    for (index = 0; index < mPatches.size(); index++) {
        if (handle == mPatches[index]->mHandle) {
            break;
        }
    }
    if (index == mPatches.size()) {
        return BAD_VALUE;
    }
    Patch *removedPatch = mPatches[index];
    mPatches.removeAt(index);

    struct audio_patch *patch = &removedPatch->mAudioPatch;

    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            audio_module_handle_t srcModule = patch->sources[0].ext.device.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(srcModule);
            if (index < 0) {
                ALOGW("releaseAudioPatch() bad src hw module %d", srcModule);
                status = BAD_VALUE;
                break;
            }

            if (removedPatch->mRecordPatchHandle != AUDIO_PATCH_HANDLE_NONE ||
                    removedPatch->mPlaybackPatchHandle != AUDIO_PATCH_HANDLE_NONE) {
                clearPatchConnections(removedPatch);
                break;
            }

            if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                                patch->sinks[0].ext.mix.handle);
                if (thread == 0) {
                    ALOGW("releaseAudioPatch() bad capture I/O handle %d",
                                                              patch->sinks[0].ext.mix.handle);
                    status = BAD_VALUE;
                    break;
                }
                status = thread->sendReleaseAudioPatchConfigEvent(removedPatch->mHalHandle);
            } else {
                AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
                if (audioHwDevice->version() < AUDIO_DEVICE_API_VERSION_3_0) {
                    status = INVALID_OPERATION;
                    break;
                }
                audio_hw_device_t *hwDevice = audioHwDevice->hwDevice();
                status = hwDevice->release_audio_patch(hwDevice, removedPatch->mHalHandle);
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            audio_module_handle_t srcModule =  patch->sources[0].ext.mix.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(srcModule);
            if (index < 0) {
                ALOGW("releaseAudioPatch() bad src hw module %d", srcModule);
                status = BAD_VALUE;
                break;
            }
            sp<ThreadBase> thread =
                            audioflinger->checkPlaybackThread_l(patch->sources[0].ext.mix.handle);
            if (thread == 0) {
                ALOGW("releaseAudioPatch() bad playback I/O handle %d",
                                                              patch->sources[0].ext.mix.handle);
                status = BAD_VALUE;
                break;
            }
            status = thread->sendReleaseAudioPatchConfigEvent(removedPatch->mHalHandle);
        } break;
        default:
            status = BAD_VALUE;
            break;
    }

    delete removedPatch;
    return status;
}


/* List connected audio ports and they attributes */
status_t AudioFlinger::PatchPanel::listAudioPatches(unsigned int *num_patches __unused,
                                  struct audio_patch *patches __unused)
{
    ALOGV("listAudioPatches");
    return NO_ERROR;
}

/* Set audio port configuration */
status_t AudioFlinger::PatchPanel::setAudioPortConfig(const struct audio_port_config *config)
{
    ALOGV("setAudioPortConfig");

    sp<AudioFlinger> audioflinger = mAudioFlinger.promote();
    if (audioflinger == 0) {
        return NO_INIT;
    }

    audio_module_handle_t module;
    if (config->type == AUDIO_PORT_TYPE_DEVICE) {
        module = config->ext.device.hw_module;
    } else {
        module = config->ext.mix.hw_module;
    }

    ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(module);
    if (index < 0) {
        ALOGW("setAudioPortConfig() bad hw module %d", module);
        return BAD_VALUE;
    }

    AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
    if (audioHwDevice->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        audio_hw_device_t *hwDevice = audioHwDevice->hwDevice();
        return hwDevice->set_audio_port_config(hwDevice, config);
    } else {
        return INVALID_OPERATION;
    }
    return NO_ERROR;
}

} // namespace android
