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
#define LOG_TAG "NuPlayer"
#include <utils/Log.h>

#include "NuPlayer.h"

#include "HTTPLiveSource.h"
#include "NuPlayerCCDecoder.h"
#include "NuPlayerDecoder.h"
#include "NuPlayerDecoderBase.h"
#include "NuPlayerDecoderPassThrough.h"
#include "NuPlayerDriver.h"
#include "NuPlayerRenderer.h"
#include "NuPlayerSource.h"
#include "RTSPSource.h"
#include "StreamingSource.h"
#include "GenericSource.h"
#include "TextDescriptions.h"

#include "ATSParser.h"

#include <cutils/properties.h>

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <gui/IGraphicBufferProducer.h>

#include "avc_utils.h"

#include "ESDS.h"
#include <media/stagefright/Utils.h>
#include "ExtendedUtils.h"

namespace android {

struct NuPlayer::Action : public RefBase {
    Action() {}

    virtual void execute(NuPlayer *player) = 0;

private:
    DISALLOW_EVIL_CONSTRUCTORS(Action);
};

struct NuPlayer::SeekAction : public Action {
    SeekAction(int64_t seekTimeUs, bool needNotify)
        : mSeekTimeUs(seekTimeUs),
          mNeedNotify(needNotify) {
    }

    virtual void execute(NuPlayer *player) {
        player->performSeek(mSeekTimeUs, mNeedNotify);
    }

private:
    int64_t mSeekTimeUs;
    bool mNeedNotify;

    DISALLOW_EVIL_CONSTRUCTORS(SeekAction);
};

struct NuPlayer::ResumeDecoderAction : public Action {
    ResumeDecoderAction(bool needNotify)
        : mNeedNotify(needNotify) {
    }

    virtual void execute(NuPlayer *player) {
        player->performResumeDecoders(mNeedNotify);
    }

private:
    bool mNeedNotify;

    DISALLOW_EVIL_CONSTRUCTORS(ResumeDecoderAction);
};

struct NuPlayer::SetSurfaceAction : public Action {
    SetSurfaceAction(const sp<NativeWindowWrapper> &wrapper)
        : mWrapper(wrapper) {
    }

    virtual void execute(NuPlayer *player) {
        player->performSetSurface(mWrapper);
    }

private:
    sp<NativeWindowWrapper> mWrapper;

    DISALLOW_EVIL_CONSTRUCTORS(SetSurfaceAction);
};

struct NuPlayer::FlushDecoderAction : public Action {
    FlushDecoderAction(FlushCommand audio, FlushCommand video)
        : mAudio(audio),
          mVideo(video) {
    }

    virtual void execute(NuPlayer *player) {
        player->performDecoderFlush(mAudio, mVideo);
    }

private:
    FlushCommand mAudio;
    FlushCommand mVideo;

    DISALLOW_EVIL_CONSTRUCTORS(FlushDecoderAction);
};

struct NuPlayer::PostMessageAction : public Action {
    PostMessageAction(const sp<AMessage> &msg)
        : mMessage(msg) {
    }

    virtual void execute(NuPlayer *) {
        mMessage->post();
    }

private:
    sp<AMessage> mMessage;

    DISALLOW_EVIL_CONSTRUCTORS(PostMessageAction);
};

// Use this if there's no state necessary to save in order to execute
// the action.
struct NuPlayer::SimpleAction : public Action {
    typedef void (NuPlayer::*ActionFunc)();

    SimpleAction(ActionFunc func)
        : mFunc(func) {
    }

    virtual void execute(NuPlayer *player) {
        (player->*mFunc)();
    }

private:
    ActionFunc mFunc;

    DISALLOW_EVIL_CONSTRUCTORS(SimpleAction);
};

////////////////////////////////////////////////////////////////////////////////

NuPlayer::NuPlayer()
    : mUIDValid(false),
      mSourceFlags(0),
      mOffloadAudio(false),
      mOffloadDecodedPCM(false),
      mSwitchingFromPcmOffload(false),
      mIsStreaming(false),
      mAudioDecoderGeneration(0),
      mVideoDecoderGeneration(0),
      mRendererGeneration(0),
      mAudioEOS(false),
      mVideoEOS(false),
      mScanSourcesPending(false),
      mScanSourcesGeneration(0),
      mPollDurationGeneration(0),
      mTimedTextGeneration(0),
      mFlushingAudio(NONE),
      mFlushingVideo(NONE),
      mResumePending(false),
      mVideoScalingMode(NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW),
      mStarted(false),
      mPaused(false),
      mPausedByClient(false),
      mBuffering(false),
      mPlaying(false),
      mSeeking(false),
      mOffloadAudioTornDown(false) {

    clearFlushComplete();
    mPlayerExtendedStats = (PlayerExtendedStats *)ExtendedStats::Create(
            ExtendedStats::PLAYER, "NuPlayer", gettid());
}

NuPlayer::~NuPlayer() {
}

void NuPlayer::setUID(uid_t uid) {
    mUIDValid = true;
    mUID = uid;
}

void NuPlayer::setDriver(const wp<NuPlayerDriver> &driver) {
    mDriver = driver;
}

void NuPlayer::setDataSourceAsync(const sp<IStreamSource> &source) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, id());

    mIsStreaming = true;
    msg->setObject("source", new StreamingSource(notify, source));
    msg->post();
}

static bool IsHTTPLiveURL(const char *url) {
    if (!strncasecmp("http://", url, 7)
            || !strncasecmp("https://", url, 8)
            || !strncasecmp("file://", url, 7)) {
        size_t len = strlen(url);
        if (len >= 5 && !strcasecmp(".m3u8", &url[len - 5])) {
            return true;
        }

        if (strstr(url,"m3u8")) {
            return true;
        }
    }

    return false;
}

void NuPlayer::setDataSourceAsync(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers) {

    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());
    size_t len = strlen(url);

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, id());

    sp<Source> source;
    if (IsHTTPLiveURL(url)) {
        source = new HTTPLiveSource(notify, httpService, url, headers);
    } else if (!strncasecmp(url, "rtsp://", 7)) {
        source = new RTSPSource(
                notify, httpService, url, headers, mUIDValid, mUID);
    } else if ((!strncasecmp(url, "http://", 7)
                || !strncasecmp(url, "https://", 8))
                    && ((len >= 4 && !strcasecmp(".sdp", &url[len - 4]))
                    || strstr(url, ".sdp?"))) {
        source = new RTSPSource(
                notify, httpService, url, headers, mUIDValid, mUID, true);
    } else {
        sp<GenericSource> genericSource =
                new GenericSource(notify, mUIDValid, mUID);
        // Don't set FLAG_SECURE on mSourceFlags here for widevine.
        // The correct flags will be updated in Source::kWhatFlagsChanged
        // handler when  GenericSource is prepared.

        status_t err = genericSource->setDataSource(httpService, url, headers);

        if (err == OK) {
            source = genericSource;
        } else {
            ALOGE("Failed to set data source!");
        }
    }
    mIsStreaming = true;
    msg->setObject("source", source);
    msg->post();
}

void NuPlayer::setDataSourceAsync(int fd, int64_t offset, int64_t length) {
    PLAYER_STATS(profileStart, STATS_PROFILE_START_LATENCY);
    PLAYER_STATS(profileStart, STATS_PROFILE_SET_DATA_SOURCE);

    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, id());

    sp<GenericSource> source =
            new GenericSource(notify, mUIDValid, mUID);

    status_t err = source->setDataSource(fd, offset, length);

    if (err != OK) {
        ALOGE("Failed to set data source!");
        source = NULL;
    }
    mIsStreaming = false;

    msg->setObject("source", source);
    msg->post();
}

void NuPlayer::prepareAsync() {
    PLAYER_STATS(profileStart, STATS_PROFILE_PREPARE);
    (new AMessage(kWhatPrepare, id()))->post();
}

void NuPlayer::setVideoSurfaceTextureAsync(
        const sp<IGraphicBufferProducer> &bufferProducer) {
    sp<AMessage> msg = new AMessage(kWhatSetVideoNativeWindow, id());

    if (bufferProducer == NULL) {
        msg->setObject("native-window", NULL);
    } else {
        msg->setObject(
                "native-window",
                new NativeWindowWrapper(
                    new Surface(bufferProducer, true /* controlledByApp */)));
    }

    msg->post();
}

void NuPlayer::setAudioSink(const sp<MediaPlayerBase::AudioSink> &sink) {
    sp<AMessage> msg = new AMessage(kWhatSetAudioSink, id());
    msg->setObject("sink", sink);
    msg->post();
}

void NuPlayer::start() {
    PLAYER_STATS(notifyPlaying, true);
    (new AMessage(kWhatStart, id()))->post();
}

void NuPlayer::pause() {
    PLAYER_STATS(profileStart, STATS_PROFILE_PAUSE);
    (new AMessage(kWhatPause, id()))->post();
    //*Note* PLAYER_STATS(notifyPause, <timeUs>) done in NuPlayerRenderer
}


void NuPlayer::resetAsync() {
    if (mSource != NULL) {
        // During a reset, the data source might be unresponsive already, we need to
        // disconnect explicitly so that reads exit promptly.
        // We can't queue the disconnect request to the looper, as it might be
        // queued behind a stuck read and never gets processed.
        // Doing a disconnect outside the looper to allows the pending reads to exit
        // (either successfully or with error).
        mSource->disconnect();
    }

    (new AMessage(kWhatReset, id()))->post();
}

void NuPlayer::seekToAsync(int64_t seekTimeUs, bool needNotify) {
    PLAYER_STATS(notifySeek, seekTimeUs);
    PLAYER_STATS(profileStart, STATS_PROFILE_SEEK);

    sp<AMessage> msg = new AMessage(kWhatSeek, id());
    msg->setInt64("seekTimeUs", seekTimeUs);
    msg->setInt32("needNotify", needNotify);
    msg->post();
}


void NuPlayer::writeTrackInfo(
        Parcel* reply, const sp<AMessage> format) const {
    int32_t trackType;
    CHECK(format->findInt32("type", &trackType));

    AString lang;
    CHECK(format->findString("language", &lang));

    reply->writeInt32(2); // write something non-zero
    reply->writeInt32(trackType);
    reply->writeString16(String16(lang.c_str()));

    if (trackType == MEDIA_TRACK_TYPE_SUBTITLE) {
        AString mime;
        CHECK(format->findString("mime", &mime));

        int32_t isAuto, isDefault, isForced;
        CHECK(format->findInt32("auto", &isAuto));
        CHECK(format->findInt32("default", &isDefault));
        CHECK(format->findInt32("forced", &isForced));

        reply->writeString16(String16(mime.c_str()));
        reply->writeInt32(isAuto);
        reply->writeInt32(isDefault);
        reply->writeInt32(isForced);
    }
}

void NuPlayer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSetDataSource:
        {
            ALOGV("kWhatSetDataSource");

            CHECK(mSource == NULL);

            status_t err = OK;
            sp<RefBase> obj;
            CHECK(msg->findObject("source", &obj));
            if (obj != NULL) {
                mSource = static_cast<Source *>(obj.get());
            } else {
                err = UNKNOWN_ERROR;
            }

            CHECK(mDriver != NULL);
            sp<NuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                driver->notifySetDataSourceCompleted(err);
            }

            PLAYER_STATS(profileStop, STATS_PROFILE_SET_DATA_SOURCE);
            break;
        }

        case kWhatPrepare:
        {
            mSource->prepareAsync();
            break;
        }

        case kWhatGetTrackInfo:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            Parcel* reply;
            CHECK(msg->findPointer("reply", (void**)&reply));

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }

            size_t ccTracks = 0;
            if (mCCDecoder != NULL) {
                ccTracks = mCCDecoder->getTrackCount();
            }

            // total track count
            reply->writeInt32(inbandTracks + ccTracks);

            // write inband tracks
            for (size_t i = 0; i < inbandTracks; ++i) {
                writeTrackInfo(reply, mSource->getTrackInfo(i));
            }

            // write CC track
            for (size_t i = 0; i < ccTracks; ++i) {
                writeTrackInfo(reply, mCCDecoder->getTrackInfo(i));
            }

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
            break;
        }

        case kWhatGetSelectedTrack:
        {
            status_t err = INVALID_OPERATION;
            if (mSource != NULL) {
                err = OK;

                int32_t type32;
                CHECK(msg->findInt32("type", (int32_t*)&type32));
                media_track_type type = (media_track_type)type32;
                ssize_t selectedTrack = mSource->getSelectedTrack(type);

                Parcel* reply;
                CHECK(msg->findPointer("reply", (void**)&reply));
                reply->writeInt32(selectedTrack);
            }

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));
            response->postReply(replyID);
            break;
        }

        case kWhatSelectTrack:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            size_t trackIndex;
            int32_t select;
            int64_t timeUs;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK(msg->findInt32("select", &select));
            CHECK(msg->findInt64("timeUs", &timeUs));

            status_t err = INVALID_OPERATION;

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }
            size_t ccTracks = 0;
            if (mCCDecoder != NULL) {
                ccTracks = mCCDecoder->getTrackCount();
            }

            if (trackIndex < inbandTracks) {
                err = mSource->selectTrack(trackIndex, select, timeUs);

                if (!select && err == OK) {
                    int32_t type;
                    sp<AMessage> info = mSource->getTrackInfo(trackIndex);
                    if (info != NULL
                            && info->findInt32("type", &type)
                            && type == MEDIA_TRACK_TYPE_TIMEDTEXT) {
                        ++mTimedTextGeneration;
                    }
                }
            } else {
                trackIndex -= inbandTracks;

                if (trackIndex < ccTracks) {
                    err = mCCDecoder->selectTrack(trackIndex, select);
                }
            }

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);

            response->postReply(replyID);
            break;
        }

        case kWhatPollDuration:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mPollDurationGeneration) {
                // stale
                break;
            }

            int64_t durationUs;
            if (mDriver != NULL && mSource->getDuration(&durationUs) == OK) {
                sp<NuPlayerDriver> driver = mDriver.promote();
                if (driver != NULL) {
                    driver->notifyDuration(durationUs);
                }
            }

            msg->post(1000000ll);  // poll again in a second.
            break;
        }

        case kWhatSetVideoNativeWindow:
        {
            ALOGV("kWhatSetVideoNativeWindow");

            sp<RefBase> obj;
            CHECK(msg->findObject("native-window", &obj));

            if (mSource == NULL || mSource->getFormat(false /* audio */) == NULL) {
                performSetSurface(static_cast<NativeWindowWrapper *>(obj.get()));
                break;
            }

            mDeferredActions.push_back(
                    new FlushDecoderAction(FLUSH_CMD_FLUSH /* audio */,
                                           FLUSH_CMD_SHUTDOWN /* video */));

            mDeferredActions.push_back(
                    new SetSurfaceAction(
                        static_cast<NativeWindowWrapper *>(obj.get())));

            if (obj != NULL) {
                if (mStarted) {
                    // Issue a seek to refresh the video screen only if started otherwise
                    // the extractor may not yet be started and will assert.
                    // If the video decoder is not set (perhaps audio only in this case)
                    // do not perform a seek as it is not needed.
                    int64_t currentPositionUs = 0;
                    if (getCurrentPosition(&currentPositionUs) == OK) {
                        mDeferredActions.push_back(
                                new SeekAction(currentPositionUs, false /* needNotify */));
                    }
                }

                // If there is a new surface texture, instantiate decoders
                // again if possible.
                mDeferredActions.push_back(
                        new SimpleAction(&NuPlayer::performScanSources));
            }

            // After a flush without shutdown, decoder is paused.
            // Don't resume it until source seek is done, otherwise it could
            // start pulling stale data too soon.
            mDeferredActions.push_back(
                    new ResumeDecoderAction(false /* needNotify */));

            processDeferredActions();
            break;
        }

        case kWhatSetAudioSink:
        {
            ALOGV("kWhatSetAudioSink");

            sp<RefBase> obj;
            CHECK(msg->findObject("sink", &obj));

            mAudioSink = static_cast<MediaPlayerBase::AudioSink *>(obj.get());
            break;
        }

        case kWhatStart:
        {
            ALOGV("kWhatStart");
            if (mStarted) {
                onResume();
            } else {
                onStart();
            }
            mPausedByClient = false;
            break;
        }

        case kWhatScanSources:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mScanSourcesGeneration) {
                // Drop obsolete msg.
                break;
            }

            mScanSourcesPending = false;

            ALOGV("scanning sources haveAudio=%d, haveVideo=%d",
                mAudioDecoder != NULL, mVideoDecoder != NULL);

            bool mHadAnySourcesBefore =
                (mAudioDecoder != NULL) || (mVideoDecoder != NULL);

            // initialize video before audio because successful initialization of
            // video may change deep buffer mode of audio.
            if (mNativeWindow != NULL) {
                instantiateDecoder(false, &mVideoDecoder);
            }

            // Don't try to re-open audio sink if there's an existing decoder.
            if (mAudioSink != NULL && mAudioDecoder == NULL) {
                sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
                sp<AMessage> videoFormat = mSource->getFormat(false /* audio */);
                audio_stream_type_t streamType = mAudioSink->getAudioStreamType();
                sp<MetaData> vMeta = new MetaData;
                convertMessageToMetaData(videoFormat, vMeta);
                const bool hasVideo = (videoFormat != NULL);
                const bool canOffload = canOffloadStream(
                        audioMeta, hasVideo, vMeta, mIsStreaming /* is_streaming */, streamType);
                if (canOffload) {
                    if (!mOffloadAudio) {
                        mRenderer->signalEnableOffloadAudio();
                    }
                    // open audio sink early under offload mode.
                    sp<AMessage> format = mSource->getFormat(true /*audio*/);
                    tryOpenAudioSinkForOffload(format, hasVideo);
                }
                instantiateDecoder(true, &mAudioDecoder);
            }

            if (!mHadAnySourcesBefore
                    && (mAudioDecoder != NULL || mVideoDecoder != NULL)) {
                // This is the first time we've found anything playable.

                if (mSourceFlags & Source::FLAG_DYNAMIC_DURATION) {
                    schedulePollDuration();
                }
            }

            status_t err;
            if ((err = mSource->feedMoreTSData()) != OK) {
                if (mAudioDecoder == NULL && mVideoDecoder == NULL) {
                    // We're not currently decoding anything (no audio or
                    // video tracks found) and we just ran out of input data.

                    if (err == ERROR_END_OF_STREAM) {
                        notifyListener(MEDIA_PLAYBACK_COMPLETE, 0, 0);
                    } else {
                        notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
                    }
                }
                break;
            }

            if ((mAudioDecoder == NULL && mAudioSink != NULL)
                    || (mVideoDecoder == NULL && mNativeWindow != NULL)) {
                msg->post(100000ll);
                mScanSourcesPending = true;
            }
            break;
        }

        case kWhatVideoNotify:
        case kWhatAudioNotify:
        {
            bool audio = msg->what() == kWhatAudioNotify;

            int32_t currentDecoderGeneration =
                (audio? mAudioDecoderGeneration : mVideoDecoderGeneration);
            int32_t requesterGeneration = currentDecoderGeneration - 1;
            CHECK(msg->findInt32("generation", &requesterGeneration));

            if (requesterGeneration != currentDecoderGeneration) {
                ALOGV("got message from old %s decoder, generation(%d:%d)",
                        audio ? "audio" : "video", requesterGeneration,
                        currentDecoderGeneration);
                sp<AMessage> reply;
                if (!(msg->findMessage("reply", &reply))) {
                    return;
                }

                reply->setInt32("err", INFO_DISCONTINUITY);
                reply->post();
                return;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == DecoderBase::kWhatInputDiscontinuity) {
                int32_t formatChange;
                CHECK(msg->findInt32("formatChange", &formatChange));

                ALOGV("%s discontinuity: formatChange %d",
                        audio ? "audio" : "video", formatChange);

                if (formatChange) {
                    mDeferredActions.push_back(
                            new FlushDecoderAction(
                                audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                                audio ? FLUSH_CMD_NONE : FLUSH_CMD_SHUTDOWN));
                }

                mDeferredActions.push_back(
                        new SimpleAction(
                                &NuPlayer::performScanSources));

                processDeferredActions();
            } else if (what == DecoderBase::kWhatEOS) {
                int32_t err;
                CHECK(msg->findInt32("err", &err));

                if (err == ERROR_END_OF_STREAM) {
                    ALOGV("got %s decoder EOS", audio ? "audio" : "video");
                } else {
                    ALOGV("got %s decoder EOS w/ error %d",
                         audio ? "audio" : "video",
                         err);
                }

                mRenderer->queueEOS(audio, err);
            } else if (what == DecoderBase::kWhatFlushCompleted) {
                ALOGV("decoder %s flush completed", audio ? "audio" : "video");

                handleFlushComplete(audio, true /* isDecoder */);
                finishFlushIfPossible();
            } else if (what == DecoderBase::kWhatVideoSizeChanged) {
                sp<AMessage> format;
                CHECK(msg->findMessage("format", &format));

                sp<AMessage> inputFormat =
                        mSource->getFormat(false /* audio */);

                updateVideoSize(inputFormat, format);
            } else if (what == DecoderBase::kWhatShutdownCompleted) {
                ALOGV("%s shutdown completed", audio ? "audio" : "video");
                if (audio) {
                    mAudioDecoder.clear();
                    ++mAudioDecoderGeneration;

                    CHECK_EQ((int)mFlushingAudio, (int)SHUTTING_DOWN_DECODER);
                    mFlushingAudio = SHUT_DOWN;
                } else {
                    mVideoDecoder.clear();
                    ++mVideoDecoderGeneration;

                    CHECK_EQ((int)mFlushingVideo, (int)SHUTTING_DOWN_DECODER);
                    mFlushingVideo = SHUT_DOWN;
                }

                finishFlushIfPossible();
            } else if (what == DecoderBase::kWhatResumeCompleted) {
                finishResume();
            } else if (what == DecoderBase::kWhatError) {
                status_t err;
                if (!msg->findInt32("err", &err) || err == OK) {
                    err = UNKNOWN_ERROR;
                }

                // Decoder errors can be due to Source (e.g. from streaming),
                // or from decoding corrupted bitstreams, or from other decoder
                // MediaCodec operations (e.g. from an ongoing reset or seek).
                // They may also be due to openAudioSink failure at
                // decoder start or after a format change.
                //
                // We try to gracefully shut down the affected decoder if possible,
                // rather than trying to force the shutdown with something
                // similar to performReset(). This method can lead to a hang
                // if MediaCodec functions block after an error, but they should
                // typically return INVALID_OPERATION instead of blocking.

                FlushStatus *flushing = audio ? &mFlushingAudio : &mFlushingVideo;
                ALOGE("received error(%#x) from %s decoder, flushing(%d), now shutting down",
                        err, audio ? "audio" : "video", *flushing);

                switch (*flushing) {
                    case NONE:
                        mDeferredActions.push_back(
                                new FlushDecoderAction(
                                    audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                                    audio ? FLUSH_CMD_NONE : FLUSH_CMD_SHUTDOWN));
                        processDeferredActions();
                        break;
                    case FLUSHING_DECODER:
                        *flushing = FLUSHING_DECODER_SHUTDOWN; // initiate shutdown after flush.
                        break; // Wait for flush to complete.
                    case FLUSHING_DECODER_SHUTDOWN:
                        break; // Wait for flush to complete.
                    case SHUTTING_DOWN_DECODER:
                        break; // Wait for shutdown to complete.
                    case FLUSHED:
                        // Widevine source reads must stop before releasing the video decoder.
                        if (!audio && mSource != NULL && mSourceFlags & Source::FLAG_SECURE) {
                            mSource->stop();
                        }
                        getDecoder(audio)->initiateShutdown(); // In the middle of a seek.
                        *flushing = SHUTTING_DOWN_DECODER;     // Shut down.
                        break;
                    case SHUT_DOWN:
                        finishFlushIfPossible();  // Should not occur.
                        break;                    // Finish anyways.
                }
                notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
            } else {
                ALOGV("Unhandled decoder notification %d '%c%c%c%c'.",
                      what,
                      what >> 24,
                      (what >> 16) & 0xff,
                      (what >> 8) & 0xff,
                      what & 0xff);
            }

            break;
        }

        case kWhatRendererNotify:
        {
            int32_t requesterGeneration = mRendererGeneration - 1;
            CHECK(msg->findInt32("generation", &requesterGeneration));
            if (requesterGeneration != mRendererGeneration) {
                ALOGV("got message from old renderer, generation(%d:%d)",
                        requesterGeneration, mRendererGeneration);
                return;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == Renderer::kWhatEOS) {
                int32_t audio;
                CHECK(msg->findInt32("audio", &audio));

                int32_t finalResult;
                CHECK(msg->findInt32("finalResult", &finalResult));

                if (audio) {
                    mAudioEOS = true;
                } else {
                    mVideoEOS = true;
                    PLAYER_STATS(notifyEOS);
                }

                if (finalResult == ERROR_END_OF_STREAM) {
                    ALOGV("reached %s EOS", audio ? "audio" : "video");
                } else {
                    ALOGE("%s track encountered an error (%d)",
                         audio ? "audio" : "video", finalResult);

                    notifyListener(
                            MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, finalResult);
                }

                if ((mAudioEOS || mAudioDecoder == NULL)
                        && (mVideoEOS || mVideoDecoder == NULL)) {
                    notifyListener(MEDIA_PLAYBACK_COMPLETE, 0, 0);
                }
            } else if (what == Renderer::kWhatFlushComplete) {
                int32_t audio;
                CHECK(msg->findInt32("audio", &audio));

                ALOGV("renderer %s flush completed.", audio ? "audio" : "video");
                handleFlushComplete(audio, false /* isDecoder */);
                finishFlushIfPossible();
            } else if (what == Renderer::kWhatVideoRenderingStart) {
                PLAYER_STATS(profileStop, STATS_PROFILE_START_LATENCY);
                notifyListener(MEDIA_INFO, MEDIA_INFO_RENDERING_START, 0);
            } else if (what == Renderer::kWhatMediaRenderingStart) {
                PLAYER_STATS(profileStop, STATS_PROFILE_RESUME);
                ALOGV("media rendering started");
                notifyListener(MEDIA_STARTED, 0, 0);
            } else if (what == Renderer::kWhatAudioOffloadTearDown) {
                ALOGV("Tear down audio offload, fall back to s/w path if due to error.");
                int64_t positionUs;
                CHECK(msg->findInt64("positionUs", &positionUs));
                int32_t reason;
                CHECK(msg->findInt32("reason", &reason));
                closeAudioSink();
                if (!mOffloadDecodedPCM) {
                    mAudioDecoder.clear();
                    ++mAudioDecoderGeneration;
                } else {
                    ALOGV("Decoded PCM offload flushing mFLushingAudio %d", mFlushingAudio);
                    if (mAudioDecoder != NULL && mFlushingAudio == NONE) {
                        flushDecoder(true /* audio */, true/* needShutdown */);
                    }
                }
                mRenderer->flush(
                        true /* audio */, false /* notifyComplete */);
                if (mVideoDecoder != NULL) {
                    mRenderer->flush(
                            false /* audio */, false /* notifyComplete */);
                }

                performSeek(positionUs, false /* needNotify */);
                if (reason == Renderer::kDueToError) {
                    if (ExtendedUtils::is24bitPCMOffloadEnabled()) {
                        sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
                        if (ExtendedUtils::is24bitPCMOffloaded(audioMeta)) {
                              mSource->stop();
                              mSource->start();
                        }
                    }
                    mRenderer->signalDisableOffloadAudio();
                    mOffloadAudio = false;
                    mOffloadDecodedPCM = false;
                    instantiateDecoder(true /* audio */, &mAudioDecoder);
                } else {
                    mOffloadAudioTornDown = true;
                }
            }
            break;
        }

        case kWhatMoreDataQueued:
        {
            break;
        }

        case kWhatReset:
        {
            ALOGV("kWhatReset");

            mDeferredActions.push_back(
                    new FlushDecoderAction(
                        FLUSH_CMD_SHUTDOWN /* audio */,
                        FLUSH_CMD_SHUTDOWN /* video */));

            mDeferredActions.push_back(
                    new SimpleAction(&NuPlayer::performReset));

            processDeferredActions();
            break;
        }

        case kWhatSeek:
        {
            int64_t seekTimeUs;
            int32_t needNotify;
            CHECK(msg->findInt64("seekTimeUs", &seekTimeUs));
            CHECK(msg->findInt32("needNotify", &needNotify));

            ALOGV("kWhatSeek seekTimeUs=%lld us, needNotify=%d",
                    seekTimeUs, needNotify);

            mDeferredActions.push_back(
                    new FlushDecoderAction(FLUSH_CMD_FLUSH /* audio */,
                                           FLUSH_CMD_FLUSH /* video */));

            mDeferredActions.push_back(
                    new SeekAction(seekTimeUs, needNotify));

            // After a flush without shutdown, decoder is paused.
            // Don't resume it until source seek is done, otherwise it could
            // start pulling stale data too soon.
            mDeferredActions.push_back(
                    new ResumeDecoderAction(needNotify));

            processDeferredActions();
            break;
        }

        case kWhatPause:
        {
            onPause();
            mPausedByClient = true;
            break;
        }

        case kWhatSourceNotify:
        {
            onSourceNotify(msg);
            break;
        }

        case kWhatClosedCaptionNotify:
        {
            onClosedCaptionNotify(msg);
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::onResume() {
    if (!mPaused) {
        return;
    }
    mPaused = false;
    PLAYER_STATS(profileStart, STATS_PROFILE_RESUME);

    if (mSource != NULL) {
        mSource->resume();
    } else {
        ALOGW("resume called when source is gone or not set");
    }
    if (mOffloadAudioTornDown && mOffloadAudio) {
          // Resuming after a pause timed out event, check if can continue with offload
          sp<AMessage> videoFormat = mSource->getFormat(false /* audio */);
          sp<AMessage> format = mSource->getFormat(true /*audio*/);
          const bool hasVideo = (videoFormat != NULL);
          tryOpenAudioSinkForOffload(format, hasVideo);
    }
    // |mAudioDecoder| may have been released due to the pause timeout, so re-create it if
    // needed.
    if (audioDecoderStillNeeded() && mAudioDecoder == NULL
            && !ExtendedUtils::ShellProp::isAudioDisabled(false)) {
        instantiateDecoder(true /* audio */, &mAudioDecoder);
    }
    if (mRenderer != NULL) {
        mRenderer->resume();
    } else {
        ALOGW("resume called when renderer is gone or not set");
    }
    PLAYER_STATS(notifyPlaying, true);
}

status_t NuPlayer::onInstantiateSecureDecoders() {
    status_t err;
    if (!(mSourceFlags & Source::FLAG_SECURE)) {
        return BAD_TYPE;
    }

    if (mRenderer != NULL) {
        ALOGE("renderer should not be set when instantiating secure decoders");
        return UNKNOWN_ERROR;
    }

    // TRICKY: We rely on mRenderer being null, so that decoder does not start requesting
    // data on instantiation.
    if (mNativeWindow != NULL) {
        err = instantiateDecoder(false, &mVideoDecoder);
        if (err != OK) {
            return err;
        }
    }

    if (mAudioSink != NULL) {
        err = instantiateDecoder(true, &mAudioDecoder);
        if (err != OK) {
            return err;
        }
    }
    return OK;
}

void NuPlayer::onStart() {
    mOffloadAudio = false;
    mOffloadDecodedPCM = false;
    mAudioEOS = false;
    mVideoEOS = false;
    mStarted = true;

    mSource->start();

    uint32_t flags = 0;

    if (mSource->isRealTime()) {
        flags |= Renderer::FLAG_REAL_TIME;
    }

    sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
    audio_stream_type_t streamType = AUDIO_STREAM_MUSIC;
    if (mAudioSink != NULL) {
        streamType = mAudioSink->getAudioStreamType();
    }

    sp<AMessage> videoFormat = mSource->getFormat(false /* audio */);
    sp<MetaData> vMeta = new MetaData;
    convertMessageToMetaData(videoFormat, vMeta);
    mOffloadAudio = canOffloadStream(audioMeta, (videoFormat != NULL), vMeta,
                         mIsStreaming /* is_streaming */, streamType);
     //For offloading decoded content
     if (!mOffloadAudio && (audioMeta != NULL)) {
        sp<MetaData> audioPCMMeta =
                ExtendedUtils::createPCMMetaFromSource(audioMeta);

        const char *mime = NULL;
        if (audioMeta != NULL) {
            audioMeta->findCString(kKeyMIMEType, &mime);
        }
        mOffloadAudio =
                ((mime && !ExtendedUtils::pcmOffloadException(mime)) &&
                canOffloadStream(audioPCMMeta, (videoFormat != NULL), vMeta,
                        mIsStreaming /* is_streaming */, streamType));
        mOffloadDecodedPCM = mOffloadAudio;
        ALOGI("Could not offload audio decode, pcm offload decided :%d",
                mOffloadDecodedPCM);
    }
    if (mOffloadAudio) {
        flags |= Renderer::FLAG_OFFLOAD_AUDIO;
    }

    sp<AMessage> notify = new AMessage(kWhatRendererNotify, id());
    ++mRendererGeneration;
    notify->setInt32("generation", mRendererGeneration);
    if (mPlayerExtendedStats != NULL) {
        notify->setObject(MEDIA_EXTENDED_STATS, mPlayerExtendedStats);
    }
    mRenderer = new Renderer(mAudioSink, notify, flags);

    mRendererLooper = new ALooper;
    mRendererLooper->setName("NuPlayerRenderer");
    mRendererLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
    mRendererLooper->registerHandler(mRenderer);

    sp<MetaData> meta = getFileMeta();
    int32_t rate;
    if (meta != NULL
            && meta->findInt32(kKeyFrameRate, &rate) && rate > 0) {
        mRenderer->setVideoFrameRate(rate);
    }

    if (mVideoDecoder != NULL) {
        mVideoDecoder->setRenderer(mRenderer);
    }
    if (mAudioDecoder != NULL) {
        mAudioDecoder->setRenderer(mRenderer);
    }

    postScanSources();
}

void NuPlayer::onPause() {
    if (mPaused) {
        return;
    }
    mPaused = true;
    if (mSource != NULL) {
        mSource->pause();
    } else {
        ALOGW("pause called when source is gone or not set");
    }
    if (mRenderer != NULL) {
        mRenderer->pause();
    } else {
        ALOGW("pause called when renderer is gone or not set");
    }
    PLAYER_STATS(profileStop, STATS_PROFILE_PAUSE);
}

bool NuPlayer::audioDecoderStillNeeded() {
    // Audio decoder is no longer needed if it's in shut/shutting down status.
    return ((mFlushingAudio != SHUT_DOWN) && (mFlushingAudio != SHUTTING_DOWN_DECODER));
}

void NuPlayer::handleFlushComplete(bool audio, bool isDecoder) {
    // We wait for both the decoder flush and the renderer flush to complete
    // before entering either the FLUSHED or the SHUTTING_DOWN_DECODER state.

    mFlushComplete[audio][isDecoder] = true;
    if (!mFlushComplete[audio][!isDecoder]) {
        return;
    }

    FlushStatus *state = audio ? &mFlushingAudio : &mFlushingVideo;
    switch (*state) {
        case FLUSHING_DECODER:
        {
            *state = FLUSHED;
            break;
        }

        case FLUSHING_DECODER_SHUTDOWN:
        {
            *state = SHUTTING_DOWN_DECODER;

            ALOGV("initiating %s decoder shutdown", audio ? "audio" : "video");
            if (!audio) {
                // Widevine source reads must stop before releasing the video decoder.
                if (mSource != NULL && mSourceFlags & Source::FLAG_SECURE) {
                    mSource->stop();
                }
            }
            getDecoder(audio)->initiateShutdown();
            break;
        }

        default:
            // decoder flush completes only occur in a flushing state.
            LOG_ALWAYS_FATAL_IF(isDecoder, "decoder flush in invalid state %d", *state);
            break;
    }
}

void NuPlayer::finishFlushIfPossible() {
    if (mFlushingAudio != NONE && mFlushingAudio != FLUSHED
            && mFlushingAudio != SHUT_DOWN) {
        return;
    }

    if (mFlushingVideo != NONE && mFlushingVideo != FLUSHED
            && mFlushingVideo != SHUT_DOWN) {
        return;
    }

    ALOGV("both audio and video are flushed now.");

    mFlushingAudio = NONE;
    mFlushingVideo = NONE;

    clearFlushComplete();

    processDeferredActions();
}

void NuPlayer::postScanSources() {
    if (mScanSourcesPending) {
        return;
    }

    sp<AMessage> msg = new AMessage(kWhatScanSources, id());
    msg->setInt32("generation", mScanSourcesGeneration);
    msg->post();

    mScanSourcesPending = true;
}

void NuPlayer::tryOpenAudioSinkForOffload(const sp<AMessage> &format, bool hasVideo) {
    // Note: This is called early in NuPlayer to determine whether offloading
    // is possible; otherwise the decoders call the renderer openAudioSink directly.

    //update bit width before opening audio sink
    if (ExtendedUtils::is24bitPCMOffloadEnabled()) {
        sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
        if (ExtendedUtils::is24bitPCMOffloaded(audioMeta)) {
            ALOGV("overriding format with 24 bits");
            format->setInt32("bits-per-sample", 24);
        }
    }

    status_t err = mRenderer->openAudioSink(
            format, true /* offloadOnly */, hasVideo, AUDIO_OUTPUT_FLAG_NONE, mIsStreaming, &mOffloadAudio);
    if (err != OK) {
        // Any failure we turn off mOffloadAudio.
        mOffloadAudio = false;
        mOffloadAudioTornDown = false;
        mOffloadDecodedPCM = false;
    } else if (mOffloadAudio) {
        sp<MetaData> audioMeta =
                mSource->getFormatMeta(true /* audio */);
        sendMetaDataToHal(mAudioSink, audioMeta);
    }
}

void NuPlayer::closeAudioSink() {
    mRenderer->closeAudioSink();
}

int64_t NuPlayer::getServerTimeoutUs() {
    return mSource->getServerTimeoutUs();
}

status_t NuPlayer::instantiateDecoder(bool audio, sp<DecoderBase> *decoder) {
    if (*decoder != NULL) {
        return OK;
    }

    if (audio && ExtendedUtils::ShellProp::isAudioDisabled(false)) {
        return OK;
    }

    sp<AMessage> format = mSource->getFormat(audio);

    if (format == NULL) {
        return -EWOULDBLOCK;
    }

    if (!audio) {
        AString mime;
        CHECK(format->findString("mime", &mime));

        sp<AMessage> ccNotify = new AMessage(kWhatClosedCaptionNotify, id());
        if (mCCDecoder == NULL) {
            mCCDecoder = new CCDecoder(ccNotify);
        }

        if (mSourceFlags & Source::FLAG_SECURE) {
            format->setInt32("secure", true);
        }

        if (mSourceFlags & Source::FLAG_PROTECTED) {
            format->setInt32("protected", true);
        }
    }

    if (audio) {
        sp<AMessage> notify = new AMessage(kWhatAudioNotify, id());
        ++mAudioDecoderGeneration;
        notify->setInt32("generation", mAudioDecoderGeneration);

        sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
        const bool hasVideo = (mSource->getFormat(false /*audio */) != NULL);

        format->setInt32("has-video", hasVideo);

        if (mOffloadAudio && !mOffloadDecodedPCM) {
            if (ExtendedUtils::is24bitPCMOffloadEnabled()) {
                sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
                if (ExtendedUtils::is24bitPCMOffloaded(audioMeta)) {
                    mSource->stop();
                    mSource->start();
                }
            }
            *decoder = new DecoderPassThrough(notify, mSource, mRenderer);
        } else {
            if (ExtendedUtils::is24bitPCMOffloadEnabled()) {
                sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
                if (ExtendedUtils::is24bitPCMOffloaded(audioMeta)) {
                    mSource->stop();
                    mSource->start();
                }
            }
            *decoder = new Decoder(notify, mSource, mRenderer);
        }
    } else {
        sp<AMessage> notify = new AMessage(kWhatVideoNotify, id());
        ++mVideoDecoderGeneration;
        notify->setInt32("generation", mVideoDecoderGeneration);

        *decoder = new Decoder(
                notify, mSource, mRenderer, mNativeWindow, mCCDecoder);

        // enable FRC if high-quality AV sync is requested, even if not
        // queuing to native window, as this will even improve textureview
        // playback.
        {
            char value[PROPERTY_VALUE_MAX];
            if (property_get("persist.sys.media.avsync", value, NULL) &&
                    (!strcmp("1", value) || !strcasecmp("true", value))) {
                format->setInt32("auto-frc", 1);
            }
        }
    }

    if (mPlayerExtendedStats != NULL) {
        format->setObject(MEDIA_EXTENDED_STATS, mPlayerExtendedStats);
    }

    (*decoder)->init();
    (*decoder)->configure(format);

    // allocate buffers to decrypt widevine source buffers
    if (!audio && (mSourceFlags & Source::FLAG_SECURE)) {
        Vector<sp<ABuffer> > inputBufs;
        CHECK_EQ((*decoder)->getInputBuffers(&inputBufs), (status_t)OK);

        Vector<MediaBuffer *> mediaBufs;
        for (size_t i = 0; i < inputBufs.size(); i++) {
            const sp<ABuffer> &buffer = inputBufs[i];
            MediaBuffer *mbuf = new MediaBuffer(buffer->data(), buffer->size());
            mediaBufs.push(mbuf);
        }

        status_t err = mSource->setBuffers(audio, mediaBufs);
        if (err != OK) {
            for (size_t i = 0; i < mediaBufs.size(); ++i) {
                mediaBufs[i]->release();
            }
            mediaBufs.clear();
            ALOGE("Secure source didn't support secure mediaBufs.");
            return err;
        }
    }
    return OK;
}

#if 0
status_t NuPlayer::feedDecoderInputData(bool audio, const sp<AMessage> &msg) {
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    if ((audio && mFlushingAudio != NONE)
            || (!audio && mFlushingVideo != NONE)
            || mSource == NULL) {
        reply->setInt32("err", INFO_DISCONTINUITY);
        reply->post();
        return OK;
    }

    sp<ABuffer> accessUnit;

    // Aggregate smaller buffers into a larger buffer.
    // The goal is to reduce power consumption.
    // Note this will not work if the decoder requires one frame per buffer.
    bool doBufferAggregation = ((audio && mOffloadAudio) && !mOffloadDecodedPCM);
    bool needMoreData = false;

    bool dropAccessUnit;
    do {
        status_t err;
        // Did we save an accessUnit earlier because of a discontinuity?
        if (audio && (mPendingAudioAccessUnit != NULL)) {
            accessUnit = mPendingAudioAccessUnit;
            mPendingAudioAccessUnit.clear();
            err = mPendingAudioErr;
            ALOGV("feedDecoderInputData() use mPendingAudioAccessUnit");
        } else {
            err = mSource->dequeueAccessUnit(audio, &accessUnit);
        }

        if (err == -EWOULDBLOCK) {
            return err;
        } else if (err != OK) {
            if (err == INFO_DISCONTINUITY) {
                if (doBufferAggregation && (mAggregateBuffer != NULL)) {
                    // We already have some data so save this for later.
                    mPendingAudioErr = err;
                    mPendingAudioAccessUnit = accessUnit;
                    accessUnit.clear();
                    ALOGD("feedDecoderInputData() save discontinuity for later");
                    break;
                }
                int32_t type;
                CHECK(accessUnit->meta()->findInt32("discontinuity", &type));

                bool formatChange =
                    (audio &&
                     (type & ATSParser::DISCONTINUITY_AUDIO_FORMAT))
                    || (!audio &&
                            (type & ATSParser::DISCONTINUITY_VIDEO_FORMAT));

                bool timeChange = (type & ATSParser::DISCONTINUITY_TIME) != 0;

                ALOGI("%s discontinuity (formatChange=%d, time=%d)",
                     audio ? "audio" : "video", formatChange, timeChange);

                if (audio) {
                    mSkipRenderingAudioUntilMediaTimeUs = -1;
                } else {
                    mSkipRenderingVideoUntilMediaTimeUs = -1;
                }

                if (timeChange) {
                    sp<AMessage> extra;
                    if (accessUnit->meta()->findMessage("extra", &extra)
                            && extra != NULL) {
                        int64_t resumeAtMediaTimeUs;
                        if (extra->findInt64(
                                    "resume-at-mediatimeUs", &resumeAtMediaTimeUs)) {
                            ALOGI("suppressing rendering of %s until %lld us",
                                    audio ? "audio" : "video", resumeAtMediaTimeUs);

                            if (audio) {
                                mSkipRenderingAudioUntilMediaTimeUs =
                                    resumeAtMediaTimeUs;
                            } else {
                                mSkipRenderingVideoUntilMediaTimeUs =
                                    resumeAtMediaTimeUs;
                            }
                        }
                    }
                }

                mTimeDiscontinuityPending =
                    mTimeDiscontinuityPending || timeChange;

                bool seamlessFormatChange = false;
                sp<AMessage> newFormat = mSource->getFormat(audio);
                if (formatChange) {
                    seamlessFormatChange =
                        getDecoder(audio)->supportsSeamlessFormatChange(newFormat);
                    // treat seamless format change separately
                    formatChange = !seamlessFormatChange;
                }
                bool shutdownOrFlush = formatChange || timeChange;

                // We want to queue up scan-sources only once per discontinuity.
                // We control this by doing it only if neither audio nor video are
                // flushing or shutting down.  (After handling 1st discontinuity, one
                // of the flushing states will not be NONE.)
                // No need to scan sources if this discontinuity does not result
                // in a flush or shutdown, as the flushing state will stay NONE.
                if (mFlushingAudio == NONE && mFlushingVideo == NONE &&
                        shutdownOrFlush) {
                    // And we'll resume scanning sources once we're done
                    // flushing.
                    mDeferredActions.push_front(
                            new SimpleAction(
                                &NuPlayer::performScanSources));
                }

                if (formatChange /* not seamless */) {
                    // must change decoder
                    flushDecoder(audio, /* needShutdown = */ true);
                } else if (timeChange) {
                    // need to flush
                    flushDecoder(audio, /* needShutdown = */ false, newFormat);
                    err = OK;
                } else if (seamlessFormatChange) {
                    // reuse existing decoder and don't flush
                    updateDecoderFormatWithoutFlush(audio, newFormat);
                    err = OK;
                } else {
                    // This stream is unaffected by the discontinuity
                    return -EWOULDBLOCK;
                }
            }

            reply->setInt32("err", err);
            reply->post();
            return OK;
        }

        if (!audio) {
            ++mNumFramesTotal;
        }

        dropAccessUnit = false;
        if (!audio
                && !(mSourceFlags & Source::FLAG_SECURE)
                && mRenderer->getVideoLateByUs() > 100000ll
                && mVideoIsAVC
                && !IsAVCReferenceFrame(accessUnit)) {
            dropAccessUnit = true;
            ++mNumFramesDropped;
        }

        size_t smallSize = accessUnit->size();
        needMoreData = false;
        if (doBufferAggregation && (mAggregateBuffer == NULL)
                // Don't bother if only room for a few small buffers.
                && (smallSize < (kAggregateBufferSizeBytes / 3))) {
            // Create a larger buffer for combining smaller buffers from the extractor.
            mAggregateBuffer = new ABuffer(kAggregateBufferSizeBytes);
            mAggregateBuffer->setRange(0, 0); // start empty
        }

        if (doBufferAggregation && (mAggregateBuffer != NULL)) {
            int64_t timeUs;
            int64_t dummy;
            bool smallTimestampValid = accessUnit->meta()->findInt64("timeUs", &timeUs);
            bool bigTimestampValid = mAggregateBuffer->meta()->findInt64("timeUs", &dummy);
            // Will the smaller buffer fit?
            size_t bigSize = mAggregateBuffer->size();
            size_t roomLeft = mAggregateBuffer->capacity() - bigSize;
            // Should we save this small buffer for the next big buffer?
            // If the first small buffer did not have a timestamp then save
            // any buffer that does have a timestamp until the next big buffer.
            if ((smallSize > roomLeft)
                || (!bigTimestampValid && (bigSize > 0) && smallTimestampValid)) {
                mPendingAudioErr = err;
                mPendingAudioAccessUnit = accessUnit;
                accessUnit.clear();
            } else {
                // Grab time from first small buffer if available.
                if ((bigSize == 0) && smallTimestampValid) {
                    mAggregateBuffer->meta()->setInt64("timeUs", timeUs);
                }
                // Append small buffer to the bigger buffer.
                memcpy(mAggregateBuffer->base() + bigSize, accessUnit->data(), smallSize);
                bigSize += smallSize;
                mAggregateBuffer->setRange(0, bigSize);

                // Keep looping until we run out of room in the mAggregateBuffer.
                needMoreData = true;

                ALOGV("feedDecoderInputData() smallSize = %zu, bigSize = %zu, capacity = %zu",
                        smallSize, bigSize, mAggregateBuffer->capacity());
            }
        }
    } while (dropAccessUnit || needMoreData);

    // ALOGV("returned a valid buffer of %s data", audio ? "audio" : "video");

#if 0
    int64_t mediaTimeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &mediaTimeUs));
    ALOGV("feeding %s input buffer at media time %.2f secs",
         audio ? "audio" : "video",
         mediaTimeUs / 1E6);
#endif

    if (!audio) {
        mCCDecoder->decode(accessUnit);
    }

    if (doBufferAggregation && (mAggregateBuffer != NULL)) {
        ALOGV("feedDecoderInputData() reply with aggregated buffer, %zu",
                mAggregateBuffer->size());
        reply->setBuffer("buffer", mAggregateBuffer);
        mAggregateBuffer.clear();
    } else {
        reply->setBuffer("buffer", accessUnit);
    }

    reply->post();

    return OK;
}

void NuPlayer::renderBuffer(bool audio, const sp<AMessage> &msg) {
    // ALOGV("renderBuffer %s", audio ? "audio" : "video");

    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    if ((audio && mFlushingAudio != NONE)
            || (!audio && mFlushingVideo != NONE)) {
        // We're currently attempting to flush the decoder, in order
        // to complete this, the decoder wants all its buffers back,
        // so we don't want any output buffers it sent us (from before
        // we initiated the flush) to be stuck in the renderer's queue.

        ALOGV("we're still flushing the %s decoder, sending its output buffer"
             " right back.", audio ? "audio" : "video");

        reply->post();
        return;
    }

    sp<ABuffer> buffer;
    CHECK(msg->findBuffer("buffer", &buffer));

    int64_t mediaTimeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &mediaTimeUs));

    int64_t &skipUntilMediaTimeUs =
        audio
            ? mSkipRenderingAudioUntilMediaTimeUs
            : mSkipRenderingVideoUntilMediaTimeUs;

    if (skipUntilMediaTimeUs >= 0) {

        if (mediaTimeUs < skipUntilMediaTimeUs) {
            ALOGV("dropping %s buffer at time %lld as requested.",
                 audio ? "audio" : "video",
                 mediaTimeUs);

            reply->post();
            return;
        }

        skipUntilMediaTimeUs = -1;
    }

    if (!audio && mCCDecoder->isSelected()) {
        mCCDecoder->display(mediaTimeUs);
    }

    if (!audio && mSeeking) {
        buffer->meta()->setInt32("seeking", mSeeking);
        mSeeking = false;
    }

    mRenderer->queueBuffer(audio, buffer, reply);
}
#endif

void NuPlayer::updateVideoSize(
        const sp<AMessage> &inputFormat,
        const sp<AMessage> &outputFormat) {
    if (inputFormat == NULL) {
        ALOGW("Unknown video size, reporting 0x0!");
        notifyListener(MEDIA_SET_VIDEO_SIZE, 0, 0);
        return;
    }

    int32_t displayWidth, displayHeight;
    int32_t cropLeft, cropTop, cropRight, cropBottom;

    if (outputFormat != NULL) {
        int32_t width, height;
        CHECK(outputFormat->findInt32("width", &width));
        CHECK(outputFormat->findInt32("height", &height));
        PLAYER_STATS(logDimensions, width, height);

        int32_t cropLeft, cropTop, cropRight, cropBottom;
        CHECK(outputFormat->findRect(
                    "crop",
                    &cropLeft, &cropTop, &cropRight, &cropBottom));

        displayWidth = cropRight - cropLeft + 1;
        displayHeight = cropBottom - cropTop + 1;

        ALOGV("Video output format changed to %d x %d "
             "(crop: %d x %d @ (%d, %d))",
             width, height,
             displayWidth,
             displayHeight,
             cropLeft, cropTop);
    } else {
        CHECK(inputFormat->findInt32("width", &displayWidth));
        CHECK(inputFormat->findInt32("height", &displayHeight));

        ALOGV("Video input format %d x %d", displayWidth, displayHeight);
    }

    // Take into account sample aspect ratio if necessary:
    int32_t sarWidth, sarHeight;
    if (inputFormat != NULL && inputFormat->findInt32("sar-width", &sarWidth)
            && inputFormat->findInt32("sar-height", &sarHeight)) {
        ALOGV("Sample aspect ratio %d : %d", sarWidth, sarHeight);

        displayWidth = (displayWidth * sarWidth) / sarHeight;

        ALOGV("display dimensions %d x %d", displayWidth, displayHeight);
    }

    int32_t rotationDegrees;
    if (!inputFormat->findInt32("rotation-degrees", &rotationDegrees)) {
        rotationDegrees = 0;
    }

    if (rotationDegrees == 90 || rotationDegrees == 270) {
        int32_t tmp = displayWidth;
        displayWidth = displayHeight;
        displayHeight = tmp;
    }

    notifyListener(
            MEDIA_SET_VIDEO_SIZE,
            displayWidth,
            displayHeight);
}

void NuPlayer::notifyListener(int msg, int ext1, int ext2, const Parcel *in) {
    if (mDriver == NULL) {
        return;
    }

    sp<NuPlayerDriver> driver = mDriver.promote();

    if (driver == NULL) {
        return;
    }

    driver->notifyListener(msg, ext1, ext2, in);
}

void NuPlayer::flushDecoder(bool audio, bool needShutdown) {
    ALOGV("[%s] flushDecoder needShutdown=%d",
          audio ? "audio" : "video", needShutdown);

    const sp<DecoderBase> &decoder = getDecoder(audio);
    if (decoder == NULL) {
        ALOGI("flushDecoder %s without decoder present",
             audio ? "audio" : "video");
        return;
    }

    // Make sure we don't continue to scan sources until we finish flushing.
    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    decoder->signalFlush();

    FlushStatus newStatus =
        needShutdown ? FLUSHING_DECODER_SHUTDOWN : FLUSHING_DECODER;

    mFlushComplete[audio][false /* isDecoder */] = (mRenderer == NULL);
    mFlushComplete[audio][true /* isDecoder */] = false;
    if (audio) {
        ALOGE_IF(mFlushingAudio != NONE,
                "audio flushDecoder() is called in state %d", mFlushingAudio);
        mFlushingAudio = newStatus;
    } else {
        ALOGE_IF(mFlushingVideo != NONE,
                "video flushDecoder() is called in state %d", mFlushingVideo);
        mFlushingVideo = newStatus;
    }
}

void NuPlayer::queueDecoderShutdown(
        bool audio, bool video, const sp<AMessage> &reply) {
    ALOGI("queueDecoderShutdown audio=%d, video=%d", audio, video);

    mDeferredActions.push_back(
            new FlushDecoderAction(
                audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                video ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE));

    mDeferredActions.push_back(
            new SimpleAction(&NuPlayer::performScanSources));

    mDeferredActions.push_back(new PostMessageAction(reply));

    processDeferredActions();
}

int64_t NuPlayer::Source::getServerTimeoutUs() {
    return 0;
}

status_t NuPlayer::setVideoScalingMode(int32_t mode) {
    mVideoScalingMode = mode;
    if (mNativeWindow != NULL) {
        status_t ret = native_window_set_scaling_mode(
                mNativeWindow->getNativeWindow().get(), mVideoScalingMode);
        if (ret != OK) {
            ALOGE("Failed to set scaling mode (%d): %s",
                -ret, strerror(-ret));
            return ret;
        }
    }
    return OK;
}

status_t NuPlayer::getTrackInfo(Parcel* reply) const {
    sp<AMessage> msg = new AMessage(kWhatGetTrackInfo, id());
    msg->setPointer("reply", reply);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    return err;
}

status_t NuPlayer::getSelectedTrack(int32_t type, Parcel* reply) const {
    sp<AMessage> msg = new AMessage(kWhatGetSelectedTrack, id());
    msg->setPointer("reply", reply);
    msg->setInt32("type", type);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("err", &err));
    }
    return err;
}

status_t NuPlayer::selectTrack(size_t trackIndex, bool select, int64_t timeUs) {
    sp<AMessage> msg = new AMessage(kWhatSelectTrack, id());
    msg->setSize("trackIndex", trackIndex);
    msg->setInt32("select", select);
    msg->setInt64("timeUs", timeUs);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

status_t NuPlayer::getCurrentPosition(int64_t *mediaUs) {
    sp<Renderer> renderer = mRenderer;
    if (renderer == NULL) {
        return NO_INIT;
    }

    return renderer->getCurrentPosition(mediaUs);
}

void NuPlayer::getStats(int64_t *numFramesTotal, int64_t *numFramesDropped) {
    sp<DecoderBase> decoder = getDecoder(false /* audio */);
    if (decoder != NULL) {
        decoder->getStats(numFramesTotal, numFramesDropped);
    } else {
        *numFramesTotal = 0;
        *numFramesDropped = 0;
    }
}

sp<MetaData> NuPlayer::getFileMeta() {
    return mSource->getFileFormatMeta();
}

void NuPlayer::schedulePollDuration() {
    sp<AMessage> msg = new AMessage(kWhatPollDuration, id());
    msg->setInt32("generation", mPollDurationGeneration);
    msg->post();
}

void NuPlayer::cancelPollDuration() {
    ++mPollDurationGeneration;
}

void NuPlayer::processDeferredActions() {
    while (!mDeferredActions.empty()) {
        // We won't execute any deferred actions until we're no longer in
        // an intermediate state, i.e. one more more decoders are currently
        // flushing or shutting down.

        if (mFlushingAudio != NONE || mFlushingVideo != NONE) {
            // We're currently flushing, postpone the reset until that's
            // completed.

            ALOGV("postponing action mFlushingAudio=%d, mFlushingVideo=%d",
                  mFlushingAudio, mFlushingVideo);

            break;
        }

        sp<Action> action = *mDeferredActions.begin();
        mDeferredActions.erase(mDeferredActions.begin());

        action->execute(this);
    }
}

void NuPlayer::performSeek(int64_t seekTimeUs, bool needNotify) {
    ALOGV("performSeek seekTimeUs=%lld us (%.2f secs), needNotify(%d)",
          seekTimeUs,
          seekTimeUs / 1E6,
          needNotify);

    mSeeking = true;
    if (mSource == NULL) {
        // This happens when reset occurs right before the loop mode
        // asynchronously seeks to the start of the stream.
        LOG_ALWAYS_FATAL_IF(mAudioDecoder != NULL || mVideoDecoder != NULL,
                "mSource is NULL and decoders not NULL audio(%p) video(%p)",
                mAudioDecoder.get(), mVideoDecoder.get());
        return;
    }
    mSource->seekTo(seekTimeUs);
    ++mTimedTextGeneration;

    // everything's flushed, continue playback.

    PLAYER_STATS(notifySeekDone);
    // everything's flushed, continue playback.
}

#if 0
void NuPlayer::performDecoderFlush() {
    ALOGV("performDecoderFlush");

    if (mAudioDecoder == NULL && mVideoDecoder == NULL) {
        return;
    }

    mTimeDiscontinuityPending = true;

    if (mAudioDecoder != NULL) {
        flushDecoder(true /* audio */, false /* needShutdown */);
    }

    if (mVideoDecoder != NULL) {
        flushDecoder(false /* audio */, false /* needShutdown */);
    }
}
#endif

void NuPlayer::performDecoderFlush(FlushCommand audio, FlushCommand video) {
    ALOGV("performDecoderFlush audio=%d, video=%d", audio, video);

    if ((audio == FLUSH_CMD_NONE || mAudioDecoder == NULL)
            && (video == FLUSH_CMD_NONE || mVideoDecoder == NULL)) {
        return;
    }

    if (audio != FLUSH_CMD_NONE && mAudioDecoder != NULL) {
        flushDecoder(true /* audio */, (audio == FLUSH_CMD_SHUTDOWN));
    }

    if (video != FLUSH_CMD_NONE && mVideoDecoder != NULL) {
        flushDecoder(false /* audio */, (video == FLUSH_CMD_SHUTDOWN));
    }
}

void NuPlayer::performReset() {
    ALOGV("performReset");

    CHECK(mAudioDecoder == NULL);
    CHECK(mVideoDecoder == NULL);

    cancelPollDuration();

    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    if (mRendererLooper != NULL) {
        if (mRenderer != NULL) {
            mRendererLooper->unregisterHandler(mRenderer->id());
        }
        mRendererLooper->stop();
        mRendererLooper.clear();
    }
    mRenderer.clear();
    ++mRendererGeneration;

    if (mSource != NULL) {
        mSource->stop();

        mSource.clear();
    }

    if (mDriver != NULL) {
        sp<NuPlayerDriver> driver = mDriver.promote();
        if (driver != NULL) {
            driver->notifyResetComplete();
        }
    }

    mStarted = false;
    PLAYER_STATS(notifyEOS);
    PLAYER_STATS(dump);
    PLAYER_STATS(reset);
}

void NuPlayer::performScanSources() {
    ALOGV("performScanSources");

    if (!mStarted) {
        return;
    }

    if (mAudioDecoder == NULL || mVideoDecoder == NULL) {
        postScanSources();
    }
}

void NuPlayer::performSetSurface(const sp<NativeWindowWrapper> &wrapper) {
    ALOGV("performSetSurface");

    mNativeWindow = wrapper;

    // XXX - ignore error from setVideoScalingMode for now
    setVideoScalingMode(mVideoScalingMode);

    if (mDriver != NULL) {
        sp<NuPlayerDriver> driver = mDriver.promote();
        if (driver != NULL) {
            driver->notifySetSurfaceComplete();
        }
    }
}

void NuPlayer::performResumeDecoders(bool needNotify) {
    if (needNotify) {
        mResumePending = true;
        if (mVideoDecoder == NULL) {
            // if audio-only, we can notify seek complete now,
            // as the resume operation will be relatively fast.
            finishResume();
        }
    }

    if (mVideoDecoder != NULL) {
        // When there is continuous seek, MediaPlayer will cache the seek
        // position, and send down new seek request when previous seek is
        // complete. Let's wait for at least one video output frame before
        // notifying seek complete, so that the video thumbnail gets updated
        // when seekbar is dragged.
        mVideoDecoder->signalResume(needNotify);
    }

    if (mAudioDecoder != NULL) {
        mAudioDecoder->signalResume(false /* needNotify */);
    }
}

void NuPlayer::finishResume() {
    if (mResumePending) {
        mResumePending = false;
        if (mDriver != NULL) {
            sp<NuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                driver->notifySeekComplete();
            }
        }
    }
}

void NuPlayer::onSourceNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case Source::kWhatInstantiateSecureDecoders:
        {
            if (mSource == NULL) {
                // This is a stale notification from a source that was
                // asynchronously preparing when the client called reset().
                // We handled the reset, the source is gone.
                break;
            }

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));
            status_t err = onInstantiateSecureDecoders();
            reply->setInt32("err", err);
            reply->post();
            break;
        }

        case Source::kWhatPrepared:
        {
            if (mSource == NULL) {
                // This is a stale notification from a source that was
                // asynchronously preparing when the client called reset().
                // We handled the reset, the source is gone.
                break;
            }

            int32_t err;
            CHECK(msg->findInt32("err", &err));

            if (err != OK) {
                // shut down potential secure codecs in case client never calls reset
                mDeferredActions.push_back(
                        new FlushDecoderAction(FLUSH_CMD_SHUTDOWN /* audio */,
                                               FLUSH_CMD_SHUTDOWN /* video */));
                processDeferredActions();
            }

            sp<NuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                // notify duration first, so that it's definitely set when
                // the app received the "prepare complete" callback.
                int64_t durationUs;
                if (mSource->getDuration(&durationUs) == OK) {
                    driver->notifyDuration(durationUs);
                }
                PLAYER_STATS(profileStop, STATS_PROFILE_PREPARE);
                driver->notifyPrepareCompleted(err);
            }

            break;
        }

        case Source::kWhatFlagsChanged:
        {
            uint32_t flags;
            CHECK(msg->findInt32("flags", (int32_t *)&flags));

            sp<NuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                if ((flags & NuPlayer::Source::FLAG_CAN_SEEK) == 0) {
                    driver->notifyListener(
                            MEDIA_INFO, MEDIA_INFO_NOT_SEEKABLE, 0);
                }
                driver->notifyFlagsChanged(flags);
            }

            if ((mSourceFlags & Source::FLAG_DYNAMIC_DURATION)
                    && (!(flags & Source::FLAG_DYNAMIC_DURATION))) {
                cancelPollDuration();
            } else if (!(mSourceFlags & Source::FLAG_DYNAMIC_DURATION)
                    && (flags & Source::FLAG_DYNAMIC_DURATION)
                    && (mAudioDecoder != NULL || mVideoDecoder != NULL)) {
                schedulePollDuration();
            }

            mSourceFlags = flags;
            break;
        }

        case Source::kWhatVideoSizeChanged:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));

            updateVideoSize(format);
            break;
        }

        case Source::kWhatBufferingUpdate:
        {
            int32_t percentage;
            CHECK(msg->findInt32("percentage", &percentage));

            notifyListener(MEDIA_BUFFERING_UPDATE, percentage, 0);
            break;
        }

        case Source::kWhatPauseOnBufferingStart:
        {
            // ignore if not playing
            if (mStarted && !mPausedByClient) {
                ALOGI("buffer low, pausing...");

                PLAYER_STATS(profileStart, STATS_PROFILE_PAUSE);
                onPause();
                PLAYER_STATS(profileStop, STATS_PROFILE_PAUSE);
            }
            // fall-thru
        }

        case Source::kWhatBufferingStart:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_BUFFERING_START, 0);
            break;
        }

        case Source::kWhatResumeOnBufferingEnd:
        {
            // ignore if not playing
            if (mStarted && !mPausedByClient) {
                ALOGI("buffer ready, resuming...");
                PLAYER_STATS(notifyPlaying, true);
                PLAYER_STATS(profileStart, STATS_PROFILE_RESUME);
                onResume();
            }
            // fall-thru
        }

        case Source::kWhatBufferingEnd:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_BUFFERING_END, 0);
            break;
        }

        case Source::kWhatCacheStats:
        {
            int32_t kbps;
            CHECK(msg->findInt32("bandwidth", &kbps));

            notifyListener(MEDIA_INFO, MEDIA_INFO_NETWORK_BANDWIDTH, kbps);
            break;
        }

        case Source::kWhatSubtitleData:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            sendSubtitleData(buffer, 0 /* baseIndex */);
            break;
        }

        case Source::kWhatTimedTextData:
        {
            int32_t generation;
            if (msg->findInt32("generation", &generation)
                    && generation != mTimedTextGeneration) {
                break;
            }

            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            sp<NuPlayerDriver> driver = mDriver.promote();
            if (driver == NULL) {
                break;
            }

            int posMs;
            int64_t timeUs, posUs;
            driver->getCurrentPosition(&posMs);
            posUs = posMs * 1000;
            CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

            if (posUs < timeUs) {
                if (!msg->findInt32("generation", &generation)) {
                    msg->setInt32("generation", mTimedTextGeneration);
                }
                msg->post(timeUs - posUs);
            } else {
                sendTimedTextData(buffer);
            }
            break;
        }

        case Source::kWhatQueueDecoderShutdown:
        {
            int32_t audio, video;
            CHECK(msg->findInt32("audio", &audio));
            CHECK(msg->findInt32("video", &video));

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));

            queueDecoderShutdown(audio, video, reply);
            break;
        }

        case Source::kWhatDrmNoLicense:
        {
            notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, ERROR_DRM_NO_LICENSE);
            break;
        }

        default:
            TRESPASS();
    }
}

void NuPlayer::onClosedCaptionNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case NuPlayer::CCDecoder::kWhatClosedCaptionData:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }

            sendSubtitleData(buffer, inbandTracks);
            break;
        }

        case NuPlayer::CCDecoder::kWhatTrackAdded:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_METADATA_UPDATE, 0);

            break;
        }

        default:
            TRESPASS();
    }


}

void NuPlayer::sendSubtitleData(const sp<ABuffer> &buffer, int32_t baseIndex) {
    int32_t trackIndex;
    int64_t timeUs, durationUs;
    CHECK(buffer->meta()->findInt32("trackIndex", &trackIndex));
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
    CHECK(buffer->meta()->findInt64("durationUs", &durationUs));

    Parcel in;
    in.writeInt32(trackIndex + baseIndex);
    in.writeInt64(timeUs);
    in.writeInt64(durationUs);
    in.writeInt32(buffer->size());
    in.writeInt32(buffer->size());
    in.write(buffer->data(), buffer->size());

    notifyListener(MEDIA_SUBTITLE_DATA, 0, 0, &in);
}

void NuPlayer::sendTimedTextData(const sp<ABuffer> &buffer) {
    const void *data;
    size_t size = 0;
    int64_t timeUs;
    int32_t flag = TextDescriptions::LOCAL_DESCRIPTIONS;

    AString mime;
    CHECK(buffer->meta()->findString("mime", &mime));
    CHECK(strcasecmp(mime.c_str(), MEDIA_MIMETYPE_TEXT_3GPP) == 0);

    data = buffer->data();
    size = buffer->size();

    Parcel parcel;
    if (size > 0) {
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
        flag |= TextDescriptions::IN_BAND_TEXT_3GPP;
        TextDescriptions::getParcelOfDescriptions(
                (const uint8_t *)data, size, flag, timeUs / 1000, &parcel);
    }

    if ((parcel.dataSize() > 0)) {
        notifyListener(MEDIA_TIMED_TEXT, 0, 0, &parcel);
    } else {  // send an empty timed text
        notifyListener(MEDIA_TIMED_TEXT, 0, 0);
    }
}
////////////////////////////////////////////////////////////////////////////////

sp<AMessage> NuPlayer::Source::getFormat(bool audio) {
    sp<MetaData> meta = getFormatMeta(audio);

    if (meta == NULL) {
        return NULL;
    }

    sp<AMessage> msg = new AMessage;

    if(convertMetaDataToMessage(meta, &msg) == OK) {
        return msg;
    }
    return NULL;
}

void NuPlayer::Source::notifyFlagsChanged(uint32_t flags) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatFlagsChanged);
    notify->setInt32("flags", flags);
    notify->post();
}

void NuPlayer::Source::notifyVideoSizeChanged(const sp<AMessage> &format) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatVideoSizeChanged);
    notify->setMessage("format", format);
    notify->post();
}

void NuPlayer::Source::notifyPrepared(status_t err) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatPrepared);
    notify->setInt32("err", err);
    notify->post();
}

void NuPlayer::Source::notifyInstantiateSecureDecoders(const sp<AMessage> &reply) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatInstantiateSecureDecoders);
    notify->setMessage("reply", reply);
    notify->post();
}

void NuPlayer::Source::onMessageReceived(const sp<AMessage> & /* msg */) {
    TRESPASS();
}

}  // namespace android
