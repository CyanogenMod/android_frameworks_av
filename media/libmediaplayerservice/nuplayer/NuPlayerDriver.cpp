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

#include "NuPlayerDriver.h"

#include "NuPlayer.h"
#include "NuPlayerSource.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MetaData.h>

namespace android {

NuPlayerDriver::NuPlayerDriver()
    : mState(STATE_IDLE),
      mIsAsyncPrepare(false),
      mAsyncResult(UNKNOWN_ERROR),
      mSetSurfaceInProgress(false),
      mDurationUs(-1),
      mPositionUs(-1),
      mNotifyTimeRealUs(-1),
      mPauseStartedTimeUs(-1),
      mNumFramesTotal(0),
      mNumFramesDropped(0),
      mLooper(new ALooper),
      mPlayerFlags(0),
      mAtEOS(false),
      mLooping(false),
      mAutoLoop(false),
      mStartupSeekTimeUs(-1) {
    mLooper->setName("NuPlayerDriver Looper");

    mLooper->start(
            false, /* runOnCallingThread */
            true,  /* canCallJava */
            PRIORITY_AUDIO);

    mPlayer = new NuPlayer;
    mLooper->registerHandler(mPlayer);

    mPlayer->setDriver(this);
}

NuPlayerDriver::~NuPlayerDriver() {
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
    Mutex::Autolock autoLock(mLock);

    ALOGV("setDataSource: url=%s", url);
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
    Mutex::Autolock autoLock(mLock);

    ALOGV("setDataSource: fd=%d", fd);
    if (mState != STATE_IDLE) {
        return INVALID_OPERATION;
    }

    mState = STATE_SET_DATASOURCE_PENDING;

    mPlayer->setDataSourceAsync(fd, offset, length);

    while (mState == STATE_SET_DATASOURCE_PENDING) {
        mCondition.wait(mLock);
    }

    return mAsyncResult;
}

status_t NuPlayerDriver::setDataSource(const sp<IStreamSource> &source) {
    Mutex::Autolock autoLock(mLock);

    ALOGV("setDataSource: stream source");
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
            mPlayer->seekToAsync(0);
            while (mState == STATE_STOPPED_AND_PREPARING) {
                mCondition.wait(mLock);
            }
            return (mState == STATE_STOPPED_AND_PREPARED) ? OK : UNKNOWN_ERROR;
        default:
            return INVALID_OPERATION;
    };
}

status_t NuPlayerDriver::prepareAsync() {
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
            mPlayer->seekToAsync(0);
            return OK;
        default:
            return INVALID_OPERATION;
    };
}

status_t NuPlayerDriver::start() {
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

        case STATE_PREPARED:
        {
            mAtEOS = false;
            mPlayer->start();

            if (mStartupSeekTimeUs >= 0) {
                if (mStartupSeekTimeUs == 0) {
                    notifySeekComplete_l();
                } else {
                    mPlayer->seekToAsync(mStartupSeekTimeUs);
                }

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

        case STATE_PAUSED:
        case STATE_STOPPED_AND_PREPARED:
        {
            mPlayer->resume();
            mPositionUs -= ALooper::GetNowUs() - mPauseStartedTimeUs;
            break;
        }

        default:
            return INVALID_OPERATION;
    }

    mState = STATE_RUNNING;
    mPauseStartedTimeUs = -1;

    return OK;
}

status_t NuPlayerDriver::stop() {
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
    setPauseStartedTimeIfNeeded();

    return OK;
}

status_t NuPlayerDriver::pause() {
    Mutex::Autolock autoLock(mLock);

    switch (mState) {
        case STATE_PAUSED:
        case STATE_PREPARED:
            return OK;

        case STATE_RUNNING:
            setPauseStartedTimeIfNeeded();
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

status_t NuPlayerDriver::seekTo(int msec) {
    Mutex::Autolock autoLock(mLock);

    int64_t seekTimeUs = msec * 1000ll;

    switch (mState) {
        case STATE_PREPARED:
        {
            mStartupSeekTimeUs = seekTimeUs;
            // pretend that the seek completed. It will actually happen when starting playback.
            // TODO: actually perform the seek here, so the player is ready to go at the new
            // location
            notifySeekComplete_l();
            break;
        }

        case STATE_RUNNING:
        case STATE_PAUSED:
        {
            mAtEOS = false;
            // seeks can take a while, so we essentially paused
            notifyListener_l(MEDIA_PAUSED);
            mPlayer->seekToAsync(seekTimeUs);
            break;
        }

        default:
            return INVALID_OPERATION;
    }

    mPositionUs = seekTimeUs;
    mNotifyTimeRealUs = -1;
    return OK;
}

status_t NuPlayerDriver::getCurrentPosition(int *msec) {
    Mutex::Autolock autoLock(mLock);

    if (mPositionUs < 0) {
        *msec = 0;
    } else if (mNotifyTimeRealUs == -1) {
        *msec = mPositionUs / 1000;
    } else {
        int64_t nowUs =
                (isPlaying() ?  ALooper::GetNowUs() : mPauseStartedTimeUs);
        *msec = (mPositionUs + nowUs - mNotifyTimeRealUs + 500ll) / 1000;
    }

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
            return mPlayer->selectTrack(trackIndex, true /* select */);
        }

        case INVOKE_ID_UNSELECT_TRACK:
        {
            int trackIndex = request.readInt32();
            return mPlayer->selectTrack(trackIndex, false /* select */);
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

    return OK;
}

void NuPlayerDriver::notifyResetComplete() {
    Mutex::Autolock autoLock(mLock);

    CHECK_EQ(mState, STATE_RESET_IN_PROGRESS);
    mState = STATE_IDLE;
    mCondition.broadcast();
}

void NuPlayerDriver::notifySetSurfaceComplete() {
    Mutex::Autolock autoLock(mLock);

    CHECK(mSetSurfaceInProgress);
    mSetSurfaceInProgress = false;

    mCondition.broadcast();
}

void NuPlayerDriver::notifyDuration(int64_t durationUs) {
    Mutex::Autolock autoLock(mLock);
    mDurationUs = durationUs;
}

void NuPlayerDriver::notifyPosition(int64_t positionUs) {
    Mutex::Autolock autoLock(mLock);
    if (isPlaying()) {
        mPositionUs = positionUs;
        mNotifyTimeRealUs = ALooper::GetNowUs();
    }
}

void NuPlayerDriver::notifySeekComplete() {
    Mutex::Autolock autoLock(mLock);
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

void NuPlayerDriver::notifyFrameStats(
        int64_t numFramesTotal, int64_t numFramesDropped) {
    Mutex::Autolock autoLock(mLock);
    mNumFramesTotal = numFramesTotal;
    mNumFramesDropped = numFramesDropped;
}

status_t NuPlayerDriver::dump(
        int fd, const Vector<String16> & /* args */) const {
    Mutex::Autolock autoLock(mLock);

    FILE *out = fdopen(dup(fd), "w");

    fprintf(out, " NuPlayer\n");
    fprintf(out, "  numFramesTotal(%" PRId64 "), numFramesDropped(%" PRId64 "), "
                 "percentageDropped(%.2f)\n",
                 mNumFramesTotal,
                 mNumFramesDropped,
                 mNumFramesTotal == 0
                    ? 0.0 : (double)mNumFramesDropped / mNumFramesTotal);

    fclose(out);
    out = NULL;

    return OK;
}

void NuPlayerDriver::notifyListener(
        int msg, int ext1, int ext2, const Parcel *in) {
    Mutex::Autolock autoLock(mLock);
    notifyListener_l(msg, ext1, ext2, in);
}

void NuPlayerDriver::notifyListener_l(
        int msg, int ext1, int ext2, const Parcel *in) {
    switch (msg) {
        case MEDIA_PLAYBACK_COMPLETE:
        {
            if (mState != STATE_RESET_IN_PROGRESS) {
                if (mLooping || (mAutoLoop
                        && (mAudioSink == NULL || mAudioSink->realtime()))) {
                    mPlayer->seekToAsync(0);
                    break;
                }

                mPlayer->pause();
                mState = STATE_PAUSED;
            }
            // fall through
        }

        case MEDIA_ERROR:
        {
            mAtEOS = true;
            setPauseStartedTimeIfNeeded();
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

void NuPlayerDriver::setPauseStartedTimeIfNeeded() {
    if (mPauseStartedTimeUs == -1) {
        mPauseStartedTimeUs = ALooper::GetNowUs();
    }
}

}  // namespace android
