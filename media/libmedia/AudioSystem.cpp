/*
 * Copyright (C) 2006-2007 The Android Open Source Project
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

#define LOG_TAG "AudioSystem"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <binder/IServiceManager.h>
#include <media/AudioSystem.h>
#include <media/IAudioFlinger.h>
#include <media/IAudioPolicyService.h>
#include <math.h>

#include <system/audio.h>

// ----------------------------------------------------------------------------

namespace android {

// client singleton for AudioFlinger binder interface
Mutex AudioSystem::gLock;
Mutex AudioSystem::gLockAPS;
sp<IAudioFlinger> AudioSystem::gAudioFlinger;
sp<AudioSystem::AudioFlingerClient> AudioSystem::gAudioFlingerClient;
audio_error_callback AudioSystem::gAudioErrorCallback = NULL;
dynamic_policy_callback AudioSystem::gDynPolicyCallback = NULL;
effect_session_callback AudioSystem::gEffectSessionCallback = NULL;

// establish binder interface to AudioFlinger service
const sp<IAudioFlinger> AudioSystem::get_audio_flinger()
{
    sp<IAudioFlinger> af;
    sp<AudioFlingerClient> afc;
    {
        Mutex::Autolock _l(gLock);
        if (gAudioFlinger == 0) {
            sp<IServiceManager> sm = defaultServiceManager();
            sp<IBinder> binder;
            do {
                binder = sm->getService(String16("media.audio_flinger"));
                if (binder != 0)
                    break;
                ALOGW("AudioFlinger not published, waiting...");
                usleep(500000); // 0.5 s
            } while (true);
            if (gAudioFlingerClient == NULL) {
                gAudioFlingerClient = new AudioFlingerClient();
            } else {
                if (gAudioErrorCallback) {
                    gAudioErrorCallback(NO_ERROR);
                }
            }
            binder->linkToDeath(gAudioFlingerClient);
            gAudioFlinger = interface_cast<IAudioFlinger>(binder);
            LOG_ALWAYS_FATAL_IF(gAudioFlinger == 0);
            afc = gAudioFlingerClient;
        }
        af = gAudioFlinger;
    }
    if (afc != 0) {
        af->registerClient(afc);
    }
    return af;
}

const sp<AudioSystem::AudioFlingerClient> AudioSystem::getAudioFlingerClient()
{
    // calling get_audio_flinger() will initialize gAudioFlingerClient if needed
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return 0;
    Mutex::Autolock _l(gLock);
    return gAudioFlingerClient;
}

sp<AudioIoDescriptor> AudioSystem::getIoDescriptor(audio_io_handle_t ioHandle)
{
    sp<AudioIoDescriptor> desc;
    const sp<AudioFlingerClient> afc = getAudioFlingerClient();
    if (afc != 0) {
        desc = afc->getIoDescriptor(ioHandle);
    }
    return desc;
}

/* static */ status_t AudioSystem::checkAudioFlinger()
{
    if (defaultServiceManager()->checkService(String16("media.audio_flinger")) != 0) {
        return NO_ERROR;
    }
    return DEAD_OBJECT;
}

status_t AudioSystem::muteMicrophone(bool state)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    return af->setMicMute(state);
}

status_t AudioSystem::isMicrophoneMuted(bool* state)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    *state = af->getMicMute();
    return NO_ERROR;
}

status_t AudioSystem::setMasterVolume(float value)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    af->setMasterVolume(value);
    return NO_ERROR;
}

status_t AudioSystem::setMasterMute(bool mute)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    af->setMasterMute(mute);
    return NO_ERROR;
}

status_t AudioSystem::getMasterVolume(float* volume)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    *volume = af->masterVolume();
    return NO_ERROR;
}

status_t AudioSystem::getMasterMute(bool* mute)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    *mute = af->masterMute();
    return NO_ERROR;
}

status_t AudioSystem::setStreamVolume(audio_stream_type_t stream, float value,
        audio_io_handle_t output)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) return BAD_VALUE;
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    af->setStreamVolume(stream, value, output);
    return NO_ERROR;
}

status_t AudioSystem::setStreamMute(audio_stream_type_t stream, bool mute)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) return BAD_VALUE;
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    af->setStreamMute(stream, mute);
    return NO_ERROR;
}

status_t AudioSystem::getStreamVolume(audio_stream_type_t stream, float* volume,
        audio_io_handle_t output)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) return BAD_VALUE;
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    *volume = af->streamVolume(stream, output);
    return NO_ERROR;
}

status_t AudioSystem::getStreamMute(audio_stream_type_t stream, bool* mute)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) return BAD_VALUE;
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    *mute = af->streamMute(stream);
    return NO_ERROR;
}

status_t AudioSystem::setMode(audio_mode_t mode)
{
    if (uint32_t(mode) >= AUDIO_MODE_CNT) return BAD_VALUE;
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    return af->setMode(mode);
}

status_t AudioSystem::setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    return af->setParameters(ioHandle, keyValuePairs);
}

String8 AudioSystem::getParameters(audio_io_handle_t ioHandle, const String8& keys)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    String8 result = String8("");
    if (af == 0) return result;

    result = af->getParameters(ioHandle, keys);
    return result;
}

status_t AudioSystem::setParameters(const String8& keyValuePairs)
{
    return setParameters(AUDIO_IO_HANDLE_NONE, keyValuePairs);
}

String8 AudioSystem::getParameters(const String8& keys)
{
    return getParameters(AUDIO_IO_HANDLE_NONE, keys);
}

// convert volume steps to natural log scale

// change this value to change volume scaling
static const float dBPerStep = 0.5f;
// shouldn't need to touch these
static const float dBConvert = -dBPerStep * 2.302585093f / 20.0f;
static const float dBConvertInverse = 1.0f / dBConvert;

float AudioSystem::linearToLog(int volume)
{
    // float v = volume ? exp(float(100 - volume) * dBConvert) : 0;
    // ALOGD("linearToLog(%d)=%f", volume, v);
    // return v;
    return volume ? exp(float(100 - volume) * dBConvert) : 0;
}

int AudioSystem::logToLinear(float volume)
{
    // int v = volume ? 100 - int(dBConvertInverse * log(volume) + 0.5) : 0;
    // ALOGD("logTolinear(%d)=%f", v, volume);
    // return v;
    return volume ? 100 - int(dBConvertInverse * log(volume) + 0.5) : 0;
}

status_t AudioSystem::getOutputSamplingRate(uint32_t* samplingRate, audio_stream_type_t streamType)
{
    audio_io_handle_t output;

    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }

    output = getOutput(streamType);
    if (output == 0) {
        return PERMISSION_DENIED;
    }

    return getSamplingRate(output, samplingRate);
}

status_t AudioSystem::getSamplingRate(audio_io_handle_t output,
                                      uint32_t* samplingRate)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    sp<AudioIoDescriptor> outputDesc = getIoDescriptor(output);
    if (outputDesc == 0) {
        ALOGV("getOutputSamplingRate() no output descriptor for output %d in gOutputs", output);
        *samplingRate = af->sampleRate(output);
    } else {
        ALOGV("getOutputSamplingRate() reading from output desc");
        *samplingRate = outputDesc->mSamplingRate;
    }
    if (*samplingRate == 0) {
        ALOGE("AudioSystem::getSamplingRate failed for output %d", output);
        return BAD_VALUE;
    }

    ALOGV("getSamplingRate() output %d, sampling rate %u", output, *samplingRate);

    return NO_ERROR;
}

status_t AudioSystem::getOutputFrameCount(size_t* frameCount, audio_stream_type_t streamType)
{
    audio_io_handle_t output;

    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }

    output = getOutput(streamType);
    if (output == AUDIO_IO_HANDLE_NONE) {
        return PERMISSION_DENIED;
    }

    return getFrameCount(output, frameCount);
}

status_t AudioSystem::getFrameCount(audio_io_handle_t output,
                                    size_t* frameCount)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    sp<AudioIoDescriptor> outputDesc = getIoDescriptor(output);
    if (outputDesc == 0) {
        *frameCount = af->frameCount(output);
    } else {
        *frameCount = outputDesc->mFrameCount;
    }
    if (*frameCount == 0) {
        ALOGE("AudioSystem::getFrameCount failed for output %d", output);
        return BAD_VALUE;
    }

    ALOGV("getFrameCount() output %d, frameCount %zu", output, *frameCount);

    return NO_ERROR;
}

status_t AudioSystem::getOutputLatency(uint32_t* latency, audio_stream_type_t streamType)
{
    audio_io_handle_t output;

    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }

    output = getOutput(streamType);
    if (output == AUDIO_IO_HANDLE_NONE) {
        return PERMISSION_DENIED;
    }

    return getLatency(output, latency);
}

status_t AudioSystem::getLatency(audio_io_handle_t output,
                                 uint32_t* latency)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    sp<AudioIoDescriptor> outputDesc = getIoDescriptor(output);
    if (outputDesc == 0) {
        *latency = af->latency(output);
    } else {
        *latency = outputDesc->mLatency;
    }

    ALOGV("getLatency() output %d, latency %d", output, *latency);

    return NO_ERROR;
}

status_t AudioSystem::getInputBufferSize(uint32_t sampleRate, audio_format_t format,
        audio_channel_mask_t channelMask, size_t* buffSize)
{
    const sp<AudioFlingerClient> afc = getAudioFlingerClient();
    if (afc == 0) {
        return NO_INIT;
    }
    return afc->getInputBufferSize(sampleRate, format, channelMask, buffSize);
}

status_t AudioSystem::setVoiceVolume(float value)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    return af->setVoiceVolume(value);
}

status_t AudioSystem::getRenderPosition(audio_io_handle_t output, uint32_t *halFrames,
                                        uint32_t *dspFrames)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;

    return af->getRenderPosition(halFrames, dspFrames, output);
}

uint32_t AudioSystem::getInputFramesLost(audio_io_handle_t ioHandle)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    uint32_t result = 0;
    if (af == 0) return result;
    if (ioHandle == AUDIO_IO_HANDLE_NONE) return result;

    result = af->getInputFramesLost(ioHandle);
    return result;
}

audio_unique_id_t AudioSystem::newAudioUniqueId()
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return AUDIO_UNIQUE_ID_ALLOCATE;
    return af->newAudioUniqueId();
}

void AudioSystem::acquireAudioSessionId(int audioSession, pid_t pid)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af != 0) {
        af->acquireAudioSessionId(audioSession, pid);
    }
}

void AudioSystem::releaseAudioSessionId(int audioSession, pid_t pid)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af != 0) {
        af->releaseAudioSessionId(audioSession, pid);
    }
}

audio_hw_sync_t AudioSystem::getAudioHwSyncForSession(audio_session_t sessionId)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return AUDIO_HW_SYNC_INVALID;
    return af->getAudioHwSyncForSession(sessionId);
}

status_t AudioSystem::systemReady()
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return NO_INIT;
    return af->systemReady();
}

// ---------------------------------------------------------------------------


void AudioSystem::AudioFlingerClient::clearIoCache()
{
    Mutex::Autolock _l(mLock);
    mIoDescriptors.clear();
    mInBuffSize = 0;
    mInSamplingRate = 0;
    mInFormat = AUDIO_FORMAT_DEFAULT;
    mInChannelMask = AUDIO_CHANNEL_NONE;
}

void AudioSystem::AudioFlingerClient::binderDied(const wp<IBinder>& who __unused)
{
    audio_error_callback cb = NULL;
    {
        Mutex::Autolock _l(AudioSystem::gLock);
        AudioSystem::gAudioFlinger.clear();
        cb = gAudioErrorCallback;
    }

    // clear output handles and stream to output map caches
    clearIoCache();

    if (cb) {
        cb(DEAD_OBJECT);
    }
    ALOGW("AudioFlinger server died!");
}

void AudioSystem::AudioFlingerClient::ioConfigChanged(audio_io_config_event event,
                                                      const sp<AudioIoDescriptor>& ioDesc) {
    ALOGV("ioConfigChanged() event %d", event);

    if (ioDesc == 0 || ioDesc->mIoHandle == AUDIO_IO_HANDLE_NONE) return;

    audio_port_handle_t deviceId = AUDIO_PORT_HANDLE_NONE;
    Vector < sp<AudioDeviceCallback> > callbacks;

    {
        Mutex::Autolock _l(mLock);

        switch (event) {
        case AUDIO_OUTPUT_OPENED:
        case AUDIO_INPUT_OPENED: {
            sp<AudioIoDescriptor> oldDesc = getIoDescriptor_l(ioDesc->mIoHandle);
            if (oldDesc == 0) {
                mIoDescriptors.add(ioDesc->mIoHandle, ioDesc);
            } else {
                deviceId = oldDesc->getDeviceId();
                mIoDescriptors.replaceValueFor(ioDesc->mIoHandle, ioDesc);
            }

            if (ioDesc->getDeviceId() != AUDIO_PORT_HANDLE_NONE) {
                deviceId = ioDesc->getDeviceId();
                ssize_t ioIndex = mAudioDeviceCallbacks.indexOfKey(ioDesc->mIoHandle);
                if (ioIndex >= 0) {
                    callbacks = mAudioDeviceCallbacks.valueAt(ioIndex);
                }
            }
            ALOGV("ioConfigChanged() new %s opened %d samplingRate %u, format %#x channel mask %#x "
                    "frameCount %zu deviceId %d", event == AUDIO_OUTPUT_OPENED ? "output" : "input",
                    ioDesc->mIoHandle, ioDesc->mSamplingRate, ioDesc->mFormat, ioDesc->mChannelMask,
                    ioDesc->mFrameCount, ioDesc->getDeviceId());
            } break;
        case AUDIO_OUTPUT_CLOSED:
        case AUDIO_INPUT_CLOSED: {
            if (getIoDescriptor_l(ioDesc->mIoHandle) == 0) {
                ALOGW("ioConfigChanged() closing unknown %s %d",
                      event == AUDIO_OUTPUT_CLOSED ? "output" : "input", ioDesc->mIoHandle);
                break;
            }
            ALOGV("ioConfigChanged() %s %d closed",
                  event == AUDIO_OUTPUT_CLOSED ? "output" : "input", ioDesc->mIoHandle);

            mIoDescriptors.removeItem(ioDesc->mIoHandle);
            mAudioDeviceCallbacks.removeItem(ioDesc->mIoHandle);
            } break;

        case AUDIO_OUTPUT_CONFIG_CHANGED:
        case AUDIO_INPUT_CONFIG_CHANGED: {
            sp<AudioIoDescriptor> oldDesc = getIoDescriptor_l(ioDesc->mIoHandle);
            if (oldDesc == 0) {
                ALOGW("ioConfigChanged() modifying unknown output! %d", ioDesc->mIoHandle);
                break;
            }

            deviceId = oldDesc->getDeviceId();
            mIoDescriptors.replaceValueFor(ioDesc->mIoHandle, ioDesc);

            if (deviceId != ioDesc->getDeviceId()) {
                deviceId = ioDesc->getDeviceId();
                ssize_t ioIndex = mAudioDeviceCallbacks.indexOfKey(ioDesc->mIoHandle);
                if (ioIndex >= 0) {
                    callbacks = mAudioDeviceCallbacks.valueAt(ioIndex);
                }
            }
            ALOGV("ioConfigChanged() new config for %s %d samplingRate %u, format %#x "
                    "channel mask %#x frameCount %zu deviceId %d",
                    event == AUDIO_OUTPUT_CONFIG_CHANGED ? "output" : "input",
                    ioDesc->mIoHandle, ioDesc->mSamplingRate, ioDesc->mFormat,
                    ioDesc->mChannelMask, ioDesc->mFrameCount, ioDesc->getDeviceId());

        } break;
        }
    }
    // callbacks.size() != 0 =>  ioDesc->mIoHandle and deviceId are valid
    for (size_t i = 0; i < callbacks.size(); i++) {
        callbacks[i]->onAudioDeviceUpdate(ioDesc->mIoHandle, deviceId);
    }
}

status_t AudioSystem::AudioFlingerClient::getInputBufferSize(
                                                uint32_t sampleRate, audio_format_t format,
                                                audio_channel_mask_t channelMask, size_t* buffSize)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock _l(mLock);
    // Do we have a stale mInBuffSize or are we requesting the input buffer size for new values
    if ((mInBuffSize == 0) || (sampleRate != mInSamplingRate) || (format != mInFormat)
        || (channelMask != mInChannelMask)) {
        size_t inBuffSize = af->getInputBufferSize(sampleRate, format, channelMask);
        if (inBuffSize == 0) {
            ALOGE("AudioSystem::getInputBufferSize failed sampleRate %d format %#x channelMask %x",
                    sampleRate, format, channelMask);
            return BAD_VALUE;
        }
        // A benign race is possible here: we could overwrite a fresher cache entry
        // save the request params
        mInSamplingRate = sampleRate;
        mInFormat = format;
        mInChannelMask = channelMask;

        mInBuffSize = inBuffSize;
    }

    *buffSize = mInBuffSize;

    return NO_ERROR;
}

sp<AudioIoDescriptor> AudioSystem::AudioFlingerClient::getIoDescriptor_l(audio_io_handle_t ioHandle)
{
    sp<AudioIoDescriptor> desc;
    ssize_t index = mIoDescriptors.indexOfKey(ioHandle);
    if (index >= 0) {
        desc = mIoDescriptors.valueAt(index);
    }
    return desc;
}

sp<AudioIoDescriptor> AudioSystem::AudioFlingerClient::getIoDescriptor(audio_io_handle_t ioHandle)
{
    Mutex::Autolock _l(mLock);
    return getIoDescriptor_l(ioHandle);
}

status_t AudioSystem::AudioFlingerClient::addAudioDeviceCallback(
        const sp<AudioDeviceCallback>& callback, audio_io_handle_t audioIo)
{
    Mutex::Autolock _l(mLock);
    Vector < sp<AudioDeviceCallback> > callbacks;
    ssize_t ioIndex = mAudioDeviceCallbacks.indexOfKey(audioIo);
    if (ioIndex >= 0) {
        callbacks = mAudioDeviceCallbacks.valueAt(ioIndex);
    }

    for (size_t cbIndex = 0; cbIndex < callbacks.size(); cbIndex++) {
        if (callbacks[cbIndex] == callback) {
            return INVALID_OPERATION;
        }
    }
    callbacks.add(callback);

    mAudioDeviceCallbacks.replaceValueFor(audioIo, callbacks);
    return NO_ERROR;
}

status_t AudioSystem::AudioFlingerClient::removeAudioDeviceCallback(
        const sp<AudioDeviceCallback>& callback, audio_io_handle_t audioIo)
{
    Mutex::Autolock _l(mLock);
    ssize_t ioIndex = mAudioDeviceCallbacks.indexOfKey(audioIo);
    if (ioIndex < 0) {
        return INVALID_OPERATION;
    }
    Vector < sp<AudioDeviceCallback> > callbacks = mAudioDeviceCallbacks.valueAt(ioIndex);

    size_t cbIndex;
    for (cbIndex = 0; cbIndex < callbacks.size(); cbIndex++) {
        if (callbacks[cbIndex] == callback) {
            break;
        }
    }
    if (cbIndex == callbacks.size()) {
        return INVALID_OPERATION;
    }
    callbacks.removeAt(cbIndex);
    if (callbacks.size() != 0) {
        mAudioDeviceCallbacks.replaceValueFor(audioIo, callbacks);
    } else {
        mAudioDeviceCallbacks.removeItem(audioIo);
    }
    return NO_ERROR;
}

/* static */ void AudioSystem::setErrorCallback(audio_error_callback cb)
{
    Mutex::Autolock _l(gLock);
    gAudioErrorCallback = cb;
}

/*static*/ void AudioSystem::setDynPolicyCallback(dynamic_policy_callback cb)
{
    Mutex::Autolock _l(gLock);
    gDynPolicyCallback = cb;
}

/*static*/ status_t AudioSystem::setEffectSessionCallback(effect_session_callback cb)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;

    Mutex::Autolock _l(gLock);
    gEffectSessionCallback = cb;

    status_t status = aps->setEffectSessionCallbacksEnabled(cb != NULL);
    if (status != OK) {
        gEffectSessionCallback = NULL;
    }
    return status;
}

// client singleton for AudioPolicyService binder interface
// protected by gLockAPS
sp<IAudioPolicyService> AudioSystem::gAudioPolicyService;
sp<AudioSystem::AudioPolicyServiceClient> AudioSystem::gAudioPolicyServiceClient;


// establish binder interface to AudioPolicy service
const sp<IAudioPolicyService> AudioSystem::get_audio_policy_service()
{
    sp<IAudioPolicyService> ap;
    sp<AudioPolicyServiceClient> apc;
    {
        Mutex::Autolock _l(gLockAPS);
        if (gAudioPolicyService == 0) {
            sp<IServiceManager> sm = defaultServiceManager();
            sp<IBinder> binder;
            do {
                binder = sm->getService(String16("media.audio_policy"));
                if (binder != 0)
                    break;
                ALOGW("AudioPolicyService not published, waiting...");
                usleep(500000); // 0.5 s
            } while (true);
            if (gAudioPolicyServiceClient == NULL) {
                gAudioPolicyServiceClient = new AudioPolicyServiceClient();
            }
            binder->linkToDeath(gAudioPolicyServiceClient);
            gAudioPolicyService = interface_cast<IAudioPolicyService>(binder);
            LOG_ALWAYS_FATAL_IF(gAudioPolicyService == 0);
            apc = gAudioPolicyServiceClient;
        }
        ap = gAudioPolicyService;
    }
    if (apc != 0) {
        ap->registerClient(apc);
    }

    return ap;
}

// ---------------------------------------------------------------------------

status_t AudioSystem::setDeviceConnectionState(audio_devices_t device,
                                               audio_policy_dev_state_t state,
                                               const char *device_address,
                                               const char *device_name)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    const char *address = "";
    const char *name = "";

    if (aps == 0) return PERMISSION_DENIED;

    if (device_address != NULL) {
        address = device_address;
    }
    if (device_name != NULL) {
        name = device_name;
    }
    return aps->setDeviceConnectionState(device, state, address, name);
}

audio_policy_dev_state_t AudioSystem::getDeviceConnectionState(audio_devices_t device,
                                                  const char *device_address)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;

    return aps->getDeviceConnectionState(device, device_address);
}

status_t AudioSystem::setPhoneState(audio_mode_t state)
{
    if (uint32_t(state) >= AUDIO_MODE_CNT) return BAD_VALUE;
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;

    return aps->setPhoneState(state);
}

status_t AudioSystem::setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->setForceUse(usage, config);
}

audio_policy_forced_cfg_t AudioSystem::getForceUse(audio_policy_force_use_t usage)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return AUDIO_POLICY_FORCE_NONE;
    return aps->getForceUse(usage);
}


audio_io_handle_t AudioSystem::getOutput(audio_stream_type_t stream,
                                    uint32_t samplingRate,
                                    audio_format_t format,
                                    audio_channel_mask_t channelMask,
                                    audio_output_flags_t flags,
                                    const audio_offload_info_t *offloadInfo)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return 0;
    return aps->getOutput(stream, samplingRate, format, channelMask, flags, offloadInfo);
}

status_t AudioSystem::getOutputForAttr(const audio_attributes_t *attr,
                                        audio_io_handle_t *output,
                                        audio_session_t session,
                                        audio_stream_type_t *stream,
                                        uid_t uid,
                                        uint32_t samplingRate,
                                        audio_format_t format,
                                        audio_channel_mask_t channelMask,
                                        audio_output_flags_t flags,
                                        audio_port_handle_t selectedDeviceId,
                                        const audio_offload_info_t *offloadInfo)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return NO_INIT;
    return aps->getOutputForAttr(attr, output, session, stream, uid,
                                 samplingRate, format, channelMask,
                                 flags, selectedDeviceId, offloadInfo);
}

status_t AudioSystem::startOutput(audio_io_handle_t output,
                                  audio_stream_type_t stream,
                                  audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->startOutput(output, stream, session);
}

status_t AudioSystem::stopOutput(audio_io_handle_t output,
                                 audio_stream_type_t stream,
                                 audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->stopOutput(output, stream, session);
}

void AudioSystem::releaseOutput(audio_io_handle_t output,
                                audio_stream_type_t stream,
                                audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return;
    aps->releaseOutput(output, stream, session);
}

status_t AudioSystem::getInputForAttr(const audio_attributes_t *attr,
                                audio_io_handle_t *input,
                                audio_session_t session,
                                uid_t uid,
                                uint32_t samplingRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                audio_input_flags_t flags,
                                audio_port_handle_t selectedDeviceId)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return NO_INIT;
    return aps->getInputForAttr(
            attr, input, session, uid, samplingRate, format, channelMask, flags, selectedDeviceId);
}

status_t AudioSystem::startInput(audio_io_handle_t input,
                                 audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->startInput(input, session);
}

status_t AudioSystem::stopInput(audio_io_handle_t input,
                                audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->stopInput(input, session);
}

void AudioSystem::releaseInput(audio_io_handle_t input,
                               audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return;
    aps->releaseInput(input, session);
}

status_t AudioSystem::initStreamVolume(audio_stream_type_t stream,
                                    int indexMin,
                                    int indexMax)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->initStreamVolume(stream, indexMin, indexMax);
}

status_t AudioSystem::setStreamVolumeIndex(audio_stream_type_t stream,
                                           int index,
                                           audio_devices_t device)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->setStreamVolumeIndex(stream, index, device);
}

status_t AudioSystem::getStreamVolumeIndex(audio_stream_type_t stream,
                                           int *index,
                                           audio_devices_t device)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->getStreamVolumeIndex(stream, index, device);
}

uint32_t AudioSystem::getStrategyForStream(audio_stream_type_t stream)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return 0;
    return aps->getStrategyForStream(stream);
}

audio_devices_t AudioSystem::getDevicesForStream(audio_stream_type_t stream)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return AUDIO_DEVICE_NONE;
    return aps->getDevicesForStream(stream);
}

audio_io_handle_t AudioSystem::getOutputForEffect(const effect_descriptor_t *desc)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    // FIXME change return type to status_t, and return PERMISSION_DENIED here
    if (aps == 0) return AUDIO_IO_HANDLE_NONE;
    return aps->getOutputForEffect(desc);
}

status_t AudioSystem::registerEffect(const effect_descriptor_t *desc,
                                audio_io_handle_t io,
                                uint32_t strategy,
                                int session,
                                int id)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->registerEffect(desc, io, strategy, session, id);
}

status_t AudioSystem::unregisterEffect(int id)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->unregisterEffect(id);
}

status_t AudioSystem::setEffectEnabled(int id, bool enabled)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->setEffectEnabled(id, enabled);
}

status_t AudioSystem::isStreamActive(audio_stream_type_t stream, bool* state, uint32_t inPastMs)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    if (state == NULL) return BAD_VALUE;
    *state = aps->isStreamActive(stream, inPastMs);
    return NO_ERROR;
}

status_t AudioSystem::isStreamActiveRemotely(audio_stream_type_t stream, bool* state,
        uint32_t inPastMs)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    if (state == NULL) return BAD_VALUE;
    *state = aps->isStreamActiveRemotely(stream, inPastMs);
    return NO_ERROR;
}

status_t AudioSystem::isSourceActive(audio_source_t stream, bool* state)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    if (state == NULL) return BAD_VALUE;
    *state = aps->isSourceActive(stream);
    return NO_ERROR;
}

uint32_t AudioSystem::getPrimaryOutputSamplingRate()
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return 0;
    return af->getPrimaryOutputSamplingRate();
}

size_t AudioSystem::getPrimaryOutputFrameCount()
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return 0;
    return af->getPrimaryOutputFrameCount();
}

status_t AudioSystem::setLowRamDevice(bool isLowRamDevice)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    return af->setLowRamDevice(isLowRamDevice);
}

void AudioSystem::clearAudioConfigCache()
{
    // called by restoreTrack_l(), which needs new IAudioFlinger and IAudioPolicyService instances
    ALOGV("clearAudioConfigCache()");
    {
        Mutex::Autolock _l(gLock);
        if (gAudioFlingerClient != 0) {
            gAudioFlingerClient->clearIoCache();
        }
        gAudioFlinger.clear();
    }
    {
        Mutex::Autolock _l(gLockAPS);
        gAudioPolicyService.clear();
    }
}

bool AudioSystem::isOffloadSupported(const audio_offload_info_t& info)
{
    ALOGV("isOffloadSupported()");
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return false;
    return aps->isOffloadSupported(info);
}

status_t AudioSystem::listAudioPorts(audio_port_role_t role,
                                     audio_port_type_t type,
                                     unsigned int *num_ports,
                                     struct audio_port *ports,
                                     unsigned int *generation)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->listAudioPorts(role, type, num_ports, ports, generation);
}

status_t AudioSystem::getAudioPort(struct audio_port *port)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->getAudioPort(port);
}

status_t AudioSystem::createAudioPatch(const struct audio_patch *patch,
                                   audio_patch_handle_t *handle)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->createAudioPatch(patch, handle);
}

status_t AudioSystem::releaseAudioPatch(audio_patch_handle_t handle)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->releaseAudioPatch(handle);
}

status_t AudioSystem::listAudioPatches(unsigned int *num_patches,
                                  struct audio_patch *patches,
                                  unsigned int *generation)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->listAudioPatches(num_patches, patches, generation);
}

status_t AudioSystem::setAudioPortConfig(const struct audio_port_config *config)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->setAudioPortConfig(config);
}

status_t AudioSystem::addAudioPortCallback(const sp<AudioPortCallback>& callback)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;

    Mutex::Autolock _l(gLockAPS);
    if (gAudioPolicyServiceClient == 0) {
        return NO_INIT;
    }
    int ret = gAudioPolicyServiceClient->addAudioPortCallback(callback);
    if (ret == 1) {
        aps->setAudioPortCallbacksEnabled(true);
    }
    return (ret < 0) ? INVALID_OPERATION : NO_ERROR;
}

/*static*/
status_t AudioSystem::removeAudioPortCallback(const sp<AudioPortCallback>& callback)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;

    Mutex::Autolock _l(gLockAPS);
    if (gAudioPolicyServiceClient == 0) {
        return NO_INIT;
    }
    int ret = gAudioPolicyServiceClient->removeAudioPortCallback(callback);
    if (ret == 0) {
        aps->setAudioPortCallbacksEnabled(false);
    }
    return (ret < 0) ? INVALID_OPERATION : NO_ERROR;
}

status_t AudioSystem::addAudioDeviceCallback(
        const sp<AudioDeviceCallback>& callback, audio_io_handle_t audioIo)
{
    const sp<AudioFlingerClient> afc = getAudioFlingerClient();
    if (afc == 0) {
        return NO_INIT;
    }
    status_t status = afc->addAudioDeviceCallback(callback, audioIo);
    if (status == NO_ERROR) {
        const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
        if (af != 0) {
            af->registerClient(afc);
        }
    }
    return status;
}

status_t AudioSystem::removeAudioDeviceCallback(
        const sp<AudioDeviceCallback>& callback, audio_io_handle_t audioIo)
{
    const sp<AudioFlingerClient> afc = getAudioFlingerClient();
    if (afc == 0) {
        return NO_INIT;
    }
    return afc->removeAudioDeviceCallback(callback, audioIo);
}

audio_port_handle_t AudioSystem::getDeviceIdForIo(audio_io_handle_t audioIo)
{
    const sp<IAudioFlinger>& af = AudioSystem::get_audio_flinger();
    if (af == 0) return PERMISSION_DENIED;
    const sp<AudioIoDescriptor> desc = getIoDescriptor(audioIo);
    if (desc == 0) {
        return AUDIO_PORT_HANDLE_NONE;
    }
    return desc->getDeviceId();
}

status_t AudioSystem::acquireSoundTriggerSession(audio_session_t *session,
                                       audio_io_handle_t *ioHandle,
                                       audio_devices_t *device)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->acquireSoundTriggerSession(session, ioHandle, device);
}

status_t AudioSystem::releaseSoundTriggerSession(audio_session_t session)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->releaseSoundTriggerSession(session);
}

audio_mode_t AudioSystem::getPhoneState()
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return AUDIO_MODE_INVALID;
    return aps->getPhoneState();
}

status_t AudioSystem::registerPolicyMixes(Vector<AudioMix> mixes, bool registration)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->registerPolicyMixes(mixes, registration);
}

status_t AudioSystem::startAudioSource(const struct audio_port_config *source,
                                       const audio_attributes_t *attributes,
                                       audio_io_handle_t *handle)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->startAudioSource(source, attributes, handle);
}

status_t AudioSystem::stopAudioSource(audio_io_handle_t handle)
{
    const sp<IAudioPolicyService>& aps = AudioSystem::get_audio_policy_service();
    if (aps == 0) return PERMISSION_DENIED;
    return aps->stopAudioSource(handle);
}

// ---------------------------------------------------------------------------

int AudioSystem::AudioPolicyServiceClient::addAudioPortCallback(
        const sp<AudioPortCallback>& callback)
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mAudioPortCallbacks.size(); i++) {
        if (mAudioPortCallbacks[i] == callback) {
            return -1;
        }
    }
    mAudioPortCallbacks.add(callback);
    return mAudioPortCallbacks.size();
}

int AudioSystem::AudioPolicyServiceClient::removeAudioPortCallback(
        const sp<AudioPortCallback>& callback)
{
    Mutex::Autolock _l(mLock);
    size_t i;
    for (i = 0; i < mAudioPortCallbacks.size(); i++) {
        if (mAudioPortCallbacks[i] == callback) {
            break;
        }
    }
    if (i == mAudioPortCallbacks.size()) {
        return -1;
    }
    mAudioPortCallbacks.removeAt(i);
    return mAudioPortCallbacks.size();
}


void AudioSystem::AudioPolicyServiceClient::onAudioPortListUpdate()
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mAudioPortCallbacks.size(); i++) {
        mAudioPortCallbacks[i]->onAudioPortListUpdate();
    }
}

void AudioSystem::AudioPolicyServiceClient::onAudioPatchListUpdate()
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mAudioPortCallbacks.size(); i++) {
        mAudioPortCallbacks[i]->onAudioPatchListUpdate();
    }
}

void AudioSystem::AudioPolicyServiceClient::onDynamicPolicyMixStateUpdate(
        String8 regId, int32_t state)
{
    ALOGV("AudioPolicyServiceClient::onDynamicPolicyMixStateUpdate(%s, %d)", regId.string(), state);
    dynamic_policy_callback cb = NULL;
    {
        Mutex::Autolock _l(AudioSystem::gLock);
        cb = gDynPolicyCallback;
    }

    if (cb != NULL) {
        cb(DYNAMIC_POLICY_EVENT_MIX_STATE_UPDATE, regId, state);
    }
}

void AudioSystem::AudioPolicyServiceClient::onOutputSessionEffectsUpdate(
        audio_stream_type_t stream, audio_session_t sessionId,
        audio_output_flags_t flags, audio_channel_mask_t channelMask,
        uid_t uid, bool added)
{
    ALOGV("AudioPolicyServiceClient::onOutputSessionEffectsUpdate(%d, %d, %d)", stream, sessionId, added);
    effect_session_callback cb = NULL;
    {
        Mutex::Autolock _l(AudioSystem::gLock);
        cb = gEffectSessionCallback;
    }

    if (cb != NULL) {
        cb(AUDIO_OUTPUT_SESSION_EFFECTS_UPDATE, stream, sessionId, flags, channelMask, uid, added);
    }
}

void AudioSystem::AudioPolicyServiceClient::binderDied(const wp<IBinder>& who __unused)
{
    {
        Mutex::Autolock _l(mLock);
        for (size_t i = 0; i < mAudioPortCallbacks.size(); i++) {
            mAudioPortCallbacks[i]->onServiceDied();
        }
    }
    {
        Mutex::Autolock _l(gLockAPS);
        AudioSystem::gAudioPolicyService.clear();
    }

    ALOGW("AudioPolicyService server died!");
}

} // namespace android
