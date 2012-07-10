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
#include <dlfcn.h>  // for dlopen/dlclose
#include "NuPlayer.h"

#include "HTTPLiveSource.h"
#include "NuPlayerDecoder.h"
#include "NuPlayerDriver.h"
#include "NuPlayerRenderer.h"
#include "NuPlayerSource.h"
#include "RTSPSource.h"
#include "StreamingSource.h"
#include "GenericSource.h"

#include "ATSParser.h"

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <gui/ISurfaceTexture.h>

#include "avc_utils.h"

namespace android {

////////////////////////////////////////////////////////////////////////////////

NuPlayer::NuPlayer()
    : mUIDValid(false),
      mVideoIsAVC(false),
      mAudioEOS(false),
      mVideoEOS(false),
      mScanSourcesPending(false),
      mScanSourcesGeneration(0),
      mTimeDiscontinuityPending(false),
      mFlushingAudio(NONE),
      mFlushingVideo(NONE),
      mResetInProgress(false),
      mResetPostponed(false),
      mSkipRenderingAudioUntilMediaTimeUs(-1ll),
      mSkipRenderingVideoUntilMediaTimeUs(-1ll),
      mVideoLateByUs(0ll),
      mNumFramesTotal(0ll),
      mNumFramesDropped(0ll)
#ifdef QCOM_HARDWARE
      ,mPauseIndication(false),
      mSourceType(kDefaultSource) 
#endif
{
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

void NuPlayer::setDataSource(const sp<IStreamSource> &source) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());

    msg->setObject("source", new StreamingSource(source));
#ifdef QCOM_HARDWARE
    mSourceType = kStreamingSource;
#endif
    msg->post();
}

static bool IsHTTPLiveURL(const char *url) {
    if (!strncasecmp("http://", url, 7)
            || !strncasecmp("https://", url, 8)) {
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

void NuPlayer::setDataSource(
        const char *url, const KeyedVector<String8, String8> *headers) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());

    sp<Source> source;
    if (IsHTTPLiveURL(url)) {
        source = new HTTPLiveSource(url, headers, mUIDValid, mUID);
#ifdef QCOM_HARDWARE
        mSourceType = kHttpLiveSource;
#endif
    } else if (!strncasecmp(url, "rtsp://", 7)) {
        source = new RTSPSource(url, headers, mUIDValid, mUID);
#ifdef QCOM_HARDWARE
        mSourceType = kRtspSource;
    } else if (!strncasecmp(url, "http://", 7) &&
	    (strlen(url) >= 4 && !strcasecmp(".mpd", &url[strlen(url) - 4]))) {
           /* Load the DASH HTTP Live source librery here */
           ALOGV("NuPlayer setDataSource url sting %s",url);
           source = LoadCreateSource(url, headers, mUIDValid, mUID,kHttpDashSource);
           if (source != NULL) {
              mSourceType = kHttpDashSource;
              msg->setObject("source", source);
           } else {
             ALOGE("Error creating DASH source");
             //return UNKNOWN_ERROR;
           }
#endif
	} else {
        source = new GenericSource(url, headers, mUIDValid, mUID);
#ifdef QCOM_HARDWARE
        mSourceType = kGenericSource;
#endif
    }

    msg->setObject("source", source);
    msg->post();
}

void NuPlayer::setDataSource(int fd, int64_t offset, int64_t length) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, id());

    sp<Source> source = new GenericSource(fd, offset, length);
#ifdef QCOM_HARDWARE
    mSourceType = kGenericSource;
#endif
    msg->setObject("source", source);
    msg->post();
}

void NuPlayer::setVideoSurfaceTexture(const sp<ISurfaceTexture> &surfaceTexture) {
    sp<AMessage> msg = new AMessage(kWhatSetVideoNativeWindow, id());
    sp<SurfaceTextureClient> surfaceTextureClient(surfaceTexture != NULL ?
                new SurfaceTextureClient(surfaceTexture) : NULL);
    msg->setObject("native-window", new NativeWindowWrapper(surfaceTextureClient));
    msg->post();
}

void NuPlayer::setAudioSink(const sp<MediaPlayerBase::AudioSink> &sink) {
    sp<AMessage> msg = new AMessage(kWhatSetAudioSink, id());
    msg->setObject("sink", sink);
    msg->post();
}

void NuPlayer::start() {
    (new AMessage(kWhatStart, id()))->post();
}

void NuPlayer::pause() {
    (new AMessage(kWhatPause, id()))->post();
}

void NuPlayer::resume() {
    (new AMessage(kWhatResume, id()))->post();
}

void NuPlayer::resetAsync() {
    (new AMessage(kWhatReset, id()))->post();
}

void NuPlayer::seekToAsync(int64_t seekTimeUs) {
    sp<AMessage> msg = new AMessage(kWhatSeek, id());
    msg->setInt64("seekTimeUs", seekTimeUs);
    msg->post();
}

// static
bool NuPlayer::IsFlushingState(FlushStatus state, bool *needShutdown) {
    switch (state) {
        case FLUSHING_DECODER:
            if (needShutdown != NULL) {
                *needShutdown = false;
            }
            return true;

        case FLUSHING_DECODER_SHUTDOWN:
#ifdef QCOM_HARDWARE
        case SHUTTING_DOWN_DECODER:
#endif
            if (needShutdown != NULL) {
                *needShutdown = true;
            }
            return true;

        default:
            return false;
    }
}

void NuPlayer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSetDataSource:
        {
            ALOGV("kWhatSetDataSource");

            CHECK(mSource == NULL);

            sp<RefBase> obj;
            CHECK(msg->findObject("source", &obj));

            mSource = static_cast<Source *>(obj.get());
            break;
        }

        case kWhatSetVideoNativeWindow:
        {
            ALOGV("kWhatSetVideoNativeWindow");

            sp<RefBase> obj;
            CHECK(msg->findObject("native-window", &obj));

            mNativeWindow = static_cast<NativeWindowWrapper *>(obj.get());
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

            mVideoIsAVC = false;
            mAudioEOS = false;
            mVideoEOS = false;
            mSkipRenderingAudioUntilMediaTimeUs = -1;
            mSkipRenderingVideoUntilMediaTimeUs = -1;
            mVideoLateByUs = 0;
            mNumFramesTotal = 0;
            mNumFramesDropped = 0;

            mSource->start();

            mRenderer = new Renderer(
                    mAudioSink,
                    new AMessage(kWhatRendererNotify, id()));

            looper()->registerHandler(mRenderer);

            postScanSources();
            break;
        }

        case kWhatScanSources:
        {
#ifdef QCOM_HARDWARE
            if (!mPauseIndication) {
#endif
                int32_t generation;
                CHECK(msg->findInt32("generation", &generation));
                if (generation != mScanSourcesGeneration) {
                    // Drop obsolete msg.
                    break;
                }

                mScanSourcesPending = false;

                ALOGV("scanning sources haveAudio=%d, haveVideo=%d",
                     mAudioDecoder != NULL, mVideoDecoder != NULL);

                if(mNativeWindow != NULL) {
                    instantiateDecoder(false, &mVideoDecoder);
                }

                if (mAudioSink != NULL) {
                    instantiateDecoder(true, &mAudioDecoder);
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

#ifdef QCOM_HARDWARE
                if ((mAudioDecoder == NULL && mAudioSink != NULL) ||
                    (mVideoDecoder == NULL && mNativeWindow != NULL)) {
#else
                if (mAudioDecoder == NULL || mVideoDecoder == NULL) {
#endif
                    msg->post(100000ll);
                    mScanSourcesPending = true;
                }
#ifdef QCOM_HARDWARE
            }
#endif
            break;
        }

        case kWhatVideoNotify:
        case kWhatAudioNotify:
        {
            bool audio = msg->what() == kWhatAudioNotify;

            sp<AMessage> codecRequest;
            CHECK(msg->findMessage("codec-request", &codecRequest));

            int32_t what;
            CHECK(codecRequest->findInt32("what", &what));

            if (what == ACodec::kWhatFillThisBuffer) {
                status_t err = feedDecoderInputData(
                        audio, codecRequest);

                if (err == -EWOULDBLOCK) {
                    if (mSource->feedMoreTSData() == OK) {
                        msg->post(10000ll);
                    }
                }
            } else if (what == ACodec::kWhatEOS) {
                int32_t err;
                CHECK(codecRequest->findInt32("err", &err));

                if (err == ERROR_END_OF_STREAM) {
                    ALOGV("got %s decoder EOS", audio ? "audio" : "video");
                } else {
                    ALOGV("got %s decoder EOS w/ error %d",
                         audio ? "audio" : "video",
                         err);
                }

                mRenderer->queueEOS(audio, err);
            } else if (what == ACodec::kWhatFlushCompleted) {
#ifdef QCOM_HARDWARE
                Mutex::Autolock autoLock(mLock);
#endif
                bool needShutdown;

                if (audio) {
                    CHECK(IsFlushingState(mFlushingAudio, &needShutdown));
                    mFlushingAudio = FLUSHED;
                } else {
                    CHECK(IsFlushingState(mFlushingVideo, &needShutdown));
                    mFlushingVideo = FLUSHED;

                    mVideoLateByUs = 0;
                }

                ALOGV("decoder %s flush completed", audio ? "audio" : "video");

                if (needShutdown) {
                    ALOGV("initiating %s decoder shutdown",
                         audio ? "audio" : "video");

                    (audio ? mAudioDecoder : mVideoDecoder)->initiateShutdown();

                    if (audio) {
                        mFlushingAudio = SHUTTING_DOWN_DECODER;
                    } else {
                        mFlushingVideo = SHUTTING_DOWN_DECODER;
                    }
                }

                finishFlushIfPossible();
            } else if (what == ACodec::kWhatOutputFormatChanged) {
                if (audio) {
                    int32_t numChannels;
                    CHECK(codecRequest->findInt32("channel-count", &numChannels));

                    int32_t sampleRate;
                    CHECK(codecRequest->findInt32("sample-rate", &sampleRate));

                    ALOGV("Audio output format changed to %d Hz, %d channels",
                         sampleRate, numChannels);

                    mAudioSink->close();

                    audio_output_flags_t flags;
                    int64_t durationUs;
                    // FIXME: we should handle the case where the video decoder is created after
                    // we receive the format change indication. Current code will just make that
                    // we select deep buffer with video which should not be a problem as it should
                    // not prevent from keeping A/V sync.
                    if (mVideoDecoder == NULL &&
                            mSource->getDuration(&durationUs) == OK &&
                            durationUs > AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US) {
                        flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
                    } else {
                        flags = AUDIO_OUTPUT_FLAG_NONE;
                    }

                    int32_t channelMask;
                    if (!codecRequest->findInt32("channel-mask", &channelMask)) {
                        channelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
                    }

                    CHECK_EQ(mAudioSink->open(
                                sampleRate,
                                numChannels,
                                (audio_channel_mask_t)channelMask,
                                AUDIO_FORMAT_PCM_16_BIT,
                                8 /* bufferCount */,
                                NULL,
                                NULL,
                                flags),
                             (status_t)OK);
                    mAudioSink->start();

                    mRenderer->signalAudioSinkChanged();
                } else {
                    // video

                    int32_t width, height;
                    CHECK(codecRequest->findInt32("width", &width));
                    CHECK(codecRequest->findInt32("height", &height));

                    int32_t cropLeft, cropTop, cropRight, cropBottom;
                    CHECK(codecRequest->findRect(
                                "crop",
                                &cropLeft, &cropTop, &cropRight, &cropBottom));

                    ALOGV("Video output format changed to %d x %d "
                         "(crop: %d x %d @ (%d, %d))",
                         width, height,
                         (cropRight - cropLeft + 1),
                         (cropBottom - cropTop + 1),
                         cropLeft, cropTop);

                    notifyListener(
                            MEDIA_SET_VIDEO_SIZE,
                            cropRight - cropLeft + 1,
                            cropBottom - cropTop + 1);
                }
            } else if (what == ACodec::kWhatShutdownCompleted) {
                ALOGV("%s shutdown completed", audio ? "audio" : "video");
                if (audio) {
                    mAudioDecoder.clear();

                    CHECK_EQ((int)mFlushingAudio, (int)SHUTTING_DOWN_DECODER);
                    mFlushingAudio = SHUT_DOWN;
                } else {
                    mVideoDecoder.clear();

                    CHECK_EQ((int)mFlushingVideo, (int)SHUTTING_DOWN_DECODER);
                    mFlushingVideo = SHUT_DOWN;
                }

                finishFlushIfPossible();
            } else if (what == ACodec::kWhatError) {
                ALOGE("Received error from %s decoder, aborting playback.",
                     audio ? "audio" : "video");

                mRenderer->queueEOS(audio, UNKNOWN_ERROR);
            } else if (what == ACodec::kWhatDrainThisBuffer) {
                renderBuffer(audio, codecRequest);
            } else {
                ALOGV("Unhandled codec notification %d.", what);
            }

            break;
        }

        case kWhatRendererNotify:
        {
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
            } else if (what == Renderer::kWhatPosition) {
                int64_t positionUs;
                CHECK(msg->findInt64("positionUs", &positionUs));

                CHECK(msg->findInt64("videoLateByUs", &mVideoLateByUs));

                if (mDriver != NULL) {
                    sp<NuPlayerDriver> driver = mDriver.promote();
                    if (driver != NULL) {
                        driver->notifyPosition(positionUs);
#ifdef QCOM_HARDWARE
                        //Notify rendering position used for HLS
                        mSource->notifyRenderingPosition(positionUs);
#endif

                        driver->notifyFrameStats(
                                mNumFramesTotal, mNumFramesDropped);
                    }
                }
            } else if (what == Renderer::kWhatFlushComplete) {
                CHECK_EQ(what, (int32_t)Renderer::kWhatFlushComplete);

                int32_t audio;
                CHECK(msg->findInt32("audio", &audio));

                ALOGV("renderer %s flush completed.", audio ? "audio" : "video");
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
#ifdef QCOM_HARDWARE
            Mutex::Autolock autoLock(mLock);
#endif

            if (mRenderer != NULL) {
                // There's an edge case where the renderer owns all output
                // buffers and is paused, therefore the decoder will not read
                // more input data and will never encounter the matching
                // discontinuity. To avoid this, we resume the renderer.

                if (mFlushingAudio == AWAITING_DISCONTINUITY
                        || mFlushingVideo == AWAITING_DISCONTINUITY) {
                    mRenderer->resume();
                }
            }
#ifdef QCOM_HARDWARE
            if ( (mAudioDecoder != NULL && IsFlushingState(mFlushingAudio)) ||
                 (mVideoDecoder != NULL && IsFlushingState(mFlushingVideo)) ) {
#else
            if (mFlushingAudio != NONE || mFlushingVideo != NONE) {
#endif

                // We're currently flushing, postpone the reset until that's
                // completed.

                ALOGV("postponing reset mFlushingAudio=%d, mFlushingVideo=%d",
                      mFlushingAudio, mFlushingVideo);

                mResetPostponed = true;
                break;
            }

            if (mAudioDecoder == NULL && mVideoDecoder == NULL) {
                finishReset();
                break;
            }

            mTimeDiscontinuityPending = true;

            if (mAudioDecoder != NULL) {
                flushDecoder(true /* audio */, true /* needShutdown */);
            }

            if (mVideoDecoder != NULL) {
                flushDecoder(false /* audio */, true /* needShutdown */);
            }

            mResetInProgress = true;
            break;
        }

        case kWhatSeek:
        {
            Mutex::Autolock autoLock(mLock);
            int64_t seekTimeUs = -1, newSeekTime = -1;
            status_t nRet = OK;
            CHECK(msg->findInt64("seekTimeUs", &seekTimeUs));

            ALOGW("kWhatSeek seekTimeUs=%lld us (%.2f secs)",
                 seekTimeUs, seekTimeUs / 1E6);

            nRet = mSource->seekTo(seekTimeUs);
#ifdef QCOM_HARDWARE
            if (mSourceType == kHttpLiveSource) {
                mSource->getNewSeekTime(&newSeekTime);
                ALOGV("newSeekTime %lld", newSeekTime);
            }
            else if ( (mSourceType == kHttpDashSource) && (nRet == OK)) // if seek success then flush the audio,video decoder and renderer
            {
                mTimeDiscontinuityPending = true;
                if( (mVideoDecoder != NULL) &&
                    (mFlushingVideo == NONE || mFlushingVideo == AWAITING_DISCONTINUITY) ) {
                    flushDecoder( false, true ); // flush video, shutdown
                }

               if( (mAudioDecoder != NULL) &&
                   (mFlushingAudio == NONE|| mFlushingAudio == AWAITING_DISCONTINUITY) )
               {
                   flushDecoder( true, true );  // flush audio,  shutdown
               }
               if( mAudioDecoder == NULL ) {
                   ALOGV("Audio is not there, set it to shutdown");
                   mFlushingAudio = SHUT_DOWN;
               }
               if( mVideoDecoder == NULL ) {
                   ALOGV("Video is not there, set it to shutdown");
                   mFlushingVideo = SHUT_DOWN;
               }
               // get the new seeked position
               newSeekTime = seekTimeUs;
               ALOGV("newSeekTime %lld", newSeekTime);
            }
            if( (newSeekTime >= 0 ) && (mSourceType != kHttpDashSource)) {
               mTimeDiscontinuityPending = true;
               if( (mAudioDecoder != NULL) &&
                   (mFlushingAudio == NONE || mFlushingAudio == AWAITING_DISCONTINUITY) ) {
                  flushDecoder( true, true );
               }
               if( (mVideoDecoder != NULL) &&
                   (mFlushingVideo == NONE || mFlushingVideo == AWAITING_DISCONTINUITY) ) {
                  flushDecoder( false, true );
               }
               if( mAudioDecoder == NULL ) {
                   ALOGV("Audio is not there, set it to shutdown");
                   mFlushingAudio = SHUT_DOWN;

               }
               if( mVideoDecoder == NULL ) {
                   ALOGV("Video is not there, set it to shutdown");
                   mFlushingVideo = SHUT_DOWN;
               }
            }
#endif
            if (mDriver != NULL) {
                sp<NuPlayerDriver> driver = mDriver.promote();
                if (driver != NULL) {
                    driver->notifySeekComplete();
                    if( newSeekTime >= 0 ) {
                        driver->notifyPosition( newSeekTime );
                     }
                }
            }

            break;
        }

        case kWhatPause:
        {
            CHECK(mRenderer != NULL);
            mRenderer->pause();
#ifdef QCOM_HARDWARE
            mPauseIndication = true;
#endif
            break;
        }

        case kWhatResume:
        {
            CHECK(mRenderer != NULL);
            mRenderer->resume();
#ifdef QCOM_HARDWARE
            mPauseIndication = false;
            if (mAudioDecoder == NULL || mVideoDecoder == NULL) {
                mScanSourcesPending = false;
                postScanSources();
            }
#endif
            break;
        }

#ifdef QCOM_HARDWARE
        case kWhatPrepareAsync:
            if (mSource == NULL)
            {
                ALOGE("Source is null in prepareAsync\n");
                break;
            }
            mSource->prepareAsync();
            postIsPrepareDone();
            break;

        case kWhatIsPrepareDone:
            if (mSource == NULL)
            {
                ALOGE("Source is null when checking for prepare done\n");
                break;
            }
            if (mSource->isPrepareDone()) {
                notifyListener(MEDIA_PREPARED, 0, 0);
            } else {
                msg->post(100000ll);
            }
            break;
#endif

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::finishFlushIfPossible() {
#ifdef QCOM_HARDWARE
    //If reset was postponed after one of the streams is flushed, complete it now
    if (mResetPostponed) {
        ALOGV("finishFlushIfPossible Handle reset postpone ");
        if ((mAudioDecoder != NULL) &&
            (mFlushingAudio == NONE || mFlushingAudio == AWAITING_DISCONTINUITY )) {
           flushDecoder( true, true );
        }
        if ((mVideoDecoder != NULL) &&
            (mFlushingVideo == NONE || mFlushingVideo == AWAITING_DISCONTINUITY )) {
           flushDecoder( false, true );
        }
    }
#endif

    //Check if both audio & video are flushed
    if (mFlushingAudio != FLUSHED && mFlushingAudio != SHUT_DOWN) {
        ALOGV("Dont finish flush, audio is in state %d ", mFlushingAudio);
        return;
    }

    if (mFlushingVideo != FLUSHED && mFlushingVideo != SHUT_DOWN) {
        ALOGV("Dont finish flush, video is in state %d ", mFlushingVideo);
        return;
    }

    ALOGV("both audio and video are flushed now.");

    if (mTimeDiscontinuityPending) {
        mRenderer->signalTimeDiscontinuity();
        mTimeDiscontinuityPending = false;
    }

    if (mAudioDecoder != NULL) {
        ALOGV("Resume Audio after flush");
        mAudioDecoder->signalResume();
    }

    if (mVideoDecoder != NULL) {
        ALOGV("Resume Video after flush");
        mVideoDecoder->signalResume();
    }

    mFlushingAudio = NONE;
    mFlushingVideo = NONE;

    if (mResetInProgress) {
        ALOGV("reset completed");

        mResetInProgress = false;
        finishReset();
    } else if (mResetPostponed) {
        (new AMessage(kWhatReset, id()))->post();
        mResetPostponed = false;
        ALOGV("Handle reset postpone");
    } else if (mAudioDecoder == NULL || mVideoDecoder == NULL) {
        ALOGV("Start scanning for sources after shutdown");
        postScanSources();
    }
}

void NuPlayer::finishReset() {
    CHECK(mAudioDecoder == NULL);
    CHECK(mVideoDecoder == NULL);

    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    mRenderer.clear();

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

status_t NuPlayer::instantiateDecoder(bool audio, sp<Decoder> *decoder) {
    if (*decoder != NULL) {
        return OK;
    }

    sp<MetaData> meta = mSource->getFormat(audio);

    if (meta == NULL) {
        return -EWOULDBLOCK;
    }

    if (!audio) {
        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));
        mVideoIsAVC = !strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime);
    }

    sp<AMessage> notify =
        new AMessage(audio ? kWhatAudioNotify : kWhatVideoNotify,
                     id());

    *decoder = audio ? new Decoder(notify) :
                       new Decoder(notify, mNativeWindow);
    looper()->registerHandler(*decoder);

#ifdef QCOM_HARDWARE
    if (mSourceType == kHttpLiveSource || mSourceType == kHttpDashSource){
        //Set flushing state to none
        Mutex::Autolock autoLock(mLock);
        if( audio ) {
            mFlushingAudio = NONE;
        } else {
            mFlushingVideo = NONE;

        }
    }
#endif

    (*decoder)->configure(meta);

    int64_t durationUs;
    if (mDriver != NULL && mSource->getDuration(&durationUs) == OK) {
        sp<NuPlayerDriver> driver = mDriver.promote();
        if (driver != NULL) {
            driver->notifyDuration(durationUs);
        }
    }

    return OK;
}

status_t NuPlayer::feedDecoderInputData(bool audio, const sp<AMessage> &msg) {
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    {
        Mutex::Autolock autoLock(mLock);

        if ((audio && IsFlushingState(mFlushingAudio))
            || (!audio && IsFlushingState(mFlushingVideo))) {
            reply->setInt32("err", INFO_DISCONTINUITY);
            reply->post();
            return OK;
        }
    }

    sp<ABuffer> accessUnit;

    bool dropAccessUnit;
    do {
        status_t err = mSource->dequeueAccessUnit(audio, &accessUnit);

        if (err == -EWOULDBLOCK) {
            return err;
        } else if (err != OK) {
            if (err == INFO_DISCONTINUITY) {
                int32_t type;
                CHECK(accessUnit->meta()->findInt32("discontinuity", &type));

                bool formatChange =
                    (audio &&
                     (type & ATSParser::DISCONTINUITY_AUDIO_FORMAT))
                    || (!audio &&
                            (type & ATSParser::DISCONTINUITY_VIDEO_FORMAT));

                bool timeChange = (type & ATSParser::DISCONTINUITY_TIME) != 0;

                ALOGW("%s discontinuity (formatChange=%d, time=%d)",
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
                            ALOGW("suppressing rendering of %s until %lld us",
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

                if (formatChange || timeChange) {
                    flushDecoder(audio, formatChange);
                } else {
                    // This stream is unaffected by the discontinuity

                    if (audio) {
                        mFlushingAudio = FLUSHED;
                    } else {
                        mFlushingVideo = FLUSHED;
                    }

                    finishFlushIfPossible();

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
                && mVideoLateByUs > 100000ll
                && mVideoIsAVC
                && !IsAVCReferenceFrame(accessUnit)) {
            dropAccessUnit = true;
            ++mNumFramesDropped;
        }
    } while (dropAccessUnit);

    // ALOGV("returned a valid buffer of %s data", audio ? "audio" : "video");

#if 0
    int64_t mediaTimeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &mediaTimeUs));
    ALOGV("feeding %s input buffer at media time %.2f secs",
         audio ? "audio" : "video",
         mediaTimeUs / 1E6);
#endif

    reply->setBuffer("buffer", accessUnit);
    reply->post();

    return OK;
}

void NuPlayer::renderBuffer(bool audio, const sp<AMessage> &msg) {
    // ALOGV("renderBuffer %s", audio ? "audio" : "video");

    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    Mutex::Autolock autoLock(mLock);
    if (IsFlushingState(audio ? mFlushingAudio : mFlushingVideo)) {
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

    int64_t &skipUntilMediaTimeUs =
        audio
            ? mSkipRenderingAudioUntilMediaTimeUs
            : mSkipRenderingVideoUntilMediaTimeUs;

    if (skipUntilMediaTimeUs >= 0) {
        int64_t mediaTimeUs;
        CHECK(buffer->meta()->findInt64("timeUs", &mediaTimeUs));

        if (mediaTimeUs < skipUntilMediaTimeUs) {
            ALOGV("dropping %s buffer at time %lld as requested.",
                 audio ? "audio" : "video",
                 mediaTimeUs);

            reply->post();
            return;
        }

        skipUntilMediaTimeUs = -1;
    }

    mRenderer->queueBuffer(audio, buffer, reply);
}

void NuPlayer::notifyListener(int msg, int ext1, int ext2) {
    if (mDriver == NULL) {
        return;
    }

    sp<NuPlayerDriver> driver = mDriver.promote();

    if (driver == NULL) {
        return;
    }

    driver->notifyListener(msg, ext1, ext2);
}

void NuPlayer::flushDecoder(bool audio, bool needShutdown) {
    if ((audio && mAudioDecoder == NULL) || (!audio && mVideoDecoder == NULL)) {
        ALOGI("flushDecoder %s without decoder present",
             audio ? "audio" : "video");
    }

    // Make sure we don't continue to scan sources until we finish flushing.
    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    (audio ? mAudioDecoder : mVideoDecoder)->signalFlush();
    mRenderer->flush(audio);

    FlushStatus newStatus =
        needShutdown ? FLUSHING_DECODER_SHUTDOWN : FLUSHING_DECODER;

    if (audio) {
        CHECK(mFlushingAudio == NONE
                || mFlushingAudio == AWAITING_DISCONTINUITY);

        mFlushingAudio = newStatus;

        if (mFlushingVideo == NONE) {
            mFlushingVideo = (mVideoDecoder != NULL)
                ? AWAITING_DISCONTINUITY
                : FLUSHED;
        }
    } else {
        CHECK(mFlushingVideo == NONE
                || mFlushingVideo == AWAITING_DISCONTINUITY);

        mFlushingVideo = newStatus;

        if (mFlushingAudio == NONE) {
            mFlushingAudio = (mAudioDecoder != NULL)
                ? AWAITING_DISCONTINUITY
                : FLUSHED;
        }
    }
}

#ifdef QCOM_HARDWARE
sp<NuPlayer::Source>
    NuPlayer::LoadCreateSource(const char * uri, const KeyedVector<String8,String8> *headers,
                               bool uidValid, uid_t uid, NuSourceType srcTyp)
{
   const char* STREAMING_SOURCE_LIB = "libmmipstreamaal.so";
   const char* DASH_HTTP_LIVE_CREATE_SOURCE = "CreateDashHttpLiveSource";
   const char* WFD_CREATE_SOURCE = "CreateWFDSource";

   void* pStreamingSourceLib = NULL;

   typedef NuPlayer::Source* (*SourceFactory)(const char * uri, const KeyedVector<String8, String8> *headers, bool uidValid, uid_t uid);

   /* Open librery */
   pStreamingSourceLib = ::dlopen(STREAMING_SOURCE_LIB, RTLD_LAZY);

   if (pStreamingSourceLib == NULL) {
       return NULL;
   }

   SourceFactory StreamingSourcePtr;

   if(srcTyp == kHttpDashSource) {

       /* Get the entry level symbol which gets us the pointer to DASH HTTP Live Source object */
       StreamingSourcePtr = (SourceFactory) dlsym(pStreamingSourceLib, DASH_HTTP_LIVE_CREATE_SOURCE);
   } else if (srcTyp == kWfdSource){

       /* Get the entry level symbol which gets us the pointer to WFD Source object */
       StreamingSourcePtr = (SourceFactory) dlsym(pStreamingSourceLib, WFD_CREATE_SOURCE);

   }

   if (StreamingSourcePtr == NULL) {
       return NULL;
   }

    /*Get the Streaming (DASH\WFD) Source object, which will be used to communicate with Source (DASH\WFD) */
    sp<NuPlayer::Source> StreamingSource = StreamingSourcePtr(uri, headers, uidValid, uid);

    if(StreamingSource==NULL) {
        return NULL;
    }


    return StreamingSource;
}

status_t NuPlayer::prepareAsync() // only for DASH
{
    if (mSourceType == kHttpDashSource) {
        sp<AMessage> msg = new AMessage(kWhatPrepareAsync, id());
        if (msg == NULL)
        {
            ALOGE("Out of memory, AMessage is null for kWhatPrepareAsync\n");
            return NO_MEMORY;
        }
        msg->post();
#ifdef QCOM_HARDWARE
        return -EWOULDBLOCK;
#endif
    }
    return OK;
}

status_t NuPlayer::getParameter(int key, Parcel *reply)
{
    void * data_8;
    void * data_16;
    size_t data_8_Size;
    size_t data_16_Size;

    status_t err = OK;
    if (key == 8002) {

        if (mSource == NULL)
        {
            ALOGE("Source is NULL in getParameter\n");
            return UNKNOWN_ERROR;
        }
        err = mSource->getParameter(key, &data_8, &data_8_Size);
        if (err != OK)
        {
            ALOGE("source getParameter returned error: %d\n",err);
            return err;
        }

        data_16_Size = data_8_Size * sizeof(char16_t);
        data_16 = malloc(data_16_Size);
        if (data_16 == NULL)
        {
            ALOGE("Out of memory in getParameter\n");
            return NO_MEMORY;
        }

        utf8_to_utf16_no_null_terminator((uint8_t *)data_8, data_8_Size, (char16_t *) data_16);
        err = reply->writeString16((char16_t *)data_16, data_8_Size);
        free(data_16);
    }
    return err;
}

status_t NuPlayer::setParameter(int key, const Parcel &request)
{
    status_t err = OK;
    if (key == 8002) {

        size_t len = 0;
        const char16_t* str = request.readString16Inplace(&len);
        void * data = malloc(len + 1);
        if (data == NULL)
        {
            ALOGE("Out of memory in setParameter\n");
            return NO_MEMORY;
        }

        utf16_to_utf8(str, len, (char*) data);
        err = mSource->setParameter(key, data, len);
        free(data);
    }
    return err;
}

void NuPlayer::postIsPrepareDone()
{
    sp<AMessage> msg = new AMessage(kWhatIsPrepareDone, id());
    if (msg == NULL)
    {
        ALOGE("Out of memory, AMessage is null for kWhatIsPrepareDone\n");
        return;
    }
    msg->post();
}
#endif
}  // namespace android
