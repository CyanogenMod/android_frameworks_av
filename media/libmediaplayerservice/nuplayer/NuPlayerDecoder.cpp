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
#define LOG_TAG "NuPlayerDecoder"
#include <utils/Log.h>
#include <inttypes.h>

#include "NuPlayerDecoder.h"

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

NuPlayer::Decoder::Decoder(
        const sp<AMessage> &notify,
        const sp<NativeWindowWrapper> &nativeWindow)
    : mNotify(notify),
      mNativeWindow(nativeWindow),
      mBufferGeneration(0),
      mPaused(true),
      mComponentName("decoder") {
    // Every decoder has its own looper because MediaCodec operations
    // are blocking, but NuPlayer needs asynchronous operations.
    mDecoderLooper = new ALooper;
    mDecoderLooper->setName("NuPlayerDecoder");
    mDecoderLooper->start(false, false, ANDROID_PRIORITY_AUDIO);

    mCodecLooper = new ALooper;
    mCodecLooper->setName("NuPlayerDecoder-MC");
    mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
}

NuPlayer::Decoder::~Decoder() {
}

static
status_t PostAndAwaitResponse(
        const sp<AMessage> &msg, sp<AMessage> *response) {
    status_t err = msg->postAndAwaitResponse(response);

    if (err != OK) {
        return err;
    }

    if (!(*response)->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

void NuPlayer::Decoder::onConfigure(const sp<AMessage> &format) {
    CHECK(mCodec == NULL);

    ++mBufferGeneration;

    AString mime;
    CHECK(format->findString("mime", &mime));

    sp<Surface> surface = NULL;
    if (mNativeWindow != NULL) {
        surface = mNativeWindow->getSurfaceTextureClient();
    }

    mComponentName = mime;
    mComponentName.append(" decoder");
    ALOGV("[%s] onConfigure (surface=%p)", mComponentName.c_str(), surface.get());

    mCodec = MediaCodec::CreateByType(mCodecLooper, mime.c_str(), false /* encoder */);
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

    mCodec->getName(&mComponentName);

    if (mNativeWindow != NULL) {
        // disconnect from surface as MediaCodec will reconnect
        CHECK_EQ((int)NO_ERROR,
                native_window_api_disconnect(
                        surface.get(),
                        NATIVE_WINDOW_API_MEDIA));
    }
    status_t err = mCodec->configure(
            format, surface, NULL /* crypto */, 0 /* flags */);
    if (err != OK) {
        ALOGE("Failed to configure %s decoder (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        return;
    }
    // the following should work in configured state
    CHECK_EQ((status_t)OK, mCodec->getOutputFormat(&mOutputFormat));
    CHECK_EQ((status_t)OK, mCodec->getInputFormat(&mInputFormat));

    err = mCodec->start();
    if (err != OK) {
        ALOGE("Failed to start %s decoder (err=%d)", mComponentName.c_str(), err);
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

    requestCodecNotification();
    mPaused = false;
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
}

void NuPlayer::Decoder::requestCodecNotification() {
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

void NuPlayer::Decoder::init() {
    mDecoderLooper->registerHandler(this);
}

void NuPlayer::Decoder::configure(const sp<AMessage> &format) {
    sp<AMessage> msg = new AMessage(kWhatConfigure, id());
    msg->setMessage("format", format);
    msg->post();
}

status_t NuPlayer::Decoder::getInputBuffers(Vector<sp<ABuffer> > *buffers) const {
    sp<AMessage> msg = new AMessage(kWhatGetInputBuffers, id());
    msg->setPointer("buffers", buffers);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

void NuPlayer::Decoder::handleError(int32_t err)
{
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

bool NuPlayer::Decoder::handleAnInputBuffer() {
    size_t bufferIx = -1;
    status_t res = mCodec->dequeueInputBuffer(&bufferIx);
    ALOGV("[%s] dequeued input: %d",
            mComponentName.c_str(), res == OK ? (int)bufferIx : res);
    if (res != OK) {
        if (res != -EAGAIN) {
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

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, id());
    reply->setSize("buffer-ix", bufferIx);
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFillThisBuffer);
    notify->setBuffer("buffer", mInputBuffers[bufferIx]);
    notify->setMessage("reply", reply);
    notify->post();
    return true;
}

void android::NuPlayer::Decoder::onInputBufferFilled(const sp<AMessage> &msg) {
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));
    CHECK_LT(bufferIx, mInputBuffers.size());
    sp<ABuffer> codecBuffer = mInputBuffers[bufferIx];

    sp<ABuffer> buffer;
    bool hasBuffer = msg->findBuffer("buffer", &buffer);

    // handle widevine classic source - that fills an arbitrary input buffer
    MediaBuffer *mediaBuffer = NULL;
    if (hasBuffer && buffer->meta()->findPointer(
            "mediaBuffer", (void **)&mediaBuffer)) {
        if (mediaBuffer == NULL) {
            // received no actual buffer
            ALOGW("[%s] received null MediaBuffer %s",
                    mComponentName.c_str(), msg->debugString().c_str());
            buffer = NULL;
        } else {
            // likely filled another buffer than we requested: adjust buffer index
            size_t ix;
            for (ix = 0; ix < mInputBuffers.size(); ix++) {
                const sp<ABuffer> &buf = mInputBuffers[ix];
                if (buf->data() == mediaBuffer->data()) {
                    // all input buffers are dequeued on start, hence the check
                    CHECK(mInputBufferIsDequeued[ix]);
                    ALOGV("[%s] received MediaBuffer for #%zu instead of #%zu",
                            mComponentName.c_str(), ix, bufferIx);

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

    mInputBufferIsDequeued.editItemAt(bufferIx) = false;

    if (buffer == NULL /* includes !hasBuffer */) {
        int32_t streamErr = ERROR_END_OF_STREAM;
        CHECK(msg->findInt32("err", &streamErr) || !hasBuffer);

        if (streamErr == OK) {
            /* buffers are returned to hold on to */
            return;
        }

        // attempt to queue EOS
        status_t err = mCodec->queueInputBuffer(
                bufferIx,
                0,
                0,
                0,
                MediaCodec::BUFFER_FLAG_EOS);
        if (streamErr == ERROR_END_OF_STREAM && err != OK) {
            streamErr = err;
            // err will not be ERROR_END_OF_STREAM
        }

        if (streamErr != ERROR_END_OF_STREAM) {
            handleError(streamErr);
        }
    } else {
        int64_t timeUs = 0;
        uint32_t flags = 0;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        int32_t eos;
        // we do not expect CODECCONFIG or SYNCFRAME for decoder
        if (buffer->meta()->findInt32("eos", &eos) && eos) {
            flags |= MediaCodec::BUFFER_FLAG_EOS;
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
            ALOGE("Failed to queue input buffer for %s (err=%d)",
                    mComponentName.c_str(), err);
            handleError(err);
        }

        if (mediaBuffer != NULL) {
            CHECK(mMediaBuffers[bufferIx] == NULL);
            mMediaBuffers.editItemAt(bufferIx) = mediaBuffer;
        }
    }
}

bool NuPlayer::Decoder::handleAnOutputBuffer() {
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

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatOutputFormatChanged);
        notify->setMessage("format", format);
        notify->post();
        return true;
    } else if (res == INFO_DISCONTINUITY) {
        // nothing to do
        return true;
    } else if (res != OK) {
        if (res != -EAGAIN) {
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
    }
    // we do not expect CODECCONFIG or SYNCFRAME for decoder

    sp<AMessage> reply = new AMessage(kWhatRenderBuffer, id());
    reply->setSize("buffer-ix", bufferIx);
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatDrainThisBuffer);
    notify->setBuffer("buffer", buffer);
    notify->setMessage("reply", reply);
    notify->post();

    // FIXME: This should be handled after rendering is complete,
    // but Renderer needs it now
    if (flags & MediaCodec::BUFFER_FLAG_EOS) {
        ALOGV("queueing eos [%s]", mComponentName.c_str());
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatEOS);
        notify->setInt32("err", ERROR_END_OF_STREAM);
        notify->post();
    }
    return true;
}

void NuPlayer::Decoder::onRenderBuffer(const sp<AMessage> &msg) {
    status_t err;
    int32_t render;
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));
    if (msg->findInt32("render", &render) && render) {
        err = mCodec->renderOutputBufferAndRelease(bufferIx);
    } else {
        err = mCodec->releaseOutputBuffer(bufferIx);
    }
    if (err != OK) {
        ALOGE("failed to release output buffer for %s (err=%d)",
                mComponentName.c_str(), err);
        handleError(err);
    }
}

void NuPlayer::Decoder::onFlush() {
    status_t err = OK;
    if (mCodec != NULL) {
        err = mCodec->flush();
        ++mBufferGeneration;
    }

    if (err != OK) {
        ALOGE("failed to flush %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        return;
    }

    releaseAndResetMediaBuffers();

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushCompleted);
    notify->post();
    mPaused = true;
}

void NuPlayer::Decoder::onResume() {
    mPaused = false;
}

void NuPlayer::Decoder::onShutdown() {
    status_t err = OK;
    if (mCodec != NULL) {
        err = mCodec->release();
        mCodec = NULL;
        ++mBufferGeneration;

        if (mNativeWindow != NULL) {
            // reconnect to surface as MediaCodec disconnected from it
            CHECK_EQ((int)NO_ERROR,
                    native_window_api_connect(
                            mNativeWindow->getNativeWindow().get(),
                            NATIVE_WINDOW_API_MEDIA));
        }
        mComponentName = "decoder";
    }

    releaseAndResetMediaBuffers();

    if (err != OK) {
        ALOGE("failed to release %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        return;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatShutdownCompleted);
    notify->post();
    mPaused = true;
}

void NuPlayer::Decoder::onMessageReceived(const sp<AMessage> &msg) {
    ALOGV("[%s] onMessage: %s", mComponentName.c_str(), msg->debugString().c_str());

    switch (msg->what()) {
        case kWhatConfigure:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));
            onConfigure(format);
            break;
        }

        case kWhatGetInputBuffers:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            Vector<sp<ABuffer> > *dstBuffers;
            CHECK(msg->findPointer("buffers", (void **)&dstBuffers));

            dstBuffers->clear();
            for (size_t i = 0; i < mInputBuffers.size(); i++) {
                dstBuffers->push(mInputBuffers[i]);
            }

            (new AMessage)->postReply(replyID);
            break;
        }

        case kWhatCodecNotify:
        {
            if (!isStaleReply(msg)) {
                if (!mPaused) {
                    while (handleAnInputBuffer()) {
                    }
                }

                while (handleAnOutputBuffer()) {
                }
            }

            requestCodecNotification();
            break;
        }

        case kWhatInputBufferFilled:
        {
            if (!isStaleReply(msg)) {
                onInputBufferFilled(msg);
            }
            break;
        }

        case kWhatRenderBuffer:
        {
            if (!isStaleReply(msg)) {
                onRenderBuffer(msg);
            }
            break;
        }

        case kWhatFlush:
        {
            onFlush();
            break;
        }

        case kWhatResume:
        {
            onResume();
            break;
        }

        case kWhatShutdown:
        {
            onShutdown();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::Decoder::signalFlush() {
    (new AMessage(kWhatFlush, id()))->post();
}

void NuPlayer::Decoder::signalResume() {
    (new AMessage(kWhatResume, id()))->post();
}

void NuPlayer::Decoder::initiateShutdown() {
    (new AMessage(kWhatShutdown, id()))->post();
}

bool NuPlayer::Decoder::supportsSeamlessAudioFormatChange(const sp<AMessage> &targetFormat) const {
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
            if (!mOutputFormat->findInt32(keys[i], &oldVal) ||
                    !targetFormat->findInt32(keys[i], &newVal) ||
                    oldVal != newVal) {
                return false;
            }
        }

        sp<ABuffer> oldBuf, newBuf;
        if (mOutputFormat->findBuffer("csd-0", &oldBuf) &&
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
    if (mOutputFormat == NULL) {
        return false;
    }

    if (targetFormat == NULL) {
        return true;
    }

    AString oldMime, newMime;
    if (!mOutputFormat->findString("mime", &oldMime)
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

struct NuPlayer::CCDecoder::CCData {
    CCData(uint8_t type, uint8_t data1, uint8_t data2)
        : mType(type), mData1(data1), mData2(data2) {
    }

    uint8_t mType;
    uint8_t mData1;
    uint8_t mData2;
};

NuPlayer::CCDecoder::CCDecoder(const sp<AMessage> &notify)
    : mNotify(notify),
      mTrackCount(0),
      mSelectedTrack(-1) {
}

size_t NuPlayer::CCDecoder::getTrackCount() const {
    return mTrackCount;
}

sp<AMessage> NuPlayer::CCDecoder::getTrackInfo(size_t index) const {
    CHECK(index == 0);

    sp<AMessage> format = new AMessage();

    format->setInt32("type", MEDIA_TRACK_TYPE_SUBTITLE);
    format->setString("language", "und");
    format->setString("mime", MEDIA_MIMETYPE_TEXT_CEA_608);
    format->setInt32("auto", 1);
    format->setInt32("default", 1);
    format->setInt32("forced", 0);

    return format;
}

status_t NuPlayer::CCDecoder::selectTrack(size_t index, bool select) {
    CHECK(index < mTrackCount);

    if (select) {
        if (mSelectedTrack == (ssize_t)index) {
            ALOGE("track %zu already selected", index);
            return BAD_VALUE;
        }
        ALOGV("selected track %zu", index);
        mSelectedTrack = index;
    } else {
        if (mSelectedTrack != (ssize_t)index) {
            ALOGE("track %zu is not selected", index);
            return BAD_VALUE;
        }
        ALOGV("unselected track %zu", index);
        mSelectedTrack = -1;
    }

    return OK;
}

bool NuPlayer::CCDecoder::isSelected() const {
    return mSelectedTrack >= 0 && mSelectedTrack < (int32_t)mTrackCount;
}

bool NuPlayer::CCDecoder::isNullPad(CCData *cc) const {
    return cc->mData1 < 0x10 && cc->mData2 < 0x10;
}

void NuPlayer::CCDecoder::dumpBytePair(const sp<ABuffer> &ccBuf) const {
    size_t offset = 0;
    AString out;

    while (offset < ccBuf->size()) {
        char tmp[128];

        CCData *cc = (CCData *) (ccBuf->data() + offset);

        if (isNullPad(cc)) {
            // 1 null pad or XDS metadata, ignore
            offset += sizeof(CCData);
            continue;
        }

        if (cc->mData1 >= 0x20 && cc->mData1 <= 0x7f) {
            // 2 basic chars
            sprintf(tmp, "[%d]Basic: %c %c", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
                 && cc->mData2 >= 0x30 && cc->mData2 <= 0x3f) {
            // 1 special char
            sprintf(tmp, "[%d]Special: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x12 || cc->mData1 == 0x1A)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f){
            // 1 Spanish/French char
            sprintf(tmp, "[%d]Spanish: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x13 || cc->mData1 == 0x1B)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f){
            // 1 Portuguese/German/Danish char
            sprintf(tmp, "[%d]German: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
                 && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f){
            // Mid-Row Codes (Table 69)
            sprintf(tmp, "[%d]Mid-row: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if (((cc->mData1 == 0x14 || cc->mData1 == 0x1c)
                  && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f)
                  ||
                   ((cc->mData1 == 0x17 || cc->mData1 == 0x1f)
                  && cc->mData2 >= 0x21 && cc->mData2 <= 0x23)){
            // Misc Control Codes (Table 70)
            sprintf(tmp, "[%d]Ctrl: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else if ((cc->mData1 & 0x70) == 0x10
                && (cc->mData2 & 0x40) == 0x40
                && ((cc->mData1 & 0x07) || !(cc->mData2 & 0x20)) ) {
            // Preamble Address Codes (Table 71)
            sprintf(tmp, "[%d]PAC: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        } else {
            sprintf(tmp, "[%d]Invalid: %02x %02x", cc->mType, cc->mData1, cc->mData2);
        }

        if (out.size() > 0) {
            out.append(", ");
        }

        out.append(tmp);

        offset += sizeof(CCData);
    }

    ALOGI("%s", out.c_str());
}

bool NuPlayer::CCDecoder::extractFromSEI(const sp<ABuffer> &accessUnit) {
    int64_t timeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

    sp<ABuffer> sei;
    if (!accessUnit->meta()->findBuffer("sei", &sei) || sei == NULL) {
        return false;
    }

    bool hasCC = false;

    NALBitReader br(sei->data() + 1, sei->size() - 1);
    // sei_message()
    while (br.atLeastNumBitsLeft(16)) { // at least 16-bit for sei_message()
        uint32_t payload_type = 0;
        size_t payload_size = 0;
        uint8_t last_byte;

        do {
            last_byte = br.getBits(8);
            payload_type += last_byte;
        } while (last_byte == 0xFF);

        do {
            last_byte = br.getBits(8);
            payload_size += last_byte;
        } while (last_byte == 0xFF);

        // sei_payload()
        if (payload_type == 4) {
            // user_data_registered_itu_t_t35()

            // ATSC A/72: 6.4.2
            uint8_t itu_t_t35_country_code = br.getBits(8);
            uint16_t itu_t_t35_provider_code = br.getBits(16);
            uint32_t user_identifier = br.getBits(32);
            uint8_t user_data_type_code = br.getBits(8);

            payload_size -= 1 + 2 + 4 + 1;

            if (itu_t_t35_country_code == 0xB5
                    && itu_t_t35_provider_code == 0x0031
                    && user_identifier == 'GA94'
                    && user_data_type_code == 0x3) {
                hasCC = true;

                // MPEG_cc_data()
                // ATSC A/53 Part 4: 6.2.3.1
                br.skipBits(1); //process_em_data_flag
                bool process_cc_data_flag = br.getBits(1);
                br.skipBits(1); //additional_data_flag
                size_t cc_count = br.getBits(5);
                br.skipBits(8); // em_data;
                payload_size -= 2;

                if (process_cc_data_flag) {
                    AString out;

                    sp<ABuffer> ccBuf = new ABuffer(cc_count * sizeof(CCData));
                    ccBuf->setRange(0, 0);

                    for (size_t i = 0; i < cc_count; i++) {
                        uint8_t marker = br.getBits(5);
                        CHECK_EQ(marker, 0x1f);

                        bool cc_valid = br.getBits(1);
                        uint8_t cc_type = br.getBits(2);
                        // remove odd parity bit
                        uint8_t cc_data_1 = br.getBits(8) & 0x7f;
                        uint8_t cc_data_2 = br.getBits(8) & 0x7f;

                        if (cc_valid
                                && (cc_type == 0 || cc_type == 1)) {
                            CCData cc(cc_type, cc_data_1, cc_data_2);
                            if (!isNullPad(&cc)) {
                                memcpy(ccBuf->data() + ccBuf->size(),
                                        (void *)&cc, sizeof(cc));
                                ccBuf->setRange(0, ccBuf->size() + sizeof(CCData));
                            }
                        }
                    }
                    payload_size -= cc_count * 3;

                    mCCMap.add(timeUs, ccBuf);
                    break;
                }
            } else {
                ALOGV("Malformed SEI payload type 4");
            }
        } else {
            ALOGV("Unsupported SEI payload type %d", payload_type);
        }

        // skipping remaining bits of this payload
        br.skipBits(payload_size * 8);
    }

    return hasCC;
}

void NuPlayer::CCDecoder::decode(const sp<ABuffer> &accessUnit) {
    if (extractFromSEI(accessUnit) && mTrackCount == 0) {
        mTrackCount++;

        ALOGI("Found CEA-608 track");
        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTrackAdded);
        msg->post();
    }
    // TODO: extract CC from other sources
}

void NuPlayer::CCDecoder::display(int64_t timeUs) {
    ssize_t index = mCCMap.indexOfKey(timeUs);
    if (index < 0) {
        ALOGV("cc for timestamp %" PRId64 " not found", timeUs);
        return;
    }

    sp<ABuffer> &ccBuf = mCCMap.editValueAt(index);

    if (ccBuf->size() > 0) {
#if 0
        dumpBytePair(ccBuf);
#endif

        ccBuf->meta()->setInt32("trackIndex", mSelectedTrack);
        ccBuf->meta()->setInt64("timeUs", timeUs);
        ccBuf->meta()->setInt64("durationUs", 0ll);

        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatClosedCaptionData);
        msg->setBuffer("buffer", ccBuf);
        msg->post();
    }

    // remove all entries before timeUs
    mCCMap.removeItemsAt(0, index + 1);
}

}  // namespace android

