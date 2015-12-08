/*
 * Copyright (C) 2010 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerDriver"
#include <inttypes.h>
#include <utils/Log.h>
#include <cutils/properties.h>

#include "NuPlayerDriver.h"

#include "NuPlayer.h"
#include "NuPlayerSource.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include "mediaplayerservice/AVNuExtensions.h"
#include "mediaplayerservice/AVMediaServiceExtensions.h"

namespace android {

NuPlayerDriver::NuPlayerDriver(pid_t pid)
    : mState(STATE_IDLE),
      mIsAsyncPrepare(false),
      mAsyncResult(UNKNOWN_ERROR),
      mSetSurfaceInProgress(false),
      mDurationUs(-1),
      mPositionUs(-1),
      mSeekInProgress(false),
      mLooper(new ALooper),
      mPlayerFlags(0),
      mAtEOS(false),
      mLooping(false),
      mAutoLoop(false),
      mStartupSeekTimeUs(-1) {
    ALOGV("NuPlayerDriver(%p)", this);
    mLooper->setName("NuPlayerDriver Looper");

    mLooper->start(
            false, /* runOnCallingThread */
            true,  /* canCallJava */
            PRIORITY_AUDIO);

    mPlayer = AVNuFactory::get()->createNuPlayer(pid);
    mLooper->registerHandler(mPlayer);

    mPlayer->setDriver(this);
}

NuPlayerDriver::~NuPlayerDriver() {
    ALOGV("~NuPlayerDriver(%p)", this);
    mLooper->stop();
}

status_t NuPlayerDriver::initCheck() {
    return OK;
}

status_t NuPlayerDriver::setUID(uid_t uid) {
    mPlayer->setUID(uid);

    return OK;
}

status_t NuPlayerDriver::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers) {
    ALOGV("setDataSource(%p) url(%s)", this, uriDebugString(url, false).c_str());
    Mutex::Autolock autoLock(mLock);

    if (mState != STATE_IDLE) {
        return INVALID_OPERATION;
    }

    mState = STATE_SET_DATASOURCE_PENDING;

    mPlayer->setDataSourceAsync(httpService, url, headers);

    while (mState == STATE_SET_DATASOURCE_PENDING) {
        mCondition.wait(mLock);
    }

    return mAsyncResult;
}

status_t NuPlayerDriver::setDataSource(int fd, int64_t offset, int64_t length) {
    ALOGV("setDataSource(%p) file(%d)", this, fd);
    Mutex::Autolock autoLock(mLock);

    if (mState != STATE_IDLE) {
        return INVALID_OPERATION;
    }

    mState = STATE_SET_DATASOURCE_PENDING;

    mPlayer->setDataSourceAsync(fd, offset, length);

    while (mState == STATE_SET_DATASOURCE_PENDING) {
        mCondition.wait(mLock);
    }

    AVNuUtils::get()->printFileName(fd);
    return mAsyncResult;
}

status_t NuPlayerDriver::setDataSource(const sp<IStreamSource> &source) {
    ALOGV("setDataSource(%p) stream source", this);
    Mutex::Autolock autoLock(mLock);

    if (mState != STATE_IDLE) {
        return INVALID_OPERATION;
    }

    mState = STATE_SET_DATASOURCE_PENDING;

    mPlayer->setDataSourceAsync(source);

    while (mState == STATE_SET_DATASOURCE_PENDING) {
        mCondition.wait(mLock);
    }

    return mAsyncResult;
}

status_t NuPlayerDriver::setDataSource(const sp<DataSource> &source) {
    ALOGV("setDataSource(%p) callback source", this);
    Mutex::Autolock autoLock(mLock);

    if (mState != STATE_IDLE) {
        return INVALID_OPERATION;
    }

    mState = STATE_SET_DATASOURCE_PENDING;

    mPlayer->setDataSourceAsync(source);

    while (mState == STATE_SET_DATASOURCE_PENDING) {
        mCondition.wait(mLock);
    }

    return mAsyncResult;
}

status_t NuPlayerDriver::setVideoSurfaceTexture(
        const sp<IGraphicBufferProducer> &bufferProducer) {
    ALOGV("setVideoSurfaceTexture(%p)", this);
    Mutex::Autolock autoLock(mLock);

    if (mSetSurfaceInProgress) {
        return INVALID_OPERATION;
    }

    switch (mState) {
        case STATE_SET_DATASOURCE_PENDING:
        case STATE_RESET_IN_PROGRESS:
            return INVALID_OPERATION;

        default:
            break;
    }

    mSetSurfaceInProgress = true;

    mPlayer->setVideoSurfaceTextureAsync(bufferProducer);

    while (mSetSurfaceInProgress) {
        mCondition.wait(mLock);
    }

    return OK;
}

status_t NuPlayerDriver::prepare() {
    ALOGV("prepare(%p)", this);
    Mutex::Autolock autoLock(mLock);
    return prepare_l();
}

status_t NuPlayerDriver::prepare_l() {
    switch (mState) {
        case STATE_UNPREPARED:
            mState = STATE_PREPARING;

            // Make sure we're not posting any notifications, success or
            // failure information is only communicated through our result
            // code.
            mIsAsyncPrepare = false;
            mPlayer->prepareAsync();
            while (mState == STATE_PREPARING) {
                mCondition.wait(mLock);
            }
            return (mState == STATE_PREPARED) ? OK : UNKNOWN_ERROR;
        case STATE_STOPPED:
            // this is really just paused. handle as seek to start
            mAtEOS = false;
            mState = STATE_STOPPED_AND_PREPARING;
            mIsAsyncPrepare = false;
            mPlayer->seekToAsync(0, true /* needNotify */);
            while (mState == STATE_STOPPED_AND_PREPARING) {
                mCondition.wait(mLock);
            }
            return (mState == STATE_STOPPED_AND_PREPARED) ? OK : UNKNOWN_ERROR;
        default:
            return INVALID_OPERATION;
    };
}

status_t NuPlayerDriver::prepareAsync() {
    ALOGV("prepareAsync(%p)", this);
    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_UNPREPARED:
            mState = STATE_PREPARING;
            mIsAsyncPrepare = true;
            mPlayer->prepareAsync();
            return OK;
        case STATE_STOPPED:
            // this is really just paused. handle as seek to start
            mAtEOS = false;
            mState = STATE_STOPPED_AND_PREPARING;
            mIsAsyncPrepare = true;
            mPlayer->seekToAsync(0, true /* needNotify */);
            return OK;
        default:
            return INVALID_OPERATION;
    };
}

status_t NuPlayerDriver::start() {
    ALOGD("start(%p), state is %d, eos is %d", this, mState, mAtEOS);
    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_UNPREPARED:
        {
            status_t err = prepare_l();

            if (err != OK) {
                return err;
            }

            CHECK_EQ(mState, STATE_PREPARED);

            // fall through
        }

        case STATE_PAUSED:
        case STATE_STOPPED_AND_PREPARED:
        {
            if (mAtEOS && mStartupSeekTimeUs < 0) {
                mStartupSeekTimeUs = 0;
                mPositionUs = -1;
            }

            // fall through
        }

        case STATE_PREPARED:
        {
            mAtEOS = false;
            mPlayer->start();

            if (mStartupSeekTimeUs >= 0) {
                mPlayer->seekToAsync(mStartupSeekTimeUs);
                mStartupSeekTimeUs = -1;
            }
            break;
        }

        case STATE_RUNNING:
        {
            if (mAtEOS) {
                mPlayer->seekToAsync(0);
                mAtEOS = false;
                mPositionUs = -1;
            }
            break;
        }

        default:
            return INVALID_OPERATION;
    }

    mState = STATE_RUNNING;

    return OK;
}

status_t NuPlayerDriver::stop() {
    ALOGD("stop(%p)", this);
    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_RUNNING:
            mPlayer->pause();
            // fall through

        case STATE_PAUSED:
            mState = STATE_STOPPED;
            notifyListener_l(MEDIA_STOPPED);
            break;

        case STATE_PREPARED:
        case STATE_STOPPED:
        case STATE_STOPPED_AND_PREPARING:
        case STATE_STOPPED_AND_PREPARED:
            mState = STATE_STOPPED;
            break;

        default:
            return INVALID_OPERATION;
    }

    return OK;
}

status_t NuPlayerDriver::pause() {
    // The NuPlayerRenderer may get flushed if pause for long enough, e.g. the pause timeout tear
    // down for audio offload mode. If that happens, the NuPlayerRenderer will no longer know the
    // current position. So similar to seekTo, update |mPositionUs| to the pause position by calling
    // getCurrentPosition here.
    int msec;
    getCurrentPosition(&msec);

    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_PAUSED:
        case STATE_PREPARED:
            return OK;

        case STATE_RUNNING:
            mState = STATE_PAUSED;
            notifyListener_l(MEDIA_PAUSED);
            mPlayer->pause();
            break;

        default:
            return INVALID_OPERATION;
    }

    return OK;
}

bool NuPlayerDriver::isPlaying() {
    return mState == STATE_RUNNING && !mAtEOS;
}

status_t NuPlayerDriver::setPlaybackSettings(const AudioPlaybackRate &rate) {
    status_t err = mPlayer->setPlaybackSettings(rate);
    if (err == OK) {
        Mutex::Autolock autoLock(mLock);
        if (rate.mSpeed == 0.f && mState == STATE_RUNNING) {
            mState = STATE_PAUSED;
            // try to update position
            (void)mPlayer->getCurrentPosition(&mPositionUs);
            notifyListener_l(MEDIA_PAUSED);
        } else if (rate.mSpeed != 0.f && mState == STATE_PAUSED) {
            mState = STATE_RUNNING;
        }
    }
    return err;
}

status_t NuPlayerDriver::getPlaybackSettings(AudioPlaybackRate *rate) {
    return mPlayer->getPlaybackSettings(rate);
}

status_t NuPlayerDriver::setSyncSettings(const AVSyncSettings &sync, float videoFpsHint) {
    return mPlayer->setSyncSettings(sync, videoFpsHint);
}

status_t NuPlayerDriver::getSyncSettings(AVSyncSettings *sync, float *videoFps) {
    return mPlayer->getSyncSettings(sync, videoFps);
}

status_t NuPlayerDriver::seekTo(int msec) {
    ALOGD("seekTo(%p) %d ms", this, msec);
    Mutex::Autolock autoLock(mLock);

    int64_t seekTimeUs = msec * 1000ll;

    switch (mState) {
        case STATE_PREPARED:
        case STATE_STOPPED_AND_PREPARED:
        case STATE_PAUSED:
            mStartupSeekTimeUs = seekTimeUs;
            // fall through.
        case STATE_RUNNING:
        {
            mAtEOS = false;
            mSeekInProgress = true;
            // seeks can take a while, so we essentially paused
            notifyListener_l(MEDIA_PAUSED);
            mPlayer->seekToAsync(seekTimeUs, true /* needNotify */);
            break;
        }

        default:
            return INVALID_OPERATION;
    }

    mPositionUs = seekTimeUs;
    return OK;
}

status_t NuPlayerDriver::getCurrentPosition(int *msec) {
    int64_t tempUs = 0;
    {
        Mutex::Autolock autoLock(mLock);
        if (mSeekInProgress || mState == STATE_PAUSED) {
            tempUs = (mPositionUs <= 0) ? 0 : mPositionUs;
            *msec = (int)divRound(tempUs, (int64_t)(1000));
            return OK;
        }
    }

    status_t ret = mPlayer->getCurrentPosition(&tempUs);

    Mutex::Autolock autoLock(mLock);
    // We need to check mSeekInProgress here because mPlayer->seekToAsync is an async call, which
    // means getCurrentPosition can be called before seek is completed. Iow, renderer may return a
    // position value that's different the seek to position.
    if (ret != OK) {
        tempUs = (mPositionUs <= 0) ? 0 : mPositionUs;
    } else {
        mPositionUs = tempUs;
    }
    *msec = (int)divRound(tempUs, (int64_t)(1000));
    return OK;
}

status_t NuPlayerDriver::getDuration(int *msec) {
    Mutex::Autolock autoLock(mLock);

    if (mDurationUs < 0) {
        return UNKNOWN_ERROR;
    }

    *msec = (mDurationUs + 500ll) / 1000;

    return OK;
}

status_t NuPlayerDriver::reset() {
    ALOGD("reset(%p)", this);
    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_IDLE:
            return OK;

        case STATE_SET_DATASOURCE_PENDING:
        case STATE_RESET_IN_PROGRESS:
            return INVALID_OPERATION;

        case STATE_PREPARING:
        {
            CHECK(mIsAsyncPrepare);

            notifyListener_l(MEDIA_PREPARED);
            break;
        }

        default:
            break;
    }

    if (mState != STATE_STOPPED) {
        notifyListener_l(MEDIA_STOPPED);
    }

    char value[PROPERTY_VALUE_MAX];
    if (property_get("persist.debug.sf.stats", value, NULL) &&
            (!strcmp("1", value) || !strcasecmp("true", value))) {
        Vector<String16> args;
        dump(-1, args);
    }

    mState = STATE_RESET_IN_PROGRESS;
    mPlayer->resetAsync();

    while (mState == STATE_RESET_IN_PROGRESS) {
        mCondition.wait(mLock);
    }

    mDurationUs = -1;
    mPositionUs = -1;
    mStartupSeekTimeUs = -1;
    mLooping = false;

    return OK;
}

status_t NuPlayerDriver::setLooping(int loop) {
    mLooping = loop != 0;
    return OK;
}

player_type NuPlayerDriver::playerType() {
    return NU_PLAYER;
}

status_t NuPlayerDriver::invoke(const Parcel &request, Parcel *reply) {
    if (reply == NULL) {
        ALOGE("reply is a NULL pointer");
        return BAD_VALUE;
    }

    int32_t methodId;
    status_t ret = request.readInt32(&methodId);
    if (ret != OK) {
        ALOGE("Failed to retrieve the requested method to invoke");
        return ret;
    }

    switch (methodId) {
        case INVOKE_ID_SET_VIDEO_SCALING_MODE:
        {
            int mode = request.readInt32();
            return mPlayer->setVideoScalingMode(mode);
        }

        case INVOKE_ID_GET_TRACK_INFO:
        {
            return mPlayer->getTrackInfo(reply);
        }

        case INVOKE_ID_SELECT_TRACK:
        {
            int trackIndex = request.readInt32();
            int msec = 0;
            // getCurrentPosition should always return OK
            getCurrentPosition(&msec);
            return mPlayer->selectTrack(trackIndex, true /* select */, msec * 1000ll);
        }

        case INVOKE_ID_UNSELECT_TRACK:
        {
            int trackIndex = request.readInt32();
            return mPlayer->selectTrack(trackIndex, false /* select */, 0xdeadbeef /* not used */);
        }

        case INVOKE_ID_GET_SELECTED_TRACK:
        {
            int32_t type = request.readInt32();
            return mPlayer->getSelectedTrack(type, reply);
        }

        default:
        {
            return INVALID_OPERATION;
        }
    }
}

void NuPlayerDriver::setAudioSink(const sp<AudioSink> &audioSink) {
    mPlayer->setAudioSink(audioSink);
    mAudioSink = audioSink;
}

status_t NuPlayerDriver::setParameter(
        int /* key */, const Parcel & /* request */) {
    return INVALID_OPERATION;
}

status_t NuPlayerDriver::getParameter(int /* key */, Parcel * /* reply */) {
    return INVALID_OPERATION;
}

status_t NuPlayerDriver::getMetadata(
        const media::Metadata::Filter& /* ids */, Parcel *records) {
    Mutex::Autolock autoLock(mLock);

    using media::Metadata;

    Metadata meta(records);

    meta.appendBool(
            Metadata::kPauseAvailable,
            mPlayerFlags & NuPlayer::Source::FLAG_CAN_PAUSE);

    meta.appendBool(
            Metadata::kSeekBackwardAvailable,
            mPlayerFlags & NuPlayer::Source::FLAG_CAN_SEEK_BACKWARD);

    meta.appendBool(
            Metadata::kSeekForwardAvailable,
            mPlayerFlags & NuPlayer::Source::FLAG_CAN_SEEK_FORWARD);

    meta.appendBool(
            Metadata::kSeekAvailable,
            mPlayerFlags & NuPlayer::Source::FLAG_CAN_SEEK);

    AVMediaServiceUtils::get()->appendMeta(&meta);

    return OK;
}

void NuPlayerDriver::notifyResetComplete() {
    ALOGD("notifyResetComplete(%p)", this);
    Mutex::Autolock autoLock(mLock);

    CHECK_EQ(mState, STATE_RESET_IN_PROGRESS);
    mState = STATE_IDLE;
    mCondition.broadcast();
}

void NuPlayerDriver::notifySetSurfaceComplete() {
    ALOGV("notifySetSurfaceComplete(%p)", this);
    Mutex::Autolock autoLock(mLock);

    CHECK(mSetSurfaceInProgress);
    mSetSurfaceInProgress = false;

    mCondition.broadcast();
}

void NuPlayerDriver::notifyDuration(int64_t durationUs) {
    Mutex::Autolock autoLock(mLock);
    mDurationUs = durationUs;
}

void NuPlayerDriver::notifySeekComplete() {
    ALOGV("notifySeekComplete(%p)", this);
    Mutex::Autolock autoLock(mLock);
    mSeekInProgress = false;
    notifySeekComplete_l();
}

void NuPlayerDriver::notifySeekComplete_l() {
    bool wasSeeking = true;
    if (mState == STATE_STOPPED_AND_PREPARING) {
        wasSeeking = false;
        mState = STATE_STOPPED_AND_PREPARED;
        mCondition.broadcast();
        if (!mIsAsyncPrepare) {
            // if we are preparing synchronously, no need to notify listener
            return;
        }
    } else if (mState == STATE_STOPPED) {
        // no need to notify listener
        return;
    }
    notifyListener_l(wasSeeking ? MEDIA_SEEK_COMPLETE : MEDIA_PREPARED);
}

status_t NuPlayerDriver::dump(
        int fd, const Vector<String16> & /* args */) const {

    Vector<sp<AMessage> > trackStats;
    mPlayer->getStats(&trackStats);

    AString logString(" NuPlayer\n");
    char buf[256] = {0};

    for (size_t i = 0; i < trackStats.size(); ++i) {
        const sp<AMessage> &stats = trackStats.itemAt(i);

        AString mime;
        if (stats->findString("mime", &mime)) {
            snprintf(buf, sizeof(buf), "  mime(%s)\n", mime.c_str());
            logString.append(buf);
        }

        AString name;
        if (stats->findString("component-name", &name)) {
            snprintf(buf, sizeof(buf), "    decoder(%s)\n", name.c_str());
            logString.append(buf);
        }

        if (mime.startsWith("video/")) {
            int32_t width, height;
            if (stats->findInt32("width", &width)
                    && stats->findInt32("height", &height)) {
                snprintf(buf, sizeof(buf), "    resolution(%d x %d)\n", width, height);
                logString.append(buf);
            }

            int64_t numFramesTotal = 0;
            int64_t numFramesDropped = 0;

            stats->findInt64("frames-total", &numFramesTotal);
            stats->findInt64("frames-dropped-output", &numFramesDropped);
            snprintf(buf, sizeof(buf), "    numFramesTotal(%lld), numFramesDropped(%lld), "
                     "percentageDropped(%.2f%%)\n",
                     (long long)numFramesTotal,
                     (long long)numFramesDropped,
                     numFramesTotal == 0
                            ? 0.0 : (double)(numFramesDropped * 100) / numFramesTotal);
            logString.append(buf);
        }
    }

    ALOGI("%s", logString.c_str());

    if (fd >= 0) {
        FILE *out = fdopen(dup(fd), "w");
        fprintf(out, "%s", logString.c_str());
        fclose(out);
        out = NULL;
    }

    return OK;
}

void NuPlayerDriver::notifyListener(
        int msg, int ext1, int ext2, const Parcel *in) {
    Mutex::Autolock autoLock(mLock);
    notifyListener_l(msg, ext1, ext2, in);
}

void NuPlayerDriver::notifyListener_l(
        int msg, int ext1, int ext2, const Parcel *in) {
    ALOGD("notifyListener_l(%p), (%d, %d, %d)", this, msg, ext1, ext2);
    switch (msg) {
        case MEDIA_PLAYBACK_COMPLETE:
        {
            if (mState != STATE_RESET_IN_PROGRESS) {
                if (mAutoLoop) {
                    audio_stream_type_t streamType = AUDIO_STREAM_MUSIC;
                    if (mAudioSink != NULL) {
                        streamType = mAudioSink->getAudioStreamType();
                    }
                    if (streamType == AUDIO_STREAM_NOTIFICATION) {
                        ALOGW("disabling auto-loop for notification");
                        mAutoLoop = false;
                    }
                }
                if (mLooping || mAutoLoop) {
                    mPlayer->seekToAsync(0);
                    if (mAudioSink != NULL) {
                        // The renderer has stopped the sink at the end in order to play out
                        // the last little bit of audio. If we're looping, we need to restart it.
                        mAudioSink->start();
                    }
                    // don't send completion event when looping
                    return;
                }

                mPlayer->pause();
                mState = STATE_PAUSED;
            }
            // fall through
        }

        case MEDIA_ERROR:
        {
            mAtEOS = true;
            break;
        }

        default:
            break;
    }

    mLock.unlock();
    sendEvent(msg, ext1, ext2, in);
    mLock.lock();
}

void NuPlayerDriver::notifySetDataSourceCompleted(status_t err) {
    Mutex::Autolock autoLock(mLock);

    CHECK_EQ(mState, STATE_SET_DATASOURCE_PENDING);

    mAsyncResult = err;
    mState = (err == OK) ? STATE_UNPREPARED : STATE_IDLE;
    mCondition.broadcast();
}

void NuPlayerDriver::notifyPrepareCompleted(status_t err) {
    Mutex::Autolock autoLock(mLock);

    if (mState != STATE_PREPARING) {
        // We were preparing asynchronously when the client called
        // reset(), we sent a premature "prepared" notification and
        // then initiated the reset. This notification is stale.
        CHECK(mState == STATE_RESET_IN_PROGRESS || mState == STATE_IDLE);
        return;
    }

    CHECK_EQ(mState, STATE_PREPARING);

    mAsyncResult = err;

    if (err == OK) {
        // update state before notifying client, so that if client calls back into NuPlayerDriver
        // in response, NuPlayerDriver has the right state
        mState = STATE_PREPARED;
        if (mIsAsyncPrepare) {
            notifyListener_l(MEDIA_PREPARED);
        }
    } else {
        mState = STATE_UNPREPARED;
        if (mIsAsyncPrepare) {
            notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
        }
    }

    sp<MetaData> meta = mPlayer->getFileMeta();
    int32_t loop;
    if (meta != NULL
            && meta->findInt32(kKeyAutoLoop, &loop) && loop != 0) {
        mAutoLoop = true;
    }

    mCondition.broadcast();
}

void NuPlayerDriver::notifyFlagsChanged(uint32_t flags) {
    Mutex::Autolock autoLock(mLock);

    mPlayerFlags = flags;
}

}  // namespace android
