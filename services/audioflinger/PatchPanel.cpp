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
    ALOGV("createAudioPatch() num_sources %d num_sinks %d handle %d",
          patch->num_sources, patch->num_sinks, *handle);
    status_t status = NO_ERROR;

    audio_patch_handle_t halHandle = AUDIO_PATCH_HANDLE_NONE;

    sp<AudioFlinger> audioflinger = mAudioFlinger.promote();
    if (audioflinger == 0) {
        return NO_INIT;
    }
    if (handle == NULL || patch == NULL) {
        return BAD_VALUE;
    }
    // limit number of sources to 1 for now
    if (patch->num_sources == 0 || patch->num_sources > 1 ||
            patch->num_sinks == 0 || patch->num_sinks > AUDIO_PATCH_PORTS_MAX) {
        return BAD_VALUE;
    }

    for (size_t index = 0; *handle != 0 && index < mPatches.size(); index++) {
        if (*handle == mPatches[index]->mHandle) {
            ALOGV("createAudioPatch() removing patch handle %d", *handle);
            halHandle = mPatches[index]->mHalHandle;
            mPatches.removeAt(index);
            break;
        }
    }

    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            // limit number of sinks to 1 for now
            if (patch->num_sinks > 1) {
                return BAD_VALUE;
            }
            audio_module_handle_t src_module = patch->sources[0].ext.device.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(src_module);
            if (index < 0) {
                ALOGW("createAudioPatch() bad src hw module %d", src_module);
                return BAD_VALUE;
            }
            AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                // reject connection to different sink types
                if (patch->sinks[i].type != patch->sinks[0].type) {
                    ALOGW("createAudioPatch() different sink types in same patch not supported");
                    return BAD_VALUE;
                }
                // limit to connections between sinks and sources on same HW module
                if (patch->sinks[i].ext.mix.hw_module != src_module) {
                    ALOGW("createAudioPatch() cannot connect source on module %d to"
                            "sink on module %d", src_module, patch->sinks[i].ext.mix.hw_module);
                    return BAD_VALUE;
                }

                // limit to connections between devices and output streams for HAL before 3.0
                if ((audioHwDevice->version() < AUDIO_DEVICE_API_VERSION_3_0) &&
                        (patch->sinks[i].type != AUDIO_PORT_TYPE_MIX)) {
                    ALOGW("createAudioPatch() invalid sink type %d for device source",
                          patch->sinks[i].type);
                    return BAD_VALUE;
                }
            }

            if (audioHwDevice->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
                if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                    sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                                    patch->sinks[0].ext.mix.handle);
                    if (thread == 0) {
                        ALOGW("createAudioPatch() bad capture I/O handle %d",
                                                                  patch->sinks[0].ext.mix.handle);
                        return BAD_VALUE;
                    }
                    status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
                } else {
                    audio_hw_device_t *hwDevice = audioHwDevice->hwDevice();
                    status = hwDevice->create_audio_patch(hwDevice,
                                                           patch->num_sources,
                                                           patch->sources,
                                                           patch->num_sinks,
                                                           patch->sinks,
                                                           &halHandle);
                }
            } else {
                sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                                    patch->sinks[0].ext.mix.handle);
                if (thread == 0) {
                    ALOGW("createAudioPatch() bad capture I/O handle %d",
                                                                  patch->sinks[0].ext.mix.handle);
                    return BAD_VALUE;
                }
                AudioParameter param;
                param.addInt(String8(AudioParameter::keyRouting),
                             (int)patch->sources[0].ext.device.type);
                param.addInt(String8(AudioParameter::keyInputSource),
                                                     (int)patch->sinks[0].ext.mix.usecase.source);

                ALOGW("createAudioPatch() AUDIO_PORT_TYPE_DEVICE setParameters %s",
                                                                      param.toString().string());
                status = thread->setParameters(param.toString());
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            audio_module_handle_t src_module =  patch->sources[0].ext.mix.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(src_module);
            if (index < 0) {
                ALOGW("createAudioPatch() bad src hw module %d", src_module);
                return BAD_VALUE;
            }
            // limit to connections between devices and output streams
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                    ALOGW("createAudioPatch() invalid sink type %d for bus source",
                          patch->sinks[i].type);
                    return BAD_VALUE;
                }
                // limit to connections between sinks and sources on same HW module
                if (patch->sinks[i].ext.device.hw_module != src_module) {
                    return BAD_VALUE;
                }
            }
            AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
            sp<ThreadBase> thread =
                            audioflinger->checkPlaybackThread_l(patch->sources[0].ext.mix.handle);
            if (thread == 0) {
                ALOGW("createAudioPatch() bad playback I/O handle %d",
                          patch->sources[0].ext.mix.handle);
                return BAD_VALUE;
            }
            if (audioHwDevice->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
                status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
            } else {
                audio_devices_t type = AUDIO_DEVICE_NONE;
                for (unsigned int i = 0; i < patch->num_sinks; i++) {
                    type |= patch->sinks[i].ext.device.type;
                }
                AudioParameter param;
                param.addInt(String8(AudioParameter::keyRouting), (int)type);
                status = thread->setParameters(param.toString());
            }

        } break;
        default:
            return BAD_VALUE;
    }
    ALOGV("createAudioPatch() status %d", status);
    if (status == NO_ERROR) {
        *handle = audioflinger->nextUniqueId();
        Patch *newPatch = new Patch(patch);
        newPatch->mHandle = *handle;
        newPatch->mHalHandle = halHandle;
        mPatches.add(newPatch);
        ALOGV("createAudioPatch() added new patch handle %d halHandle %d", *handle, halHandle);
    }
    return status;
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

    struct audio_patch *patch = &mPatches[index]->mAudioPatch;

    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            audio_module_handle_t src_module = patch->sources[0].ext.device.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(src_module);
            if (index < 0) {
                ALOGW("releaseAudioPatch() bad src hw module %d", src_module);
                status = BAD_VALUE;
                break;
            }
            AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
            if (audioHwDevice->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
                if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                    sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                                    patch->sinks[0].ext.mix.handle);
                    if (thread == 0) {
                        ALOGW("createAudioPatch() bad capture I/O handle %d",
                                                                  patch->sinks[0].ext.mix.handle);
                        status = BAD_VALUE;
                        break;
                    }
                    status = thread->sendReleaseAudioPatchConfigEvent(mPatches[index]->mHalHandle);
                } else {
                    audio_hw_device_t *hwDevice = audioHwDevice->hwDevice();
                    status = hwDevice->release_audio_patch(hwDevice, mPatches[index]->mHalHandle);
                }
            } else {
                sp<ThreadBase> thread = audioflinger->checkRecordThread_l(
                                                                    patch->sinks[0].ext.mix.handle);
                if (thread == 0) {
                    ALOGW("releaseAudioPatch() bad capture I/O handle %d",
                                                                  patch->sinks[0].ext.mix.handle);
                    status = BAD_VALUE;
                    break;
                }
                AudioParameter param;
                param.addInt(String8(AudioParameter::keyRouting), 0);
                ALOGW("releaseAudioPatch() AUDIO_PORT_TYPE_DEVICE setParameters %s",
                                                                      param.toString().string());
                status = thread->setParameters(param.toString());
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            audio_module_handle_t src_module =  patch->sources[0].ext.mix.hw_module;
            ssize_t index = audioflinger->mAudioHwDevs.indexOfKey(src_module);
            if (index < 0) {
                ALOGW("releaseAudioPatch() bad src hw module %d", src_module);
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
            AudioHwDevice *audioHwDevice = audioflinger->mAudioHwDevs.valueAt(index);
            if (audioHwDevice->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
                status = thread->sendReleaseAudioPatchConfigEvent(mPatches[index]->mHalHandle);
            } else {
                AudioParameter param;
                param.addInt(String8(AudioParameter::keyRouting), (int)0);
                status = thread->setParameters(param.toString());
            }
        } break;
        default:
            status = BAD_VALUE;
            break;
    }

    delete (mPatches[index]);
    mPatches.removeAt(index);
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
status_t AudioFlinger::PatchPanel::setAudioPortConfig(
        const struct audio_port_config *config __unused)
{
    ALOGV("setAudioPortConfig");
    return NO_ERROR;
}



}; // namespace android
