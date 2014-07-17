/*
**
** Copyright 2007, The Android Open Source Project
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

#define LOG_TAG "AudioMixer"
//#define LOG_NDEBUG 0

#include "Configuration.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>

#include <utils/Errors.h>
#include <utils/Log.h>

#include <cutils/bitops.h>
#include <cutils/compiler.h>
#include <utils/Debug.h>

#include <system/audio.h>

#include <audio_utils/primitives.h>
#include <audio_utils/format.h>
#include <common_time/local_clock.h>
#include <common_time/cc_helper.h>

#include <media/EffectsFactoryApi.h>

#include "AudioMixerOps.h"
#include "AudioMixer.h"

// Use the FCC_2 macro for code assuming Fixed Channel Count of 2 and
// whose stereo assumption may need to be revisited later.
#ifndef FCC_2
#define FCC_2 2
#endif

/* VERY_VERY_VERBOSE_LOGGING will show exactly which process hook and track hook is
 * being used. This is a considerable amount of log spam, so don't enable unless you
 * are verifying the hook based code.
 */
//#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
//define ALOGVV printf  // for test-mixer.cpp
#else
#define ALOGVV(a...) do { } while (0)
#endif

// Set kUseNewMixer to true to use the new mixer engine. Otherwise the
// original code will be used.  This is false for now.
static const bool kUseNewMixer = false;

// Set kUseFloat to true to allow floating input into the mixer engine.
// If kUseNewMixer is false, this is ignored or may be overridden internally
// because of downmix/upmix support.
static const bool kUseFloat = true;

// Set to default copy buffer size in frames for input processing.
static const size_t kCopyBufferFrameCount = 256;

namespace android {

// ----------------------------------------------------------------------------

template <typename T>
T min(const T& a, const T& b)
{
    return a < b ? a : b;
}

AudioMixer::CopyBufferProvider::CopyBufferProvider(size_t inputFrameSize,
        size_t outputFrameSize, size_t bufferFrameCount) :
        mInputFrameSize(inputFrameSize),
        mOutputFrameSize(outputFrameSize),
        mLocalBufferFrameCount(bufferFrameCount),
        mLocalBufferData(NULL),
        mConsumed(0)
{
    ALOGV("CopyBufferProvider(%p)(%zu, %zu, %zu)", this,
            inputFrameSize, outputFrameSize, bufferFrameCount);
    LOG_ALWAYS_FATAL_IF(inputFrameSize < outputFrameSize && bufferFrameCount == 0,
            "Requires local buffer if inputFrameSize(%d) < outputFrameSize(%d)",
            inputFrameSize, outputFrameSize);
    if (mLocalBufferFrameCount) {
        (void)posix_memalign(&mLocalBufferData, 32, mLocalBufferFrameCount * mOutputFrameSize);
    }
    mBuffer.frameCount = 0;
}

AudioMixer::CopyBufferProvider::~CopyBufferProvider()
{
    ALOGV("~CopyBufferProvider(%p)", this);
    if (mBuffer.frameCount != 0) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    }
    free(mLocalBufferData);
}

status_t AudioMixer::CopyBufferProvider::getNextBuffer(AudioBufferProvider::Buffer *pBuffer,
        int64_t pts)
{
    //ALOGV("CopyBufferProvider(%p)::getNextBuffer(%p (%zu), %lld)",
    //        this, pBuffer, pBuffer->frameCount, pts);
    if (mLocalBufferFrameCount == 0) {
        status_t res = mTrackBufferProvider->getNextBuffer(pBuffer, pts);
        if (res == OK) {
            copyFrames(pBuffer->raw, pBuffer->raw, pBuffer->frameCount);
        }
        return res;
    }
    if (mBuffer.frameCount == 0) {
        mBuffer.frameCount = pBuffer->frameCount;
        status_t res = mTrackBufferProvider->getNextBuffer(&mBuffer, pts);
        // At one time an upstream buffer provider had
        // res == OK and mBuffer.frameCount == 0, doesn't seem to happen now 7/18/2014.
        //
        // By API spec, if res != OK, then mBuffer.frameCount == 0.
        // but there may be improper implementations.
        ALOG_ASSERT(res == OK || mBuffer.frameCount == 0);
        if (res != OK || mBuffer.frameCount == 0) { // not needed by API spec, but to be safe.
            pBuffer->raw = NULL;
            pBuffer->frameCount = 0;
            return res;
        }
        mConsumed = 0;
    }
    ALOG_ASSERT(mConsumed < mBuffer.frameCount);
    size_t count = min(mLocalBufferFrameCount, mBuffer.frameCount - mConsumed);
    count = min(count, pBuffer->frameCount);
    pBuffer->raw = mLocalBufferData;
    pBuffer->frameCount = count;
    copyFrames(pBuffer->raw, (uint8_t*)mBuffer.raw + mConsumed * mInputFrameSize,
            pBuffer->frameCount);
    return OK;
}

void AudioMixer::CopyBufferProvider::releaseBuffer(AudioBufferProvider::Buffer *pBuffer)
{
    //ALOGV("CopyBufferProvider(%p)::releaseBuffer(%p(%zu))",
    //        this, pBuffer, pBuffer->frameCount);
    if (mLocalBufferFrameCount == 0) {
        mTrackBufferProvider->releaseBuffer(pBuffer);
        return;
    }
    // LOG_ALWAYS_FATAL_IF(pBuffer->frameCount == 0, "Invalid framecount");
    mConsumed += pBuffer->frameCount; // TODO: update for efficiency to reuse existing content
    if (mConsumed != 0 && mConsumed >= mBuffer.frameCount) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
        ALOG_ASSERT(mBuffer.frameCount == 0);
    }
    pBuffer->raw = NULL;
    pBuffer->frameCount = 0;
}

void AudioMixer::CopyBufferProvider::reset()
{
    if (mBuffer.frameCount != 0) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    }
    mConsumed = 0;
}

AudioMixer::DownmixerBufferProvider::DownmixerBufferProvider(
        audio_channel_mask_t inputChannelMask,
        audio_channel_mask_t outputChannelMask, audio_format_t format,
        uint32_t sampleRate, int32_t sessionId, size_t bufferFrameCount) :
        CopyBufferProvider(
            audio_bytes_per_sample(format) * audio_channel_count_from_out_mask(inputChannelMask),
            audio_bytes_per_sample(format) * audio_channel_count_from_out_mask(outputChannelMask),
            bufferFrameCount)  // set bufferFrameCount to 0 to do in-place
{
    ALOGV("DownmixerBufferProvider(%p)(%#x, %#x, %#x %u %d)",
            this, inputChannelMask, outputChannelMask, format,
            sampleRate, sessionId);
    if (!sIsMultichannelCapable
            || EffectCreate(&sDwnmFxDesc.uuid,
                    sessionId,
                    SESSION_ID_INVALID_AND_IGNORED,
                    &mDownmixHandle) != 0) {
         ALOGE("DownmixerBufferProvider() error creating downmixer effect");
         mDownmixHandle = NULL;
         return;
     }
     // channel input configuration will be overridden per-track
     mDownmixConfig.inputCfg.channels = inputChannelMask;   // FIXME: Should be bits
     mDownmixConfig.outputCfg.channels = outputChannelMask; // FIXME: should be bits
     mDownmixConfig.inputCfg.format = format;
     mDownmixConfig.outputCfg.format = format;
     mDownmixConfig.inputCfg.samplingRate = sampleRate;
     mDownmixConfig.outputCfg.samplingRate = sampleRate;
     mDownmixConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
     mDownmixConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
     // input and output buffer provider, and frame count will not be used as the downmix effect
     // process() function is called directly (see DownmixerBufferProvider::getNextBuffer())
     mDownmixConfig.inputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS |
             EFFECT_CONFIG_FORMAT | EFFECT_CONFIG_ACC_MODE;
     mDownmixConfig.outputCfg.mask = mDownmixConfig.inputCfg.mask;

     int cmdStatus;
     uint32_t replySize = sizeof(int);

     // Configure downmixer
     status_t status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_SET_CONFIG /*cmdCode*/, sizeof(effect_config_t) /*cmdSize*/,
             &mDownmixConfig /*pCmdData*/,
             &replySize, &cmdStatus /*pReplyData*/);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while configuring downmixer",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }

     // Enable downmixer
     replySize = sizeof(int);
     status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_ENABLE /*cmdCode*/, 0 /*cmdSize*/, NULL /*pCmdData*/,
             &replySize, &cmdStatus /*pReplyData*/);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while enabling downmixer",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }

     // Set downmix type
     // parameter size rounded for padding on 32bit boundary
     const int psizePadded = ((sizeof(downmix_params_t) - 1)/sizeof(int) + 1) * sizeof(int);
     const int downmixParamSize =
             sizeof(effect_param_t) + psizePadded + sizeof(downmix_type_t);
     effect_param_t * const param = (effect_param_t *) malloc(downmixParamSize);
     param->psize = sizeof(downmix_params_t);
     const downmix_params_t downmixParam = DOWNMIX_PARAM_TYPE;
     memcpy(param->data, &downmixParam, param->psize);
     const downmix_type_t downmixType = DOWNMIX_TYPE_FOLD;
     param->vsize = sizeof(downmix_type_t);
     memcpy(param->data + psizePadded, &downmixType, param->vsize);
     replySize = sizeof(int);
     status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_SET_PARAM /* cmdCode */, downmixParamSize /* cmdSize */,
             param /*pCmdData*/, &replySize, &cmdStatus /*pReplyData*/);
     free(param);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while setting downmix type",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }
     ALOGV("DownmixerBufferProvider() downmix type set to %d", (int) downmixType);
}

AudioMixer::DownmixerBufferProvider::~DownmixerBufferProvider()
{
    ALOGV("~DownmixerBufferProvider (%p)", this);
    EffectRelease(mDownmixHandle);
    mDownmixHandle = NULL;
}

void AudioMixer::DownmixerBufferProvider::copyFrames(void *dst, const void *src, size_t frames)
{
    mDownmixConfig.inputCfg.buffer.frameCount = frames;
    mDownmixConfig.inputCfg.buffer.raw = const_cast<void *>(src);
    mDownmixConfig.outputCfg.buffer.frameCount = frames;
    mDownmixConfig.outputCfg.buffer.raw = dst;
    // may be in-place if src == dst.
    status_t res = (*mDownmixHandle)->process(mDownmixHandle,
            &mDownmixConfig.inputCfg.buffer, &mDownmixConfig.outputCfg.buffer);
    ALOGE_IF(res != OK, "DownmixBufferProvider error %d", res);
}

/* call once in a pthread_once handler. */
/*static*/ status_t AudioMixer::DownmixerBufferProvider::init()
{
    // find multichannel downmix effect if we have to play multichannel content
    uint32_t numEffects = 0;
    int ret = EffectQueryNumberEffects(&numEffects);
    if (ret != 0) {
        ALOGE("AudioMixer() error %d querying number of effects", ret);
        return NO_INIT;
    }
    ALOGV("EffectQueryNumberEffects() numEffects=%d", numEffects);

    for (uint32_t i = 0 ; i < numEffects ; i++) {
        if (EffectQueryEffect(i, &sDwnmFxDesc) == 0) {
            ALOGV("effect %d is called %s", i, sDwnmFxDesc.name);
            if (memcmp(&sDwnmFxDesc.type, EFFECT_UIID_DOWNMIX, sizeof(effect_uuid_t)) == 0) {
                ALOGI("found effect \"%s\" from %s",
                        sDwnmFxDesc.name, sDwnmFxDesc.implementor);
                sIsMultichannelCapable = true;
                break;
            }
        }
    }
    ALOGW_IF(!sIsMultichannelCapable, "unable to find downmix effect");
    return NO_INIT;
}

/*static*/ bool AudioMixer::DownmixerBufferProvider::sIsMultichannelCapable = false;
/*static*/ effect_descriptor_t AudioMixer::DownmixerBufferProvider::sDwnmFxDesc;

AudioMixer::ReformatBufferProvider::ReformatBufferProvider(int32_t channels,
        audio_format_t inputFormat, audio_format_t outputFormat,
        size_t bufferFrameCount) :
        CopyBufferProvider(
            channels * audio_bytes_per_sample(inputFormat),
            channels * audio_bytes_per_sample(outputFormat),
            bufferFrameCount),
        mChannels(channels),
        mInputFormat(inputFormat),
        mOutputFormat(outputFormat)
{
    ALOGV("ReformatBufferProvider(%p)(%d, %#x, %#x)", this, channels, inputFormat, outputFormat);
}

void AudioMixer::ReformatBufferProvider::copyFrames(void *dst, const void *src, size_t frames)
{
    memcpy_by_audio_format(dst, mOutputFormat, src, mInputFormat, frames * mChannels);
}

// ----------------------------------------------------------------------------

// Ensure mConfiguredNames bitmask is initialized properly on all architectures.
// The value of 1 << x is undefined in C when x >= 32.

AudioMixer::AudioMixer(size_t frameCount, uint32_t sampleRate, uint32_t maxNumTracks)
    :   mTrackNames(0), mConfiguredNames((maxNumTracks >= 32 ? 0 : 1 << maxNumTracks) - 1),
        mSampleRate(sampleRate)
{
    // AudioMixer is not yet capable of multi-channel beyond stereo
    COMPILE_TIME_ASSERT_FUNCTION_SCOPE(2 == MAX_NUM_CHANNELS);

    ALOG_ASSERT(maxNumTracks <= MAX_NUM_TRACKS, "maxNumTracks %u > MAX_NUM_TRACKS %u",
            maxNumTracks, MAX_NUM_TRACKS);

    // AudioMixer is not yet capable of more than 32 active track inputs
    ALOG_ASSERT(32 >= MAX_NUM_TRACKS, "bad MAX_NUM_TRACKS %d", MAX_NUM_TRACKS);

    // AudioMixer is not yet capable of multi-channel output beyond stereo
    ALOG_ASSERT(2 == MAX_NUM_CHANNELS, "bad MAX_NUM_CHANNELS %d", MAX_NUM_CHANNELS);

    pthread_once(&sOnceControl, &sInitRoutine);

    mState.enabledTracks= 0;
    mState.needsChanged = 0;
    mState.frameCount   = frameCount;
    mState.hook         = process__nop;
    mState.outputTemp   = NULL;
    mState.resampleTemp = NULL;
    mState.mLog         = &mDummyLog;
    // mState.reserved

    // FIXME Most of the following initialization is probably redundant since
    // tracks[i] should only be referenced if (mTrackNames & (1 << i)) != 0
    // and mTrackNames is initially 0.  However, leave it here until that's verified.
    track_t* t = mState.tracks;
    for (unsigned i=0 ; i < MAX_NUM_TRACKS ; i++) {
        t->resampler = NULL;
        t->downmixerBufferProvider = NULL;
        t->mReformatBufferProvider = NULL;
        t++;
    }

}

AudioMixer::~AudioMixer()
{
    track_t* t = mState.tracks;
    for (unsigned i=0 ; i < MAX_NUM_TRACKS ; i++) {
        delete t->resampler;
        delete t->downmixerBufferProvider;
        delete t->mReformatBufferProvider;
        t++;
    }
    delete [] mState.outputTemp;
    delete [] mState.resampleTemp;
}

void AudioMixer::setLog(NBLog::Writer *log)
{
    mState.mLog = log;
}

int AudioMixer::getTrackName(audio_channel_mask_t channelMask,
        audio_format_t format, int sessionId)
{
    if (!isValidPcmTrackFormat(format)) {
        ALOGE("AudioMixer::getTrackName invalid format (%#x)", format);
        return -1;
    }
    uint32_t names = (~mTrackNames) & mConfiguredNames;
    if (names != 0) {
        int n = __builtin_ctz(names);
        ALOGV("add track (%d)", n);
        // assume default parameters for the track, except where noted below
        track_t* t = &mState.tracks[n];
        t->needs = 0;

        // Integer volume.
        // Currently integer volume is kept for the legacy integer mixer.
        // Will be removed when the legacy mixer path is removed.
        t->volume[0] = UNITY_GAIN_INT;
        t->volume[1] = UNITY_GAIN_INT;
        t->prevVolume[0] = UNITY_GAIN_INT << 16;
        t->prevVolume[1] = UNITY_GAIN_INT << 16;
        t->volumeInc[0] = 0;
        t->volumeInc[1] = 0;
        t->auxLevel = 0;
        t->auxInc = 0;
        t->prevAuxLevel = 0;

        // Floating point volume.
        t->mVolume[0] = UNITY_GAIN_FLOAT;
        t->mVolume[1] = UNITY_GAIN_FLOAT;
        t->mPrevVolume[0] = UNITY_GAIN_FLOAT;
        t->mPrevVolume[1] = UNITY_GAIN_FLOAT;
        t->mVolumeInc[0] = 0.;
        t->mVolumeInc[1] = 0.;
        t->mAuxLevel = 0.;
        t->mAuxInc = 0.;
        t->mPrevAuxLevel = 0.;

        // no initialization needed
        // t->frameCount
        t->channelCount = audio_channel_count_from_out_mask(channelMask);
        t->enabled = false;
        ALOGV_IF(channelMask != AUDIO_CHANNEL_OUT_STEREO,
                "Non-stereo channel mask: %d\n", channelMask);
        t->channelMask = channelMask;
        t->sessionId = sessionId;
        // setBufferProvider(name, AudioBufferProvider *) is required before enable(name)
        t->bufferProvider = NULL;
        t->buffer.raw = NULL;
        // no initialization needed
        // t->buffer.frameCount
        t->hook = NULL;
        t->in = NULL;
        t->resampler = NULL;
        t->sampleRate = mSampleRate;
        // setParameter(name, TRACK, MAIN_BUFFER, mixBuffer) is required before enable(name)
        t->mainBuffer = NULL;
        t->auxBuffer = NULL;
        t->mInputBufferProvider = NULL;
        t->mReformatBufferProvider = NULL;
        t->downmixerBufferProvider = NULL;
        t->mMixerFormat = AUDIO_FORMAT_PCM_16_BIT;
        t->mFormat = format;
        t->mMixerInFormat = kUseFloat && kUseNewMixer
                ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
        // Check the downmixing (or upmixing) requirements.
        status_t status = initTrackDownmix(t, n, channelMask);
        if (status != OK) {
            ALOGE("AudioMixer::getTrackName invalid channelMask (%#x)", channelMask);
            return -1;
        }
        // initTrackDownmix() may change the input format requirement.
        // If you desire floating point input to the mixer, it may change
        // to integer because the downmixer requires integer to process.
        ALOGVV("mMixerFormat:%#x  mMixerInFormat:%#x\n", t->mMixerFormat, t->mMixerInFormat);
        prepareTrackForReformat(t, n);
        mTrackNames |= 1 << n;
        return TRACK0 + n;
    }
    ALOGE("AudioMixer::getTrackName out of available tracks");
    return -1;
}

void AudioMixer::invalidateState(uint32_t mask)
{
    if (mask != 0) {
        mState.needsChanged |= mask;
        mState.hook = process__validate;
    }
 }

status_t AudioMixer::initTrackDownmix(track_t* pTrack, int trackNum, audio_channel_mask_t mask)
{
    uint32_t channelCount = audio_channel_count_from_out_mask(mask);
    ALOG_ASSERT((channelCount <= MAX_NUM_CHANNELS_TO_DOWNMIX) && channelCount);
    status_t status = OK;
    if (channelCount > MAX_NUM_CHANNELS) {
        pTrack->channelMask = mask;
        pTrack->channelCount = channelCount;
        ALOGV("initTrackDownmix(track=%d, mask=0x%x) calls prepareTrackForDownmix()",
                trackNum, mask);
        status = prepareTrackForDownmix(pTrack, trackNum);
    } else {
        unprepareTrackForDownmix(pTrack, trackNum);
    }
    return status;
}

void AudioMixer::unprepareTrackForDownmix(track_t* pTrack, int trackName __unused) {
    ALOGV("AudioMixer::unprepareTrackForDownmix(%d)", trackName);

    if (pTrack->downmixerBufferProvider != NULL) {
        // this track had previously been configured with a downmixer, delete it
        ALOGV(" deleting old downmixer");
        delete pTrack->downmixerBufferProvider;
        pTrack->downmixerBufferProvider = NULL;
        reconfigureBufferProviders(pTrack);
    } else {
        ALOGV(" nothing to do, no downmixer to delete");
    }
}

status_t AudioMixer::prepareTrackForDownmix(track_t* pTrack, int trackName)
{
    ALOGV("AudioMixer::prepareTrackForDownmix(%d) with mask 0x%x", trackName, pTrack->channelMask);

    // discard the previous downmixer if there was one
    unprepareTrackForDownmix(pTrack, trackName);
    if (DownmixerBufferProvider::isMultichannelCapable()) {
        DownmixerBufferProvider* pDbp = new DownmixerBufferProvider(pTrack->channelMask,
                /* pTrack->mMixerChannelMask */ audio_channel_out_mask_from_count(2),
                /* pTrack->mMixerInFormat */ AUDIO_FORMAT_PCM_16_BIT,
                pTrack->sampleRate, pTrack->sessionId, kCopyBufferFrameCount);

        if (pDbp->isValid()) { // if constructor completed properly
            pTrack->mMixerInFormat = AUDIO_FORMAT_PCM_16_BIT; // PCM 16 bit required for downmix
            pTrack->downmixerBufferProvider = pDbp;
            reconfigureBufferProviders(pTrack);
            return NO_ERROR;
        }
        delete pDbp;
    }
    pTrack->downmixerBufferProvider = NULL;
    reconfigureBufferProviders(pTrack);
    return NO_INIT;
}

void AudioMixer::unprepareTrackForReformat(track_t* pTrack, int trackName __unused) {
    ALOGV("AudioMixer::unprepareTrackForReformat(%d)", trackName);
    if (pTrack->mReformatBufferProvider != NULL) {
        delete pTrack->mReformatBufferProvider;
        pTrack->mReformatBufferProvider = NULL;
        reconfigureBufferProviders(pTrack);
    }
}

status_t AudioMixer::prepareTrackForReformat(track_t* pTrack, int trackName)
{
    ALOGV("AudioMixer::prepareTrackForReformat(%d) with format %#x", trackName, pTrack->mFormat);
    // discard the previous reformatter if there was one
    unprepareTrackForReformat(pTrack, trackName);
    // only configure reformatter if needed
    if (pTrack->mFormat != pTrack->mMixerInFormat) {
        pTrack->mReformatBufferProvider = new ReformatBufferProvider(
                audio_channel_count_from_out_mask(pTrack->channelMask),
                pTrack->mFormat, pTrack->mMixerInFormat,
                kCopyBufferFrameCount);
        reconfigureBufferProviders(pTrack);
    }
    return NO_ERROR;
}

void AudioMixer::reconfigureBufferProviders(track_t* pTrack)
{
    pTrack->bufferProvider = pTrack->mInputBufferProvider;
    if (pTrack->mReformatBufferProvider) {
        pTrack->mReformatBufferProvider->setBufferProvider(pTrack->bufferProvider);
        pTrack->bufferProvider = pTrack->mReformatBufferProvider;
    }
    if (pTrack->downmixerBufferProvider) {
        pTrack->downmixerBufferProvider->setBufferProvider(pTrack->bufferProvider);
        pTrack->bufferProvider = pTrack->downmixerBufferProvider;
    }
}

void AudioMixer::deleteTrackName(int name)
{
    ALOGV("AudioMixer::deleteTrackName(%d)", name);
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    ALOGV("deleteTrackName(%d)", name);
    track_t& track(mState.tracks[ name ]);
    if (track.enabled) {
        track.enabled = false;
        invalidateState(1<<name);
    }
    // delete the resampler
    delete track.resampler;
    track.resampler = NULL;
    // delete the downmixer
    unprepareTrackForDownmix(&mState.tracks[name], name);
    // delete the reformatter
    unprepareTrackForReformat(&mState.tracks[name], name);

    mTrackNames &= ~(1<<name);
}

void AudioMixer::enable(int name)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    if (!track.enabled) {
        track.enabled = true;
        ALOGV("enable(%d)", name);
        invalidateState(1 << name);
    }
}

void AudioMixer::disable(int name)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    if (track.enabled) {
        track.enabled = false;
        ALOGV("disable(%d)", name);
        invalidateState(1 << name);
    }
}

/* Sets the volume ramp variables for the AudioMixer.
 *
 * The volume ramp variables are used to transition from the previous
 * volume to the set volume.  ramp controls the duration of the transition.
 * Its value is typically one state framecount period, but may also be 0,
 * meaning "immediate."
 *
 * FIXME: 1) Volume ramp is enabled only if there is a nonzero integer increment
 * even if there is a nonzero floating point increment (in that case, the volume
 * change is immediate).  This restriction should be changed when the legacy mixer
 * is removed (see #2).
 * FIXME: 2) Integer volume variables are used for Legacy mixing and should be removed
 * when no longer needed.
 *
 * @param newVolume set volume target in floating point [0.0, 1.0].
 * @param ramp number of frames to increment over. if ramp is 0, the volume
 * should be set immediately.  Currently ramp should not exceed 65535 (frames).
 * @param pIntSetVolume pointer to the U4.12 integer target volume, set on return.
 * @param pIntPrevVolume pointer to the U4.28 integer previous volume, set on return.
 * @param pIntVolumeInc pointer to the U4.28 increment per output audio frame, set on return.
 * @param pSetVolume pointer to the float target volume, set on return.
 * @param pPrevVolume pointer to the float previous volume, set on return.
 * @param pVolumeInc pointer to the float increment per output audio frame, set on return.
 * @return true if the volume has changed, false if volume is same.
 */
static inline bool setVolumeRampVariables(float newVolume, int32_t ramp,
        int16_t *pIntSetVolume, int32_t *pIntPrevVolume, int32_t *pIntVolumeInc,
        float *pSetVolume, float *pPrevVolume, float *pVolumeInc) {
    if (newVolume == *pSetVolume) {
        return false;
    }
    /* set the floating point volume variables */
    if (ramp != 0) {
        *pVolumeInc = (newVolume - *pSetVolume) / ramp;
        *pPrevVolume = *pSetVolume;
    } else {
        *pVolumeInc = 0;
        *pPrevVolume = newVolume;
    }
    *pSetVolume = newVolume;

    /* set the legacy integer volume variables */
    int32_t intVolume = newVolume * AudioMixer::UNITY_GAIN_INT;
    if (intVolume > AudioMixer::UNITY_GAIN_INT) {
        intVolume = AudioMixer::UNITY_GAIN_INT;
    } else if (intVolume < 0) {
        ALOGE("negative volume %.7g", newVolume);
        intVolume = 0; // should never happen, but for safety check.
    }
    if (intVolume == *pIntSetVolume) {
        *pIntVolumeInc = 0;
        /* TODO: integer/float workaround: ignore floating volume ramp */
        *pVolumeInc = 0;
        *pPrevVolume = newVolume;
        return true;
    }
    if (ramp != 0) {
        *pIntVolumeInc = ((intVolume - *pIntSetVolume) << 16) / ramp;
        *pIntPrevVolume = (*pIntVolumeInc == 0 ? intVolume : *pIntSetVolume) << 16;
    } else {
        *pIntVolumeInc = 0;
        *pIntPrevVolume = intVolume << 16;
    }
    *pIntSetVolume = intVolume;
    return true;
}

void AudioMixer::setParameter(int name, int target, int param, void *value)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    int valueInt = static_cast<int>(reinterpret_cast<uintptr_t>(value));
    int32_t *valueBuf = reinterpret_cast<int32_t*>(value);

    switch (target) {

    case TRACK:
        switch (param) {
        case CHANNEL_MASK: {
            audio_channel_mask_t mask =
                static_cast<audio_channel_mask_t>(reinterpret_cast<uintptr_t>(value));
            if (track.channelMask != mask) {
                uint32_t channelCount = audio_channel_count_from_out_mask(mask);
                ALOG_ASSERT((channelCount <= MAX_NUM_CHANNELS_TO_DOWNMIX) && channelCount);
                track.channelMask = mask;
                track.channelCount = channelCount;
                // the mask has changed, does this track need a downmixer?
                // update to try using our desired format (if we aren't already using it)
                track.mMixerInFormat = kUseFloat && kUseNewMixer
                        ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
                status_t status = initTrackDownmix(&mState.tracks[name], name, mask);
                ALOGE_IF(status != OK,
                        "Invalid channel mask %#x, initTrackDownmix returned %d",
                        mask, status);
                ALOGV("setParameter(TRACK, CHANNEL_MASK, %x)", mask);
                prepareTrackForReformat(&track, name); // format may have changed
                invalidateState(1 << name);
            }
            } break;
        case MAIN_BUFFER:
            if (track.mainBuffer != valueBuf) {
                track.mainBuffer = valueBuf;
                ALOGV("setParameter(TRACK, MAIN_BUFFER, %p)", valueBuf);
                invalidateState(1 << name);
            }
            break;
        case AUX_BUFFER:
            if (track.auxBuffer != valueBuf) {
                track.auxBuffer = valueBuf;
                ALOGV("setParameter(TRACK, AUX_BUFFER, %p)", valueBuf);
                invalidateState(1 << name);
            }
            break;
        case FORMAT: {
            audio_format_t format = static_cast<audio_format_t>(valueInt);
            if (track.mFormat != format) {
                ALOG_ASSERT(audio_is_linear_pcm(format), "Invalid format %#x", format);
                track.mFormat = format;
                ALOGV("setParameter(TRACK, FORMAT, %#x)", format);
                prepareTrackForReformat(&track, name);
                invalidateState(1 << name);
            }
            } break;
        // FIXME do we want to support setting the downmix type from AudioFlinger?
        //         for a specific track? or per mixer?
        /* case DOWNMIX_TYPE:
            break          */
        case MIXER_FORMAT: {
            audio_format_t format = static_cast<audio_format_t>(valueInt);
            if (track.mMixerFormat != format) {
                track.mMixerFormat = format;
                ALOGV("setParameter(TRACK, MIXER_FORMAT, %#x)", format);
            }
            } break;
        default:
            LOG_ALWAYS_FATAL("setParameter track: bad param %d", param);
        }
        break;

    case RESAMPLE:
        switch (param) {
        case SAMPLE_RATE:
            ALOG_ASSERT(valueInt > 0, "bad sample rate %d", valueInt);
            if (track.setResampler(uint32_t(valueInt), mSampleRate)) {
                ALOGV("setParameter(RESAMPLE, SAMPLE_RATE, %u)",
                        uint32_t(valueInt));
                invalidateState(1 << name);
            }
            break;
        case RESET:
            track.resetResampler();
            invalidateState(1 << name);
            break;
        case REMOVE:
            delete track.resampler;
            track.resampler = NULL;
            track.sampleRate = mSampleRate;
            invalidateState(1 << name);
            break;
        default:
            LOG_ALWAYS_FATAL("setParameter resample: bad param %d", param);
        }
        break;

    case RAMP_VOLUME:
    case VOLUME:
        switch (param) {
        case VOLUME0:
        case VOLUME1:
            if (setVolumeRampVariables(*reinterpret_cast<float*>(value),
                    target == RAMP_VOLUME ? mState.frameCount : 0,
                    &track.volume[param - VOLUME0], &track.prevVolume[param - VOLUME0],
                    &track.volumeInc[param - VOLUME0],
                    &track.mVolume[param - VOLUME0], &track.mPrevVolume[param - VOLUME0],
                    &track.mVolumeInc[param - VOLUME0])) {
                ALOGV("setParameter(%s, VOLUME%d: %04x)",
                        target == VOLUME ? "VOLUME" : "RAMP_VOLUME", param - VOLUME0,
                                track.volume[param - VOLUME0]);
                invalidateState(1 << name);
            }
            break;
        case AUXLEVEL:
            if (setVolumeRampVariables(*reinterpret_cast<float*>(value),
                    target == RAMP_VOLUME ? mState.frameCount : 0,
                    &track.auxLevel, &track.prevAuxLevel, &track.auxInc,
                    &track.mAuxLevel, &track.mPrevAuxLevel, &track.mAuxInc)) {
                ALOGV("setParameter(%s, AUXLEVEL: %04x)",
                        target == VOLUME ? "VOLUME" : "RAMP_VOLUME", track.auxLevel);
                invalidateState(1 << name);
            }
            break;
        default:
            LOG_ALWAYS_FATAL("setParameter volume: bad param %d", param);
        }
        break;

    default:
        LOG_ALWAYS_FATAL("setParameter: bad target %d", target);
    }
}

bool AudioMixer::track_t::setResampler(uint32_t value, uint32_t devSampleRate)
{
    if (value != devSampleRate || resampler != NULL) {
        if (sampleRate != value) {
            sampleRate = value;
            if (resampler == NULL) {
                ALOGV("creating resampler from track %d Hz to device %d Hz", value, devSampleRate);
                AudioResampler::src_quality quality;
                // force lowest quality level resampler if use case isn't music or video
                // FIXME this is flawed for dynamic sample rates, as we choose the resampler
                // quality level based on the initial ratio, but that could change later.
                // Should have a way to distinguish tracks with static ratios vs. dynamic ratios.
                if (!((value == 44100 && devSampleRate == 48000) ||
                      (value == 48000 && devSampleRate == 44100))) {
                    quality = AudioResampler::DYN_LOW_QUALITY;
                } else {
                    quality = AudioResampler::DEFAULT_QUALITY;
                }

                ALOGVV("Creating resampler with %d bits\n", bits);
                resampler = AudioResampler::create(
                        mMixerInFormat,
                        // the resampler sees the number of channels after the downmixer, if any
                        (int) (downmixerBufferProvider != NULL ? MAX_NUM_CHANNELS : channelCount),
                        devSampleRate, quality);
                resampler->setLocalTimeFreq(sLocalTimeFreq);
            }
            return true;
        }
    }
    return false;
}

/* Checks to see if the volume ramp has completed and clears the increment
 * variables appropriately.
 *
 * FIXME: There is code to handle int/float ramp variable switchover should it not
 * complete within a mixer buffer processing call, but it is preferred to avoid switchover
 * due to precision issues.  The switchover code is included for legacy code purposes
 * and can be removed once the integer volume is removed.
 *
 * It is not sufficient to clear only the volumeInc integer variable because
 * if one channel requires ramping, all channels are ramped.
 *
 * There is a bit of duplicated code here, but it keeps backward compatibility.
 */
inline void AudioMixer::track_t::adjustVolumeRamp(bool aux, bool useFloat)
{
    if (useFloat) {
        for (uint32_t i=0 ; i<MAX_NUM_CHANNELS ; i++) {
            if (mVolumeInc[i] != 0 && fabs(mVolume[i] - mPrevVolume[i]) <= fabs(mVolumeInc[i])) {
                volumeInc[i] = 0;
                prevVolume[i] = volume[i] << 16;
                mVolumeInc[i] = 0.;
                mPrevVolume[i] = mVolume[i];

            } else {
                //ALOGV("ramp: %f %f %f", mVolume[i], mPrevVolume[i], mVolumeInc[i]);
                prevVolume[i] = u4_28_from_float(mPrevVolume[i]);
            }
        }
    } else {
        for (uint32_t i=0 ; i<MAX_NUM_CHANNELS ; i++) {
            if (((volumeInc[i]>0) && (((prevVolume[i]+volumeInc[i])>>16) >= volume[i])) ||
                    ((volumeInc[i]<0) && (((prevVolume[i]+volumeInc[i])>>16) <= volume[i]))) {
                volumeInc[i] = 0;
                prevVolume[i] = volume[i] << 16;
                mVolumeInc[i] = 0.;
                mPrevVolume[i] = mVolume[i];
            } else {
                //ALOGV("ramp: %d %d %d", volume[i] << 16, prevVolume[i], volumeInc[i]);
                mPrevVolume[i]  = float_from_u4_28(prevVolume[i]);
            }
        }
    }
    /* TODO: aux is always integer regardless of output buffer type */
    if (aux) {
        if (((auxInc>0) && (((prevAuxLevel+auxInc)>>16) >= auxLevel)) ||
                ((auxInc<0) && (((prevAuxLevel+auxInc)>>16) <= auxLevel))) {
            auxInc = 0;
            prevAuxLevel = auxLevel << 16;
            mAuxInc = 0.;
            mPrevAuxLevel = mAuxLevel;
        } else {
            //ALOGV("aux ramp: %d %d %d", auxLevel << 16, prevAuxLevel, auxInc);
        }
    }
}

size_t AudioMixer::getUnreleasedFrames(int name) const
{
    name -= TRACK0;
    if (uint32_t(name) < MAX_NUM_TRACKS) {
        return mState.tracks[name].getUnreleasedFrames();
    }
    return 0;
}

void AudioMixer::setBufferProvider(int name, AudioBufferProvider* bufferProvider)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);

    if (mState.tracks[name].mInputBufferProvider == bufferProvider) {
        return; // don't reset any buffer providers if identical.
    }
    if (mState.tracks[name].mReformatBufferProvider != NULL) {
        mState.tracks[name].mReformatBufferProvider->reset();
    } else if (mState.tracks[name].downmixerBufferProvider != NULL) {
    }

    mState.tracks[name].mInputBufferProvider = bufferProvider;
    reconfigureBufferProviders(&mState.tracks[name]);
}


void AudioMixer::process(int64_t pts)
{
    mState.hook(&mState, pts);
}


void AudioMixer::process__validate(state_t* state, int64_t pts)
{
    ALOGW_IF(!state->needsChanged,
        "in process__validate() but nothing's invalid");

    uint32_t changed = state->needsChanged;
    state->needsChanged = 0; // clear the validation flag

    // recompute which tracks are enabled / disabled
    uint32_t enabled = 0;
    uint32_t disabled = 0;
    while (changed) {
        const int i = 31 - __builtin_clz(changed);
        const uint32_t mask = 1<<i;
        changed &= ~mask;
        track_t& t = state->tracks[i];
        (t.enabled ? enabled : disabled) |= mask;
    }
    state->enabledTracks &= ~disabled;
    state->enabledTracks |=  enabled;

    // compute everything we need...
    int countActiveTracks = 0;
    bool all16BitsStereoNoResample = true;
    bool resampling = false;
    bool volumeRamp = false;
    uint32_t en = state->enabledTracks;
    while (en) {
        const int i = 31 - __builtin_clz(en);
        en &= ~(1<<i);

        countActiveTracks++;
        track_t& t = state->tracks[i];
        uint32_t n = 0;
        // FIXME can overflow (mask is only 3 bits)
        n |= NEEDS_CHANNEL_1 + t.channelCount - 1;
        if (t.doesResample()) {
            n |= NEEDS_RESAMPLE;
        }
        if (t.auxLevel != 0 && t.auxBuffer != NULL) {
            n |= NEEDS_AUX;
        }

        if (t.volumeInc[0]|t.volumeInc[1]) {
            volumeRamp = true;
        } else if (!t.doesResample() && t.volumeRL == 0) {
            n |= NEEDS_MUTE;
        }
        t.needs = n;

        if (n & NEEDS_MUTE) {
            t.hook = track__nop;
        } else {
            if (n & NEEDS_AUX) {
                all16BitsStereoNoResample = false;
            }
            if (n & NEEDS_RESAMPLE) {
                all16BitsStereoNoResample = false;
                resampling = true;
                t.hook = getTrackHook(TRACKTYPE_RESAMPLE, FCC_2,
                        t.mMixerInFormat, t.mMixerFormat);
                ALOGV_IF((n & NEEDS_CHANNEL_COUNT__MASK) > NEEDS_CHANNEL_2,
                        "Track %d needs downmix + resample", i);
            } else {
                if ((n & NEEDS_CHANNEL_COUNT__MASK) == NEEDS_CHANNEL_1){
                    t.hook = getTrackHook(TRACKTYPE_NORESAMPLEMONO, FCC_2,
                            t.mMixerInFormat, t.mMixerFormat);
                    all16BitsStereoNoResample = false;
                }
                if ((n & NEEDS_CHANNEL_COUNT__MASK) >= NEEDS_CHANNEL_2){
                    t.hook = getTrackHook(TRACKTYPE_NORESAMPLE, FCC_2,
                            t.mMixerInFormat, t.mMixerFormat);
                    ALOGV_IF((n & NEEDS_CHANNEL_COUNT__MASK) > NEEDS_CHANNEL_2,
                            "Track %d needs downmix", i);
                }
            }
        }
    }

    // select the processing hooks
    state->hook = process__nop;
    if (countActiveTracks > 0) {
        if (resampling) {
            if (!state->outputTemp) {
                state->outputTemp = new int32_t[MAX_NUM_CHANNELS * state->frameCount];
            }
            if (!state->resampleTemp) {
                state->resampleTemp = new int32_t[MAX_NUM_CHANNELS * state->frameCount];
            }
            state->hook = process__genericResampling;
        } else {
            if (state->outputTemp) {
                delete [] state->outputTemp;
                state->outputTemp = NULL;
            }
            if (state->resampleTemp) {
                delete [] state->resampleTemp;
                state->resampleTemp = NULL;
            }
            state->hook = process__genericNoResampling;
            if (all16BitsStereoNoResample && !volumeRamp) {
                if (countActiveTracks == 1) {
                    const int i = 31 - __builtin_clz(state->enabledTracks);
                    track_t& t = state->tracks[i];
                    state->hook = getProcessHook(PROCESSTYPE_NORESAMPLEONETRACK, FCC_2,
                            t.mMixerInFormat, t.mMixerFormat);
                }
            }
        }
    }

    ALOGV("mixer configuration change: %d activeTracks (%08x) "
        "all16BitsStereoNoResample=%d, resampling=%d, volumeRamp=%d",
        countActiveTracks, state->enabledTracks,
        all16BitsStereoNoResample, resampling, volumeRamp);

   state->hook(state, pts);

    // Now that the volume ramp has been done, set optimal state and
    // track hooks for subsequent mixer process
    if (countActiveTracks > 0) {
        bool allMuted = true;
        uint32_t en = state->enabledTracks;
        while (en) {
            const int i = 31 - __builtin_clz(en);
            en &= ~(1<<i);
            track_t& t = state->tracks[i];
            if (!t.doesResample() && t.volumeRL == 0) {
                t.needs |= NEEDS_MUTE;
                t.hook = track__nop;
            } else {
                allMuted = false;
            }
        }
        if (allMuted) {
            state->hook = process__nop;
        } else if (all16BitsStereoNoResample) {
            if (countActiveTracks == 1) {
                state->hook = process__OneTrack16BitsStereoNoResampling;
            }
        }
    }
}


void AudioMixer::track__genericResample(track_t* t, int32_t* out, size_t outFrameCount,
        int32_t* temp, int32_t* aux)
{
    ALOGVV("track__genericResample\n");
    t->resampler->setSampleRate(t->sampleRate);

    // ramp gain - resample to temp buffer and scale/mix in 2nd step
    if (aux != NULL) {
        // always resample with unity gain when sending to auxiliary buffer to be able
        // to apply send level after resampling
        // TODO: modify each resampler to support aux channel?
        t->resampler->setVolume(UNITY_GAIN_FLOAT, UNITY_GAIN_FLOAT);
        memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS * sizeof(int32_t));
        t->resampler->resample(temp, outFrameCount, t->bufferProvider);
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1]|t->auxInc)) {
            volumeRampStereo(t, out, outFrameCount, temp, aux);
        } else {
            volumeStereo(t, out, outFrameCount, temp, aux);
        }
    } else {
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1])) {
            t->resampler->setVolume(UNITY_GAIN_FLOAT, UNITY_GAIN_FLOAT);
            memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS * sizeof(int32_t));
            t->resampler->resample(temp, outFrameCount, t->bufferProvider);
            volumeRampStereo(t, out, outFrameCount, temp, aux);
        }

        // constant gain
        else {
            t->resampler->setVolume(t->mVolume[0], t->mVolume[1]);
            t->resampler->resample(out, outFrameCount, t->bufferProvider);
        }
    }
}

void AudioMixer::track__nop(track_t* t __unused, int32_t* out __unused,
        size_t outFrameCount __unused, int32_t* temp __unused, int32_t* aux __unused)
{
}

void AudioMixer::volumeRampStereo(track_t* t, int32_t* out, size_t frameCount, int32_t* temp,
        int32_t* aux)
{
    int32_t vl = t->prevVolume[0];
    int32_t vr = t->prevVolume[1];
    const int32_t vlInc = t->volumeInc[0];
    const int32_t vrInc = t->volumeInc[1];

    //ALOGD("[0] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
    //        t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
    //       (vl + vlInc*frameCount)/65536.0f, frameCount);

    // ramp volume
    if (CC_UNLIKELY(aux != NULL)) {
        int32_t va = t->prevAuxLevel;
        const int32_t vaInc = t->auxInc;
        int32_t l;
        int32_t r;

        do {
            l = (*temp++ >> 12);
            r = (*temp++ >> 12);
            *out++ += (vl >> 16) * l;
            *out++ += (vr >> 16) * r;
            *aux++ += (va >> 17) * (l + r);
            vl += vlInc;
            vr += vrInc;
            va += vaInc;
        } while (--frameCount);
        t->prevAuxLevel = va;
    } else {
        do {
            *out++ += (vl >> 16) * (*temp++ >> 12);
            *out++ += (vr >> 16) * (*temp++ >> 12);
            vl += vlInc;
            vr += vrInc;
        } while (--frameCount);
    }
    t->prevVolume[0] = vl;
    t->prevVolume[1] = vr;
    t->adjustVolumeRamp(aux != NULL);
}

void AudioMixer::volumeStereo(track_t* t, int32_t* out, size_t frameCount, int32_t* temp,
        int32_t* aux)
{
    const int16_t vl = t->volume[0];
    const int16_t vr = t->volume[1];

    if (CC_UNLIKELY(aux != NULL)) {
        const int16_t va = t->auxLevel;
        do {
            int16_t l = (int16_t)(*temp++ >> 12);
            int16_t r = (int16_t)(*temp++ >> 12);
            out[0] = mulAdd(l, vl, out[0]);
            int16_t a = (int16_t)(((int32_t)l + r) >> 1);
            out[1] = mulAdd(r, vr, out[1]);
            out += 2;
            aux[0] = mulAdd(a, va, aux[0]);
            aux++;
        } while (--frameCount);
    } else {
        do {
            int16_t l = (int16_t)(*temp++ >> 12);
            int16_t r = (int16_t)(*temp++ >> 12);
            out[0] = mulAdd(l, vl, out[0]);
            out[1] = mulAdd(r, vr, out[1]);
            out += 2;
        } while (--frameCount);
    }
}

void AudioMixer::track__16BitsStereo(track_t* t, int32_t* out, size_t frameCount,
        int32_t* temp __unused, int32_t* aux)
{
    ALOGVV("track__16BitsStereo\n");
    const int16_t *in = static_cast<const int16_t *>(t->in);

    if (CC_UNLIKELY(aux != NULL)) {
        int32_t l;
        int32_t r;
        // ramp gain
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1]|t->auxInc)) {
            int32_t vl = t->prevVolume[0];
            int32_t vr = t->prevVolume[1];
            int32_t va = t->prevAuxLevel;
            const int32_t vlInc = t->volumeInc[0];
            const int32_t vrInc = t->volumeInc[1];
            const int32_t vaInc = t->auxInc;
            // ALOGD("[1] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
            //        t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
            //        (vl + vlInc*frameCount)/65536.0f, frameCount);

            do {
                l = (int32_t)*in++;
                r = (int32_t)*in++;
                *out++ += (vl >> 16) * l;
                *out++ += (vr >> 16) * r;
                *aux++ += (va >> 17) * (l + r);
                vl += vlInc;
                vr += vrInc;
                va += vaInc;
            } while (--frameCount);

            t->prevVolume[0] = vl;
            t->prevVolume[1] = vr;
            t->prevAuxLevel = va;
            t->adjustVolumeRamp(true);
        }

        // constant gain
        else {
            const uint32_t vrl = t->volumeRL;
            const int16_t va = (int16_t)t->auxLevel;
            do {
                uint32_t rl = *reinterpret_cast<const uint32_t *>(in);
                int16_t a = (int16_t)(((int32_t)in[0] + in[1]) >> 1);
                in += 2;
                out[0] = mulAddRL(1, rl, vrl, out[0]);
                out[1] = mulAddRL(0, rl, vrl, out[1]);
                out += 2;
                aux[0] = mulAdd(a, va, aux[0]);
                aux++;
            } while (--frameCount);
        }
    } else {
        // ramp gain
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1])) {
            int32_t vl = t->prevVolume[0];
            int32_t vr = t->prevVolume[1];
            const int32_t vlInc = t->volumeInc[0];
            const int32_t vrInc = t->volumeInc[1];

            // ALOGD("[1] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
            //        t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
            //        (vl + vlInc*frameCount)/65536.0f, frameCount);

            do {
                *out++ += (vl >> 16) * (int32_t) *in++;
                *out++ += (vr >> 16) * (int32_t) *in++;
                vl += vlInc;
                vr += vrInc;
            } while (--frameCount);

            t->prevVolume[0] = vl;
            t->prevVolume[1] = vr;
            t->adjustVolumeRamp(false);
        }

        // constant gain
        else {
            const uint32_t vrl = t->volumeRL;
            do {
                uint32_t rl = *reinterpret_cast<const uint32_t *>(in);
                in += 2;
                out[0] = mulAddRL(1, rl, vrl, out[0]);
                out[1] = mulAddRL(0, rl, vrl, out[1]);
                out += 2;
            } while (--frameCount);
        }
    }
    t->in = in;
}

void AudioMixer::track__16BitsMono(track_t* t, int32_t* out, size_t frameCount,
        int32_t* temp __unused, int32_t* aux)
{
    ALOGVV("track__16BitsMono\n");
    const int16_t *in = static_cast<int16_t const *>(t->in);

    if (CC_UNLIKELY(aux != NULL)) {
        // ramp gain
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1]|t->auxInc)) {
            int32_t vl = t->prevVolume[0];
            int32_t vr = t->prevVolume[1];
            int32_t va = t->prevAuxLevel;
            const int32_t vlInc = t->volumeInc[0];
            const int32_t vrInc = t->volumeInc[1];
            const int32_t vaInc = t->auxInc;

            // ALOGD("[2] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
            //         t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
            //         (vl + vlInc*frameCount)/65536.0f, frameCount);

            do {
                int32_t l = *in++;
                *out++ += (vl >> 16) * l;
                *out++ += (vr >> 16) * l;
                *aux++ += (va >> 16) * l;
                vl += vlInc;
                vr += vrInc;
                va += vaInc;
            } while (--frameCount);

            t->prevVolume[0] = vl;
            t->prevVolume[1] = vr;
            t->prevAuxLevel = va;
            t->adjustVolumeRamp(true);
        }
        // constant gain
        else {
            const int16_t vl = t->volume[0];
            const int16_t vr = t->volume[1];
            const int16_t va = (int16_t)t->auxLevel;
            do {
                int16_t l = *in++;
                out[0] = mulAdd(l, vl, out[0]);
                out[1] = mulAdd(l, vr, out[1]);
                out += 2;
                aux[0] = mulAdd(l, va, aux[0]);
                aux++;
            } while (--frameCount);
        }
    } else {
        // ramp gain
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1])) {
            int32_t vl = t->prevVolume[0];
            int32_t vr = t->prevVolume[1];
            const int32_t vlInc = t->volumeInc[0];
            const int32_t vrInc = t->volumeInc[1];

            // ALOGD("[2] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
            //         t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
            //         (vl + vlInc*frameCount)/65536.0f, frameCount);

            do {
                int32_t l = *in++;
                *out++ += (vl >> 16) * l;
                *out++ += (vr >> 16) * l;
                vl += vlInc;
                vr += vrInc;
            } while (--frameCount);

            t->prevVolume[0] = vl;
            t->prevVolume[1] = vr;
            t->adjustVolumeRamp(false);
        }
        // constant gain
        else {
            const int16_t vl = t->volume[0];
            const int16_t vr = t->volume[1];
            do {
                int16_t l = *in++;
                out[0] = mulAdd(l, vl, out[0]);
                out[1] = mulAdd(l, vr, out[1]);
                out += 2;
            } while (--frameCount);
        }
    }
    t->in = in;
}

// no-op case
void AudioMixer::process__nop(state_t* state, int64_t pts)
{
    ALOGVV("process__nop\n");
    uint32_t e0 = state->enabledTracks;
    size_t sampleCount = state->frameCount * MAX_NUM_CHANNELS;
    while (e0) {
        // process by group of tracks with same output buffer to
        // avoid multiple memset() on same buffer
        uint32_t e1 = e0, e2 = e0;
        int i = 31 - __builtin_clz(e1);
        {
            track_t& t1 = state->tracks[i];
            e2 &= ~(1<<i);
            while (e2) {
                i = 31 - __builtin_clz(e2);
                e2 &= ~(1<<i);
                track_t& t2 = state->tracks[i];
                if (CC_UNLIKELY(t2.mainBuffer != t1.mainBuffer)) {
                    e1 &= ~(1<<i);
                }
            }
            e0 &= ~(e1);

            memset(t1.mainBuffer, 0, sampleCount
                    * audio_bytes_per_sample(t1.mMixerFormat));
        }

        while (e1) {
            i = 31 - __builtin_clz(e1);
            e1 &= ~(1<<i);
            {
                track_t& t3 = state->tracks[i];
                size_t outFrames = state->frameCount;
                while (outFrames) {
                    t3.buffer.frameCount = outFrames;
                    int64_t outputPTS = calculateOutputPTS(
                        t3, pts, state->frameCount - outFrames);
                    t3.bufferProvider->getNextBuffer(&t3.buffer, outputPTS);
                    if (t3.buffer.raw == NULL) break;
                    outFrames -= t3.buffer.frameCount;
                    t3.bufferProvider->releaseBuffer(&t3.buffer);
                }
            }
        }
    }
}

// generic code without resampling
void AudioMixer::process__genericNoResampling(state_t* state, int64_t pts)
{
    ALOGVV("process__genericNoResampling\n");
    int32_t outTemp[BLOCKSIZE * MAX_NUM_CHANNELS] __attribute__((aligned(32)));

    // acquire each track's buffer
    uint32_t enabledTracks = state->enabledTracks;
    uint32_t e0 = enabledTracks;
    while (e0) {
        const int i = 31 - __builtin_clz(e0);
        e0 &= ~(1<<i);
        track_t& t = state->tracks[i];
        t.buffer.frameCount = state->frameCount;
        t.bufferProvider->getNextBuffer(&t.buffer, pts);
        t.frameCount = t.buffer.frameCount;
        t.in = t.buffer.raw;
    }

    e0 = enabledTracks;
    while (e0) {
        // process by group of tracks with same output buffer to
        // optimize cache use
        uint32_t e1 = e0, e2 = e0;
        int j = 31 - __builtin_clz(e1);
        track_t& t1 = state->tracks[j];
        e2 &= ~(1<<j);
        while (e2) {
            j = 31 - __builtin_clz(e2);
            e2 &= ~(1<<j);
            track_t& t2 = state->tracks[j];
            if (CC_UNLIKELY(t2.mainBuffer != t1.mainBuffer)) {
                e1 &= ~(1<<j);
            }
        }
        e0 &= ~(e1);
        // this assumes output 16 bits stereo, no resampling
        int32_t *out = t1.mainBuffer;
        size_t numFrames = 0;
        do {
            memset(outTemp, 0, sizeof(outTemp));
            e2 = e1;
            while (e2) {
                const int i = 31 - __builtin_clz(e2);
                e2 &= ~(1<<i);
                track_t& t = state->tracks[i];
                size_t outFrames = BLOCKSIZE;
                int32_t *aux = NULL;
                if (CC_UNLIKELY(t.needs & NEEDS_AUX)) {
                    aux = t.auxBuffer + numFrames;
                }
                while (outFrames) {
                    // t.in == NULL can happen if the track was flushed just after having
                    // been enabled for mixing.
                   if (t.in == NULL) {
                        enabledTracks &= ~(1<<i);
                        e1 &= ~(1<<i);
                        break;
                    }
                    size_t inFrames = (t.frameCount > outFrames)?outFrames:t.frameCount;
                    if (inFrames > 0) {
                        t.hook(&t, outTemp + (BLOCKSIZE-outFrames)*MAX_NUM_CHANNELS, inFrames,
                                state->resampleTemp, aux);
                        t.frameCount -= inFrames;
                        outFrames -= inFrames;
                        if (CC_UNLIKELY(aux != NULL)) {
                            aux += inFrames;
                        }
                    }
                    if (t.frameCount == 0 && outFrames) {
                        t.bufferProvider->releaseBuffer(&t.buffer);
                        t.buffer.frameCount = (state->frameCount - numFrames) -
                                (BLOCKSIZE - outFrames);
                        int64_t outputPTS = calculateOutputPTS(
                            t, pts, numFrames + (BLOCKSIZE - outFrames));
                        t.bufferProvider->getNextBuffer(&t.buffer, outputPTS);
                        t.in = t.buffer.raw;
                        if (t.in == NULL) {
                            enabledTracks &= ~(1<<i);
                            e1 &= ~(1<<i);
                            break;
                        }
                        t.frameCount = t.buffer.frameCount;
                    }
                }
            }

            convertMixerFormat(out, t1.mMixerFormat, outTemp, t1.mMixerInFormat,
                    BLOCKSIZE * FCC_2);
            // TODO: fix ugly casting due to choice of out pointer type
            out = reinterpret_cast<int32_t*>((uint8_t*)out
                    + BLOCKSIZE * FCC_2 * audio_bytes_per_sample(t1.mMixerFormat));
            numFrames += BLOCKSIZE;
        } while (numFrames < state->frameCount);
    }

    // release each track's buffer
    e0 = enabledTracks;
    while (e0) {
        const int i = 31 - __builtin_clz(e0);
        e0 &= ~(1<<i);
        track_t& t = state->tracks[i];
        t.bufferProvider->releaseBuffer(&t.buffer);
    }
}


// generic code with resampling
void AudioMixer::process__genericResampling(state_t* state, int64_t pts)
{
    ALOGVV("process__genericResampling\n");
    // this const just means that local variable outTemp doesn't change
    int32_t* const outTemp = state->outputTemp;
    const size_t size = sizeof(int32_t) * MAX_NUM_CHANNELS * state->frameCount;

    size_t numFrames = state->frameCount;

    uint32_t e0 = state->enabledTracks;
    while (e0) {
        // process by group of tracks with same output buffer
        // to optimize cache use
        uint32_t e1 = e0, e2 = e0;
        int j = 31 - __builtin_clz(e1);
        track_t& t1 = state->tracks[j];
        e2 &= ~(1<<j);
        while (e2) {
            j = 31 - __builtin_clz(e2);
            e2 &= ~(1<<j);
            track_t& t2 = state->tracks[j];
            if (CC_UNLIKELY(t2.mainBuffer != t1.mainBuffer)) {
                e1 &= ~(1<<j);
            }
        }
        e0 &= ~(e1);
        int32_t *out = t1.mainBuffer;
        memset(outTemp, 0, size);
        while (e1) {
            const int i = 31 - __builtin_clz(e1);
            e1 &= ~(1<<i);
            track_t& t = state->tracks[i];
            int32_t *aux = NULL;
            if (CC_UNLIKELY(t.needs & NEEDS_AUX)) {
                aux = t.auxBuffer;
            }

            // this is a little goofy, on the resampling case we don't
            // acquire/release the buffers because it's done by
            // the resampler.
            if (t.needs & NEEDS_RESAMPLE) {
                t.resampler->setPTS(pts);
                t.hook(&t, outTemp, numFrames, state->resampleTemp, aux);
            } else {

                size_t outFrames = 0;

                while (outFrames < numFrames) {
                    t.buffer.frameCount = numFrames - outFrames;
                    int64_t outputPTS = calculateOutputPTS(t, pts, outFrames);
                    t.bufferProvider->getNextBuffer(&t.buffer, outputPTS);
                    t.in = t.buffer.raw;
                    // t.in == NULL can happen if the track was flushed just after having
                    // been enabled for mixing.
                    if (t.in == NULL) break;

                    if (CC_UNLIKELY(aux != NULL)) {
                        aux += outFrames;
                    }
                    t.hook(&t, outTemp + outFrames*MAX_NUM_CHANNELS, t.buffer.frameCount,
                            state->resampleTemp, aux);
                    outFrames += t.buffer.frameCount;
                    t.bufferProvider->releaseBuffer(&t.buffer);
                }
            }
        }
        convertMixerFormat(out, t1.mMixerFormat, outTemp, t1.mMixerInFormat, numFrames * FCC_2);
    }
}

// one track, 16 bits stereo without resampling is the most common case
void AudioMixer::process__OneTrack16BitsStereoNoResampling(state_t* state,
                                                           int64_t pts)
{
    ALOGVV("process__OneTrack16BitsStereoNoResampling\n");
    // This method is only called when state->enabledTracks has exactly
    // one bit set.  The asserts below would verify this, but are commented out
    // since the whole point of this method is to optimize performance.
    //ALOG_ASSERT(0 != state->enabledTracks, "no tracks enabled");
    const int i = 31 - __builtin_clz(state->enabledTracks);
    //ALOG_ASSERT((1 << i) == state->enabledTracks, "more than 1 track enabled");
    const track_t& t = state->tracks[i];

    AudioBufferProvider::Buffer& b(t.buffer);

    int32_t* out = t.mainBuffer;
    float *fout = reinterpret_cast<float*>(out);
    size_t numFrames = state->frameCount;

    const int16_t vl = t.volume[0];
    const int16_t vr = t.volume[1];
    const uint32_t vrl = t.volumeRL;
    while (numFrames) {
        b.frameCount = numFrames;
        int64_t outputPTS = calculateOutputPTS(t, pts, out - t.mainBuffer);
        t.bufferProvider->getNextBuffer(&b, outputPTS);
        const int16_t *in = b.i16;

        // in == NULL can happen if the track was flushed just after having
        // been enabled for mixing.
        if (in == NULL || (((uintptr_t)in) & 3)) {
            memset(out, 0, numFrames
                    * MAX_NUM_CHANNELS * audio_bytes_per_sample(t.mMixerFormat));
            ALOGE_IF((((uintptr_t)in) & 3), "process stereo track: input buffer alignment pb: "
                                              "buffer %p track %d, channels %d, needs %08x",
                    in, i, t.channelCount, t.needs);
            return;
        }
        size_t outFrames = b.frameCount;

        switch (t.mMixerFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            do {
                uint32_t rl = *reinterpret_cast<const uint32_t *>(in);
                in += 2;
                int32_t l = mulRL(1, rl, vrl);
                int32_t r = mulRL(0, rl, vrl);
                *fout++ = float_from_q4_27(l);
                *fout++ = float_from_q4_27(r);
                // Note: In case of later int16_t sink output,
                // conversion and clamping is done by memcpy_to_i16_from_float().
            } while (--outFrames);
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            if (CC_UNLIKELY(uint32_t(vl) > UNITY_GAIN_INT || uint32_t(vr) > UNITY_GAIN_INT)) {
                // volume is boosted, so we might need to clamp even though
                // we process only one track.
                do {
                    uint32_t rl = *reinterpret_cast<const uint32_t *>(in);
                    in += 2;
                    int32_t l = mulRL(1, rl, vrl) >> 12;
                    int32_t r = mulRL(0, rl, vrl) >> 12;
                    // clamping...
                    l = clamp16(l);
                    r = clamp16(r);
                    *out++ = (r<<16) | (l & 0xFFFF);
                } while (--outFrames);
            } else {
                do {
                    uint32_t rl = *reinterpret_cast<const uint32_t *>(in);
                    in += 2;
                    int32_t l = mulRL(1, rl, vrl) >> 12;
                    int32_t r = mulRL(0, rl, vrl) >> 12;
                    *out++ = (r<<16) | (l & 0xFFFF);
                } while (--outFrames);
            }
            break;
        default:
            LOG_ALWAYS_FATAL("bad mixer format: %d", t.mMixerFormat);
        }
        numFrames -= b.frameCount;
        t.bufferProvider->releaseBuffer(&b);
    }
}

#if 0
// 2 tracks is also a common case
// NEVER used in current implementation of process__validate()
// only use if the 2 tracks have the same output buffer
void AudioMixer::process__TwoTracks16BitsStereoNoResampling(state_t* state,
                                                            int64_t pts)
{
    int i;
    uint32_t en = state->enabledTracks;

    i = 31 - __builtin_clz(en);
    const track_t& t0 = state->tracks[i];
    AudioBufferProvider::Buffer& b0(t0.buffer);

    en &= ~(1<<i);
    i = 31 - __builtin_clz(en);
    const track_t& t1 = state->tracks[i];
    AudioBufferProvider::Buffer& b1(t1.buffer);

    const int16_t *in0;
    const int16_t vl0 = t0.volume[0];
    const int16_t vr0 = t0.volume[1];
    size_t frameCount0 = 0;

    const int16_t *in1;
    const int16_t vl1 = t1.volume[0];
    const int16_t vr1 = t1.volume[1];
    size_t frameCount1 = 0;

    //FIXME: only works if two tracks use same buffer
    int32_t* out = t0.mainBuffer;
    size_t numFrames = state->frameCount;
    const int16_t *buff = NULL;


    while (numFrames) {

        if (frameCount0 == 0) {
            b0.frameCount = numFrames;
            int64_t outputPTS = calculateOutputPTS(t0, pts,
                                                   out - t0.mainBuffer);
            t0.bufferProvider->getNextBuffer(&b0, outputPTS);
            if (b0.i16 == NULL) {
                if (buff == NULL) {
                    buff = new int16_t[MAX_NUM_CHANNELS * state->frameCount];
                }
                in0 = buff;
                b0.frameCount = numFrames;
            } else {
                in0 = b0.i16;
            }
            frameCount0 = b0.frameCount;
        }
        if (frameCount1 == 0) {
            b1.frameCount = numFrames;
            int64_t outputPTS = calculateOutputPTS(t1, pts,
                                                   out - t0.mainBuffer);
            t1.bufferProvider->getNextBuffer(&b1, outputPTS);
            if (b1.i16 == NULL) {
                if (buff == NULL) {
                    buff = new int16_t[MAX_NUM_CHANNELS * state->frameCount];
                }
                in1 = buff;
                b1.frameCount = numFrames;
            } else {
                in1 = b1.i16;
            }
            frameCount1 = b1.frameCount;
        }

        size_t outFrames = frameCount0 < frameCount1?frameCount0:frameCount1;

        numFrames -= outFrames;
        frameCount0 -= outFrames;
        frameCount1 -= outFrames;

        do {
            int32_t l0 = *in0++;
            int32_t r0 = *in0++;
            l0 = mul(l0, vl0);
            r0 = mul(r0, vr0);
            int32_t l = *in1++;
            int32_t r = *in1++;
            l = mulAdd(l, vl1, l0) >> 12;
            r = mulAdd(r, vr1, r0) >> 12;
            // clamping...
            l = clamp16(l);
            r = clamp16(r);
            *out++ = (r<<16) | (l & 0xFFFF);
        } while (--outFrames);

        if (frameCount0 == 0) {
            t0.bufferProvider->releaseBuffer(&b0);
        }
        if (frameCount1 == 0) {
            t1.bufferProvider->releaseBuffer(&b1);
        }
    }

    delete [] buff;
}
#endif

int64_t AudioMixer::calculateOutputPTS(const track_t& t, int64_t basePTS,
                                       int outputFrameIndex)
{
    if (AudioBufferProvider::kInvalidPTS == basePTS) {
        return AudioBufferProvider::kInvalidPTS;
    }

    return basePTS + ((outputFrameIndex * sLocalTimeFreq) / t.sampleRate);
}

/*static*/ uint64_t AudioMixer::sLocalTimeFreq;
/*static*/ pthread_once_t AudioMixer::sOnceControl = PTHREAD_ONCE_INIT;

/*static*/ void AudioMixer::sInitRoutine()
{
    LocalClock lc;
    sLocalTimeFreq = lc.getLocalFreq(); // for the resampler

    DownmixerBufferProvider::init(); // for the downmixer
}

template <int MIXTYPE, int NCHAN, bool USEFLOATVOL, bool ADJUSTVOL,
    typename TO, typename TI, typename TA>
void AudioMixer::volumeMix(TO *out, size_t outFrames,
        const TI *in, TA *aux, bool ramp, AudioMixer::track_t *t)
{
    if (USEFLOATVOL) {
        if (ramp) {
            volumeRampMulti<MIXTYPE, NCHAN>(out, outFrames, in, aux,
                    t->mPrevVolume, t->mVolumeInc, &t->prevAuxLevel, t->auxInc);
            if (ADJUSTVOL) {
                t->adjustVolumeRamp(aux != NULL, true);
            }
        } else {
            volumeMulti<MIXTYPE, NCHAN>(out, outFrames, in, aux,
                    t->mVolume, t->auxLevel);
        }
    } else {
        if (ramp) {
            volumeRampMulti<MIXTYPE, NCHAN>(out, outFrames, in, aux,
                    t->prevVolume, t->volumeInc, &t->prevAuxLevel, t->auxInc);
            if (ADJUSTVOL) {
                t->adjustVolumeRamp(aux != NULL);
            }
        } else {
            volumeMulti<MIXTYPE, NCHAN>(out, outFrames, in, aux,
                    t->volume, t->auxLevel);
        }
    }
}

/* This process hook is called when there is a single track without
 * aux buffer, volume ramp, or resampling.
 * TODO: Update the hook selection: this can properly handle aux and ramp.
 */
template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
void AudioMixer::process_NoResampleOneTrack(state_t* state, int64_t pts)
{
    ALOGVV("process_NoResampleOneTrack\n");
    // CLZ is faster than CTZ on ARM, though really not sure if true after 31 - clz.
    const int i = 31 - __builtin_clz(state->enabledTracks);
    ALOG_ASSERT((1 << i) == state->enabledTracks, "more than 1 track enabled");
    track_t *t = &state->tracks[i];
    TO* out = reinterpret_cast<TO*>(t->mainBuffer);
    TA* aux = reinterpret_cast<TA*>(t->auxBuffer);
    const bool ramp = t->needsRamp();

    for (size_t numFrames = state->frameCount; numFrames; ) {
        AudioBufferProvider::Buffer& b(t->buffer);
        // get input buffer
        b.frameCount = numFrames;
        const int64_t outputPTS = calculateOutputPTS(*t, pts, state->frameCount - numFrames);
        t->bufferProvider->getNextBuffer(&b, outputPTS);
        const TI *in = reinterpret_cast<TI*>(b.raw);

        // in == NULL can happen if the track was flushed just after having
        // been enabled for mixing.
        if (in == NULL || (((uintptr_t)in) & 3)) {
            memset(out, 0, numFrames
                    * NCHAN * audio_bytes_per_sample(t->mMixerFormat));
            ALOGE_IF((((uintptr_t)in) & 3), "process_NoResampleOneTrack: bus error: "
                    "buffer %p track %p, channels %d, needs %#x",
                    in, t, t->channelCount, t->needs);
            return;
        }

        const size_t outFrames = b.frameCount;
        volumeMix<MIXTYPE, NCHAN, is_same<TI, float>::value, false> (out,
                outFrames, in, aux, ramp, t);

        out += outFrames * NCHAN;
        if (aux != NULL) {
            aux += NCHAN;
        }
        numFrames -= b.frameCount;

        // release buffer
        t->bufferProvider->releaseBuffer(&b);
    }
    if (ramp) {
        t->adjustVolumeRamp(aux != NULL, is_same<TI, float>::value);
    }
}

/* This track hook is called to do resampling then mixing,
 * pulling from the track's upstream AudioBufferProvider.
 */
template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
void AudioMixer::track__Resample(track_t* t, TO* out, size_t outFrameCount, TO* temp, TA* aux)
{
    ALOGVV("track__Resample\n");
    t->resampler->setSampleRate(t->sampleRate);

    const bool ramp = t->needsRamp();
    if (ramp || aux != NULL) {
        // if ramp:        resample with unity gain to temp buffer and scale/mix in 2nd step.
        // if aux != NULL: resample with unity gain to temp buffer then apply send level.

        t->resampler->setVolume(UNITY_GAIN_FLOAT, UNITY_GAIN_FLOAT);
        memset(temp, 0, outFrameCount * NCHAN * sizeof(TO));
        t->resampler->resample((int32_t*)temp, outFrameCount, t->bufferProvider);

        volumeMix<MIXTYPE, NCHAN, is_same<TI, float>::value, true>(out, outFrameCount,
                temp, aux, ramp, t);

    } else { // constant volume gain
        t->resampler->setVolume(t->mVolume[0], t->mVolume[1]);
        t->resampler->resample((int32_t*)out, outFrameCount, t->bufferProvider);
    }
}

/* This track hook is called to mix a track, when no resampling is required.
 * The input buffer should be present in t->in.
 */
template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
void AudioMixer::track__NoResample(track_t* t, TO* out, size_t frameCount,
        TO* temp __unused, TA* aux)
{
    ALOGVV("track__NoResample\n");
    const TI *in = static_cast<const TI *>(t->in);

    volumeMix<MIXTYPE, NCHAN, is_same<TI, float>::value, true>(out, frameCount,
            in, aux, t->needsRamp(), t);

    // MIXTYPE_MONOEXPAND reads a single input channel and expands to NCHAN output channels.
    // MIXTYPE_MULTI reads NCHAN input channels and places to NCHAN output channels.
    in += (MIXTYPE == MIXTYPE_MONOEXPAND) ? frameCount : frameCount * NCHAN;
    t->in = in;
}

/* The Mixer engine generates either int32_t (Q4_27) or float data.
 * We use this function to convert the engine buffers
 * to the desired mixer output format, either int16_t (Q.15) or float.
 */
void AudioMixer::convertMixerFormat(void *out, audio_format_t mixerOutFormat,
        void *in, audio_format_t mixerInFormat, size_t sampleCount)
{
    switch (mixerInFormat) {
    case AUDIO_FORMAT_PCM_FLOAT:
        switch (mixerOutFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            memcpy(out, in, sampleCount * sizeof(float)); // MEMCPY. TODO optimize out
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            memcpy_to_i16_from_float((int16_t*)out, (float*)in, sampleCount);
            break;
        default:
            LOG_ALWAYS_FATAL("bad mixerOutFormat: %#x", mixerOutFormat);
            break;
        }
        break;
    case AUDIO_FORMAT_PCM_16_BIT:
        switch (mixerOutFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            memcpy_to_float_from_q4_27((float*)out, (int32_t*)in, sampleCount);
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            // two int16_t are produced per iteration
            ditherAndClamp((int32_t*)out, (int32_t*)in, sampleCount >> 1);
            break;
        default:
            LOG_ALWAYS_FATAL("bad mixerOutFormat: %#x", mixerOutFormat);
            break;
        }
        break;
    default:
        LOG_ALWAYS_FATAL("bad mixerInFormat: %#x", mixerInFormat);
        break;
    }
}

/* Returns the proper track hook to use for mixing the track into the output buffer.
 */
AudioMixer::hook_t AudioMixer::getTrackHook(int trackType, int channels,
        audio_format_t mixerInFormat, audio_format_t mixerOutFormat __unused)
{
    if (!kUseNewMixer && channels == FCC_2 && mixerInFormat == AUDIO_FORMAT_PCM_16_BIT) {
        switch (trackType) {
        case TRACKTYPE_NOP:
            return track__nop;
        case TRACKTYPE_RESAMPLE:
            return track__genericResample;
        case TRACKTYPE_NORESAMPLEMONO:
            return track__16BitsMono;
        case TRACKTYPE_NORESAMPLE:
            return track__16BitsStereo;
        default:
            LOG_ALWAYS_FATAL("bad trackType: %d", trackType);
            break;
        }
    }
    LOG_ALWAYS_FATAL_IF(channels != FCC_2); // TODO: must be stereo right now
    switch (trackType) {
    case TRACKTYPE_NOP:
        return track__nop;
    case TRACKTYPE_RESAMPLE:
        switch (mixerInFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            return (AudioMixer::hook_t)
                    track__Resample<MIXTYPE_MULTI, 2, float, float, int32_t>;
        case AUDIO_FORMAT_PCM_16_BIT:
            return (AudioMixer::hook_t)\
                    track__Resample<MIXTYPE_MULTI, 2, int32_t, int16_t, int32_t>;
        default:
            LOG_ALWAYS_FATAL("bad mixerInFormat: %#x", mixerInFormat);
            break;
        }
        break;
    case TRACKTYPE_NORESAMPLEMONO:
        switch (mixerInFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            return (AudioMixer::hook_t)
                    track__NoResample<MIXTYPE_MONOEXPAND, 2, float, float, int32_t>;
        case AUDIO_FORMAT_PCM_16_BIT:
            return (AudioMixer::hook_t)
                    track__NoResample<MIXTYPE_MONOEXPAND, 2, int32_t, int16_t, int32_t>;
        default:
            LOG_ALWAYS_FATAL("bad mixerInFormat: %#x", mixerInFormat);
            break;
        }
        break;
    case TRACKTYPE_NORESAMPLE:
        switch (mixerInFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            return (AudioMixer::hook_t)
                    track__NoResample<MIXTYPE_MULTI, 2, float, float, int32_t>;
        case AUDIO_FORMAT_PCM_16_BIT:
            return (AudioMixer::hook_t)
                    track__NoResample<MIXTYPE_MULTI, 2, int32_t, int16_t, int32_t>;
        default:
            LOG_ALWAYS_FATAL("bad mixerInFormat: %#x", mixerInFormat);
            break;
        }
        break;
    default:
        LOG_ALWAYS_FATAL("bad trackType: %d", trackType);
        break;
    }
    return NULL;
}

/* Returns the proper process hook for mixing tracks. Currently works only for
 * PROCESSTYPE_NORESAMPLEONETRACK, a mix involving one track, no resampling.
 */
AudioMixer::process_hook_t AudioMixer::getProcessHook(int processType, int channels,
        audio_format_t mixerInFormat, audio_format_t mixerOutFormat)
{
    if (processType != PROCESSTYPE_NORESAMPLEONETRACK) { // Only NORESAMPLEONETRACK
        LOG_ALWAYS_FATAL("bad processType: %d", processType);
        return NULL;
    }
    if (!kUseNewMixer && channels == FCC_2 && mixerInFormat == AUDIO_FORMAT_PCM_16_BIT) {
        return process__OneTrack16BitsStereoNoResampling;
    }
    LOG_ALWAYS_FATAL_IF(channels != FCC_2); // TODO: must be stereo right now
    switch (mixerInFormat) {
    case AUDIO_FORMAT_PCM_FLOAT:
        switch (mixerOutFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            return process_NoResampleOneTrack<MIXTYPE_MULTI_SAVEONLY, 2,
                    float, float, int32_t>;
        case AUDIO_FORMAT_PCM_16_BIT:
            return process_NoResampleOneTrack<MIXTYPE_MULTI_SAVEONLY, 2,
                    int16_t, float, int32_t>;
        default:
            LOG_ALWAYS_FATAL("bad mixerOutFormat: %#x", mixerOutFormat);
            break;
        }
        break;
    case AUDIO_FORMAT_PCM_16_BIT:
        switch (mixerOutFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            return process_NoResampleOneTrack<MIXTYPE_MULTI_SAVEONLY, 2,
                    float, int16_t, int32_t>;
        case AUDIO_FORMAT_PCM_16_BIT:
            return process_NoResampleOneTrack<MIXTYPE_MULTI_SAVEONLY, 2,
                    int16_t, int16_t, int32_t>;
        default:
            LOG_ALWAYS_FATAL("bad mixerOutFormat: %#x", mixerOutFormat);
            break;
        }
        break;
    default:
        LOG_ALWAYS_FATAL("bad mixerInFormat: %#x", mixerInFormat);
        break;
    }
    return NULL;
}

// ----------------------------------------------------------------------------
}; // namespace android
