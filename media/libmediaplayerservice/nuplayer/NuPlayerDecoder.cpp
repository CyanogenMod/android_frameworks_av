/*
 * Copyright 2014 The Android Open Source Project
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
#define LOG_TAG "NuPlayerDecoder"
#include <utils/Log.h>
#include <inttypes.h>

#include "NuPlayerCCDecoder.h"
#include "NuPlayerDecoder.h"
#include "NuPlayerRenderer.h"
#include "NuPlayerSource.h"

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/ExtendedCodec.h>


#include "avc_utils.h"
#include "ATSParser.h"

namespace android {

NuPlayer::Decoder::Decoder(
        const sp<AMessage> &notify,
        const sp<Source> &source,
        const sp<Renderer> &renderer,
        const sp<NativeWindowWrapper> &nativeWindow,
        const sp<CCDecoder> &ccDecoder)
    : DecoderBase(notify),
      mNativeWindow(nativeWindow),
      mSource(source),
      mRenderer(renderer),
      mCCDecoder(ccDecoder),
      mSkipRenderingUntilMediaTimeUs(-1ll),
      mNumFramesTotal(0ll),
      mNumFramesDropped(0ll),
      mIsAudio(true),
      mIsVideoAVC(false),
      mIsSecure(false),
      mFormatChangePending(false),
      mPaused(true),
      mResumePending(false),
      mComponentName("decoder") {
    mCodecLooper = new ALooper;
    mCodecLooper->setName("NPDecoder-CL");
    mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
}

NuPlayer::Decoder::~Decoder() {
    releaseAndResetMediaBuffers();
}

void NuPlayer::Decoder::getStats(
        int64_t *numFramesTotal,
        int64_t *numFramesDropped) const {
    *numFramesTotal = mNumFramesTotal;
    *numFramesDropped = mNumFramesDropped;
}

void NuPlayer::Decoder::onMessageReceived(const sp<AMessage> &msg) {
    ALOGV("[%s] onMessage: %s", mComponentName.c_str(), msg->debugString().c_str());

    switch (msg->what()) {
        case kWhatCodecNotify:
        {
            if (!isStaleReply(msg)) {
                int32_t numInput, numOutput;

                if (!msg->findInt32("input-buffers", &numInput)) {
                    numInput = INT32_MAX;
                }

                if (!msg->findInt32("output-buffers", &numOutput)) {
                    numOutput = INT32_MAX;
                }

                if (!mPaused) {
                    while (numInput-- > 0 && handleAnInputBuffer()) {}
                }

                while (numOutput-- > 0 && handleAnOutputBuffer()) {}
            }

            requestCodecNotification();
            break;
        }

        case kWhatRenderBuffer:
        {
            if (!isStaleReply(msg)) {
                onRenderBuffer(msg);
            }
            break;
        }

        default:
            DecoderBase::onMessageReceived(msg);
            break;
    }
}

void NuPlayer::Decoder::onConfigure(const sp<AMessage> &format) {
    CHECK(mCodec == NULL);

    mFormatChangePending = false;

    ++mBufferGeneration;

    AString mime;
    CHECK(format->findString("mime", &mime));

    mIsAudio = !strncasecmp("audio/", mime.c_str(), 6);
    mIsVideoAVC = !strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime.c_str());

    sp<Surface> surface = NULL;
    if (mNativeWindow != NULL) {
        surface = mNativeWindow->getSurfaceTextureClient();
    }

    mComponentName = mime;
    mComponentName.append(" decoder");
    ALOGV("[%s] onConfigure (surface=%p)", mComponentName.c_str(), surface.get());

    ExtendedCodec::overrideMimeType(format, &mime);
    ExtendedCodec::overrideComponentName(0, format, &mComponentName, &mime, false);

    /* time allocateNode here */
    {
        if (mPlayerExtendedStats == NULL) {
            format->findObject(MEDIA_EXTENDED_STATS, (sp<RefBase>*)&mPlayerExtendedStats);
        }
        int32_t isVideo = !strncasecmp(mime.c_str(), "video/", 6);
        ExtendedStats::AutoProfile autoProfile(
                STATS_PROFILE_ALLOCATE_NODE(isVideo), mPlayerExtendedStats);

        if (!mComponentName.startsWith(mime.c_str())) {
            mCodec = MediaCodec::CreateByComponentName(mCodecLooper, mComponentName.c_str());
        } else {
            mCodec = MediaCodec::CreateByType(mCodecLooper, mime.c_str(), false /* encoder */);
        }
    }

    int32_t secure = 0;
    if (format->findInt32("secure", &secure) && secure != 0) {
        if (mCodec != NULL) {
            mCodec->getName(&mComponentName);
            mComponentName.append(".secure");
            mCodec->release();
            ALOGI("[%s] creating", mComponentName.c_str());
            mCodec = MediaCodec::CreateByComponentName(
                    mCodecLooper, mComponentName.c_str());
        }
    }
    if (mCodec == NULL) {
        ALOGE("Failed to create %s%s decoder",
                (secure ? "secure " : ""), mime.c_str());
        handleError(UNKNOWN_ERROR);
        return;
    }
    mIsSecure = secure;

    mCodec->getName(&mComponentName);

    status_t err;
    if (mNativeWindow != NULL) {
        // disconnect from surface as MediaCodec will reconnect
        err = native_window_api_disconnect(
                surface.get(), NATIVE_WINDOW_API_MEDIA);
        // We treat this as a warning, as this is a preparatory step.
        // Codec will try to connect to the surface, which is where
        // any error signaling will occur.
        ALOGW_IF(err != OK, "failed to disconnect from surface: %d", err);
    }
    if (mPlayerExtendedStats != NULL) {
        format->setObject(MEDIA_EXTENDED_STATS, mPlayerExtendedStats);
    }
    err = mCodec->configure(
            format, surface, NULL /* crypto */, 0 /* flags */);
    if (err != OK) {
        ALOGE("Failed to configure %s decoder (err=%d)", mComponentName.c_str(), err);
        mCodec->release();
        mCodec.clear();
        handleError(err);
        return;
    }
    rememberCodecSpecificData(format);

    // the following should work in configured state
    CHECK_EQ((status_t)OK, mCodec->getInputFormat(&mInputFormat));

    err = mCodec->start();
    if (err != OK) {
        ALOGE("Failed to start %s decoder (err=%d)", mComponentName.c_str(), err);
        mCodec->release();
        mCodec.clear();
        handleError(err);
        return;
    }

    // the following should work after start
    CHECK_EQ((status_t)OK, mCodec->getInputBuffers(&mInputBuffers));
    releaseAndResetMediaBuffers();
    CHECK_EQ((status_t)OK, mCodec->getOutputBuffers(&mOutputBuffers));
    ALOGV("[%s] got %zu input and %zu output buffers",
            mComponentName.c_str(),
            mInputBuffers.size(),
            mOutputBuffers.size());

    if (mRenderer != NULL) {
        requestCodecNotification();
    }
    mPaused = false;
    mResumePending = false;
}

void NuPlayer::Decoder::onSetRenderer(const sp<Renderer> &renderer) {
    bool hadNoRenderer = (mRenderer == NULL);
    mRenderer = renderer;
    if (hadNoRenderer && mRenderer != NULL) {
        requestCodecNotification();
    }
}

void NuPlayer::Decoder::onGetInputBuffers(
        Vector<sp<ABuffer> > *dstBuffers) {
    dstBuffers->clear();
    for (size_t i = 0; i < mInputBuffers.size(); i++) {
        dstBuffers->push(mInputBuffers[i]);
    }
}

void NuPlayer::Decoder::onResume(bool notifyComplete) {
    mPaused = false;

    if (notifyComplete) {
        mResumePending = true;
    }
}

void NuPlayer::Decoder::onFlush(bool notifyComplete) {
    if (mCCDecoder != NULL) {
        mCCDecoder->flush();
    }

    if (mRenderer != NULL) {
        mRenderer->flush(mIsAudio, notifyComplete);
        mRenderer->signalTimeDiscontinuity(mIsAudio);
    }

    status_t err = OK;
    if (mCodec != NULL) {
        err = mCodec->flush();
        mCSDsToSubmit = mCSDsForCurrentFormat; // copy operator
        ++mBufferGeneration;
    }

    if (err != OK) {
        ALOGE("failed to flush %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        // finish with posting kWhatFlushCompleted.
        // we attempt to release the buffers even if flush fails.
    }
    releaseAndResetMediaBuffers();

    if (notifyComplete) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatFlushCompleted);
        notify->post();
        mPaused = true;
    }
}

void NuPlayer::Decoder::onShutdown(bool notifyComplete) {
    status_t err = OK;

    // if there is a pending resume request, notify complete now
    notifyResumeCompleteIfNecessary();

    if (mCodec != NULL) {
        err = mCodec->release();
        mCodec = NULL;
        ++mBufferGeneration;

        if (mNativeWindow != NULL) {
            // reconnect to surface as MediaCodec disconnected from it
            status_t error =
                    native_window_api_connect(
                            mNativeWindow->getNativeWindow().get(),
                            NATIVE_WINDOW_API_MEDIA);
            ALOGW_IF(error != NO_ERROR,
                    "[%s] failed to connect to native window, error=%d",
                    mComponentName.c_str(), error);
        }
        mComponentName = "decoder";
    }

    releaseAndResetMediaBuffers();

    if (err != OK) {
        ALOGE("failed to release %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        // finish with posting kWhatShutdownCompleted.
    }

    if (notifyComplete) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatShutdownCompleted);
        notify->post();
        mPaused = true;
    }
}

void NuPlayer::Decoder::doRequestBuffers() {
    if (mFormatChangePending) {
        return;
    }
    status_t err = OK;
    while (!mDequeuedInputBuffers.empty()) {
        size_t bufferIx = *mDequeuedInputBuffers.begin();
        sp<AMessage> msg = new AMessage();
        msg->setSize("buffer-ix", bufferIx);
        err = fetchInputData(msg);
        if (err != OK) {
            break;
        }
        mDequeuedInputBuffers.erase(mDequeuedInputBuffers.begin());

        if (!mPendingInputMessages.empty()
                || !onInputBufferFetched(msg)) {
            mPendingInputMessages.push_back(msg);
        }
    }

    if (err == -EWOULDBLOCK
            && mSource->feedMoreTSData() == OK) {
        scheduleRequestBuffers();
    }
}

bool NuPlayer::Decoder::handleAnInputBuffer() {
    if (mFormatChangePending) {
        return false;
    }
    size_t bufferIx = -1;
    status_t res = mCodec->dequeueInputBuffer(&bufferIx);
    ALOGV("[%s] dequeued input: %d",
            mComponentName.c_str(), res == OK ? (int)bufferIx : res);
    if (res != OK) {
        if (res != -EAGAIN) {
            ALOGE("Failed to dequeue input buffer for %s (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
        }
        return false;
    }

    CHECK_LT(bufferIx, mInputBuffers.size());

    if (mMediaBuffers[bufferIx] != NULL) {
        mMediaBuffers[bufferIx]->release();
        mMediaBuffers.editItemAt(bufferIx) = NULL;
    }
    mInputBufferIsDequeued.editItemAt(bufferIx) = true;

    if (!mCSDsToSubmit.isEmpty()) {
        sp<AMessage> msg = new AMessage();
        msg->setSize("buffer-ix", bufferIx);

        sp<ABuffer> buffer = mCSDsToSubmit.itemAt(0);
        ALOGI("[%s] resubmitting CSD", mComponentName.c_str());
        msg->setBuffer("buffer", buffer);
        mCSDsToSubmit.removeAt(0);
        CHECK(onInputBufferFetched(msg));
        return true;
    }

    while (!mPendingInputMessages.empty()) {
        sp<AMessage> msg = *mPendingInputMessages.begin();
        if (!onInputBufferFetched(msg)) {
            break;
        }
        mPendingInputMessages.erase(mPendingInputMessages.begin());
    }

    if (!mInputBufferIsDequeued.editItemAt(bufferIx)) {
        return true;
    }

    mDequeuedInputBuffers.push_back(bufferIx);

    onRequestInputBuffers();
    return true;
}

bool NuPlayer::Decoder::handleAnOutputBuffer() {
    if (mFormatChangePending) {
        return false;
    }
    size_t bufferIx = -1;
    size_t offset;
    size_t size;
    int64_t timeUs;
    uint32_t flags;
    status_t res = mCodec->dequeueOutputBuffer(
            &bufferIx, &offset, &size, &timeUs, &flags);

    if (res != OK) {
        ALOGV("[%s] dequeued output: %d", mComponentName.c_str(), res);
    } else {
        ALOGV("[%s] dequeued output: %d (time=%lld flags=%" PRIu32 ")",
                mComponentName.c_str(), (int)bufferIx, timeUs, flags);
    }

    if (res == INFO_OUTPUT_BUFFERS_CHANGED) {
        res = mCodec->getOutputBuffers(&mOutputBuffers);
        if (res != OK) {
            ALOGE("Failed to get output buffers for %s after INFO event (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
            return false;
        }
        // NuPlayer ignores this
        return true;
    } else if (res == INFO_FORMAT_CHANGED) {
        sp<AMessage> format = new AMessage();
        res = mCodec->getOutputFormat(&format);
        if (res != OK) {
            ALOGE("Failed to get output format for %s after INFO event (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
            return false;
        }

        if (!mIsAudio) {
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatVideoSizeChanged);
            notify->setMessage("format", format);
            notify->post();
        } else if (mRenderer != NULL) {
            uint32_t flags;
            int64_t durationUs;
            bool hasVideo = (mSource->getFormat(false /* audio */) != NULL);
            if (!hasVideo &&
                    mSource->getDuration(&durationUs) == OK &&
                    durationUs
                        > AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US) {
                flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
            } else {
                flags = AUDIO_OUTPUT_FLAG_NONE;
            }

            uint32_t isStreaming = 0;
            sp<AMessage> notify = mNotify->dup();
            notify->findInt32("isStreaming", (int32_t *)&isStreaming);

            res = mRenderer->openAudioSink(
                    format, false /* offloadOnly */, hasVideo, flags, isStreaming, NULL /* isOffloaded */);
            if (res != OK) {
                ALOGE("Failed to open AudioSink on format change for %s (err=%d)",
                        mComponentName.c_str(), res);
                handleError(res);
                return false;
            }
        }
        return true;
    } else if (res == INFO_DISCONTINUITY) {
        // nothing to do
        return true;
    } else if (res != OK) {
        if (res != -EAGAIN) {
            ALOGE("Failed to dequeue output buffer for %s (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
        }
        return false;
    }

    CHECK_LT(bufferIx, mOutputBuffers.size());
    sp<ABuffer> buffer = mOutputBuffers[bufferIx];
    buffer->setRange(offset, size);
    buffer->meta()->clear();
    buffer->meta()->setInt64("timeUs", timeUs);
    if (flags & MediaCodec::BUFFER_FLAG_EOS) {
        buffer->meta()->setInt32("eos", true);
        notifyResumeCompleteIfNecessary();
    }
    // we do not expect CODECCONFIG or SYNCFRAME for decoder

    sp<AMessage> reply = new AMessage(kWhatRenderBuffer, id());
    reply->setSize("buffer-ix", bufferIx);
    reply->setInt32("generation", mBufferGeneration);

    if (mSkipRenderingUntilMediaTimeUs >= 0) {
        if (timeUs < mSkipRenderingUntilMediaTimeUs) {
            ALOGV("[%s] dropping buffer at time %lld as requested.",
                     mComponentName.c_str(), (long long)timeUs);

            reply->post();
            return true;
        }

        mSkipRenderingUntilMediaTimeUs = -1;
    }

    // wait until 1st frame comes out to signal resume complete
    notifyResumeCompleteIfNecessary();

    if (mRenderer != NULL) {
        // send the buffer to renderer.
        mRenderer->queueBuffer(mIsAudio, buffer, reply);
        if (flags & MediaCodec::BUFFER_FLAG_EOS) {
            mRenderer->queueEOS(mIsAudio, ERROR_END_OF_STREAM);
        }
    }

    return true;
}

void NuPlayer::Decoder::releaseAndResetMediaBuffers() {
    for (size_t i = 0; i < mMediaBuffers.size(); i++) {
        if (mMediaBuffers[i] != NULL) {
            mMediaBuffers[i]->release();
            mMediaBuffers.editItemAt(i) = NULL;
        }
    }
    mMediaBuffers.resize(mInputBuffers.size());
    for (size_t i = 0; i < mMediaBuffers.size(); i++) {
        mMediaBuffers.editItemAt(i) = NULL;
    }
    mInputBufferIsDequeued.clear();
    mInputBufferIsDequeued.resize(mInputBuffers.size());
    for (size_t i = 0; i < mInputBufferIsDequeued.size(); i++) {
        mInputBufferIsDequeued.editItemAt(i) = false;
    }

    mPendingInputMessages.clear();
    mDequeuedInputBuffers.clear();
    mSkipRenderingUntilMediaTimeUs = -1;
}

void NuPlayer::Decoder::requestCodecNotification() {
    if (mFormatChangePending) {
        return;
    }
    if (mCodec != NULL) {
        sp<AMessage> reply = new AMessage(kWhatCodecNotify, id());
        reply->setInt32("generation", mBufferGeneration);
        mCodec->requestActivityNotification(reply);
    }
}

bool NuPlayer::Decoder::isStaleReply(const sp<AMessage> &msg) {
    int32_t generation;
    CHECK(msg->findInt32("generation", &generation));
    return generation != mBufferGeneration;
}

status_t NuPlayer::Decoder::fetchInputData(sp<AMessage> &reply) {
    sp<ABuffer> accessUnit;
    bool dropAccessUnit;
    do {
        status_t err = mSource->dequeueAccessUnit(mIsAudio, &accessUnit);

        if (err == -EWOULDBLOCK) {
            return err;
        } else if (err != OK) {
            if (err == INFO_DISCONTINUITY) {
                int32_t type;
                CHECK(accessUnit->meta()->findInt32("discontinuity", &type));

                bool formatChange =
                    (mIsAudio &&
                     (type & ATSParser::DISCONTINUITY_AUDIO_FORMAT))
                    || (!mIsAudio &&
                            (type & ATSParser::DISCONTINUITY_VIDEO_FORMAT));

                bool timeChange = (type & ATSParser::DISCONTINUITY_TIME) != 0;

                ALOGI("%s discontinuity (format=%d, time=%d)",
                        mIsAudio ? "audio" : "video", formatChange, timeChange);

                bool seamlessFormatChange = false;
                sp<AMessage> newFormat = mSource->getFormat(mIsAudio);
                if (formatChange) {
                    seamlessFormatChange =
                        supportsSeamlessFormatChange(newFormat);
                    // treat seamless format change separately
                    formatChange = !seamlessFormatChange;
                }

                if (formatChange || timeChange) {
                    sp<AMessage> msg = mNotify->dup();
                    msg->setInt32("what", kWhatInputDiscontinuity);
                    msg->setInt32("formatChange", formatChange);
                    msg->post();
                }

                if (formatChange /* not seamless */) {
                    // must change decoder
                    // return EOS and wait to be killed
                    mFormatChangePending = true;
                    return ERROR_END_OF_STREAM;
                } else if (timeChange) {
                    // need to flush
                    // TODO: Ideally we shouldn't need a flush upon time
                    // discontinuity, flushing will cause loss of frames.
                    // We probably should queue a time change marker to the
                    // output queue, and handles it in renderer instead.
                    rememberCodecSpecificData(newFormat);
                    onFlush(false /* notifyComplete */);
                    err = OK;
                } else if (seamlessFormatChange) {
                    // reuse existing decoder and don't flush
                    rememberCodecSpecificData(newFormat);
                    err = OK;
                } else {
                    // This stream is unaffected by the discontinuity
                    return -EWOULDBLOCK;
                }
            }

            reply->setInt32("err", err);
            return OK;
        }

        if (!mIsAudio) {
            ++mNumFramesTotal;
        }

        dropAccessUnit = false;
        if (!mIsAudio
                && !mIsSecure
                && mRenderer->getVideoLateByUs() > 100000ll
                && mIsVideoAVC
                && !IsAVCReferenceFrame(accessUnit)) {
            dropAccessUnit = true;
            ++mNumFramesDropped;
        }
    } while (dropAccessUnit);

    // ALOGV("returned a valid buffer of %s data", mIsAudio ? "mIsAudio" : "video");
#if 0
    int64_t mediaTimeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &mediaTimeUs));
    ALOGV("feeding %s input buffer at media time %.2f secs",
         mIsAudio ? "audio" : "video",
         mediaTimeUs / 1E6);
#endif

    if (mCCDecoder != NULL) {
        mCCDecoder->decode(accessUnit);
    }

    reply->setBuffer("buffer", accessUnit);

    return OK;
}

bool NuPlayer::Decoder::onInputBufferFetched(const sp<AMessage> &msg) {
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));
    CHECK_LT(bufferIx, mInputBuffers.size());
    sp<ABuffer> codecBuffer = mInputBuffers[bufferIx];

    sp<ABuffer> buffer;
    bool hasBuffer = msg->findBuffer("buffer", &buffer);

    // handle widevine classic source - that fills an arbitrary input buffer
    MediaBuffer *mediaBuffer = NULL;
    if (hasBuffer) {
        mediaBuffer = (MediaBuffer *)(buffer->getMediaBufferBase());
        if (mediaBuffer != NULL) {
            // likely filled another buffer than we requested: adjust buffer index
            size_t ix;
            for (ix = 0; ix < mInputBuffers.size(); ix++) {
                const sp<ABuffer> &buf = mInputBuffers[ix];
                if (buf->data() == mediaBuffer->data()) {
                    // all input buffers are dequeued on start, hence the check
                    if (!mInputBufferIsDequeued[ix]) {
                        ALOGV("[%s] received MediaBuffer for #%zu instead of #%zu",
                                mComponentName.c_str(), ix, bufferIx);
                        mediaBuffer->release();
                        return false;
                    }

                    // TRICKY: need buffer for the metadata, so instead, set
                    // codecBuffer to the same (though incorrect) buffer to
                    // avoid a memcpy into the codecBuffer
                    codecBuffer = buffer;
                    codecBuffer->setRange(
                            mediaBuffer->range_offset(),
                            mediaBuffer->range_length());
                    bufferIx = ix;
                    break;
                }
            }
            CHECK(ix < mInputBuffers.size());
        }
    }

    if (buffer == NULL /* includes !hasBuffer */) {
        int32_t streamErr = ERROR_END_OF_STREAM;
        CHECK(msg->findInt32("err", &streamErr) || !hasBuffer);

        if (streamErr == OK) {
            /* buffers are returned to hold on to */
            return true;
        }

        // attempt to queue EOS
        status_t err = mCodec->queueInputBuffer(
                bufferIx,
                0,
                0,
                0,
                MediaCodec::BUFFER_FLAG_EOS);
        if (err == OK) {
            mInputBufferIsDequeued.editItemAt(bufferIx) = false;
        } else if (streamErr == ERROR_END_OF_STREAM) {
            streamErr = err;
            // err will not be ERROR_END_OF_STREAM
        }

        if (streamErr != ERROR_END_OF_STREAM) {
            ALOGE("Stream error for %s (err=%d), EOS %s queued",
                    mComponentName.c_str(),
                    streamErr,
                    err == OK ? "successfully" : "unsuccessfully");
            handleError(streamErr);
        }
    } else {
        sp<AMessage> extra;
        if (buffer->meta()->findMessage("extra", &extra) && extra != NULL) {
            int64_t resumeAtMediaTimeUs;
            if (extra->findInt64(
                        "resume-at-mediaTimeUs", &resumeAtMediaTimeUs)) {
                ALOGI("[%s] suppressing rendering until %lld us",
                        mComponentName.c_str(), (long long)resumeAtMediaTimeUs);
                mSkipRenderingUntilMediaTimeUs = resumeAtMediaTimeUs;
            }
        }

        int64_t timeUs = 0;
        uint32_t flags = 0;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        int32_t eos, csd;
        // we do not expect SYNCFRAME for decoder
        if (buffer->meta()->findInt32("eos", &eos) && eos) {
            flags |= MediaCodec::BUFFER_FLAG_EOS;
        } else if (buffer->meta()->findInt32("csd", &csd) && csd) {
            flags |= MediaCodec::BUFFER_FLAG_CODECCONFIG;
        }

        // copy into codec buffer
        if (buffer != codecBuffer) {
            CHECK_LE(buffer->size(), codecBuffer->capacity());
            codecBuffer->setRange(0, buffer->size());
            memcpy(codecBuffer->data(), buffer->data(), buffer->size());
        }

        status_t err = mCodec->queueInputBuffer(
                        bufferIx,
                        codecBuffer->offset(),
                        codecBuffer->size(),
                        timeUs,
                        flags);
        if (err != OK) {
            if (mediaBuffer != NULL) {
                mediaBuffer->release();
            }
            ALOGE("Failed to queue input buffer for %s (err=%d)",
                    mComponentName.c_str(), err);
            handleError(err);
        } else {
            mInputBufferIsDequeued.editItemAt(bufferIx) = false;
            if (mediaBuffer != NULL) {
                CHECK(mMediaBuffers[bufferIx] == NULL);
                mMediaBuffers.editItemAt(bufferIx) = mediaBuffer;
            }
        }

        PLAYER_STATS(logBitRate, buffer->size(), timeUs);
    }
    return true;
}

void NuPlayer::Decoder::onRenderBuffer(const sp<AMessage> &msg) {
    status_t err;
    int32_t render;
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));

    if (!mIsAudio) {
        int64_t timeUs;
        sp<ABuffer> buffer = mOutputBuffers[bufferIx];
        buffer->meta()->findInt64("timeUs", &timeUs);

        if (mCCDecoder != NULL && mCCDecoder->isSelected()) {
            mCCDecoder->display(timeUs);
        }
    }

    if (msg->findInt32("render", &render) && render) {
        int64_t timestampNs;
        CHECK(msg->findInt64("timestampNs", &timestampNs));
        err = mCodec->renderOutputBufferAndRelease(bufferIx, timestampNs);
    } else {
        err = mCodec->releaseOutputBuffer(bufferIx);
    }
    if (err != OK) {
        ALOGE("failed to release output buffer for %s (err=%d)",
                mComponentName.c_str(), err);
        handleError(err);
    }
}

bool NuPlayer::Decoder::supportsSeamlessAudioFormatChange(
        const sp<AMessage> &targetFormat) const {
    if (targetFormat == NULL) {
        return true;
    }

    AString mime;
    if (!targetFormat->findString("mime", &mime)) {
        return false;
    }

    if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        // field-by-field comparison
        const char * keys[] = { "channel-count", "sample-rate", "is-adts" };
        for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            int32_t oldVal, newVal;
            if (!mInputFormat->findInt32(keys[i], &oldVal) ||
                    !targetFormat->findInt32(keys[i], &newVal) ||
                    oldVal != newVal) {
                return false;
            }
        }

        sp<ABuffer> oldBuf, newBuf;
        if (mInputFormat->findBuffer("csd-0", &oldBuf) &&
                targetFormat->findBuffer("csd-0", &newBuf)) {
            if (oldBuf->size() != newBuf->size()) {
                return false;
            }
            return !memcmp(oldBuf->data(), newBuf->data(), oldBuf->size());
        }
    }
    return false;
}

bool NuPlayer::Decoder::supportsSeamlessFormatChange(const sp<AMessage> &targetFormat) const {
    if (mInputFormat == NULL) {
        return false;
    }

    if (targetFormat == NULL) {
        return true;
    }

    AString oldMime, newMime;
    if (!mInputFormat->findString("mime", &oldMime)
            || !targetFormat->findString("mime", &newMime)
            || !(oldMime == newMime)) {
        return false;
    }

    bool audio = !strncasecmp(oldMime.c_str(), "audio/", strlen("audio/"));
    bool seamless;
    if (audio) {
        seamless = supportsSeamlessAudioFormatChange(targetFormat);
    } else {
        int32_t isAdaptive;
        seamless = (mCodec != NULL &&
                mInputFormat->findInt32("adaptive-playback", &isAdaptive) &&
                isAdaptive);
    }

    ALOGV("%s seamless support for %s", seamless ? "yes" : "no", oldMime.c_str());
    return seamless;
}

void NuPlayer::Decoder::rememberCodecSpecificData(const sp<AMessage> &format) {
    if (format == NULL) {
        return;
    }
    mCSDsForCurrentFormat.clear();
    for (int32_t i = 0; ; ++i) {
        AString tag = "csd-";
        tag.append(i);
        sp<ABuffer> buffer;
        if (!format->findBuffer(tag.c_str(), &buffer)) {
            break;
        }
        mCSDsForCurrentFormat.push(buffer);
    }
}

void NuPlayer::Decoder::notifyResumeCompleteIfNecessary() {
    if (mResumePending) {
        mResumePending = false;

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatResumeCompleted);
        notify->post();
    }
}

}  // namespace android

