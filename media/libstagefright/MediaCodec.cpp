/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaCodec"
#include <inttypes.h>

#include "include/avc_utils.h"
#include "include/SoftwareRenderer.h"

#include <binder/IBatteryStats.h>
#include <binder/IServiceManager.h>
#include <gui/Surface.h>
#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/BufferProducerWrapper.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/NativeWindowWrapper.h>
#include <private/android_filesystem_config.h>
#include <utils/Log.h>
#include <utils/Singleton.h>

namespace android {

struct MediaCodec::BatteryNotifier : public Singleton<BatteryNotifier> {
    BatteryNotifier();

    void noteStartVideo();
    void noteStopVideo();
    void noteStartAudio();
    void noteStopAudio();

private:
    int32_t mVideoRefCount;
    int32_t mAudioRefCount;
    sp<IBatteryStats> mBatteryStatService;
};

ANDROID_SINGLETON_STATIC_INSTANCE(MediaCodec::BatteryNotifier)

MediaCodec::BatteryNotifier::BatteryNotifier() :
    mVideoRefCount(0),
    mAudioRefCount(0) {
    // get battery service
    const sp<IServiceManager> sm(defaultServiceManager());
    if (sm != NULL) {
        const String16 name("batterystats");
        mBatteryStatService = interface_cast<IBatteryStats>(sm->getService(name));
        if (mBatteryStatService == NULL) {
            ALOGE("batterystats service unavailable!");
        }
    }
}

void MediaCodec::BatteryNotifier::noteStartVideo() {
    if (mVideoRefCount == 0 && mBatteryStatService != NULL) {
        mBatteryStatService->noteStartVideo(AID_MEDIA);
    }
    mVideoRefCount++;
}

void MediaCodec::BatteryNotifier::noteStopVideo() {
    if (mVideoRefCount == 0) {
        ALOGW("BatteryNotifier::noteStop(): video refcount is broken!");
        return;
    }

    mVideoRefCount--;
    if (mVideoRefCount == 0 && mBatteryStatService != NULL) {
        mBatteryStatService->noteStopVideo(AID_MEDIA);
    }
}

void MediaCodec::BatteryNotifier::noteStartAudio() {
    if (mAudioRefCount == 0 && mBatteryStatService != NULL) {
        mBatteryStatService->noteStartAudio(AID_MEDIA);
    }
    mAudioRefCount++;
}

void MediaCodec::BatteryNotifier::noteStopAudio() {
    if (mAudioRefCount == 0) {
        ALOGW("BatteryNotifier::noteStop(): audio refcount is broken!");
        return;
    }

    mAudioRefCount--;
    if (mAudioRefCount == 0 && mBatteryStatService != NULL) {
        mBatteryStatService->noteStopAudio(AID_MEDIA);
    }
}
// static
sp<MediaCodec> MediaCodec::CreateByType(
        const sp<ALooper> &looper, const char *mime, bool encoder, status_t *err) {
    sp<MediaCodec> codec = new MediaCodec(looper);

    const status_t ret = codec->init(mime, true /* nameIsType */, encoder);
    if (err != NULL) {
        *err = ret;
    }
    return ret == OK ? codec : NULL; // NULL deallocates codec.
}

// static
sp<MediaCodec> MediaCodec::CreateByComponentName(
        const sp<ALooper> &looper, const char *name, status_t *err) {
    sp<MediaCodec> codec = new MediaCodec(looper);

    const status_t ret = codec->init(name, false /* nameIsType */, false /* encoder */);
    if (err != NULL) {
        *err = ret;
    }
    return ret == OK ? codec : NULL; // NULL deallocates codec.
}

MediaCodec::MediaCodec(const sp<ALooper> &looper)
    : mState(UNINITIALIZED),
      mLooper(looper),
      mCodec(NULL),
      mReplyID(0),
      mFlags(0),
      mStickyError(OK),
      mSoftRenderer(NULL),
      mBatteryStatNotified(false),
      mIsVideo(false),
      mDequeueInputTimeoutGeneration(0),
      mDequeueInputReplyID(0),
      mDequeueOutputTimeoutGeneration(0),
      mDequeueOutputReplyID(0),
      mHaveInputSurface(false) {
}

MediaCodec::~MediaCodec() {
    CHECK_EQ(mState, UNINITIALIZED);
}

// static
status_t MediaCodec::PostAndAwaitResponse(
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

// static
void MediaCodec::PostReplyWithError(int32_t replyID, int32_t err) {
    sp<AMessage> response = new AMessage;
    response->setInt32("err", err);
    response->postReply(replyID);
}

status_t MediaCodec::init(const char *name, bool nameIsType, bool encoder) {
    // save init parameters for reset
    mInitName = name;
    mInitNameIsType = nameIsType;
    mInitIsEncoder = encoder;

    // Current video decoders do not return from OMX_FillThisBuffer
    // quickly, violating the OpenMAX specs, until that is remedied
    // we need to invest in an extra looper to free the main event
    // queue.
    mCodec = new ACodec;
    bool needDedicatedLooper = false;
    if (nameIsType && !strncasecmp(name, "video/", 6)) {
        needDedicatedLooper = true;
    } else {
        AString tmp = name;
        if (tmp.endsWith(".secure")) {
            tmp.erase(tmp.size() - 7, 7);
        }
        const sp<IMediaCodecList> mcl = MediaCodecList::getInstance();
        ssize_t codecIdx = mcl->findCodecByName(tmp.c_str());
        if (codecIdx >= 0) {
            const sp<MediaCodecInfo> info = mcl->getCodecInfo(codecIdx);
            Vector<AString> mimes;
            info->getSupportedMimes(&mimes);
            for (size_t i = 0; i < mimes.size(); i++) {
                if (mimes[i].startsWith("video/")) {
                    needDedicatedLooper = true;
                    break;
                }
            }
        }
    }

    if (needDedicatedLooper) {
        if (mCodecLooper == NULL) {
            mCodecLooper = new ALooper;
            mCodecLooper->setName("CodecLooper");
            mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
        }

        mCodecLooper->registerHandler(mCodec);
    } else {
        mLooper->registerHandler(mCodec);
    }

    mLooper->registerHandler(this);

    mCodec->setNotificationMessage(new AMessage(kWhatCodecNotify, id()));

    sp<AMessage> msg = new AMessage(kWhatInit, id());
    msg->setString("name", name);
    msg->setInt32("nameIsType", nameIsType);

    if (nameIsType) {
        msg->setInt32("encoder", encoder);
    }

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::setCallback(const sp<AMessage> &callback) {
    sp<AMessage> msg = new AMessage(kWhatSetCallback, id());
    msg->setMessage("callback", callback);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::configure(
        const sp<AMessage> &format,
        const sp<Surface> &nativeWindow,
        const sp<ICrypto> &crypto,
        uint32_t flags) {
    sp<AMessage> msg = new AMessage(kWhatConfigure, id());

    msg->setMessage("format", format);
    msg->setInt32("flags", flags);

    if (nativeWindow != NULL) {
        msg->setObject(
                "native-window",
                new NativeWindowWrapper(nativeWindow));
    }

    if (crypto != NULL) {
        msg->setPointer("crypto", crypto.get());
    }

    sp<AMessage> response;
    status_t err = PostAndAwaitResponse(msg, &response);

    if (err != OK && err != INVALID_OPERATION) {
        // MediaCodec now set state to UNINITIALIZED upon any fatal error.
        // To maintain backward-compatibility, do a reset() to put codec
        // back into INITIALIZED state.
        // But don't reset if the err is INVALID_OPERATION, which means
        // the configure failure is due to wrong state.

        ALOGE("configure failed with err 0x%08x, resetting...", err);
        reset();
    }

    return err;
}

status_t MediaCodec::createInputSurface(
        sp<IGraphicBufferProducer>* bufferProducer) {
    sp<AMessage> msg = new AMessage(kWhatCreateInputSurface, id());

    sp<AMessage> response;
    status_t err = PostAndAwaitResponse(msg, &response);
    if (err == NO_ERROR) {
        // unwrap the sp<IGraphicBufferProducer>
        sp<RefBase> obj;
        bool found = response->findObject("input-surface", &obj);
        CHECK(found);
        sp<BufferProducerWrapper> wrapper(
                static_cast<BufferProducerWrapper*>(obj.get()));
        *bufferProducer = wrapper->getBufferProducer();
    } else {
        ALOGW("createInputSurface failed, err=%d", err);
    }
    return err;
}

status_t MediaCodec::start() {
    sp<AMessage> msg = new AMessage(kWhatStart, id());

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::stop() {
    sp<AMessage> msg = new AMessage(kWhatStop, id());

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::release() {
    sp<AMessage> msg = new AMessage(kWhatRelease, id());

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::reset() {
    /* When external-facing MediaCodec object is created,
       it is already initialized.  Thus, reset is essentially
       release() followed by init(), plus clearing the state */

    status_t err = release();

    // unregister handlers
    if (mCodec != NULL) {
        if (mCodecLooper != NULL) {
            mCodecLooper->unregisterHandler(mCodec->id());
        } else {
            mLooper->unregisterHandler(mCodec->id());
        }
        mCodec = NULL;
    }
    mLooper->unregisterHandler(id());

    mFlags = 0;    // clear all flags
    mStickyError = OK;

    // reset state not reset by setState(UNINITIALIZED)
    mReplyID = 0;
    mDequeueInputReplyID = 0;
    mDequeueOutputReplyID = 0;
    mDequeueInputTimeoutGeneration = 0;
    mDequeueOutputTimeoutGeneration = 0;
    mHaveInputSurface = false;

    if (err == OK) {
        err = init(mInitName.c_str(), mInitNameIsType, mInitIsEncoder);
    }
    return err;
}

status_t MediaCodec::queueInputBuffer(
        size_t index,
        size_t offset,
        size_t size,
        int64_t presentationTimeUs,
        uint32_t flags,
        AString *errorDetailMsg) {
    if (errorDetailMsg != NULL) {
        errorDetailMsg->clear();
    }

    sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, id());
    msg->setSize("index", index);
    msg->setSize("offset", offset);
    msg->setSize("size", size);
    msg->setInt64("timeUs", presentationTimeUs);
    msg->setInt32("flags", flags);
    msg->setPointer("errorDetailMsg", errorDetailMsg);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::queueSecureInputBuffer(
        size_t index,
        size_t offset,
        const CryptoPlugin::SubSample *subSamples,
        size_t numSubSamples,
        const uint8_t key[16],
        const uint8_t iv[16],
        CryptoPlugin::Mode mode,
        int64_t presentationTimeUs,
        uint32_t flags,
        AString *errorDetailMsg) {
    if (errorDetailMsg != NULL) {
        errorDetailMsg->clear();
    }

    sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, id());
    msg->setSize("index", index);
    msg->setSize("offset", offset);
    msg->setPointer("subSamples", (void *)subSamples);
    msg->setSize("numSubSamples", numSubSamples);
    msg->setPointer("key", (void *)key);
    msg->setPointer("iv", (void *)iv);
    msg->setInt32("mode", mode);
    msg->setInt64("timeUs", presentationTimeUs);
    msg->setInt32("flags", flags);
    msg->setPointer("errorDetailMsg", errorDetailMsg);

    sp<AMessage> response;
    status_t err = PostAndAwaitResponse(msg, &response);

    return err;
}

status_t MediaCodec::dequeueInputBuffer(size_t *index, int64_t timeoutUs) {
    sp<AMessage> msg = new AMessage(kWhatDequeueInputBuffer, id());
    msg->setInt64("timeoutUs", timeoutUs);

    sp<AMessage> response;
    status_t err;
    if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
        return err;
    }

    CHECK(response->findSize("index", index));

    return OK;
}

status_t MediaCodec::dequeueOutputBuffer(
        size_t *index,
        size_t *offset,
        size_t *size,
        int64_t *presentationTimeUs,
        uint32_t *flags,
        int64_t timeoutUs) {
    sp<AMessage> msg = new AMessage(kWhatDequeueOutputBuffer, id());
    msg->setInt64("timeoutUs", timeoutUs);

    sp<AMessage> response;
    status_t err;
    if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
        return err;
    }

    CHECK(response->findSize("index", index));
    CHECK(response->findSize("offset", offset));
    CHECK(response->findSize("size", size));
    CHECK(response->findInt64("timeUs", presentationTimeUs));
    CHECK(response->findInt32("flags", (int32_t *)flags));

    return OK;
}

status_t MediaCodec::renderOutputBufferAndRelease(size_t index) {
    sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, id());
    msg->setSize("index", index);
    msg->setInt32("render", true);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::renderOutputBufferAndRelease(size_t index, int64_t timestampNs) {
    sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, id());
    msg->setSize("index", index);
    msg->setInt32("render", true);
    msg->setInt64("timestampNs", timestampNs);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::releaseOutputBuffer(size_t index) {
    sp<AMessage> msg = new AMessage(kWhatReleaseOutputBuffer, id());
    msg->setSize("index", index);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::signalEndOfInputStream() {
    sp<AMessage> msg = new AMessage(kWhatSignalEndOfInputStream, id());

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::getOutputFormat(sp<AMessage> *format) const {
    sp<AMessage> msg = new AMessage(kWhatGetOutputFormat, id());

    sp<AMessage> response;
    status_t err;
    if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
        return err;
    }

    CHECK(response->findMessage("format", format));

    return OK;
}

status_t MediaCodec::getInputFormat(sp<AMessage> *format) const {
    sp<AMessage> msg = new AMessage(kWhatGetInputFormat, id());

    sp<AMessage> response;
    status_t err;
    if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
        return err;
    }

    CHECK(response->findMessage("format", format));

    return OK;
}

status_t MediaCodec::getName(AString *name) const {
    sp<AMessage> msg = new AMessage(kWhatGetName, id());

    sp<AMessage> response;
    status_t err;
    if ((err = PostAndAwaitResponse(msg, &response)) != OK) {
        return err;
    }

    CHECK(response->findString("name", name));

    return OK;
}

status_t MediaCodec::getInputBuffers(Vector<sp<ABuffer> > *buffers) const {
    sp<AMessage> msg = new AMessage(kWhatGetBuffers, id());
    msg->setInt32("portIndex", kPortIndexInput);
    msg->setPointer("buffers", buffers);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::getOutputBuffers(Vector<sp<ABuffer> > *buffers) const {
    sp<AMessage> msg = new AMessage(kWhatGetBuffers, id());
    msg->setInt32("portIndex", kPortIndexOutput);
    msg->setPointer("buffers", buffers);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::getOutputBuffer(size_t index, sp<ABuffer> *buffer) {
    sp<AMessage> format;
    return getBufferAndFormat(kPortIndexOutput, index, buffer, &format);
}

status_t MediaCodec::getOutputFormat(size_t index, sp<AMessage> *format) {
    sp<ABuffer> buffer;
    return getBufferAndFormat(kPortIndexOutput, index, &buffer, format);
}

status_t MediaCodec::getInputBuffer(size_t index, sp<ABuffer> *buffer) {
    sp<AMessage> format;
    return getBufferAndFormat(kPortIndexInput, index, buffer, &format);
}

bool MediaCodec::isExecuting() const {
    return mState == STARTED || mState == FLUSHED;
}

status_t MediaCodec::getBufferAndFormat(
        size_t portIndex, size_t index,
        sp<ABuffer> *buffer, sp<AMessage> *format) {
    // use mutex instead of a context switch

    buffer->clear();
    format->clear();
    if (!isExecuting()) {
        return INVALID_OPERATION;
    }

    // we do not want mPortBuffers to change during this section
    // we also don't want mOwnedByClient to change during this
    Mutex::Autolock al(mBufferLock);
    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];
    if (index < buffers->size()) {
        const BufferInfo &info = buffers->itemAt(index);
        if (info.mOwnedByClient) {
            *buffer = info.mData;
            *format = info.mFormat;
        }
    }
    return OK;
}

status_t MediaCodec::flush() {
    sp<AMessage> msg = new AMessage(kWhatFlush, id());

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::requestIDRFrame() {
    (new AMessage(kWhatRequestIDRFrame, id()))->post();

    return OK;
}

void MediaCodec::requestActivityNotification(const sp<AMessage> &notify) {
    sp<AMessage> msg = new AMessage(kWhatRequestActivityNotification, id());
    msg->setMessage("notify", notify);
    msg->post();
}

////////////////////////////////////////////////////////////////////////////////

void MediaCodec::cancelPendingDequeueOperations() {
    if (mFlags & kFlagDequeueInputPending) {
        PostReplyWithError(mDequeueInputReplyID, INVALID_OPERATION);

        ++mDequeueInputTimeoutGeneration;
        mDequeueInputReplyID = 0;
        mFlags &= ~kFlagDequeueInputPending;
    }

    if (mFlags & kFlagDequeueOutputPending) {
        PostReplyWithError(mDequeueOutputReplyID, INVALID_OPERATION);

        ++mDequeueOutputTimeoutGeneration;
        mDequeueOutputReplyID = 0;
        mFlags &= ~kFlagDequeueOutputPending;
    }
}

bool MediaCodec::handleDequeueInputBuffer(uint32_t replyID, bool newRequest) {
    if (!isExecuting() || (mFlags & kFlagIsAsync)
            || (newRequest && (mFlags & kFlagDequeueInputPending))) {
        PostReplyWithError(replyID, INVALID_OPERATION);
        return true;
    } else if (mFlags & kFlagStickyError) {
        PostReplyWithError(replyID, getStickyError());
        return true;
    }

    ssize_t index = dequeuePortBuffer(kPortIndexInput);

    if (index < 0) {
        CHECK_EQ(index, -EAGAIN);
        return false;
    }

    sp<AMessage> response = new AMessage;
    response->setSize("index", index);
    response->postReply(replyID);

    return true;
}

bool MediaCodec::handleDequeueOutputBuffer(uint32_t replyID, bool newRequest) {
    sp<AMessage> response = new AMessage;

    if (!isExecuting() || (mFlags & kFlagIsAsync)
            || (newRequest && (mFlags & kFlagDequeueOutputPending))) {
        response->setInt32("err", INVALID_OPERATION);
    } else if (mFlags & kFlagStickyError) {
        response->setInt32("err", getStickyError());
    } else if (mFlags & kFlagOutputBuffersChanged) {
        response->setInt32("err", INFO_OUTPUT_BUFFERS_CHANGED);
        mFlags &= ~kFlagOutputBuffersChanged;
    } else if (mFlags & kFlagOutputFormatChanged) {
        response->setInt32("err", INFO_FORMAT_CHANGED);
        mFlags &= ~kFlagOutputFormatChanged;
    } else {
        ssize_t index = dequeuePortBuffer(kPortIndexOutput);

        if (index < 0) {
            CHECK_EQ(index, -EAGAIN);
            return false;
        }

        const sp<ABuffer> &buffer =
            mPortBuffers[kPortIndexOutput].itemAt(index).mData;

        response->setSize("index", index);
        response->setSize("offset", buffer->offset());
        response->setSize("size", buffer->size());

        int64_t timeUs;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        response->setInt64("timeUs", timeUs);

        int32_t omxFlags;
        CHECK(buffer->meta()->findInt32("omxFlags", &omxFlags));

        uint32_t flags = 0;
        if (omxFlags & OMX_BUFFERFLAG_SYNCFRAME) {
            flags |= BUFFER_FLAG_SYNCFRAME;
        }
        if (omxFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            flags |= BUFFER_FLAG_CODECCONFIG;
        }
        if (omxFlags & OMX_BUFFERFLAG_EOS) {
            flags |= BUFFER_FLAG_EOS;
        }

        response->setInt32("flags", flags);
    }

    response->postReply(replyID);

    return true;
}

void MediaCodec::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatCodecNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case CodecBase::kWhatError:
                {
                    int32_t err, actionCode;
                    CHECK(msg->findInt32("err", &err));
                    CHECK(msg->findInt32("actionCode", &actionCode));

                    ALOGE("Codec reported err %#x, actionCode %d, while in state %d",
                            err, actionCode, mState);
                    if (err == DEAD_OBJECT) {
                        mFlags |= kFlagSawMediaServerDie;
                    }

                    bool sendErrorResponse = true;

                    switch (mState) {
                        case INITIALIZING:
                        {
                            setState(UNINITIALIZED);
                            break;
                        }

                        case CONFIGURING:
                        {
                            setState(actionCode == ACTION_CODE_FATAL ?
                                    UNINITIALIZED : INITIALIZED);
                            break;
                        }

                        case STARTING:
                        {
                            setState(actionCode == ACTION_CODE_FATAL ?
                                    UNINITIALIZED : CONFIGURED);
                            break;
                        }

                        case STOPPING:
                        case RELEASING:
                        {
                            // Ignore the error, assuming we'll still get
                            // the shutdown complete notification.

                            sendErrorResponse = false;

                            if (mFlags & kFlagSawMediaServerDie) {
                                // MediaServer died, there definitely won't
                                // be a shutdown complete notification after
                                // all.

                                // note that we're directly going from
                                // STOPPING->UNINITIALIZED, instead of the
                                // usual STOPPING->INITIALIZED state.
                                setState(UNINITIALIZED);

                                (new AMessage)->postReply(mReplyID);
                            }
                            break;
                        }

                        case FLUSHING:
                        {
                            if (actionCode == ACTION_CODE_FATAL) {
                                setState(UNINITIALIZED);
                            } else {
                                setState(
                                        (mFlags & kFlagIsAsync) ? FLUSHED : STARTED);
                            }
                            break;
                        }

                        case FLUSHED:
                        case STARTED:
                        {
                            sendErrorResponse = false;

                            setStickyError(err);
                            postActivityNotificationIfPossible();

                            cancelPendingDequeueOperations();

                            if (mFlags & kFlagIsAsync) {
                                onError(err, actionCode);
                            }
                            switch (actionCode) {
                            case ACTION_CODE_TRANSIENT:
                                break;
                            case ACTION_CODE_RECOVERABLE:
                                setState(INITIALIZED);
                                break;
                            default:
                                setState(UNINITIALIZED);
                                break;
                            }
                            break;
                        }

                        default:
                        {
                            sendErrorResponse = false;

                            setStickyError(err);
                            postActivityNotificationIfPossible();

                            // actionCode in an uninitialized state is always fatal.
                            if (mState == UNINITIALIZED) {
                                actionCode = ACTION_CODE_FATAL;
                            }
                            if (mFlags & kFlagIsAsync) {
                                onError(err, actionCode);
                            }
                            switch (actionCode) {
                            case ACTION_CODE_TRANSIENT:
                                break;
                            case ACTION_CODE_RECOVERABLE:
                                setState(INITIALIZED);
                                break;
                            default:
                                setState(UNINITIALIZED);
                                break;
                            }
                            break;
                        }
                    }

                    if (sendErrorResponse) {
                        PostReplyWithError(mReplyID, err);
                    }
                    break;
                }

                case CodecBase::kWhatComponentAllocated:
                {
                    CHECK_EQ(mState, INITIALIZING);
                    setState(INITIALIZED);

                    CHECK(msg->findString("componentName", &mComponentName));

                    if (mComponentName.startsWith("OMX.google.")) {
                        mFlags |= kFlagIsSoftwareCodec;
                    } else {
                        mFlags &= ~kFlagIsSoftwareCodec;
                    }

                    if (mComponentName.endsWith(".secure")) {
                        mFlags |= kFlagIsSecure;
                    } else {
                        mFlags &= ~kFlagIsSecure;
                    }

                    (new AMessage)->postReply(mReplyID);
                    break;
                }

                case CodecBase::kWhatComponentConfigured:
                {
                    CHECK_EQ(mState, CONFIGURING);

                    // reset input surface flag
                    mHaveInputSurface = false;

                    CHECK(msg->findMessage("input-format", &mInputFormat));
                    CHECK(msg->findMessage("output-format", &mOutputFormat));

                    setState(CONFIGURED);
                    (new AMessage)->postReply(mReplyID);
                    break;
                }

                case CodecBase::kWhatInputSurfaceCreated:
                {
                    // response to initiateCreateInputSurface()
                    status_t err = NO_ERROR;
                    sp<AMessage> response = new AMessage();
                    if (!msg->findInt32("err", &err)) {
                        sp<RefBase> obj;
                        msg->findObject("input-surface", &obj);
                        CHECK(obj != NULL);
                        response->setObject("input-surface", obj);
                        mHaveInputSurface = true;
                    } else {
                        response->setInt32("err", err);
                    }
                    response->postReply(mReplyID);
                    break;
                }

                case CodecBase::kWhatSignaledInputEOS:
                {
                    // response to signalEndOfInputStream()
                    sp<AMessage> response = new AMessage();
                    status_t err;
                    if (msg->findInt32("err", &err)) {
                        response->setInt32("err", err);
                    }
                    response->postReply(mReplyID);
                    break;
                }


                case CodecBase::kWhatBuffersAllocated:
                {
                    Mutex::Autolock al(mBufferLock);
                    int32_t portIndex;
                    CHECK(msg->findInt32("portIndex", &portIndex));

                    ALOGV("%s buffers allocated",
                          portIndex == kPortIndexInput ? "input" : "output");

                    CHECK(portIndex == kPortIndexInput
                            || portIndex == kPortIndexOutput);

                    mPortBuffers[portIndex].clear();

                    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

                    sp<RefBase> obj;
                    CHECK(msg->findObject("portDesc", &obj));

                    sp<CodecBase::PortDescription> portDesc =
                        static_cast<CodecBase::PortDescription *>(obj.get());

                    size_t numBuffers = portDesc->countBuffers();

                    for (size_t i = 0; i < numBuffers; ++i) {
                        BufferInfo info;
                        info.mBufferID = portDesc->bufferIDAt(i);
                        info.mOwnedByClient = false;
                        info.mData = portDesc->bufferAt(i);

                        if (portIndex == kPortIndexInput && mCrypto != NULL) {
                            info.mEncryptedData =
                                new ABuffer(info.mData->capacity());
                        }

                        buffers->push_back(info);
                    }

                    if (portIndex == kPortIndexOutput) {
                        if (mState == STARTING) {
                            // We're always allocating output buffers after
                            // allocating input buffers, so this is a good
                            // indication that now all buffers are allocated.
                            setState(STARTED);
                            (new AMessage)->postReply(mReplyID);
                        } else {
                            mFlags |= kFlagOutputBuffersChanged;
                            postActivityNotificationIfPossible();
                        }
                    }
                    break;
                }

                case CodecBase::kWhatOutputFormatChanged:
                {
                    ALOGV("codec output format changed");

                    if (mSoftRenderer == NULL &&
                            mNativeWindow != NULL &&
                            (mFlags & kFlagIsSoftwareCodec)) {
                        AString mime;
                        CHECK(msg->findString("mime", &mime));

                        if (mime.startsWithIgnoreCase("video/")) {
                            mSoftRenderer = new SoftwareRenderer(mNativeWindow);
                        }
                    }

                    mOutputFormat = msg;

                    if (mFlags & kFlagIsEncoder) {
                        // Before we announce the format change we should
                        // collect codec specific data and amend the output
                        // format as necessary.
                        mFlags |= kFlagGatherCodecSpecificData;
                    } else if (mFlags & kFlagIsAsync) {
                        onOutputFormatChanged();
                    } else {
                        mFlags |= kFlagOutputFormatChanged;
                        postActivityNotificationIfPossible();
                    }
                    break;
                }

                case CodecBase::kWhatFillThisBuffer:
                {
                    /* size_t index = */updateBuffers(kPortIndexInput, msg);

                    if (mState == FLUSHING
                            || mState == STOPPING
                            || mState == RELEASING) {
                        returnBuffersToCodecOnPort(kPortIndexInput);
                        break;
                    }

                    if (!mCSD.empty()) {
                        ssize_t index = dequeuePortBuffer(kPortIndexInput);
                        CHECK_GE(index, 0);

                        // If codec specific data had been specified as
                        // part of the format in the call to configure and
                        // if there's more csd left, we submit it here
                        // clients only get access to input buffers once
                        // this data has been exhausted.

                        status_t err = queueCSDInputBuffer(index);

                        if (err != OK) {
                            ALOGE("queueCSDInputBuffer failed w/ error %d",
                                  err);

                            setStickyError(err);
                            postActivityNotificationIfPossible();

                            cancelPendingDequeueOperations();
                        }
                        break;
                    }

                    if (mFlags & kFlagIsAsync) {
                        onInputBufferAvailable();
                    } else if (mFlags & kFlagDequeueInputPending) {
                        CHECK(handleDequeueInputBuffer(mDequeueInputReplyID));

                        ++mDequeueInputTimeoutGeneration;
                        mFlags &= ~kFlagDequeueInputPending;
                        mDequeueInputReplyID = 0;
                    } else {
                        postActivityNotificationIfPossible();
                    }
                    break;
                }

                case CodecBase::kWhatDrainThisBuffer:
                {
                    /* size_t index = */updateBuffers(kPortIndexOutput, msg);

                    if (mState == FLUSHING
                            || mState == STOPPING
                            || mState == RELEASING) {
                        returnBuffersToCodecOnPort(kPortIndexOutput);
                        break;
                    }

                    sp<ABuffer> buffer;
                    CHECK(msg->findBuffer("buffer", &buffer));

                    int32_t omxFlags;
                    CHECK(msg->findInt32("flags", &omxFlags));

                    buffer->meta()->setInt32("omxFlags", omxFlags);

                    if (mFlags & kFlagGatherCodecSpecificData) {
                        // This is the very first output buffer after a
                        // format change was signalled, it'll either contain
                        // the one piece of codec specific data we can expect
                        // or there won't be codec specific data.
                        if (omxFlags & OMX_BUFFERFLAG_CODECCONFIG) {
                            status_t err =
                                amendOutputFormatWithCodecSpecificData(buffer);

                            if (err != OK) {
                                ALOGE("Codec spit out malformed codec "
                                      "specific data!");
                            }
                        }

                        mFlags &= ~kFlagGatherCodecSpecificData;
                        if (mFlags & kFlagIsAsync) {
                            onOutputFormatChanged();
                        } else {
                            mFlags |= kFlagOutputFormatChanged;
                        }
                    }

                    if (mFlags & kFlagIsAsync) {
                        onOutputBufferAvailable();
                    } else if (mFlags & kFlagDequeueOutputPending) {
                        CHECK(handleDequeueOutputBuffer(mDequeueOutputReplyID));

                        ++mDequeueOutputTimeoutGeneration;
                        mFlags &= ~kFlagDequeueOutputPending;
                        mDequeueOutputReplyID = 0;
                    } else {
                        postActivityNotificationIfPossible();
                    }

                    break;
                }

                case CodecBase::kWhatEOS:
                {
                    // We already notify the client of this by using the
                    // corresponding flag in "onOutputBufferReady".
                    break;
                }

                case CodecBase::kWhatShutdownCompleted:
                {
                    if (mState == STOPPING) {
                        setState(INITIALIZED);
                    } else {
                        CHECK_EQ(mState, RELEASING);
                        setState(UNINITIALIZED);
                    }

                    (new AMessage)->postReply(mReplyID);
                    break;
                }

                case CodecBase::kWhatFlushCompleted:
                {
                    if (mState != FLUSHING) {
                        ALOGW("received FlushCompleted message in state %d",
                                mState);
                        break;
                    }

                    if (mFlags & kFlagIsAsync) {
                        setState(FLUSHED);
                    } else {
                        setState(STARTED);
                        mCodec->signalResume();
                    }

                    (new AMessage)->postReply(mReplyID);
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatInit:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mState != UNINITIALIZED) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            mReplyID = replyID;
            setState(INITIALIZING);

            AString name;
            CHECK(msg->findString("name", &name));

            int32_t nameIsType;
            int32_t encoder = false;
            CHECK(msg->findInt32("nameIsType", &nameIsType));
            if (nameIsType) {
                CHECK(msg->findInt32("encoder", &encoder));
            }

            sp<AMessage> format = new AMessage;

            if (nameIsType) {
                format->setString("mime", name.c_str());
                format->setInt32("encoder", encoder);
            } else {
                format->setString("componentName", name.c_str());
            }

            mCodec->initiateAllocateComponent(format);
            break;
        }

        case kWhatSetCallback:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mState == UNINITIALIZED
                    || mState == INITIALIZING
                    || isExecuting()) {
                // callback can't be set after codec is executing,
                // or before it's initialized (as the callback
                // will be cleared when it goes to INITIALIZED)
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            sp<AMessage> callback;
            CHECK(msg->findMessage("callback", &callback));

            mCallback = callback;

            if (mCallback != NULL) {
                ALOGI("MediaCodec will operate in async mode");
                mFlags |= kFlagIsAsync;
            } else {
                mFlags &= ~kFlagIsAsync;
            }

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
            break;
        }

        case kWhatConfigure:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mState != INITIALIZED) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            sp<RefBase> obj;
            if (!msg->findObject("native-window", &obj)) {
                obj.clear();
            }

            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));

            if (obj != NULL) {
                format->setObject("native-window", obj);

                status_t err = setNativeWindow(
                    static_cast<NativeWindowWrapper *>(obj.get())
                        ->getSurfaceTextureClient());

                if (err != OK) {
                    PostReplyWithError(replyID, err);
                    break;
                }
            } else {
                setNativeWindow(NULL);
            }

            mReplyID = replyID;
            setState(CONFIGURING);

            void *crypto;
            if (!msg->findPointer("crypto", &crypto)) {
                crypto = NULL;
            }

            mCrypto = static_cast<ICrypto *>(crypto);

            uint32_t flags;
            CHECK(msg->findInt32("flags", (int32_t *)&flags));

            if (flags & CONFIGURE_FLAG_ENCODE) {
                format->setInt32("encoder", true);
                mFlags |= kFlagIsEncoder;
            }

            extractCSD(format);

            mCodec->initiateConfigureComponent(format);
            break;
        }

        case kWhatCreateInputSurface:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            // Must be configured, but can't have been started yet.
            if (mState != CONFIGURED) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            mReplyID = replyID;
            mCodec->initiateCreateInputSurface();
            break;
        }

        case kWhatStart:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mState == FLUSHED) {
                mCodec->signalResume();
                PostReplyWithError(replyID, OK);
            } else if (mState != CONFIGURED) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            mReplyID = replyID;
            setState(STARTING);

            mCodec->initiateStart();
            break;
        }

        case kWhatStop:
        case kWhatRelease:
        {
            State targetState =
                (msg->what() == kWhatStop) ? INITIALIZED : UNINITIALIZED;

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mState != INITIALIZED
                    && mState != CONFIGURED && !isExecuting()) {
                // We may be in "UNINITIALIZED" state already without the
                // client being aware of this if media server died while
                // we were being stopped. The client would assume that
                // after stop() returned, it would be safe to call release()
                // and it should be in this case, no harm to allow a release()
                // if we're already uninitialized.
                // Similarly stopping a stopped MediaCodec should be benign.
                sp<AMessage> response = new AMessage;
                response->setInt32(
                        "err",
                        mState == targetState ? OK : INVALID_OPERATION);

                response->postReply(replyID);
                break;
            }

            if (mFlags & kFlagSawMediaServerDie) {
                // It's dead, Jim. Don't expect initiateShutdown to yield
                // any useful results now...
                setState(UNINITIALIZED);
                (new AMessage)->postReply(replyID);
                break;
            }

            mReplyID = replyID;
            setState(msg->what() == kWhatStop ? STOPPING : RELEASING);

            mCodec->initiateShutdown(
                    msg->what() == kWhatStop /* keepComponentAllocated */);

            returnBuffersToCodec();
            break;
        }

        case kWhatDequeueInputBuffer:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mFlags & kFlagIsAsync) {
                ALOGE("dequeueOutputBuffer can't be used in async mode");
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            if (mHaveInputSurface) {
                ALOGE("dequeueInputBuffer can't be used with input surface");
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            if (handleDequeueInputBuffer(replyID, true /* new request */)) {
                break;
            }

            int64_t timeoutUs;
            CHECK(msg->findInt64("timeoutUs", &timeoutUs));

            if (timeoutUs == 0ll) {
                PostReplyWithError(replyID, -EAGAIN);
                break;
            }

            mFlags |= kFlagDequeueInputPending;
            mDequeueInputReplyID = replyID;

            if (timeoutUs > 0ll) {
                sp<AMessage> timeoutMsg =
                    new AMessage(kWhatDequeueInputTimedOut, id());
                timeoutMsg->setInt32(
                        "generation", ++mDequeueInputTimeoutGeneration);
                timeoutMsg->post(timeoutUs);
            }
            break;
        }

        case kWhatDequeueInputTimedOut:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mDequeueInputTimeoutGeneration) {
                // Obsolete
                break;
            }

            CHECK(mFlags & kFlagDequeueInputPending);

            PostReplyWithError(mDequeueInputReplyID, -EAGAIN);

            mFlags &= ~kFlagDequeueInputPending;
            mDequeueInputReplyID = 0;
            break;
        }

        case kWhatQueueInputBuffer:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (!isExecuting()) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            status_t err = onQueueInputBuffer(msg);

            PostReplyWithError(replyID, err);
            break;
        }

        case kWhatDequeueOutputBuffer:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mFlags & kFlagIsAsync) {
                ALOGE("dequeueOutputBuffer can't be used in async mode");
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            if (handleDequeueOutputBuffer(replyID, true /* new request */)) {
                break;
            }

            int64_t timeoutUs;
            CHECK(msg->findInt64("timeoutUs", &timeoutUs));

            if (timeoutUs == 0ll) {
                PostReplyWithError(replyID, -EAGAIN);
                break;
            }

            mFlags |= kFlagDequeueOutputPending;
            mDequeueOutputReplyID = replyID;

            if (timeoutUs > 0ll) {
                sp<AMessage> timeoutMsg =
                    new AMessage(kWhatDequeueOutputTimedOut, id());
                timeoutMsg->setInt32(
                        "generation", ++mDequeueOutputTimeoutGeneration);
                timeoutMsg->post(timeoutUs);
            }
            break;
        }

        case kWhatDequeueOutputTimedOut:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mDequeueOutputTimeoutGeneration) {
                // Obsolete
                break;
            }

            CHECK(mFlags & kFlagDequeueOutputPending);

            PostReplyWithError(mDequeueOutputReplyID, -EAGAIN);

            mFlags &= ~kFlagDequeueOutputPending;
            mDequeueOutputReplyID = 0;
            break;
        }

        case kWhatReleaseOutputBuffer:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (!isExecuting()) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            status_t err = onReleaseOutputBuffer(msg);

            PostReplyWithError(replyID, err);
            break;
        }

        case kWhatSignalEndOfInputStream:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (!isExecuting()) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            mReplyID = replyID;
            mCodec->signalEndOfInputStream();
            break;
        }

        case kWhatGetBuffers:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (!isExecuting() || (mFlags & kFlagIsAsync)) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            int32_t portIndex;
            CHECK(msg->findInt32("portIndex", &portIndex));

            Vector<sp<ABuffer> > *dstBuffers;
            CHECK(msg->findPointer("buffers", (void **)&dstBuffers));

            dstBuffers->clear();
            const Vector<BufferInfo> &srcBuffers = mPortBuffers[portIndex];

            for (size_t i = 0; i < srcBuffers.size(); ++i) {
                const BufferInfo &info = srcBuffers.itemAt(i);

                dstBuffers->push_back(
                        (portIndex == kPortIndexInput && mCrypto != NULL)
                                ? info.mEncryptedData : info.mData);
            }

            (new AMessage)->postReply(replyID);
            break;
        }

        case kWhatFlush:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (!isExecuting()) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            mReplyID = replyID;
            // TODO: skip flushing if already FLUSHED
            setState(FLUSHING);

            mCodec->signalFlush();
            returnBuffersToCodec();
            break;
        }

        case kWhatGetInputFormat:
        case kWhatGetOutputFormat:
        {
            sp<AMessage> format =
                (msg->what() == kWhatGetOutputFormat ? mOutputFormat : mInputFormat);

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if ((mState != CONFIGURED && mState != STARTING &&
                 mState != STARTED && mState != FLUSHING &&
                 mState != FLUSHED)
                    || format == NULL) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            } else if (mFlags & kFlagStickyError) {
                PostReplyWithError(replyID, getStickyError());
                break;
            }

            sp<AMessage> response = new AMessage;
            response->setMessage("format", format);
            response->postReply(replyID);
            break;
        }

        case kWhatRequestIDRFrame:
        {
            mCodec->signalRequestIDRFrame();
            break;
        }

        case kWhatRequestActivityNotification:
        {
            CHECK(mActivityNotify == NULL);
            CHECK(msg->findMessage("notify", &mActivityNotify));

            postActivityNotificationIfPossible();
            break;
        }

        case kWhatGetName:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mComponentName.empty()) {
                PostReplyWithError(replyID, INVALID_OPERATION);
                break;
            }

            sp<AMessage> response = new AMessage;
            response->setString("name", mComponentName.c_str());
            response->postReply(replyID);
            break;
        }

        case kWhatSetParameters:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            sp<AMessage> params;
            CHECK(msg->findMessage("params", &params));

            status_t err = onSetParameters(params);

            PostReplyWithError(replyID, err);
            break;
        }

        default:
            TRESPASS();
    }
}

void MediaCodec::extractCSD(const sp<AMessage> &format) {
    mCSD.clear();

    size_t i = 0;
    for (;;) {
        sp<ABuffer> csd;
        if (!format->findBuffer(StringPrintf("csd-%u", i).c_str(), &csd)) {
            break;
        }

        mCSD.push_back(csd);
        ++i;
    }

    ALOGV("Found %zu pieces of codec specific data.", mCSD.size());
}

status_t MediaCodec::queueCSDInputBuffer(size_t bufferIndex) {
    CHECK(!mCSD.empty());

    const BufferInfo *info =
        &mPortBuffers[kPortIndexInput].itemAt(bufferIndex);

    sp<ABuffer> csd = *mCSD.begin();
    mCSD.erase(mCSD.begin());

    const sp<ABuffer> &codecInputData =
        (mCrypto != NULL) ? info->mEncryptedData : info->mData;

    if (csd->size() > codecInputData->capacity()) {
        return -EINVAL;
    }

    memcpy(codecInputData->data(), csd->data(), csd->size());

    AString errorDetailMsg;

    sp<AMessage> msg = new AMessage(kWhatQueueInputBuffer, id());
    msg->setSize("index", bufferIndex);
    msg->setSize("offset", 0);
    msg->setSize("size", csd->size());
    msg->setInt64("timeUs", 0ll);
    msg->setInt32("flags", BUFFER_FLAG_CODECCONFIG);
    msg->setPointer("errorDetailMsg", &errorDetailMsg);

    return onQueueInputBuffer(msg);
}

void MediaCodec::setState(State newState) {
    if (newState == INITIALIZED || newState == UNINITIALIZED) {
        delete mSoftRenderer;
        mSoftRenderer = NULL;

        mCrypto.clear();
        setNativeWindow(NULL);

        mInputFormat.clear();
        mOutputFormat.clear();
        mFlags &= ~kFlagOutputFormatChanged;
        mFlags &= ~kFlagOutputBuffersChanged;
        mFlags &= ~kFlagStickyError;
        mFlags &= ~kFlagIsEncoder;
        mFlags &= ~kFlagGatherCodecSpecificData;
        mFlags &= ~kFlagIsAsync;
        mStickyError = OK;

        mActivityNotify.clear();
        mCallback.clear();
    }

    if (newState == UNINITIALIZED) {
        // return any straggling buffers, e.g. if we got here on an error
        returnBuffersToCodec();

        mComponentName.clear();

        // The component is gone, mediaserver's probably back up already
        // but should definitely be back up should we try to instantiate
        // another component.. and the cycle continues.
        mFlags &= ~kFlagSawMediaServerDie;
    }

    mState = newState;

    cancelPendingDequeueOperations();

    updateBatteryStat();
}

void MediaCodec::returnBuffersToCodec() {
    returnBuffersToCodecOnPort(kPortIndexInput);
    returnBuffersToCodecOnPort(kPortIndexOutput);
}

void MediaCodec::returnBuffersToCodecOnPort(int32_t portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
    Mutex::Autolock al(mBufferLock);

    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);

        if (info->mNotify != NULL) {
            sp<AMessage> msg = info->mNotify;
            info->mNotify = NULL;
            info->mOwnedByClient = false;

            if (portIndex == kPortIndexInput) {
                /* no error, just returning buffers */
                msg->setInt32("err", OK);
            }
            msg->post();
        }
    }

    mAvailPortBuffers[portIndex].clear();
}

size_t MediaCodec::updateBuffers(
        int32_t portIndex, const sp<AMessage> &msg) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    uint32_t bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));

    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);

        if (info->mBufferID == bufferID) {
            CHECK(info->mNotify == NULL);
            CHECK(msg->findMessage("reply", &info->mNotify));

            info->mFormat =
                (portIndex == kPortIndexInput) ? mInputFormat : mOutputFormat;
            mAvailPortBuffers[portIndex].push_back(i);

            return i;
        }
    }

    TRESPASS();

    return 0;
}

status_t MediaCodec::onQueueInputBuffer(const sp<AMessage> &msg) {
    size_t index;
    size_t offset;
    size_t size;
    int64_t timeUs;
    uint32_t flags;
    CHECK(msg->findSize("index", &index));
    CHECK(msg->findSize("offset", &offset));
    CHECK(msg->findInt64("timeUs", &timeUs));
    CHECK(msg->findInt32("flags", (int32_t *)&flags));

    const CryptoPlugin::SubSample *subSamples;
    size_t numSubSamples;
    const uint8_t *key;
    const uint8_t *iv;
    CryptoPlugin::Mode mode = CryptoPlugin::kMode_Unencrypted;

    // We allow the simpler queueInputBuffer API to be used even in
    // secure mode, by fabricating a single unencrypted subSample.
    CryptoPlugin::SubSample ss;

    if (msg->findSize("size", &size)) {
        if (mCrypto != NULL) {
            ss.mNumBytesOfClearData = size;
            ss.mNumBytesOfEncryptedData = 0;

            subSamples = &ss;
            numSubSamples = 1;
            key = NULL;
            iv = NULL;
        }
    } else {
        if (mCrypto == NULL) {
            return -EINVAL;
        }

        CHECK(msg->findPointer("subSamples", (void **)&subSamples));
        CHECK(msg->findSize("numSubSamples", &numSubSamples));
        CHECK(msg->findPointer("key", (void **)&key));
        CHECK(msg->findPointer("iv", (void **)&iv));

        int32_t tmp;
        CHECK(msg->findInt32("mode", &tmp));

        mode = (CryptoPlugin::Mode)tmp;

        size = 0;
        for (size_t i = 0; i < numSubSamples; ++i) {
            size += subSamples[i].mNumBytesOfClearData;
            size += subSamples[i].mNumBytesOfEncryptedData;
        }
    }

    if (index >= mPortBuffers[kPortIndexInput].size()) {
        return -ERANGE;
    }

    BufferInfo *info = &mPortBuffers[kPortIndexInput].editItemAt(index);

    if (info->mNotify == NULL || !info->mOwnedByClient) {
        return -EACCES;
    }

    if (offset + size > info->mData->capacity()) {
        return -EINVAL;
    }

    sp<AMessage> reply = info->mNotify;
    info->mData->setRange(offset, size);
    info->mData->meta()->setInt64("timeUs", timeUs);

    if (flags & BUFFER_FLAG_EOS) {
        info->mData->meta()->setInt32("eos", true);
    }

    if (flags & BUFFER_FLAG_CODECCONFIG) {
        info->mData->meta()->setInt32("csd", true);
    }

    if (mCrypto != NULL) {
        if (size > info->mEncryptedData->capacity()) {
            return -ERANGE;
        }

        AString *errorDetailMsg;
        CHECK(msg->findPointer("errorDetailMsg", (void **)&errorDetailMsg));

        ssize_t result = mCrypto->decrypt(
                (mFlags & kFlagIsSecure) != 0,
                key,
                iv,
                mode,
                info->mEncryptedData->base() + offset,
                subSamples,
                numSubSamples,
                info->mData->base(),
                errorDetailMsg);

        if (result < 0) {
            return result;
        }

        info->mData->setRange(0, result);
    }

    // synchronization boundary for getBufferAndFormat
    {
        Mutex::Autolock al(mBufferLock);
        info->mOwnedByClient = false;
    }
    reply->setBuffer("buffer", info->mData);
    reply->post();

    info->mNotify = NULL;

    return OK;
}

status_t MediaCodec::onReleaseOutputBuffer(const sp<AMessage> &msg) {
    size_t index;
    CHECK(msg->findSize("index", &index));

    int32_t render;
    if (!msg->findInt32("render", &render)) {
        render = 0;
    }

    if (!isExecuting()) {
        return -EINVAL;
    }

    if (index >= mPortBuffers[kPortIndexOutput].size()) {
        return -ERANGE;
    }

    BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(index);

    if (info->mNotify == NULL || !info->mOwnedByClient) {
        return -EACCES;
    }

    // synchronization boundary for getBufferAndFormat
    {
        Mutex::Autolock al(mBufferLock);
        info->mOwnedByClient = false;
    }

    if (render && info->mData != NULL && info->mData->size() != 0) {
        info->mNotify->setInt32("render", true);

        int64_t timestampNs = 0;
        if (msg->findInt64("timestampNs", &timestampNs)) {
            info->mNotify->setInt64("timestampNs", timestampNs);
        } else {
            // TODO: it seems like we should use the timestamp
            // in the (media)buffer as it potentially came from
            // an input surface, but we did not propagate it prior to
            // API 20.  Perhaps check for target SDK version.
#if 0
            if (info->mData->meta()->findInt64("timeUs", &timestampNs)) {
                ALOGV("using buffer PTS of %" PRId64, timestampNs);
                timestampNs *= 1000;
            }
#endif
        }

        if (mSoftRenderer != NULL) {
            mSoftRenderer->render(
                    info->mData->data(), info->mData->size(),
                    timestampNs, NULL, info->mFormat);
        }
    }

    info->mNotify->post();
    info->mNotify = NULL;

    return OK;
}

ssize_t MediaCodec::dequeuePortBuffer(int32_t portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    List<size_t> *availBuffers = &mAvailPortBuffers[portIndex];

    if (availBuffers->empty()) {
        return -EAGAIN;
    }

    size_t index = *availBuffers->begin();
    availBuffers->erase(availBuffers->begin());

    BufferInfo *info = &mPortBuffers[portIndex].editItemAt(index);
    CHECK(!info->mOwnedByClient);
    {
        Mutex::Autolock al(mBufferLock);
        info->mOwnedByClient = true;

        // set image-data
        if (info->mFormat != NULL) {
            sp<ABuffer> imageData;
            if (info->mFormat->findBuffer("image-data", &imageData)) {
                info->mData->meta()->setBuffer("image-data", imageData);
            }
            int32_t left, top, right, bottom;
            if (info->mFormat->findRect("crop", &left, &top, &right, &bottom)) {
                info->mData->meta()->setRect("crop-rect", left, top, right, bottom);
            }
        }
    }

    return index;
}

status_t MediaCodec::setNativeWindow(
        const sp<Surface> &surfaceTextureClient) {
    status_t err;

    if (mNativeWindow != NULL) {
        err = native_window_api_disconnect(
                mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);

        if (err != OK) {
            ALOGW("native_window_api_disconnect returned an error: %s (%d)",
                    strerror(-err), err);
        }

        mNativeWindow.clear();
    }

    if (surfaceTextureClient != NULL) {
        err = native_window_api_connect(
                surfaceTextureClient.get(), NATIVE_WINDOW_API_MEDIA);

        if (err != OK) {
            ALOGE("native_window_api_connect returned an error: %s (%d)",
                    strerror(-err), err);

            return err;
        }

        mNativeWindow = surfaceTextureClient;
    }

    return OK;
}

void MediaCodec::onInputBufferAvailable() {
    int32_t index;
    while ((index = dequeuePortBuffer(kPortIndexInput)) >= 0) {
        sp<AMessage> msg = mCallback->dup();
        msg->setInt32("callbackID", CB_INPUT_AVAILABLE);
        msg->setInt32("index", index);
        msg->post();
    }
}

void MediaCodec::onOutputBufferAvailable() {
    int32_t index;
    while ((index = dequeuePortBuffer(kPortIndexOutput)) >= 0) {
        const sp<ABuffer> &buffer =
            mPortBuffers[kPortIndexOutput].itemAt(index).mData;
        sp<AMessage> msg = mCallback->dup();
        msg->setInt32("callbackID", CB_OUTPUT_AVAILABLE);
        msg->setInt32("index", index);
        msg->setSize("offset", buffer->offset());
        msg->setSize("size", buffer->size());

        int64_t timeUs;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        msg->setInt64("timeUs", timeUs);

        int32_t omxFlags;
        CHECK(buffer->meta()->findInt32("omxFlags", &omxFlags));

        uint32_t flags = 0;
        if (omxFlags & OMX_BUFFERFLAG_SYNCFRAME) {
            flags |= BUFFER_FLAG_SYNCFRAME;
        }
        if (omxFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            flags |= BUFFER_FLAG_CODECCONFIG;
        }
        if (omxFlags & OMX_BUFFERFLAG_EOS) {
            flags |= BUFFER_FLAG_EOS;
        }

        msg->setInt32("flags", flags);

        msg->post();
    }
}

void MediaCodec::onError(status_t err, int32_t actionCode, const char *detail) {
    if (mCallback != NULL) {
        sp<AMessage> msg = mCallback->dup();
        msg->setInt32("callbackID", CB_ERROR);
        msg->setInt32("err", err);
        msg->setInt32("actionCode", actionCode);

        if (detail != NULL) {
            msg->setString("detail", detail);
        }

        msg->post();
    }
}

void MediaCodec::onOutputFormatChanged() {
    if (mCallback != NULL) {
        sp<AMessage> msg = mCallback->dup();
        msg->setInt32("callbackID", CB_OUTPUT_FORMAT_CHANGED);
        msg->setMessage("format", mOutputFormat);
        msg->post();
    }
}


void MediaCodec::postActivityNotificationIfPossible() {
    if (mActivityNotify == NULL) {
        return;
    }

    if ((mFlags & (kFlagStickyError
                    | kFlagOutputBuffersChanged
                    | kFlagOutputFormatChanged))
            || !mAvailPortBuffers[kPortIndexInput].empty()
            || !mAvailPortBuffers[kPortIndexOutput].empty()) {
        mActivityNotify->post();
        mActivityNotify.clear();
    }
}

status_t MediaCodec::setParameters(const sp<AMessage> &params) {
    sp<AMessage> msg = new AMessage(kWhatSetParameters, id());
    msg->setMessage("params", params);

    sp<AMessage> response;
    return PostAndAwaitResponse(msg, &response);
}

status_t MediaCodec::onSetParameters(const sp<AMessage> &params) {
    mCodec->signalSetParameters(params);

    return OK;
}

status_t MediaCodec::amendOutputFormatWithCodecSpecificData(
        const sp<ABuffer> &buffer) {
    AString mime;
    CHECK(mOutputFormat->findString("mime", &mime));

    if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_VIDEO_AVC)) {
        // Codec specific data should be SPS and PPS in a single buffer,
        // each prefixed by a startcode (0x00 0x00 0x00 0x01).
        // We separate the two and put them into the output format
        // under the keys "csd-0" and "csd-1".

        unsigned csdIndex = 0;

        const uint8_t *data = buffer->data();
        size_t size = buffer->size();

        const uint8_t *nalStart;
        size_t nalSize;
        while (getNextNALUnit(&data, &size, &nalStart, &nalSize, true) == OK) {
            sp<ABuffer> csd = new ABuffer(nalSize + 4);
            memcpy(csd->data(), "\x00\x00\x00\x01", 4);
            memcpy(csd->data() + 4, nalStart, nalSize);

            mOutputFormat->setBuffer(
                    StringPrintf("csd-%u", csdIndex).c_str(), csd);

            ++csdIndex;
        }

        if (csdIndex != 2) {
            return ERROR_MALFORMED;
        }
    } else {
        // For everything else we just stash the codec specific data into
        // the output format as a single piece of csd under "csd-0".
        mOutputFormat->setBuffer("csd-0", buffer);
    }

    return OK;
}

void MediaCodec::updateBatteryStat() {
    if (mState == CONFIGURED && !mBatteryStatNotified) {
        AString mime;
        CHECK(mOutputFormat != NULL &&
                mOutputFormat->findString("mime", &mime));

        mIsVideo = mime.startsWithIgnoreCase("video/");

        BatteryNotifier& notifier(BatteryNotifier::getInstance());

        if (mIsVideo) {
            notifier.noteStartVideo();
        } else {
            notifier.noteStartAudio();
        }

        mBatteryStatNotified = true;
    } else if (mState == UNINITIALIZED && mBatteryStatNotified) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());

        if (mIsVideo) {
            notifier.noteStopVideo();
        } else {
            notifier.noteStopAudio();
        }

        mBatteryStatNotified = false;
    }
}

}  // namespace android
