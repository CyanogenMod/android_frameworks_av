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
#define LOG_TAG "ACodec"

#ifdef __LP64__
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#endif

#include <inttypes.h>
#include <utils/Trace.h>

#include <gui/Surface.h>

#include <media/stagefright/ACodec.h>

#include <binder/MemoryDealer.h>

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>

#include <media/stagefright/BufferProducerWrapper.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/PersistentSurface.h>
#include <media/stagefright/SurfaceUtils.h>
#include <media/stagefright/FFMPEGSoftCodec.h>

#include <media/hardware/HardwareAPI.h>

#include <OMX_AudioExt.h>
#include <OMX_VideoExt.h>
#include <OMX_Component.h>
#include <OMX_IndexExt.h>
#include <OMX_AsString.h>

#ifdef USE_SAMSUNG_COLORFORMAT
#include <sec_format.h>
#endif

#include "include/avc_utils.h"
#include "include/DataConverter.h"
#include "omx/OMXUtils.h"

#include <stagefright/AVExtensions.h>

namespace android {

enum {
    kMaxIndicesToCheck = 32, // used when enumerating supported formats and profiles
};

// OMX errors are directly mapped into status_t range if
// there is no corresponding MediaError status code.
// Use the statusFromOMXError(int32_t omxError) function.
//
// Currently this is a direct map.
// See frameworks/native/include/media/openmax/OMX_Core.h
//
// Vendor OMX errors     from 0x90000000 - 0x9000FFFF
// Extension OMX errors  from 0x8F000000 - 0x90000000
// Standard OMX errors   from 0x80001000 - 0x80001024 (0x80001024 current)
//

// returns true if err is a recognized OMX error code.
// as OMX error is OMX_S32, this is an int32_t type
static inline bool isOMXError(int32_t err) {
    return (ERROR_CODEC_MIN <= err && err <= ERROR_CODEC_MAX);
}

// converts an OMX error to a status_t
static inline status_t statusFromOMXError(int32_t omxError) {
    switch (omxError) {
    case OMX_ErrorInvalidComponentName:
    case OMX_ErrorComponentNotFound:
        return NAME_NOT_FOUND; // can trigger illegal argument error for provided names.
    default:
        return isOMXError(omxError) ? omxError : 0; // no translation required
    }
}

// checks and converts status_t to a non-side-effect status_t
static inline status_t makeNoSideEffectStatus(status_t err) {
    switch (err) {
    // the following errors have side effects and may come
    // from other code modules. Remap for safety reasons.
    case INVALID_OPERATION:
    case DEAD_OBJECT:
        return UNKNOWN_ERROR;
    default:
        return err;
    }
}

struct MessageList : public RefBase {
    MessageList() {
    }
    virtual ~MessageList() {
    }
    std::list<sp<AMessage> > &getList() { return mList; }
private:
    std::list<sp<AMessage> > mList;

    DISALLOW_EVIL_CONSTRUCTORS(MessageList);
};

static sp<DataConverter> getCopyConverter() {
    static pthread_once_t once = PTHREAD_ONCE_INIT; // const-inited
    static sp<DataConverter> sCopyConverter;        // zero-inited
    pthread_once(&once, [](){ sCopyConverter = new DataConverter(); });
    return sCopyConverter;
}

struct CodecObserver : public BnOMXObserver {
    CodecObserver() {}

    void setNotificationMessage(const sp<AMessage> &msg) {
        mNotify = msg;
    }

    // from IOMXObserver
    virtual void onMessages(const std::list<omx_message> &messages) {
        if (messages.empty()) {
            return;
        }

        sp<AMessage> notify = mNotify->dup();
        bool first = true;
        sp<MessageList> msgList = new MessageList();
        for (std::list<omx_message>::const_iterator it = messages.cbegin();
              it != messages.cend(); ++it) {
            const omx_message &omx_msg = *it;
            if (first) {
                notify->setInt32("node", omx_msg.node);
                first = false;
            }

            sp<AMessage> msg = new AMessage;
            msg->setInt32("type", omx_msg.type);
            switch (omx_msg.type) {
                case omx_message::EVENT:
                {
                    msg->setInt32("event", omx_msg.u.event_data.event);
                    msg->setInt32("data1", omx_msg.u.event_data.data1);
                    msg->setInt32("data2", omx_msg.u.event_data.data2);
                    break;
                }

                case omx_message::EMPTY_BUFFER_DONE:
                {
                    msg->setInt32("buffer", omx_msg.u.buffer_data.buffer);
                    msg->setInt32("fence_fd", omx_msg.fenceFd);
                    break;
                }

                case omx_message::FILL_BUFFER_DONE:
                {
                    msg->setInt32(
                            "buffer", omx_msg.u.extended_buffer_data.buffer);
                    msg->setInt32(
                            "range_offset",
                            omx_msg.u.extended_buffer_data.range_offset);
                    msg->setInt32(
                            "range_length",
                            omx_msg.u.extended_buffer_data.range_length);
                    msg->setInt32(
                            "flags",
                            omx_msg.u.extended_buffer_data.flags);
                    msg->setInt64(
                            "timestamp",
                            omx_msg.u.extended_buffer_data.timestamp);
                    msg->setInt32(
                            "fence_fd", omx_msg.fenceFd);
                    break;
                }

                case omx_message::FRAME_RENDERED:
                {
                    msg->setInt64(
                            "media_time_us", omx_msg.u.render_data.timestamp);
                    msg->setInt64(
                            "system_nano", omx_msg.u.render_data.nanoTime);
                    break;
                }

                default:
                    ALOGE("Unrecognized message type: %d", omx_msg.type);
                    break;
            }
            msgList->getList().push_back(msg);
        }
        notify->setObject("messages", msgList);
        notify->post();
    }

protected:
    virtual ~CodecObserver() {}

private:
    sp<AMessage> mNotify;

    DISALLOW_EVIL_CONSTRUCTORS(CodecObserver);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::BaseState : public AState {
    BaseState(ACodec *codec, const sp<AState> &parentState = NULL);

protected:
    enum PortMode {
        KEEP_BUFFERS,
        RESUBMIT_BUFFERS,
        FREE_BUFFERS,
    };

    ACodec *mCodec;

    virtual PortMode getPortMode(OMX_U32 portIndex);

    virtual bool onMessageReceived(const sp<AMessage> &msg);

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

    void postFillThisBuffer(BufferInfo *info);

private:
    // Handles an OMX message. Returns true iff message was handled.
    bool onOMXMessage(const sp<AMessage> &msg);

    // Handles a list of messages. Returns true iff messages were handled.
    bool onOMXMessageList(const sp<AMessage> &msg);

    // returns true iff this message is for this component and the component is alive
    bool checkOMXMessage(const sp<AMessage> &msg);

    bool onOMXEmptyBufferDone(IOMX::buffer_id bufferID, int fenceFd);

    bool onOMXFillBufferDone(
            IOMX::buffer_id bufferID,
            size_t rangeOffset, size_t rangeLength,
            OMX_U32 flags,
            int64_t timeUs,
            int fenceFd);

    virtual bool onOMXFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano);

    void getMoreInputDataIfPossible();

    DISALLOW_EVIL_CONSTRUCTORS(BaseState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::DeathNotifier : public IBinder::DeathRecipient {
    DeathNotifier(const sp<AMessage> &notify)
        : mNotify(notify) {
    }

    virtual void binderDied(const wp<IBinder> &) {
        mNotify->post();
    }

protected:
    virtual ~DeathNotifier() {}

private:
    sp<AMessage> mNotify;

    DISALLOW_EVIL_CONSTRUCTORS(DeathNotifier);
};

struct ACodec::UninitializedState : public ACodec::BaseState {
    UninitializedState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

private:
    void onSetup(const sp<AMessage> &msg);
    bool onAllocateComponent(const sp<AMessage> &msg);

    sp<DeathNotifier> mDeathNotifier;

    DISALLOW_EVIL_CONSTRUCTORS(UninitializedState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::LoadedState : public ACodec::BaseState {
    LoadedState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

private:
    friend struct ACodec::UninitializedState;

    bool onConfigureComponent(const sp<AMessage> &msg);
    void onCreateInputSurface(const sp<AMessage> &msg);
    void onSetInputSurface(const sp<AMessage> &msg);
    void onStart();
    void onShutdown(bool keepComponentAllocated);

    status_t setupInputSurface();

    DISALLOW_EVIL_CONSTRUCTORS(LoadedState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::LoadedToIdleState : public ACodec::BaseState {
    LoadedToIdleState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual void stateEntered();

private:
    status_t allocateBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(LoadedToIdleState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::IdleToExecutingState : public ACodec::BaseState {
    IdleToExecutingState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual void stateEntered();

private:
    DISALLOW_EVIL_CONSTRUCTORS(IdleToExecutingState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::ExecutingState : public ACodec::BaseState {
    ExecutingState(ACodec *codec);

    void submitRegularOutputBuffers();
    void submitOutputMetaBuffers();
    void submitOutputBuffers();

    // Submit output buffers to the decoder, submit input buffers to client
    // to fill with data.
    void resume();

    // Returns true iff input and output buffers are in play.
    bool active() const { return mActive; }

protected:
    virtual PortMode getPortMode(OMX_U32 portIndex);
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual bool onOMXFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano);

private:
    bool mActive;

    DISALLOW_EVIL_CONSTRUCTORS(ExecutingState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::OutputPortSettingsChangedState : public ACodec::BaseState {
    OutputPortSettingsChangedState(ACodec *codec);

protected:
    virtual PortMode getPortMode(OMX_U32 portIndex);
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual bool onOMXFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano);

private:
    DISALLOW_EVIL_CONSTRUCTORS(OutputPortSettingsChangedState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::ExecutingToIdleState : public ACodec::BaseState {
    ExecutingToIdleState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

private:
    void changeStateIfWeOwnAllBuffers();

    bool mComponentNowIdle;

    DISALLOW_EVIL_CONSTRUCTORS(ExecutingToIdleState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::IdleToLoadedState : public ACodec::BaseState {
    IdleToLoadedState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

private:
    DISALLOW_EVIL_CONSTRUCTORS(IdleToLoadedState);
};

////////////////////////////////////////////////////////////////////////////////

struct ACodec::FlushingState : public ACodec::BaseState {
    FlushingState(ACodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

private:
    bool mFlushComplete[2];

    void changeStateIfWeOwnAllBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(FlushingState);
};

////////////////////////////////////////////////////////////////////////////////

void ACodec::BufferInfo::setWriteFence(int fenceFd, const char *dbg) {
    if (mFenceFd >= 0) {
        ALOGW("OVERWRITE OF %s fence %d by write fence %d in %s",
                mIsReadFence ? "read" : "write", mFenceFd, fenceFd, dbg);
    }
    mFenceFd = fenceFd;
    mIsReadFence = false;
}

void ACodec::BufferInfo::setReadFence(int fenceFd, const char *dbg) {
    if (mFenceFd >= 0) {
        ALOGW("OVERWRITE OF %s fence %d by read fence %d in %s",
                mIsReadFence ? "read" : "write", mFenceFd, fenceFd, dbg);
    }
    mFenceFd = fenceFd;
    mIsReadFence = true;
}

void ACodec::BufferInfo::checkWriteFence(const char *dbg) {
    if (mFenceFd >= 0 && mIsReadFence) {
        ALOGD("REUSING read fence %d as write fence in %s", mFenceFd, dbg);
    }
}

void ACodec::BufferInfo::checkReadFence(const char *dbg) {
    if (mFenceFd >= 0 && !mIsReadFence) {
        ALOGD("REUSING write fence %d as read fence in %s", mFenceFd, dbg);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::ACodec()
    : mQuirks(0),
      mNode(0),
      mUsingNativeWindow(false),
      mNativeWindowUsageBits(0),
      mLastNativeWindowDataSpace(HAL_DATASPACE_UNKNOWN),
      mIsVideo(false),
      mIsEncoder(false),
      mFatalError(false),
      mShutdownInProgress(false),
      mExplicitShutdown(false),
      mIsLegacyVP9Decoder(false),
      mEncoderDelay(0),
      mEncoderPadding(0),
      mRotationDegrees(0),
      mChannelMaskPresent(false),
      mChannelMask(0),
      mDequeueCounter(0),
      mInputMetadataType(kMetadataBufferTypeInvalid),
      mOutputMetadataType(kMetadataBufferTypeInvalid),
      mLegacyAdaptiveExperiment(false),
      mMetadataBuffersToSubmit(0),
      mNumUndequeuedBuffers(0),
      mRepeatFrameDelayUs(-1ll),
      mMaxPtsGapUs(-1ll),
      mMaxFps(-1),
      mTimePerFrameUs(-1ll),
      mTimePerCaptureUs(-1ll),
      mCreateInputBuffersSuspended(false),
      mTunneled(false),
      mDescribeColorAspectsIndex((OMX_INDEXTYPE)0),
      mDescribeHDRStaticInfoIndex((OMX_INDEXTYPE)0) {
    mUninitializedState = new UninitializedState(this);
    mLoadedState = new LoadedState(this);
    mLoadedToIdleState = new LoadedToIdleState(this);
    mIdleToExecutingState = new IdleToExecutingState(this);
    mExecutingState = new ExecutingState(this);

    mOutputPortSettingsChangedState =
        new OutputPortSettingsChangedState(this);

    mExecutingToIdleState = new ExecutingToIdleState(this);
    mIdleToLoadedState = new IdleToLoadedState(this);
    mFlushingState = new FlushingState(this);

    mPortEOS[kPortIndexInput] = mPortEOS[kPortIndexOutput] = false;
    mInputEOSResult = OK;

    memset(&mLastNativeWindowCrop, 0, sizeof(mLastNativeWindowCrop));

    changeState(mUninitializedState);
}

ACodec::~ACodec() {
}

status_t ACodec::setupCustomCodec(status_t err, const char *mime, const sp<AMessage> &msg) {
     if (!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11) && !mIsEncoder) {
         return FFMPEGSoftCodec::setAudioFormat(
               msg, mime, mOMX, mNode);
     }

    return err;
}

void ACodec::setNotificationMessage(const sp<AMessage> &msg) {
    mNotify = msg;
}

void ACodec::initiateSetup(const sp<AMessage> &msg) {
    msg->setWhat(kWhatSetup);
    msg->setTarget(this);
    msg->post();
}

void ACodec::signalSetParameters(const sp<AMessage> &params) {
    sp<AMessage> msg = new AMessage(kWhatSetParameters, this);
    msg->setMessage("params", params);
    msg->post();
}

void ACodec::initiateAllocateComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatAllocateComponent);
    msg->setTarget(this);
    msg->post();
}

void ACodec::initiateConfigureComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatConfigureComponent);
    msg->setTarget(this);
    msg->post();
}

status_t ACodec::setSurface(const sp<Surface> &surface) {
    sp<AMessage> msg = new AMessage(kWhatSetSurface, this);
    msg->setObject("surface", surface);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err == OK) {
        (void)response->findInt32("err", &err);
    }
    return err;
}

void ACodec::initiateCreateInputSurface() {
    (new AMessage(kWhatCreateInputSurface, this))->post();
}

void ACodec::initiateSetInputSurface(
        const sp<PersistentSurface> &surface) {
    sp<AMessage> msg = new AMessage(kWhatSetInputSurface, this);
    msg->setObject("input-surface", surface);
    msg->post();
}

void ACodec::signalEndOfInputStream() {
    (new AMessage(kWhatSignalEndOfInputStream, this))->post();
}

void ACodec::initiateStart() {
    (new AMessage(kWhatStart, this))->post();
}

void ACodec::signalFlush() {
    ALOGV("[%s] signalFlush", mComponentName.c_str());
    (new AMessage(kWhatFlush, this))->post();
}

void ACodec::signalResume() {
    (new AMessage(kWhatResume, this))->post();
}

void ACodec::initiateShutdown(bool keepComponentAllocated) {
    sp<AMessage> msg = new AMessage(kWhatShutdown, this);
    msg->setInt32("keepComponentAllocated", keepComponentAllocated);
    msg->post();
    if (!keepComponentAllocated) {
        // ensure shutdown completes in 3 seconds
        (new AMessage(kWhatReleaseCodecInstance, this))->post(3000000);
    }
}

void ACodec::signalRequestIDRFrame() {
    (new AMessage(kWhatRequestIDRFrame, this))->post();
}

// *** NOTE: THE FOLLOWING WORKAROUND WILL BE REMOVED ***
// Some codecs may return input buffers before having them processed.
// This causes a halt if we already signaled an EOS on the input
// port.  For now keep submitting an output buffer if there was an
// EOS on the input port, but not yet on the output port.
void ACodec::signalSubmitOutputMetadataBufferIfEOS_workaround() {
    if (mPortEOS[kPortIndexInput] && !mPortEOS[kPortIndexOutput] &&
            mMetadataBuffersToSubmit > 0) {
        (new AMessage(kWhatSubmitOutputMetadataBufferIfEOS, this))->post();
    }
}

status_t ACodec::handleSetSurface(const sp<Surface> &surface) {
    // allow keeping unset surface
    if (surface == NULL) {
        if (mNativeWindow != NULL) {
            ALOGW("cannot unset a surface");
            return INVALID_OPERATION;
        }
        return OK;
    }

    // cannot switch from bytebuffers to surface
    if (mNativeWindow == NULL) {
        ALOGW("component was not configured with a surface");
        return INVALID_OPERATION;
    }

    ANativeWindow *nativeWindow = surface.get();
    // if we have not yet started the codec, we can simply set the native window
    if (mBuffers[kPortIndexInput].size() == 0) {
        mNativeWindow = surface;
        return OK;
    }

    // we do not support changing a tunneled surface after start
    if (mTunneled) {
        ALOGW("cannot change tunneled surface");
        return INVALID_OPERATION;
    }

    int usageBits = 0;
    // no need to reconnect as we will not dequeue all buffers
    status_t err = setupNativeWindowSizeFormatAndUsage(
            nativeWindow, &usageBits,
            !storingMetadataInDecodedBuffers() || mLegacyAdaptiveExperiment /* reconnect */);
    if (err != OK) {
        return err;
    }

    int ignoredFlags = kVideoGrallocUsage;
    // New output surface is not allowed to add new usage flag except ignored ones.
    if ((usageBits & ~(mNativeWindowUsageBits | ignoredFlags)) != 0) {
        ALOGW("cannot change usage from %#x to %#x", mNativeWindowUsageBits, usageBits);
        return BAD_VALUE;
    }

    // get min undequeued count. We cannot switch to a surface that has a higher
    // undequeued count than we allocated.
    int minUndequeuedBuffers = 0;
    err = nativeWindow->query(
            nativeWindow, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
            &minUndequeuedBuffers);
    if (err != 0) {
        ALOGE("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }
    if (minUndequeuedBuffers > (int)mNumUndequeuedBuffers) {
        ALOGE("new surface holds onto more buffers (%d) than planned for (%zu)",
                minUndequeuedBuffers, mNumUndequeuedBuffers);
        return BAD_VALUE;
    }

    // we cannot change the number of output buffers while OMX is running
    // set up surface to the same count
    Vector<BufferInfo> &buffers = mBuffers[kPortIndexOutput];
    ALOGV("setting up surface for %zu buffers", buffers.size());

    err = native_window_set_buffer_count(nativeWindow, buffers.size());
    if (err != 0) {
        ALOGE("native_window_set_buffer_count failed: %s (%d)", strerror(-err),
                -err);
        return err;
    }

    // need to enable allocation when attaching
    surface->getIGraphicBufferProducer()->allowAllocation(true);

    // for meta data mode, we move dequeud buffers to the new surface.
    // for non-meta mode, we must move all registered buffers
    for (size_t i = 0; i < buffers.size(); ++i) {
        const BufferInfo &info = buffers[i];
        // skip undequeued buffers for meta data mode
        if (storingMetadataInDecodedBuffers()
                && !mLegacyAdaptiveExperiment
                && info.mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
            ALOGV("skipping buffer");
            continue;
        }
        ALOGV("attaching buffer %p", info.mGraphicBuffer->getNativeBuffer());

        err = surface->attachBuffer(info.mGraphicBuffer->getNativeBuffer());
        if (err != OK) {
            ALOGE("failed to attach buffer %p to the new surface: %s (%d)",
                    info.mGraphicBuffer->getNativeBuffer(),
                    strerror(-err), -err);
            return err;
        }
    }

    // cancel undequeued buffers to new surface
    if (!storingMetadataInDecodedBuffers() || mLegacyAdaptiveExperiment) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            BufferInfo &info = buffers.editItemAt(i);
            if (info.mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                ALOGV("canceling buffer %p", info.mGraphicBuffer->getNativeBuffer());
                err = nativeWindow->cancelBuffer(
                        nativeWindow, info.mGraphicBuffer->getNativeBuffer(), info.mFenceFd);
                info.mFenceFd = -1;
                if (err != OK) {
                    ALOGE("failed to cancel buffer %p to the new surface: %s (%d)",
                            info.mGraphicBuffer->getNativeBuffer(),
                            strerror(-err), -err);
                    return err;
                }
            }
        }
        // disallow further allocation
        (void)surface->getIGraphicBufferProducer()->allowAllocation(false);
    }

    // push blank buffers to previous window if requested
    if (mFlags & kFlagPushBlankBuffersToNativeWindowOnShutdown) {
        pushBlankBuffersToNativeWindow(mNativeWindow.get());
    }

    mNativeWindow = nativeWindow;
    mNativeWindowUsageBits = usageBits;
    return OK;
}

status_t ACodec::allocateBuffersOnPort(OMX_U32 portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    CHECK(mDealer[portIndex] == NULL);
    CHECK(mBuffers[portIndex].isEmpty());

    status_t err;
    if (mNativeWindow != NULL && portIndex == kPortIndexOutput) {
        if (storingMetadataInDecodedBuffers()) {
            err = allocateOutputMetadataBuffers();
        } else {
            err = allocateOutputBuffersFromNativeWindow();
        }
    } else {
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = portIndex;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err == OK) {
            MetadataBufferType type =
                portIndex == kPortIndexOutput ? mOutputMetadataType : mInputMetadataType;
            size_t bufSize = def.nBufferSize;
            if (type == kMetadataBufferTypeANWBuffer) {
                bufSize = sizeof(VideoNativeMetadata);
            } else if (type == kMetadataBufferTypeNativeHandleSource) {
                bufSize = sizeof(VideoNativeHandleMetadata);
            }
#ifdef CAMCORDER_GRALLOC_SOURCE
            else if (type == kMetadataBufferTypeGrallocSource) {
                bufSize = sizeof(VideoGrallocMetadata);
            }
#endif

            // If using gralloc or native source input metadata buffers, allocate largest
            // metadata size as we prefer to generate native source metadata, but component
            // may require gralloc source. For camera source, allocate at least enough
            // size for native metadata buffers.
            size_t allottedSize = bufSize;
#ifdef CAMCORDER_GRALLOC_SOURCE
            if (portIndex == kPortIndexInput && type >= kMetadataBufferTypeGrallocSource) {
#else
            if (portIndex == kPortIndexInput && type == kMetadataBufferTypeANWBuffer) {
#endif
                bufSize = max(sizeof(VideoGrallocMetadata), sizeof(VideoNativeMetadata));
            } else if (portIndex == kPortIndexInput && type == kMetadataBufferTypeCameraSource) {
                bufSize = max(bufSize, sizeof(VideoNativeMetadata));
            }

            size_t conversionBufferSize = 0;

            sp<DataConverter> converter = mConverter[portIndex];
            if (converter != NULL) {
                // here we assume sane conversions of max 4:1, so result fits in int32
                if (portIndex == kPortIndexInput) {
                    conversionBufferSize = converter->sourceSize(bufSize);
                } else {
                    conversionBufferSize = converter->targetSize(bufSize);
                }
            }

            size_t alignment = MemoryDealer::getAllocationAlignment();

            ALOGV("[%s] Allocating %u buffers of size %zu/%zu (from %u using %s) on %s port",
                    mComponentName.c_str(),
                    def.nBufferCountActual, bufSize, allottedSize, def.nBufferSize, asString(type),
                    portIndex == kPortIndexInput ? "input" : "output");

            // verify buffer sizes to avoid overflow in align()
            if (bufSize == 0 || max(bufSize, conversionBufferSize) > kMaxCodecBufferSize) {
                ALOGE("b/22885421");
                return NO_MEMORY;
            }

            // don't modify bufSize as OMX may not expect it to increase after negotiation
            size_t alignedSize = align(bufSize, alignment);
            size_t alignedConvSize = align(conversionBufferSize, alignment);
            if (def.nBufferCountActual > SIZE_MAX / (alignedSize + alignedConvSize)) {
                ALOGE("b/22885421");
                return NO_MEMORY;
            }

            size_t totalSize = def.nBufferCountActual * (alignedSize + alignedConvSize);
            mDealer[portIndex] = new MemoryDealer(totalSize, "ACodec");

            for (OMX_U32 i = 0; i < def.nBufferCountActual && err == OK; ++i) {
                sp<IMemory> mem = mDealer[portIndex]->allocate(bufSize);
                if (mem == NULL || mem->pointer() == NULL) {
                    return NO_MEMORY;
                }

                BufferInfo info;
                info.mStatus = BufferInfo::OWNED_BY_US;
                info.mFenceFd = -1;
                info.mRenderInfo = NULL;
                info.mNativeHandle = NULL;

                uint32_t requiresAllocateBufferBit =
                    (portIndex == kPortIndexInput)
                        ? kRequiresAllocateBufferOnInputPorts
                        : kRequiresAllocateBufferOnOutputPorts;

                if (portIndex == kPortIndexInput && (mFlags & kFlagIsSecure)) {
                    mem.clear();

                    void *ptr = NULL;
                    sp<NativeHandle> native_handle;
                    err = mOMX->allocateSecureBuffer(
                            mNode, portIndex, bufSize, &info.mBufferID,
                            &ptr, &native_handle);

                    // TRICKY: this representation is unorthodox, but ACodec requires
                    // an ABuffer with a proper size to validate range offsets and lengths.
                    // Since mData is never referenced for secure input, it is used to store
                    // either the pointer to the secure buffer, or the opaque handle as on
                    // some devices ptr is actually an opaque handle, not a pointer.

                    // TRICKY2: use native handle as the base of the ABuffer if received one,
                    // because Widevine source only receives these base addresses.
                    const native_handle_t *native_handle_ptr =
                        native_handle == NULL ? NULL : native_handle->handle();
                    info.mData = new ABuffer(
                            ptr != NULL ? ptr : (void *)native_handle_ptr, bufSize);
                    info.mNativeHandle = native_handle;
                    info.mCodecData = info.mData;
                } else if (mQuirks & requiresAllocateBufferBit) {
                    err = mOMX->allocateBufferWithBackup(
                            mNode, portIndex, mem, &info.mBufferID, allottedSize);
                } else {
                    err = mOMX->useBuffer(mNode, portIndex, mem, &info.mBufferID, allottedSize);
                }

                if (mem != NULL) {
                    info.mCodecData = new ABuffer(mem->pointer(), bufSize);
                    info.mCodecRef = mem;

                    if (type == kMetadataBufferTypeANWBuffer) {
                        ((VideoNativeMetadata *)mem->pointer())->nFenceFd = -1;
                    }

                    // if we require conversion, allocate conversion buffer for client use;
                    // otherwise, reuse codec buffer
                    if (mConverter[portIndex] != NULL) {
                        CHECK_GT(conversionBufferSize, (size_t)0);
                        mem = mDealer[portIndex]->allocate(conversionBufferSize);
                        if (mem == NULL|| mem->pointer() == NULL) {
                            return NO_MEMORY;
                        }
                        info.mData = new ABuffer(mem->pointer(), conversionBufferSize);
                        info.mMemRef = mem;
                    } else {
                        info.mData = info.mCodecData;
                        info.mMemRef = info.mCodecRef;
                    }
                }

                mBuffers[portIndex].push(info);
            }
        }
    }

    if (err != OK) {
        return err;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatBuffersAllocated);

    notify->setInt32("portIndex", portIndex);

    sp<PortDescription> desc = new PortDescription;

    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex][i];
        desc->addBuffer(info.mBufferID, info.mData, info.mNativeHandle, info.mMemRef);
    }

    notify->setObject("portDesc", desc);
    notify->post();

    return OK;
}

status_t ACodec::setupNativeWindowSizeFormatAndUsage(
        ANativeWindow *nativeWindow /* nonnull */, int *finalUsage /* nonnull */,
        bool reconnect) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    OMX_U32 usage = 0;
    err = mOMX->getGraphicBufferUsage(mNode, kPortIndexOutput, &usage);
    if (err != 0) {
        ALOGW("querying usage flags from OMX IL component failed: %d", err);
        // XXX: Currently this error is logged, but not fatal.
        usage = 0;
    }
    int omxUsage = usage;

    if (mFlags & kFlagIsGrallocUsageProtected) {
        usage |= GRALLOC_USAGE_PROTECTED;
    }

    usage |= kVideoGrallocUsage;
    *finalUsage = usage;

    memset(&mLastNativeWindowCrop, 0, sizeof(mLastNativeWindowCrop));
    mLastNativeWindowDataSpace = HAL_DATASPACE_UNKNOWN;

#ifdef USE_SAMSUNG_COLORFORMAT
    OMX_COLOR_FORMATTYPE eNativeColorFormat = def.format.video.eColorFormat;
    setNativeWindowColorFormat(eNativeColorFormat);
#endif

    ALOGV("gralloc usage: %#x(OMX) => %#x(ACodec)", omxUsage, usage);
    err = setNativeWindowSizeFormatAndUsage(
            nativeWindow,
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
#ifdef USE_SAMSUNG_COLORFORMAT
            eNativeColorFormat,
#else
            def.format.video.eColorFormat,
#endif
            mRotationDegrees,
            usage,
            reconnect);
#ifdef QCOM_HARDWARE
    if (err == OK) {
        OMX_CONFIG_RECTTYPE rect;
        InitOMXParams(&rect);
        rect.nPortIndex = kPortIndexOutput;
        err = mOMX->getConfig(
                mNode, OMX_IndexConfigCommonOutputCrop, &rect, sizeof(rect));
        if (err == OK) {
            ALOGV("rect size = %d, %d, %d, %d", rect.nLeft, rect.nTop, rect.nWidth, rect.nHeight);
            android_native_rect_t crop;
            crop.left = rect.nLeft;
            crop.top = rect.nTop;
            crop.right = rect.nLeft + rect.nWidth - 1;
            crop.bottom = rect.nTop + rect.nHeight - 1;
            ALOGV("crop update (%d, %d), (%d, %d)", crop.left, crop.top, crop.right, crop.bottom);
            err = native_window_set_crop(nativeWindow, &crop);
        }
    }
#endif
    return err;
}

status_t ACodec::configureOutputBuffersFromNativeWindow(
        OMX_U32 *bufferCount, OMX_U32 *bufferSize,
        OMX_U32 *minUndequeuedBuffers, bool preregister) {

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err == OK) {
        err = setupNativeWindowSizeFormatAndUsage(
                mNativeWindow.get(), &mNativeWindowUsageBits, preregister /* reconnect */);
    }
    if (err != OK) {
        mNativeWindowUsageBits = 0;
        return err;
    }

    // Exits here for tunneled video playback codecs -- i.e. skips native window
    // buffer allocation step as this is managed by the tunneled OMX omponent
    // itself and explicitly sets def.nBufferCountActual to 0.
    if (mTunneled) {
        ALOGV("Tunneled Playback: skipping native window buffer allocation.");
        def.nBufferCountActual = 0;
        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        *minUndequeuedBuffers = 0;
        *bufferCount = 0;
        *bufferSize = 0;
        return err;
    }

    *minUndequeuedBuffers = 0;
    err = mNativeWindow->query(
            mNativeWindow.get(), NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
            (int *)minUndequeuedBuffers);

    if (err != 0) {
        ALOGE("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    // FIXME: assume that surface is controlled by app (native window
    // returns the number for the case when surface is not controlled by app)
    // FIXME2: This means that minUndeqeueudBufs can be 1 larger than reported
    // For now, try to allocate 1 more buffer, but don't fail if unsuccessful

    // Use conservative allocation while also trying to reduce starvation
    //
    // 1. allocate at least nBufferCountMin + minUndequeuedBuffers - that is the
    //    minimum needed for the consumer to be able to work
    // 2. try to allocate two (2) additional buffers to reduce starvation from
    //    the consumer
    //    plus an extra buffer to account for incorrect minUndequeuedBufs
#ifdef BOARD_CANT_REALLOCATE_OMX_BUFFERS
    // Some devices don't like to set OMX_IndexParamPortDefinition at this
    // point (even with an unmodified def), so skip it if possible.
    // This check was present in KitKat.
    if (def.nBufferCountActual < def.nBufferCountMin + *minUndequeuedBuffers) {
#endif
    for (OMX_U32 extraBuffers = 2 + 1; /* condition inside loop */; extraBuffers--) {
        OMX_U32 newBufferCount =
            def.nBufferCountMin + *minUndequeuedBuffers + extraBuffers;
        def.nBufferCountActual = newBufferCount;
        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err == OK) {
            *minUndequeuedBuffers += extraBuffers;
            break;
        }

        ALOGW("[%s] setting nBufferCountActual to %u failed: %d",
                mComponentName.c_str(), newBufferCount, err);
        /* exit condition */
        if (extraBuffers == 0) {
            return err;
        }
    }
#ifdef BOARD_CANT_REALLOCATE_OMX_BUFFERS
    }
#endif

    err = native_window_set_buffer_count(
            mNativeWindow.get(), def.nBufferCountActual);

    if (err != 0) {
        ALOGE("native_window_set_buffer_count failed: %s (%d)", strerror(-err),
                -err);
        return err;
    }

    *bufferCount = def.nBufferCountActual;
    *bufferSize =  def.nBufferSize;
    return err;
}

status_t ACodec::allocateOutputBuffersFromNativeWindow() {
    OMX_U32 bufferCount, bufferSize, minUndequeuedBuffers;
    status_t err = configureOutputBuffersFromNativeWindow(
            &bufferCount, &bufferSize, &minUndequeuedBuffers, true /* preregister */);
    if (err != 0)
        return err;
    mNumUndequeuedBuffers = minUndequeuedBuffers;

    if (!storingMetadataInDecodedBuffers()) {
        static_cast<Surface*>(mNativeWindow.get())
                ->getIGraphicBufferProducer()->allowAllocation(true);
    }

    ALOGV("[%s] Allocating %u buffers from a native window of size %u on "
         "output port",
         mComponentName.c_str(), bufferCount, bufferSize);

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        ANativeWindowBuffer *buf;
        int fenceFd;
        err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buf, &fenceFd);
        if (err != 0) {
            ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
            break;
        }

        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_US;
        info.mFenceFd = fenceFd;
        info.mIsReadFence = false;
        info.mRenderInfo = NULL;
        info.mData = new ABuffer(NULL /* data */, bufferSize /* capacity */);
        info.mCodecData = info.mData;
        info.mGraphicBuffer = graphicBuffer;
        mBuffers[kPortIndexOutput].push(info);

        IOMX::buffer_id bufferId;
        err = mOMX->useGraphicBuffer(mNode, kPortIndexOutput, graphicBuffer,
                &bufferId);
        if (err != 0) {
            ALOGE("registering GraphicBuffer %u with OMX IL component failed: "
                 "%d", i, err);
            break;
        }

        mBuffers[kPortIndexOutput].editItemAt(i).mBufferID = bufferId;

        ALOGV("[%s] Registered graphic buffer with ID %u (pointer = %p)",
             mComponentName.c_str(),
             bufferId, graphicBuffer.get());
    }

    OMX_U32 cancelStart;
    OMX_U32 cancelEnd;

    if (err != 0) {
        // If an error occurred while dequeuing we need to cancel any buffers
        // that were dequeued.
        cancelStart = 0;
        cancelEnd = mBuffers[kPortIndexOutput].size();
    } else {
        // Return the required minimum undequeued buffers to the native window.
        cancelStart = bufferCount - minUndequeuedBuffers;
        cancelEnd = bufferCount;
    }

    for (OMX_U32 i = cancelStart; i < cancelEnd; i++) {
        BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);
        if (info->mStatus == BufferInfo::OWNED_BY_US) {
            status_t error = cancelBufferToNativeWindow(info);
            if (err == 0) {
                err = error;
            }
        }
    }

    if (!storingMetadataInDecodedBuffers()) {
        static_cast<Surface*>(mNativeWindow.get())
                ->getIGraphicBufferProducer()->allowAllocation(false);
    }

    return err;
}

status_t ACodec::allocateOutputMetadataBuffers() {
    OMX_U32 bufferCount, bufferSize, minUndequeuedBuffers;
    status_t err = configureOutputBuffersFromNativeWindow(
            &bufferCount, &bufferSize, &minUndequeuedBuffers,
            mLegacyAdaptiveExperiment /* preregister */);
    if (err != 0)
        return err;
    mNumUndequeuedBuffers = minUndequeuedBuffers;

    ALOGV("[%s] Allocating %u meta buffers on output port",
         mComponentName.c_str(), bufferCount);

    size_t bufSize = mOutputMetadataType == kMetadataBufferTypeANWBuffer ?
            sizeof(struct VideoNativeMetadata) : sizeof(struct VideoGrallocMetadata);
    size_t totalSize = bufferCount * align(bufSize, MemoryDealer::getAllocationAlignment());
    mDealer[kPortIndexOutput] = new MemoryDealer(totalSize, "ACodec");

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        info.mFenceFd = -1;
        info.mRenderInfo = NULL;
        info.mGraphicBuffer = NULL;
        info.mDequeuedAt = mDequeueCounter;

        sp<IMemory> mem = mDealer[kPortIndexOutput]->allocate(bufSize);
        if (mem == NULL || mem->pointer() == NULL) {
            return NO_MEMORY;
        }
        if (mOutputMetadataType == kMetadataBufferTypeANWBuffer) {
            ((VideoNativeMetadata *)mem->pointer())->nFenceFd = -1;
        }
        info.mData = new ABuffer(mem->pointer(), mem->size());
        info.mMemRef = mem;
        info.mCodecData = info.mData;
        info.mCodecRef = mem;

        // we use useBuffer for metadata regardless of quirks
        err = mOMX->useBuffer(
                mNode, kPortIndexOutput, mem, &info.mBufferID, mem->size());
        mBuffers[kPortIndexOutput].push(info);

        ALOGV("[%s] allocated meta buffer with ID %u (pointer = %p)",
             mComponentName.c_str(), info.mBufferID, mem->pointer());
    }

    if (mLegacyAdaptiveExperiment) {
        // preallocate and preregister buffers
        static_cast<Surface *>(mNativeWindow.get())
                ->getIGraphicBufferProducer()->allowAllocation(true);

        ALOGV("[%s] Allocating %u buffers from a native window of size %u on "
             "output port",
             mComponentName.c_str(), bufferCount, bufferSize);

        // Dequeue buffers then cancel them all
        for (OMX_U32 i = 0; i < bufferCount; i++) {
            BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);

            ANativeWindowBuffer *buf;
            int fenceFd;
            err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buf, &fenceFd);
            if (err != 0) {
                ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
                break;
            }

            sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
            mOMX->updateGraphicBufferInMeta(
                    mNode, kPortIndexOutput, graphicBuffer, info->mBufferID);
            info->mStatus = BufferInfo::OWNED_BY_US;
            info->setWriteFence(fenceFd, "allocateOutputMetadataBuffers for legacy");
            info->mGraphicBuffer = graphicBuffer;
        }

        for (OMX_U32 i = 0; i < mBuffers[kPortIndexOutput].size(); i++) {
            BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);
            if (info->mStatus == BufferInfo::OWNED_BY_US) {
                status_t error = cancelBufferToNativeWindow(info);
                if (err == OK) {
                    err = error;
                }
            }
        }

        static_cast<Surface*>(mNativeWindow.get())
                ->getIGraphicBufferProducer()->allowAllocation(false);
    }

    mMetadataBuffersToSubmit = bufferCount - minUndequeuedBuffers;
    return err;
}

status_t ACodec::submitOutputMetadataBuffer() {
    CHECK(storingMetadataInDecodedBuffers());
    if (mMetadataBuffersToSubmit == 0)
        return OK;

    BufferInfo *info = dequeueBufferFromNativeWindow();
    if (info == NULL) {
        return ERROR_IO;
    }

    ALOGV("[%s] submitting output meta buffer ID %u for graphic buffer %p",
          mComponentName.c_str(), info->mBufferID, info->mGraphicBuffer.get());

    --mMetadataBuffersToSubmit;
    info->checkWriteFence("submitOutputMetadataBuffer");
    status_t err = mOMX->fillBuffer(mNode, info->mBufferID, info->mFenceFd);
    info->mFenceFd = -1;
    if (err == OK) {
        info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    }

    return err;
}

status_t ACodec::waitForFence(int fd, const char *dbg ) {
    status_t res = OK;
    if (fd >= 0) {
        sp<Fence> fence = new Fence(fd);
        res = fence->wait(IOMX::kFenceTimeoutMs);
        ALOGW_IF(res != OK, "FENCE TIMEOUT for %d in %s", fd, dbg);
    }
    return res;
}

// static
const char *ACodec::_asString(BufferInfo::Status s) {
    switch (s) {
        case BufferInfo::OWNED_BY_US:            return "OUR";
        case BufferInfo::OWNED_BY_COMPONENT:     return "COMPONENT";
        case BufferInfo::OWNED_BY_UPSTREAM:      return "UPSTREAM";
        case BufferInfo::OWNED_BY_DOWNSTREAM:    return "DOWNSTREAM";
        case BufferInfo::OWNED_BY_NATIVE_WINDOW: return "SURFACE";
        case BufferInfo::UNRECOGNIZED:           return "UNRECOGNIZED";
        default:                                 return "?";
    }
}

void ACodec::dumpBuffers(OMX_U32 portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
    ALOGI("[%s] %s port has %zu buffers:", mComponentName.c_str(),
            portIndex == kPortIndexInput ? "input" : "output", mBuffers[portIndex].size());
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex][i];
        ALOGI("  slot %2zu: #%8u %p/%p %s(%d) dequeued:%u",
                i, info.mBufferID, info.mGraphicBuffer.get(),
                info.mGraphicBuffer == NULL ? NULL : info.mGraphicBuffer->getNativeBuffer(),
                _asString(info.mStatus), info.mStatus, info.mDequeuedAt);
    }
}

#ifdef USE_SAMSUNG_COLORFORMAT
void ACodec::setNativeWindowColorFormat(OMX_COLOR_FORMATTYPE &eNativeColorFormat)
{
    // In case of Samsung decoders, we set proper native color format for the Native Window
    if (!strcasecmp(mComponentName.c_str(), "OMX.SEC.AVC.Decoder")
        || !strcasecmp(mComponentName.c_str(), "OMX.SEC.FP.AVC.Decoder")
        || !strcasecmp(mComponentName.c_str(), "OMX.SEC.MPEG4.Decoder")
        || !strcasecmp(mComponentName.c_str(), "OMX.Exynos.AVC.Decoder")) {
        switch (eNativeColorFormat) {
            case OMX_COLOR_FormatYUV420SemiPlanar:
                eNativeColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YCbCr_420_SP;
                break;
            case OMX_COLOR_FormatYUV420Planar:
            default:
                eNativeColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YCbCr_420_P;
                break;
        }
    }
}
#endif

status_t ACodec::cancelBufferToNativeWindow(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    ALOGV("[%s] Calling cancelBuffer on buffer %u",
         mComponentName.c_str(), info->mBufferID);

    info->checkWriteFence("cancelBufferToNativeWindow");
    int err = mNativeWindow->cancelBuffer(
        mNativeWindow.get(), info->mGraphicBuffer.get(), info->mFenceFd);
    info->mFenceFd = -1;

    ALOGW_IF(err != 0, "[%s] can not return buffer %u to native window",
            mComponentName.c_str(), info->mBufferID);
    // change ownership even if cancelBuffer fails
    info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;

    return err;
}

void ACodec::updateRenderInfoForDequeuedBuffer(
        ANativeWindowBuffer *buf, int fenceFd, BufferInfo *info) {

    info->mRenderInfo =
        mRenderTracker.updateInfoForDequeuedBuffer(
                buf, fenceFd, info - &mBuffers[kPortIndexOutput][0]);

    // check for any fences already signaled
    notifyOfRenderedFrames(false /* dropIncomplete */, info->mRenderInfo);
}

void ACodec::onFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano) {
    if (mRenderTracker.onFrameRendered(mediaTimeUs, systemNano) != OK) {
        mRenderTracker.dumpRenderQueue();
    }
}

void ACodec::notifyOfRenderedFrames(bool dropIncomplete, FrameRenderTracker::Info *until) {
    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("what", CodecBase::kWhatOutputFramesRendered);
    std::list<FrameRenderTracker::Info> done =
        mRenderTracker.checkFencesAndGetRenderedFrames(until, dropIncomplete);

    // unlink untracked frames
    for (std::list<FrameRenderTracker::Info>::const_iterator it = done.cbegin();
            it != done.cend(); ++it) {
        ssize_t index = it->getIndex();
        if (index >= 0 && (size_t)index < mBuffers[kPortIndexOutput].size()) {
            mBuffers[kPortIndexOutput].editItemAt(index).mRenderInfo = NULL;
        } else if (index >= 0) {
            // THIS SHOULD NEVER HAPPEN
            ALOGE("invalid index %zd in %zu", index, mBuffers[kPortIndexOutput].size());
        }
    }

    if (MediaCodec::CreateFramesRenderedMessage(done, msg)) {
        msg->post();
    }
}

ACodec::BufferInfo *ACodec::dequeueBufferFromNativeWindow() {
    ANativeWindowBuffer *buf;
    CHECK(mNativeWindow.get() != NULL);

    if (mTunneled) {
        ALOGW("dequeueBufferFromNativeWindow() should not be called in tunnel"
              " video playback mode mode!");
        return NULL;
    }

    if (mFatalError) {
        ALOGW("not dequeuing from native window due to fatal error");
        return NULL;
    }

    int fenceFd = -1;
    do {
        status_t err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buf, &fenceFd);
        if (err != 0) {
            ALOGE("dequeueBuffer failed: %s(%d).", asString(err), err);
            return NULL;
        }

        bool stale = false;
        for (size_t i = mBuffers[kPortIndexOutput].size(); i > 0;) {
            i--;
            BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);

            if (info->mGraphicBuffer != NULL &&
                    info->mGraphicBuffer->handle == buf->handle) {
                // Since consumers can attach buffers to BufferQueues, it is possible
                // that a known yet stale buffer can return from a surface that we
                // once used.  We can simply ignore this as we have already dequeued
                // this buffer properly.  NOTE: this does not eliminate all cases,
                // e.g. it is possible that we have queued the valid buffer to the
                // NW, and a stale copy of the same buffer gets dequeued - which will
                // be treated as the valid buffer by ACodec.
                if (info->mStatus != BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                    ALOGI("dequeued stale buffer %p. discarding", buf);
                    stale = true;
                    break;
                }

                ALOGV("dequeued buffer %p", info->mGraphicBuffer->getNativeBuffer());
                info->mStatus = BufferInfo::OWNED_BY_US;
                info->setWriteFence(fenceFd, "dequeueBufferFromNativeWindow");
                updateRenderInfoForDequeuedBuffer(buf, fenceFd, info);
                return info;
            }
        }

        // It is also possible to receive a previously unregistered buffer
        // in non-meta mode. These should be treated as stale buffers. The
        // same is possible in meta mode, in which case, it will be treated
        // as a normal buffer, which is not desirable.
        // TODO: fix this.
        if (!stale && (!storingMetadataInDecodedBuffers() || mLegacyAdaptiveExperiment)) {
            ALOGI("dequeued unrecognized (stale) buffer %p. discarding", buf);
            stale = true;
        }
        if (stale) {
            // TODO: detach stale buffer, but there is no API yet to do it.
            buf = NULL;
        }
    } while (buf == NULL);

    // get oldest undequeued buffer
    BufferInfo *oldest = NULL;
    for (size_t i = mBuffers[kPortIndexOutput].size(); i > 0;) {
        i--;
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);
        if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW &&
            (oldest == NULL ||
             // avoid potential issues from counter rolling over
             mDequeueCounter - info->mDequeuedAt >
                    mDequeueCounter - oldest->mDequeuedAt)) {
            oldest = info;
        }
    }

    // it is impossible dequeue a buffer when there are no buffers with ANW
    CHECK(oldest != NULL);
    // it is impossible to dequeue an unknown buffer in non-meta mode, as the
    // while loop above does not complete
    CHECK(storingMetadataInDecodedBuffers());

    // discard buffer in LRU info and replace with new buffer
    oldest->mGraphicBuffer = new GraphicBuffer(buf, false);
    oldest->mStatus = BufferInfo::OWNED_BY_US;
    oldest->setWriteFence(fenceFd, "dequeueBufferFromNativeWindow for oldest");
    mRenderTracker.untrackFrame(oldest->mRenderInfo);
    oldest->mRenderInfo = NULL;

    mOMX->updateGraphicBufferInMeta(
            mNode, kPortIndexOutput, oldest->mGraphicBuffer,
            oldest->mBufferID);

    if (mOutputMetadataType == kMetadataBufferTypeGrallocSource) {
        VideoGrallocMetadata *grallocMeta =
            reinterpret_cast<VideoGrallocMetadata *>(oldest->mData->base());
        ALOGV("replaced oldest buffer #%u with age %u (%p/%p stored in %p)",
                (unsigned)(oldest - &mBuffers[kPortIndexOutput][0]),
                mDequeueCounter - oldest->mDequeuedAt,
                (void *)(uintptr_t)grallocMeta->pHandle,
                oldest->mGraphicBuffer->handle, oldest->mData->base());
    } else if (mOutputMetadataType == kMetadataBufferTypeANWBuffer) {
        VideoNativeMetadata *nativeMeta =
            reinterpret_cast<VideoNativeMetadata *>(oldest->mData->base());
        ALOGV("replaced oldest buffer #%u with age %u (%p/%p stored in %p)",
                (unsigned)(oldest - &mBuffers[kPortIndexOutput][0]),
                mDequeueCounter - oldest->mDequeuedAt,
                (void *)(uintptr_t)nativeMeta->pBuffer,
                oldest->mGraphicBuffer->getNativeBuffer(), oldest->mData->base());
    }

    updateRenderInfoForDequeuedBuffer(buf, fenceFd, oldest);
    return oldest;
}

status_t ACodec::freeBuffersOnPort(OMX_U32 portIndex) {
    status_t err = OK;
    for (size_t i = mBuffers[portIndex].size(); i > 0;) {
        i--;
        status_t err2 = freeBuffer(portIndex, i);
        if (err == OK) {
            err = err2;
        }
    }

    // clear mDealer even on an error
    mDealer[portIndex].clear();
    return err;
}

status_t ACodec::freeOutputBuffersNotOwnedByComponent() {
    status_t err = OK;
    for (size_t i = mBuffers[kPortIndexOutput].size(); i > 0;) {
        i--;
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);

        // At this time some buffers may still be with the component
        // or being drained.
        if (info->mStatus != BufferInfo::OWNED_BY_COMPONENT &&
            info->mStatus != BufferInfo::OWNED_BY_DOWNSTREAM) {
            status_t err2 = freeBuffer(kPortIndexOutput, i);
            if (err == OK) {
                err = err2;
            }
        }
    }

    return err;
}

status_t ACodec::freeBuffer(OMX_U32 portIndex, size_t i) {
    BufferInfo *info = &mBuffers[portIndex].editItemAt(i);
    status_t err = OK;

    // there should not be any fences in the metadata
    MetadataBufferType type =
        portIndex == kPortIndexOutput ? mOutputMetadataType : mInputMetadataType;
    if (type == kMetadataBufferTypeANWBuffer && info->mData != NULL
            && info->mData->size() >= sizeof(VideoNativeMetadata)) {
        int fenceFd = ((VideoNativeMetadata *)info->mData->data())->nFenceFd;
        if (fenceFd >= 0) {
            ALOGW("unreleased fence (%d) in %s metadata buffer %zu",
                    fenceFd, portIndex == kPortIndexInput ? "input" : "output", i);
        }
    }

    switch (info->mStatus) {
        case BufferInfo::OWNED_BY_US:
            if (portIndex == kPortIndexOutput && mNativeWindow != NULL) {
                (void)cancelBufferToNativeWindow(info);
            }
            // fall through

        case BufferInfo::OWNED_BY_NATIVE_WINDOW:
            err = mOMX->freeBuffer(mNode, portIndex, info->mBufferID);
            break;

        default:
            ALOGE("trying to free buffer not owned by us or ANW (%d)", info->mStatus);
            err = FAILED_TRANSACTION;
            break;
    }

    if (info->mFenceFd >= 0) {
        ::close(info->mFenceFd);
    }

    if (portIndex == kPortIndexOutput) {
        mRenderTracker.untrackFrame(info->mRenderInfo, i);
        info->mRenderInfo = NULL;
    }

    // remove buffer even if mOMX->freeBuffer fails
    mBuffers[portIndex].removeAt(i);
    return err;
}

ACodec::BufferInfo *ACodec::findBufferByID(
        uint32_t portIndex, IOMX::buffer_id bufferID, ssize_t *index) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mBufferID == bufferID) {
            if (index != NULL) {
                *index = i;
            }
            return info;
        }
    }

    ALOGE("Could not find buffer with ID %u", bufferID);
    return NULL;
}

status_t ACodec::setComponentRole(
        bool isEncoder, const char *mime) {
    const char *role = getComponentRole(isEncoder, mime);
    if (role == NULL) {
        return BAD_VALUE;
    }
    status_t err = setComponentRole(mOMX, mNode, role);
    if (err != OK) {
        ALOGW("[%s] Failed to set standard component role '%s'.",
             mComponentName.c_str(), role);
    }
    return err;
}

//static
const char *ACodec::getComponentRole(
        bool isEncoder, const char *mime) {
    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_MPEG,
            "audio_decoder.mp3", "audio_encoder.mp3" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I,
            "audio_decoder.mp1", "audio_encoder.mp1" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
            "audio_decoder.mp2", "audio_encoder.mp2" },
        { MEDIA_MIMETYPE_AUDIO_AMR_NB,
            "audio_decoder.amrnb", "audio_encoder.amrnb" },
        { MEDIA_MIMETYPE_AUDIO_AMR_WB,
            "audio_decoder.amrwb", "audio_encoder.amrwb" },
        { MEDIA_MIMETYPE_AUDIO_AAC,
            "audio_decoder.aac", "audio_encoder.aac" },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
            "audio_decoder.vorbis", "audio_encoder.vorbis" },
        { MEDIA_MIMETYPE_AUDIO_OPUS,
            "audio_decoder.opus", "audio_encoder.opus" },
        { MEDIA_MIMETYPE_AUDIO_G711_MLAW,
            "audio_decoder.g711mlaw", "audio_encoder.g711mlaw" },
        { MEDIA_MIMETYPE_AUDIO_G711_ALAW,
            "audio_decoder.g711alaw", "audio_encoder.g711alaw" },
        { MEDIA_MIMETYPE_VIDEO_AVC,
            "video_decoder.avc", "video_encoder.avc" },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
            "video_decoder.hevc", "video_encoder.hevc" },
        { MEDIA_MIMETYPE_VIDEO_MPEG4,
            "video_decoder.mpeg4", "video_encoder.mpeg4" },
        { MEDIA_MIMETYPE_VIDEO_H263,
            "video_decoder.h263", "video_encoder.h263" },
        { MEDIA_MIMETYPE_VIDEO_VP8,
            "video_decoder.vp8", "video_encoder.vp8" },
        { MEDIA_MIMETYPE_VIDEO_VP9,
            "video_decoder.vp9", "video_encoder.vp9" },
        { MEDIA_MIMETYPE_AUDIO_RAW,
            "audio_decoder.raw", "audio_encoder.raw" },
        { MEDIA_MIMETYPE_VIDEO_DOLBY_VISION,
            "video_decoder.dolby-vision", "video_encoder.dolby-vision" },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", "audio_encoder.flac" },
        { MEDIA_MIMETYPE_AUDIO_MSGSM,
            "audio_decoder.gsm", "audio_encoder.gsm" },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
            "video_decoder.mpeg2", "video_encoder.mpeg2" },
        { MEDIA_MIMETYPE_AUDIO_AC3,
            "audio_decoder.ac3", "audio_encoder.ac3" },
        { MEDIA_MIMETYPE_AUDIO_EAC3,
            "audio_decoder.eac3", "audio_encoder.eac3" },
    };

    static const size_t kNumMimeToRole =
        sizeof(kMimeToRole) / sizeof(kMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return FFMPEGSoftCodec::getComponentRole(isEncoder, mime);
    }

    return isEncoder ? kMimeToRole[i].encoderRole
                  : kMimeToRole[i].decoderRole;
}

//static
status_t ACodec::setComponentRole(
        const sp<IOMX> &omx, IOMX::node_id node, const char *role) {
    OMX_PARAM_COMPONENTROLETYPE roleParams;
    InitOMXParams(&roleParams);

    strncpy((char *)roleParams.cRole,
            role, OMX_MAX_STRINGNAME_SIZE - 1);

    roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

    return omx->setParameter(
            node, OMX_IndexParamStandardComponentRole,
            &roleParams, sizeof(roleParams));
}

status_t ACodec::configureCodec(
        const char *mime, const sp<AMessage> &msg) {
    int32_t encoder;
    if (!msg->findInt32("encoder", &encoder)) {
        encoder = false;
    }

    sp<AMessage> inputFormat = new AMessage;
    sp<AMessage> outputFormat = new AMessage;
    mConfigFormat = msg;

    mIsEncoder = encoder;

    mInputMetadataType = kMetadataBufferTypeInvalid;
    mOutputMetadataType = kMetadataBufferTypeInvalid;

    status_t err = setComponentRole(encoder /* isEncoder */, mime);

    if (err != OK) {
        return err;
    }

    int32_t bitRate = 0;
    // FLAC encoder doesn't need a bitrate, other encoders do
    if (encoder && strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)
            && !msg->findInt32("bitrate", &bitRate)) {
        return INVALID_OPERATION;
    }

    // propagate bitrate to the output so that the muxer has it
    if (encoder && msg->findInt32("bitrate", &bitRate)) {
        // Technically ISO spec says that 'bitrate' should be 0 for VBR even though it is the
        // average bitrate. We've been setting both bitrate and max-bitrate to this same value.
        outputFormat->setInt32("bitrate", bitRate);
        outputFormat->setInt32("max-bitrate", bitRate);
    }

    int32_t storeMeta;
    if (encoder
            && msg->findInt32("android._input-metadata-buffer-type", &storeMeta)
            && storeMeta != kMetadataBufferTypeInvalid) {
        mInputMetadataType = (MetadataBufferType)storeMeta;
        err = mOMX->storeMetaDataInBuffers(
                mNode, kPortIndexInput, OMX_TRUE, &mInputMetadataType);
        if (err != OK) {
            ALOGE("[%s] storeMetaDataInBuffers (input) failed w/ err %d",
                    mComponentName.c_str(), err);

            return err;
        } else if (storeMeta == kMetadataBufferTypeANWBuffer
                && mInputMetadataType == kMetadataBufferTypeGrallocSource) {
            // IOMX translates ANWBuffers to gralloc source already.
            mInputMetadataType = (MetadataBufferType)storeMeta;
        }

#ifdef CAMCORDER_GRALLOC_SOURCE
        // For this specific case we could be using camera source even if storeMetaDataInBuffers
        // returns Gralloc source. Pretend that we are; this will force us to use nBufferSize.
        if (mInputMetadataType == kMetadataBufferTypeGrallocSource) {
            mInputMetadataType = kMetadataBufferTypeCameraSource;
        }
#endif

        uint32_t usageBits;
        if (mOMX->getParameter(
                mNode, (OMX_INDEXTYPE)OMX_IndexParamConsumerUsageBits,
                &usageBits, sizeof(usageBits)) == OK) {
            inputFormat->setInt32(
                    "using-sw-read-often", !!(usageBits & GRALLOC_USAGE_SW_READ_OFTEN));
        }
    }

    int32_t prependSPSPPS = 0;
    if (encoder
            && msg->findInt32("prepend-sps-pps-to-idr-frames", &prependSPSPPS)
            && prependSPSPPS != 0) {
        OMX_INDEXTYPE index;
        err = mOMX->getExtensionIndex(
                mNode,
                "OMX.google.android.index.prependSPSPPSToIDRFrames",
                &index);

        if (err == OK) {
            PrependSPSPPSToIDRFramesParams params;
            InitOMXParams(&params);
            params.bEnable = OMX_TRUE;

            err = mOMX->setParameter(
                    mNode, index, &params, sizeof(params));
        }

        if (err != OK) {
            ALOGE("Encoder could not be configured to emit SPS/PPS before "
                  "IDR frames. (err %d)", err);

            return err;
        }
    }

    // Only enable metadata mode on encoder output if encoder can prepend
    // sps/pps to idr frames, since in metadata mode the bitstream is in an
    // opaque handle, to which we don't have access.
    int32_t video = !strncasecmp(mime, "video/", 6);
    mIsVideo = video;
    if (encoder && video) {
        OMX_BOOL enable = (OMX_BOOL) (prependSPSPPS
            && msg->findInt32("android._store-metadata-in-buffers-output", &storeMeta)
            && storeMeta != 0);

        mOutputMetadataType = kMetadataBufferTypeNativeHandleSource;
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexOutput, enable, &mOutputMetadataType);
        if (err != OK) {
            ALOGE("[%s] storeMetaDataInBuffers (output) failed w/ err %d",
                mComponentName.c_str(), err);
        }

        if (!msg->findInt64(
                    "repeat-previous-frame-after",
                    &mRepeatFrameDelayUs)) {
            mRepeatFrameDelayUs = -1ll;
        }

        if (!msg->findInt64("max-pts-gap-to-encoder", &mMaxPtsGapUs)) {
            mMaxPtsGapUs = -1ll;
        }

        if (!msg->findFloat("max-fps-to-encoder", &mMaxFps)) {
            mMaxFps = -1;
        }

        if (!msg->findInt64("time-lapse", &mTimePerCaptureUs)) {
            mTimePerCaptureUs = -1ll;
        }

        if (!msg->findInt32(
                    "create-input-buffers-suspended",
                    (int32_t*)&mCreateInputBuffersSuspended)) {
            mCreateInputBuffersSuspended = false;
        }
    }

    // NOTE: we only use native window for video decoders
    sp<RefBase> obj;
    bool haveNativeWindow = msg->findObject("native-window", &obj)
            && obj != NULL && video && !encoder;
    mUsingNativeWindow = haveNativeWindow;
    mLegacyAdaptiveExperiment = false;
    if (video && !encoder) {
        inputFormat->setInt32("adaptive-playback", false);

        int32_t usageProtected;
        if (msg->findInt32("protected", &usageProtected) && usageProtected) {
            if (!haveNativeWindow) {
                ALOGE("protected output buffers must be sent to an ANativeWindow");
                return PERMISSION_DENIED;
            }
            mFlags |= kFlagIsGrallocUsageProtected;
            mFlags |= kFlagPushBlankBuffersToNativeWindowOnShutdown;
        }

        if (mFlags & kFlagIsSecure) {
            // use native_handles for secure input buffers
            err = mOMX->enableNativeBuffers(
                    mNode, kPortIndexInput, OMX_FALSE /* graphic */, OMX_TRUE);
            ALOGI_IF(err != OK, "falling back to non-native_handles");
            err = OK; // ignore error for now
        }
    }
    if (haveNativeWindow) {
        sp<ANativeWindow> nativeWindow =
            static_cast<ANativeWindow *>(static_cast<Surface *>(obj.get()));

        // START of temporary support for automatic FRC - THIS WILL BE REMOVED
        int32_t autoFrc;
        if (msg->findInt32("auto-frc", &autoFrc)) {
            bool enabled = autoFrc;
            OMX_CONFIG_BOOLEANTYPE config;
            InitOMXParams(&config);
            config.bEnabled = (OMX_BOOL)enabled;
            status_t temp = mOMX->setConfig(
                    mNode, (OMX_INDEXTYPE)OMX_IndexConfigAutoFramerateConversion,
                    &config, sizeof(config));
            if (temp == OK) {
                outputFormat->setInt32("auto-frc", enabled);
            } else if (enabled) {
                ALOGI("codec does not support requested auto-frc (err %d)", temp);
            }
        }
        // END of temporary support for automatic FRC

        int32_t tunneled;
        if (msg->findInt32("feature-tunneled-playback", &tunneled) &&
            tunneled != 0) {
            ALOGI("Configuring TUNNELED video playback.");
            mTunneled = true;

            int32_t audioHwSync = 0;
            if (!msg->findInt32("audio-hw-sync", &audioHwSync)) {
                ALOGW("No Audio HW Sync provided for video tunnel");
            }
            err = configureTunneledVideoPlayback(audioHwSync, nativeWindow);
            if (err != OK) {
                ALOGE("configureTunneledVideoPlayback(%d,%p) failed!",
                        audioHwSync, nativeWindow.get());
                return err;
            }

            int32_t maxWidth = 0, maxHeight = 0;
            if (msg->findInt32("max-width", &maxWidth) &&
                    msg->findInt32("max-height", &maxHeight)) {

                err = mOMX->prepareForAdaptivePlayback(
                        mNode, kPortIndexOutput, OMX_TRUE, maxWidth, maxHeight);
                if (err != OK) {
                    ALOGW("[%s] prepareForAdaptivePlayback failed w/ err %d",
                            mComponentName.c_str(), err);
                    // allow failure
                    err = OK;
                } else {
                    inputFormat->setInt32("max-width", maxWidth);
                    inputFormat->setInt32("max-height", maxHeight);
                    inputFormat->setInt32("adaptive-playback", true);
                }
            }
        } else {
            ALOGV("Configuring CPU controlled video playback.");
            mTunneled = false;

            // Explicity reset the sideband handle of the window for
            // non-tunneled video in case the window was previously used
            // for a tunneled video playback.
            err = native_window_set_sideband_stream(nativeWindow.get(), NULL);
            if (err != OK) {
                ALOGE("set_sideband_stream(NULL) failed! (err %d).", err);
                return err;
            }

            // Always try to enable dynamic output buffers on native surface
            mOutputMetadataType = kMetadataBufferTypeANWBuffer;
            err = mOMX->storeMetaDataInBuffers(
                    mNode, kPortIndexOutput, OMX_TRUE, &mOutputMetadataType);
            if (err != OK) {
                ALOGE("[%s] storeMetaDataInBuffers failed w/ err %d",
                        mComponentName.c_str(), err);

                // if adaptive playback has been requested, try JB fallback
                // NOTE: THIS FALLBACK MECHANISM WILL BE REMOVED DUE TO ITS
                // LARGE MEMORY REQUIREMENT

                // we will not do adaptive playback on software accessed
                // surfaces as they never had to respond to changes in the
                // crop window, and we don't trust that they will be able to.
                int usageBits = 0;
                bool canDoAdaptivePlayback;

                if (nativeWindow->query(
                        nativeWindow.get(),
                        NATIVE_WINDOW_CONSUMER_USAGE_BITS,
                        &usageBits) != OK) {
                    canDoAdaptivePlayback = false;
                } else {
                    canDoAdaptivePlayback =
                        (usageBits &
                                (GRALLOC_USAGE_SW_READ_MASK |
                                 GRALLOC_USAGE_SW_WRITE_MASK)) == 0;
                }

                int32_t maxWidth = 0, maxHeight = 0;
                if (canDoAdaptivePlayback &&
                        msg->findInt32("max-width", &maxWidth) &&
                        msg->findInt32("max-height", &maxHeight)) {
                    ALOGV("[%s] prepareForAdaptivePlayback(%dx%d)",
                            mComponentName.c_str(), maxWidth, maxHeight);

                    err = mOMX->prepareForAdaptivePlayback(
                            mNode, kPortIndexOutput, OMX_TRUE, maxWidth,
                            maxHeight);
                    ALOGW_IF(err != OK,
                            "[%s] prepareForAdaptivePlayback failed w/ err %d",
                            mComponentName.c_str(), err);

                    if (err == OK) {
                        inputFormat->setInt32("max-width", maxWidth);
                        inputFormat->setInt32("max-height", maxHeight);
                        inputFormat->setInt32("adaptive-playback", true);
                    }
                }
                // allow failure
                err = OK;
            } else {
                ALOGV("[%s] storeMetaDataInBuffers succeeded",
                        mComponentName.c_str());
                CHECK(storingMetadataInDecodedBuffers());
                mLegacyAdaptiveExperiment = ADebug::isExperimentEnabled(
                        "legacy-adaptive", !msg->contains("no-experiments"));

                inputFormat->setInt32("adaptive-playback", true);
            }

            int32_t push;
            if (msg->findInt32("push-blank-buffers-on-shutdown", &push)
                    && push != 0) {
                mFlags |= kFlagPushBlankBuffersToNativeWindowOnShutdown;
            }
        }

        int32_t rotationDegrees;
        if (msg->findInt32("rotation-degrees", &rotationDegrees)) {
            mRotationDegrees = rotationDegrees;
        } else {
            mRotationDegrees = 0;
        }
    }

    AudioEncoding pcmEncoding = kAudioEncodingPcm16bit;
    (void)msg->findInt32("pcm-encoding", (int32_t*)&pcmEncoding);
    // invalid encodings will default to PCM-16bit in setupRawAudioFormat.

    if (video) {
        // determine need for software renderer
        bool usingSwRenderer = false;
        if (haveNativeWindow && (mComponentName.startsWith("OMX.google.") ||
                                 mComponentName.startsWith("OMX.ffmpeg."))) {
            usingSwRenderer = true;
            haveNativeWindow = false;
        }

        if (encoder) {
            err = setupVideoEncoder(mime, msg, outputFormat, inputFormat);
        } else {
            err = setupVideoDecoder(mime, msg, haveNativeWindow, usingSwRenderer, outputFormat);
        }

        if (err != OK) {
            return err;
        }

        if (haveNativeWindow) {
            mNativeWindow = static_cast<Surface *>(obj.get());
        }

        // initialize native window now to get actual output format
        // TODO: this is needed for some encoders even though they don't use native window
        err = initNativeWindow();
        if (err != OK) {
            return err;
        }

        // fallback for devices that do not handle flex-YUV for native buffers
        if (haveNativeWindow) {
            int32_t requestedColorFormat = OMX_COLOR_FormatUnused;
            if (msg->findInt32("color-format", &requestedColorFormat) &&
                    requestedColorFormat == OMX_COLOR_FormatYUV420Flexible) {
                status_t err = getPortFormat(kPortIndexOutput, outputFormat);
                if (err != OK) {
                    return err;
                }
                int32_t colorFormat = OMX_COLOR_FormatUnused;
                OMX_U32 flexibleEquivalent = OMX_COLOR_FormatUnused;
                if (!outputFormat->findInt32("color-format", &colorFormat)) {
                    ALOGE("ouptut port did not have a color format (wrong domain?)");
                    return BAD_VALUE;
                }
                ALOGD("[%s] Requested output format %#x and got %#x.",
                        mComponentName.c_str(), requestedColorFormat, colorFormat);
                if (!isFlexibleColorFormat(
                                mOMX, mNode, colorFormat, haveNativeWindow, &flexibleEquivalent)
                        || flexibleEquivalent != (OMX_U32)requestedColorFormat) {
                    // device did not handle flex-YUV request for native window, fall back
                    // to SW renderer
                    ALOGI("[%s] Falling back to software renderer", mComponentName.c_str());
                    mNativeWindow.clear();
                    mNativeWindowUsageBits = 0;
                    haveNativeWindow = false;
                    usingSwRenderer = true;
                    if (storingMetadataInDecodedBuffers()) {
                        err = mOMX->storeMetaDataInBuffers(
                                mNode, kPortIndexOutput, OMX_FALSE, &mOutputMetadataType);
                        mOutputMetadataType = kMetadataBufferTypeInvalid; // just in case
                        // TODO: implement adaptive-playback support for bytebuffer mode.
                        // This is done by SW codecs, but most HW codecs don't support it.
                        inputFormat->setInt32("adaptive-playback", false);
                    }
                    if (err == OK) {
                        err = mOMX->enableNativeBuffers(
                                mNode, kPortIndexOutput, OMX_TRUE /* graphic */, OMX_FALSE);
                    }
                    if (mFlags & kFlagIsGrallocUsageProtected) {
                        // fallback is not supported for protected playback
                        err = PERMISSION_DENIED;
                    } else if (err == OK) {
                        err = setupVideoDecoder(
                                mime, msg, haveNativeWindow, usingSwRenderer, outputFormat);
                    }
                }
            }
        }

        if (usingSwRenderer) {
            outputFormat->setInt32("using-sw-renderer", 1);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
        int32_t numChannels, sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            // Since we did not always check for these, leave them optional
            // and have the decoder figure it all out.
            err = OK;
        } else {
            err = setupRawAudioFormat(
                    encoder ? kPortIndexInput : kPortIndexOutput,
                    sampleRate,
                    numChannels, pcmEncoding);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        int32_t numChannels, sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            int32_t isADTS, aacProfile;
            int32_t sbrMode;
            int32_t maxOutputChannelCount;
            int32_t pcmLimiterEnable;
            drcParams_t drc;
            if (!msg->findInt32("is-adts", &isADTS)) {
                isADTS = 0;
            }
            if (!msg->findInt32("aac-profile", &aacProfile)) {
                aacProfile = OMX_AUDIO_AACObjectNull;
            }
            if (!msg->findInt32("aac-sbr-mode", &sbrMode)) {
                sbrMode = -1;
            }

            if (!msg->findInt32("aac-max-output-channel_count", &maxOutputChannelCount)) {
                maxOutputChannelCount = -1;
            }
            if (!msg->findInt32("aac-pcm-limiter-enable", &pcmLimiterEnable)) {
                // value is unknown
                pcmLimiterEnable = -1;
            }
            if (!msg->findInt32("aac-encoded-target-level", &drc.encodedTargetLevel)) {
                // value is unknown
                drc.encodedTargetLevel = -1;
            }
            if (!msg->findInt32("aac-drc-cut-level", &drc.drcCut)) {
                // value is unknown
                drc.drcCut = -1;
            }
            if (!msg->findInt32("aac-drc-boost-level", &drc.drcBoost)) {
                // value is unknown
                drc.drcBoost = -1;
            }
            if (!msg->findInt32("aac-drc-heavy-compression", &drc.heavyCompression)) {
                // value is unknown
                drc.heavyCompression = -1;
            }
            if (!msg->findInt32("aac-target-ref-level", &drc.targetRefLevel)) {
                // value is unknown
                drc.targetRefLevel = -1;
            }

            err = setupAACCodec(
                    encoder, numChannels, sampleRate, bitRate, aacProfile,
                    isADTS != 0, sbrMode, maxOutputChannelCount, drc,
                    pcmLimiterEnable, pcmEncoding);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_NB)) {
        err = setupAMRCodec(encoder, false /* isWAMR */, bitRate);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        err = setupAMRCodec(encoder, true /* isWAMR */, bitRate);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_G711_ALAW)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_G711_MLAW)) {
        // These are PCM-like formats with a fixed sample rate but
        // a variable number of channels.

        int32_t numChannels;
        if (!msg->findInt32("channel-count", &numChannels)) {
            err = INVALID_OPERATION;
        } else {
            int32_t sampleRate;
            if (!msg->findInt32("sample-rate", &sampleRate)) {
                sampleRate = 8000;
            }
            err = setupG711Codec(encoder, sampleRate, numChannels);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC) && encoder) {
        int32_t numChannels = 0, sampleRate = 0, compressionLevel = -1;
        if (encoder &&
                (!msg->findInt32("channel-count", &numChannels)
                        || !msg->findInt32("sample-rate", &sampleRate))) {
            ALOGE("missing channel count or sample rate for FLAC encoder");
            err = INVALID_OPERATION;
        } else {
            if (encoder) {
                if (!msg->findInt32(
                            "complexity", &compressionLevel) &&
                    !msg->findInt32(
                            "flac-compression-level", &compressionLevel)) {
                    compressionLevel = 5; // default FLAC compression level
                } else if (compressionLevel < 0) {
                    ALOGW("compression level %d outside [0..8] range, "
                          "using 0",
                          compressionLevel);
                    compressionLevel = 0;
                } else if (compressionLevel > 8) {
                    ALOGW("compression level %d outside [0..8] range, "
                          "using 8",
                          compressionLevel);
                    compressionLevel = 8;
                }
            }
            err = setupFlacCodec(
                    encoder, numChannels, sampleRate, compressionLevel);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
        int32_t numChannels, sampleRate;
        if (encoder
                || !msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            err = setupRawAudioFormat(kPortIndexInput, sampleRate, numChannels, pcmEncoding);
        }
    } else if (!strncmp(mComponentName.c_str(), "OMX.google.", 11)
            && !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AC3)) {
        int32_t numChannels;
        int32_t sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            err = setupAC3Codec(encoder, numChannels, sampleRate, pcmEncoding);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_EAC3)) {
        int32_t numChannels;
        int32_t sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            err = setupEAC3Codec(encoder, numChannels, sampleRate, pcmEncoding);
        }
    } else {
        err = setupCustomCodec(err, mime, msg);
    }

    if (err != OK) {
        return err;
    }

    if (!msg->findInt32("encoder-delay", &mEncoderDelay)) {
        mEncoderDelay = 0;
    }

    if (!msg->findInt32("encoder-padding", &mEncoderPadding)) {
        mEncoderPadding = 0;
    }

    if (msg->findInt32("channel-mask", &mChannelMask)) {
        mChannelMaskPresent = true;
    } else {
        mChannelMaskPresent = false;
    }

    int32_t maxInputSize;
    if (msg->findInt32("max-input-size", &maxInputSize)) {
        err = setMinBufferSize(kPortIndexInput, (size_t)maxInputSize);
    } else if (!strcmp("OMX.Nvidia.aac.decoder", mComponentName.c_str())) {
        err = setMinBufferSize(kPortIndexInput, 8192);  // XXX
    }

    int32_t priority;
    if (msg->findInt32("priority", &priority)) {
        err = setPriority(priority);
    }

    int32_t rateInt = -1;
    float rateFloat = -1;
    if (!msg->findFloat("operating-rate", &rateFloat)) {
        msg->findInt32("operating-rate", &rateInt);
        rateFloat = (float)rateInt;  // 16MHz (FLINTMAX) is OK for upper bound.
    }
    if (rateFloat > 0) {
        err = setOperatingRate(rateFloat, video);
    }

    // NOTE: both mBaseOutputFormat and mOutputFormat are outputFormat to signal first frame.
    mBaseOutputFormat = outputFormat;
    // trigger a kWhatOutputFormatChanged msg on first buffer
    mLastOutputFormat.clear();

    err = getPortFormat(kPortIndexInput, inputFormat);
    if (err == OK) {
        err = getPortFormat(kPortIndexOutput, outputFormat);
        if (err == OK) {
            mInputFormat = inputFormat;
            mOutputFormat = outputFormat;
        }
    }

    // create data converters if needed
    if (!video && err == OK) {
        AudioEncoding codecPcmEncoding = kAudioEncodingPcm16bit;
        if (encoder) {
            (void)mInputFormat->findInt32("pcm-encoding", (int32_t*)&codecPcmEncoding);
            mConverter[kPortIndexInput] = AudioConverter::Create(pcmEncoding, codecPcmEncoding);
            if (mConverter[kPortIndexInput] != NULL) {
                mInputFormat->setInt32("pcm-encoding", pcmEncoding);
            }
        } else {
            (void)mOutputFormat->findInt32("pcm-encoding", (int32_t*)&codecPcmEncoding);
            mConverter[kPortIndexOutput] = AudioConverter::Create(codecPcmEncoding, pcmEncoding);
            if (mConverter[kPortIndexOutput] != NULL) {
                mOutputFormat->setInt32("pcm-encoding", pcmEncoding);
            }
        }
    }

    return err;
}

status_t ACodec::setPriority(int32_t priority) {
    if (priority < 0) {
        return BAD_VALUE;
    }
    OMX_PARAM_U32TYPE config;
    InitOMXParams(&config);
    config.nU32 = (OMX_U32)priority;
    status_t temp = mOMX->setConfig(
            mNode, (OMX_INDEXTYPE)OMX_IndexConfigPriority,
            &config, sizeof(config));
    if (temp != OK) {
        ALOGI("codec does not support config priority (err %d)", temp);
    }
    return OK;
}

status_t ACodec::setOperatingRate(float rateFloat, bool isVideo) {
    if (rateFloat < 0) {
        return BAD_VALUE;
    }
    OMX_U32 rate;
    if (isVideo) {
        if (rateFloat > 65535) {
            return BAD_VALUE;
        }
        rate = (OMX_U32)(rateFloat * 65536.0f + 0.5f);
    } else {
        if (rateFloat > UINT_MAX) {
            return BAD_VALUE;
        }
        rate = (OMX_U32)(rateFloat);
    }
    OMX_PARAM_U32TYPE config;
    InitOMXParams(&config);
    config.nU32 = rate;
    status_t err = mOMX->setConfig(
            mNode, (OMX_INDEXTYPE)OMX_IndexConfigOperatingRate,
            &config, sizeof(config));
    if (err != OK) {
        ALOGI("codec does not support config operating rate (err %d)", err);
    }
    return OK;
}

status_t ACodec::getIntraRefreshPeriod(uint32_t *intraRefreshPeriod) {
    OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;
    status_t err = mOMX->getConfig(
            mNode, (OMX_INDEXTYPE)OMX_IndexConfigAndroidIntraRefresh, &params, sizeof(params));
    if (err == OK) {
        *intraRefreshPeriod = params.nRefreshPeriod;
        return OK;
    }

    // Fallback to query through standard OMX index.
    OMX_VIDEO_PARAM_INTRAREFRESHTYPE refreshParams;
    InitOMXParams(&refreshParams);
    refreshParams.nPortIndex = kPortIndexOutput;
    refreshParams.eRefreshMode = OMX_VIDEO_IntraRefreshCyclic;
    err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoIntraRefresh, &refreshParams, sizeof(refreshParams));
    if (err != OK || refreshParams.nCirMBs == 0) {
        *intraRefreshPeriod = 0;
        return OK;
    }

    // Calculate period based on width and height
    uint32_t width, height;
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;
    def.nPortIndex = kPortIndexOutput;
    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        *intraRefreshPeriod = 0;
        return err;
    }
    width = video_def->nFrameWidth;
    height = video_def->nFrameHeight;
    // Use H.264/AVC MacroBlock size 16x16
    *intraRefreshPeriod = divUp((divUp(width, 16u) * divUp(height, 16u)), refreshParams.nCirMBs);

    return OK;
}

status_t ACodec::setIntraRefreshPeriod(uint32_t intraRefreshPeriod, bool inConfigure) {
    OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;
    params.nRefreshPeriod = intraRefreshPeriod;
    status_t err = mOMX->setConfig(
            mNode, (OMX_INDEXTYPE)OMX_IndexConfigAndroidIntraRefresh, &params, sizeof(params));
    if (err == OK) {
        return OK;
    }

    // Only in configure state, a component could invoke setParameter.
    if (!inConfigure) {
        return INVALID_OPERATION;
    } else {
        ALOGI("[%s] try falling back to Cyclic", mComponentName.c_str());
    }

    OMX_VIDEO_PARAM_INTRAREFRESHTYPE refreshParams;
    InitOMXParams(&refreshParams);
    refreshParams.nPortIndex = kPortIndexOutput;
    refreshParams.eRefreshMode = OMX_VIDEO_IntraRefreshCyclic;

    if (intraRefreshPeriod == 0) {
        // 0 means disable intra refresh.
        refreshParams.nCirMBs = 0;
    } else {
        // Calculate macroblocks that need to be intra coded base on width and height
        uint32_t width, height;
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;
        def.nPortIndex = kPortIndexOutput;
        err = mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
        if (err != OK) {
            return err;
        }
        width = video_def->nFrameWidth;
        height = video_def->nFrameHeight;
        // Use H.264/AVC MacroBlock size 16x16
        refreshParams.nCirMBs = divUp((divUp(width, 16u) * divUp(height, 16u)), intraRefreshPeriod);
    }

    err = mOMX->setParameter(mNode, OMX_IndexParamVideoIntraRefresh,
                             &refreshParams, sizeof(refreshParams));
    if (err != OK) {
        return err;
    }

    return OK;
}

status_t ACodec::configureTemporalLayers(
        const sp<AMessage> &msg, bool inConfigure, sp<AMessage> &outputFormat) {
    if (!mIsVideo || !mIsEncoder) {
        return INVALID_OPERATION;
    }

    AString tsSchema;
    if (!msg->findString("ts-schema", &tsSchema)) {
        return OK;
    }

    unsigned int numLayers = 0;
    unsigned int numBLayers = 0;
    int tags;
    char dummy;
    OMX_VIDEO_ANDROID_TEMPORALLAYERINGPATTERNTYPE pattern =
        OMX_VIDEO_AndroidTemporalLayeringPatternNone;
    if (sscanf(tsSchema.c_str(), "webrtc.vp8.%u-layer%c", &numLayers, &dummy) == 1
            && numLayers > 0) {
        pattern = OMX_VIDEO_AndroidTemporalLayeringPatternWebRTC;
    } else if ((tags = sscanf(tsSchema.c_str(), "android.generic.%u%c%u%c",
                    &numLayers, &dummy, &numBLayers, &dummy))
            && (tags == 1 || (tags == 3 && dummy == '+'))
            && numLayers > 0 && numLayers < UINT32_MAX - numBLayers) {
        numLayers += numBLayers;
        pattern = OMX_VIDEO_AndroidTemporalLayeringPatternAndroid;
    } else {
        ALOGI("Ignoring unsupported ts-schema [%s]", tsSchema.c_str());
        return BAD_VALUE;
    }

    OMX_VIDEO_PARAM_ANDROID_TEMPORALLAYERINGTYPE layerParams;
    InitOMXParams(&layerParams);
    layerParams.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
        mNode, (OMX_INDEXTYPE)OMX_IndexParamAndroidVideoTemporalLayering,
        &layerParams, sizeof(layerParams));

    if (err != OK) {
        return err;
    } else if (!(layerParams.eSupportedPatterns & pattern)) {
        return BAD_VALUE;
    }

    numLayers = min(numLayers, layerParams.nLayerCountMax);
    numBLayers = min(numBLayers, layerParams.nBLayerCountMax);

    if (!inConfigure) {
        OMX_VIDEO_CONFIG_ANDROID_TEMPORALLAYERINGTYPE layerConfig;
        InitOMXParams(&layerConfig);
        layerConfig.nPortIndex = kPortIndexOutput;
        layerConfig.ePattern = pattern;
        layerConfig.nPLayerCountActual = numLayers - numBLayers;
        layerConfig.nBLayerCountActual = numBLayers;
        layerConfig.bBitrateRatiosSpecified = OMX_FALSE;

        err = mOMX->setConfig(
                mNode, (OMX_INDEXTYPE)OMX_IndexConfigAndroidVideoTemporalLayering,
                &layerConfig, sizeof(layerConfig));
    } else {
        layerParams.ePattern = pattern;
        layerParams.nPLayerCountActual = numLayers - numBLayers;
        layerParams.nBLayerCountActual = numBLayers;
        layerParams.bBitrateRatiosSpecified = OMX_FALSE;

        err = mOMX->setParameter(
                mNode, (OMX_INDEXTYPE)OMX_IndexParamAndroidVideoTemporalLayering,
                &layerParams, sizeof(layerParams));
    }

    AString configSchema;
    if (pattern == OMX_VIDEO_AndroidTemporalLayeringPatternAndroid) {
        configSchema = AStringPrintf("android.generic.%u+%u", numLayers - numBLayers, numBLayers);
    } else if (pattern == OMX_VIDEO_AndroidTemporalLayeringPatternWebRTC) {
        configSchema = AStringPrintf("webrtc.vp8.%u", numLayers);
    }

    if (err != OK) {
        ALOGW("Failed to set temporal layers to %s (requested %s)",
                configSchema.c_str(), tsSchema.c_str());
        return err;
    }

    err = mOMX->getParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamAndroidVideoTemporalLayering,
            &layerParams, sizeof(layerParams));

    if (err == OK) {
        ALOGD("Temporal layers requested:%s configured:%s got:%s(%u: P=%u, B=%u)",
                tsSchema.c_str(), configSchema.c_str(),
                asString(layerParams.ePattern), layerParams.ePattern,
                layerParams.nPLayerCountActual, layerParams.nBLayerCountActual);

        if (outputFormat.get() == mOutputFormat.get()) {
            mOutputFormat = mOutputFormat->dup(); // trigger an output format change event
        }
        // assume we got what we configured
        outputFormat->setString("ts-schema", configSchema);
    }
    return err;
}

status_t ACodec::setMinBufferSize(OMX_U32 portIndex, size_t size) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    if (def.nBufferSize >= size) {
        return OK;
    }

    def.nBufferSize = size;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    if (def.nBufferSize < size) {
        ALOGE("failed to set min buffer size to %zu (is still %u)", size, def.nBufferSize);
        return FAILED_TRANSACTION;
    }

    return OK;
}

status_t ACodec::selectAudioPortFormat(
        OMX_U32 portIndex, OMX_AUDIO_CODINGTYPE desiredFormat) {
    OMX_AUDIO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);

    format.nPortIndex = portIndex;
    for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        if (format.eEncoding == desiredFormat) {
            break;
        }

        if (index == kMaxIndicesToCheck) {
            ALOGW("[%s] stopping checking formats after %u: %s(%x)",
                    mComponentName.c_str(), index,
                    asString(format.eEncoding), format.eEncoding);
            return ERROR_UNSUPPORTED;
        }
    }

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioPortFormat, &format, sizeof(format));
}

status_t ACodec::setupAACCodec(
        bool encoder, int32_t numChannels, int32_t sampleRate,
        int32_t bitRate, int32_t aacProfile, bool isADTS, int32_t sbrMode,
        int32_t maxOutputChannelCount, const drcParams_t& drc,
        int32_t pcmLimiterEnable, AudioEncoding encoding) {
    if (encoder && isADTS) {
        return -EINVAL;
    }

    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            sampleRate,
            numChannels,
            encoding);

    if (err != OK) {
        return err;
    }

    if (encoder) {
        err = selectAudioPortFormat(kPortIndexOutput, OMX_AUDIO_CodingAAC);

        if (err != OK) {
            return err;
        }

        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            return err;
        }

        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            return err;
        }

        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            return err;
        }

        profile.nChannels = numChannels;

        profile.eChannelMode =
            (numChannels == 1)
                ? OMX_AUDIO_ChannelModeMono: OMX_AUDIO_ChannelModeStereo;

        profile.nSampleRate = sampleRate;
        profile.nBitRate = bitRate;
        profile.nAudioBandWidth = 0;
        profile.nFrameLength = 0;
        profile.nAACtools = OMX_AUDIO_AACToolAll;
        profile.nAACERtools = OMX_AUDIO_AACERNone;
        profile.eAACProfile = (OMX_AUDIO_AACPROFILETYPE) aacProfile;
        profile.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
        switch (sbrMode) {
        case 0:
            // disable sbr
            profile.nAACtools &= ~OMX_AUDIO_AACToolAndroidSSBR;
            profile.nAACtools &= ~OMX_AUDIO_AACToolAndroidDSBR;
            break;
        case 1:
            // enable single-rate sbr
            profile.nAACtools |= OMX_AUDIO_AACToolAndroidSSBR;
            profile.nAACtools &= ~OMX_AUDIO_AACToolAndroidDSBR;
            break;
        case 2:
            // enable dual-rate sbr
            profile.nAACtools &= ~OMX_AUDIO_AACToolAndroidSSBR;
            profile.nAACtools |= OMX_AUDIO_AACToolAndroidDSBR;
            break;
        case -1:
            // enable both modes -> the codec will decide which mode should be used
            profile.nAACtools |= OMX_AUDIO_AACToolAndroidSSBR;
            profile.nAACtools |= OMX_AUDIO_AACToolAndroidDSBR;
            break;
        default:
            // unsupported sbr mode
            return BAD_VALUE;
        }


        err = mOMX->setParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            return err;
        }

        return err;
    }

    OMX_AUDIO_PARAM_AACPROFILETYPE profile;
    InitOMXParams(&profile);
    profile.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

    if (err != OK) {
        return err;
    }

    profile.nChannels = numChannels;
    profile.nSampleRate = sampleRate;

    profile.eAACStreamFormat =
        isADTS
            ? OMX_AUDIO_AACStreamFormatMP4ADTS
            : OMX_AUDIO_AACStreamFormatMP4FF;

    OMX_AUDIO_PARAM_ANDROID_AACPRESENTATIONTYPE presentation;
    InitOMXParams(&presentation);
    presentation.nMaxOutputChannels = maxOutputChannelCount;
    presentation.nDrcCut = drc.drcCut;
    presentation.nDrcBoost = drc.drcBoost;
    presentation.nHeavyCompression = drc.heavyCompression;
    presentation.nTargetReferenceLevel = drc.targetRefLevel;
    presentation.nEncodedTargetLevel = drc.encodedTargetLevel;
    presentation.nPCMLimiterEnable = pcmLimiterEnable;

    status_t res = mOMX->setParameter(mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));
    if (res == OK) {
        // optional parameters, will not cause configuration failure
        mOMX->setParameter(mNode, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAacPresentation,
                &presentation, sizeof(presentation));
    } else {
        ALOGW("did not set AudioAndroidAacPresentation due to error %d when setting AudioAac", res);
    }
    return res;
}

status_t ACodec::setupAC3Codec(
        bool encoder, int32_t numChannels, int32_t sampleRate, AudioEncoding encoding) {
    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput, sampleRate, numChannels, encoding);

    if (err != OK) {
        return err;
    }

    if (encoder) {
        ALOGW("AC3 encoding is not supported.");
        return INVALID_OPERATION;
    }

    OMX_AUDIO_PARAM_ANDROID_AC3TYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
            &def,
            sizeof(def));

    if (err != OK) {
        return err;
    }

    def.nChannels = numChannels;
    def.nSampleRate = sampleRate;

    return mOMX->setParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
            &def,
            sizeof(def));
}

status_t ACodec::setupEAC3Codec(
        bool encoder, int32_t numChannels, int32_t sampleRate, AudioEncoding encoding) {
    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput, sampleRate, numChannels, encoding);

    if (err != OK) {
        return err;
    }

    if (encoder) {
        ALOGW("EAC3 encoding is not supported.");
        return INVALID_OPERATION;
    }

    OMX_AUDIO_PARAM_ANDROID_EAC3TYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidEac3,
            &def,
            sizeof(def));

    if (err != OK) {
        return err;
    }

    def.nChannels = numChannels;
    def.nSampleRate = sampleRate;

    return mOMX->setParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidEac3,
            &def,
            sizeof(def));
}

static OMX_AUDIO_AMRBANDMODETYPE pickModeFromBitRate(
        bool isAMRWB, int32_t bps) {
    if (isAMRWB) {
        if (bps <= 6600) {
            return OMX_AUDIO_AMRBandModeWB0;
        } else if (bps <= 8850) {
            return OMX_AUDIO_AMRBandModeWB1;
        } else if (bps <= 12650) {
            return OMX_AUDIO_AMRBandModeWB2;
        } else if (bps <= 14250) {
            return OMX_AUDIO_AMRBandModeWB3;
        } else if (bps <= 15850) {
            return OMX_AUDIO_AMRBandModeWB4;
        } else if (bps <= 18250) {
            return OMX_AUDIO_AMRBandModeWB5;
        } else if (bps <= 19850) {
            return OMX_AUDIO_AMRBandModeWB6;
        } else if (bps <= 23050) {
            return OMX_AUDIO_AMRBandModeWB7;
        }

        // 23850 bps
        return OMX_AUDIO_AMRBandModeWB8;
    } else {  // AMRNB
        if (bps <= 4750) {
            return OMX_AUDIO_AMRBandModeNB0;
        } else if (bps <= 5150) {
            return OMX_AUDIO_AMRBandModeNB1;
        } else if (bps <= 5900) {
            return OMX_AUDIO_AMRBandModeNB2;
        } else if (bps <= 6700) {
            return OMX_AUDIO_AMRBandModeNB3;
        } else if (bps <= 7400) {
            return OMX_AUDIO_AMRBandModeNB4;
        } else if (bps <= 7950) {
            return OMX_AUDIO_AMRBandModeNB5;
        } else if (bps <= 10200) {
            return OMX_AUDIO_AMRBandModeNB6;
        }

        // 12200 bps
        return OMX_AUDIO_AMRBandModeNB7;
    }
}

status_t ACodec::setupAMRCodec(bool encoder, bool isWAMR, int32_t bitrate) {
    OMX_AUDIO_PARAM_AMRTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = encoder ? kPortIndexOutput : kPortIndexInput;

    status_t err =
        mOMX->getParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.eAMRFrameFormat = OMX_AUDIO_AMRFrameFormatFSF;
    def.eAMRBandMode = pickModeFromBitRate(isWAMR, bitrate);

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    return setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            isWAMR ? 16000 : 8000 /* sampleRate */,
            1 /* numChannels */);
}

status_t ACodec::setupG711Codec(bool encoder, int32_t sampleRate, int32_t numChannels) {
    if (encoder) {
        return INVALID_OPERATION;
    }

    return setupRawAudioFormat(
            kPortIndexInput, sampleRate, numChannels);
}

status_t ACodec::setupFlacCodec(
        bool encoder, int32_t numChannels, int32_t sampleRate, int32_t compressionLevel) {

    if (encoder) {
        OMX_AUDIO_PARAM_FLACTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;

        // configure compression level
        status_t err = mOMX->getParameter(mNode, OMX_IndexParamAudioFlac, &def, sizeof(def));
        if (err != OK) {
            ALOGE("setupFlacCodec(): Error %d getting OMX_IndexParamAudioFlac parameter", err);
            return err;
        }
        def.nCompressionLevel = compressionLevel;
        err = mOMX->setParameter(mNode, OMX_IndexParamAudioFlac, &def, sizeof(def));
        if (err != OK) {
            ALOGE("setupFlacCodec(): Error %d setting OMX_IndexParamAudioFlac parameter", err);
            return err;
        }
    }

    return setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            sampleRate,
            numChannels);
}

status_t ACodec::setupRawAudioFormat(
        OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels, AudioEncoding encoding) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    if (err != OK) {
        return err;
    }

    pcmParams.nChannels = numChannels;
    switch (encoding) {
        case kAudioEncodingPcm8bit:
            pcmParams.eNumData = OMX_NumericalDataUnsigned;
            pcmParams.nBitPerSample = 8;
            break;
        case kAudioEncodingPcmFloat:
            pcmParams.eNumData = OMX_NumericalDataFloat;
            pcmParams.nBitPerSample = 32;
            break;
        case kAudioEncodingPcm16bit:
            pcmParams.eNumData = OMX_NumericalDataSigned;
            pcmParams.nBitPerSample = 16;
            break;
        case kAudioEncodingPcm24bitPacked:
            pcmParams.eNumData = OMX_NumericalDataSigned;
            pcmParams.nBitPerSample = 24;
            break;
        default:
            return BAD_VALUE;
    }
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    if (getOMXChannelMapping(numChannels, pcmParams.eChannelMapping) != OK) {
        return OMX_ErrorNone;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
    // if we could not set up raw format to non-16-bit, try with 16-bit
    // NOTE: we will also verify this via readback, in case codec ignores these fields
    if (err != OK && encoding != kAudioEncodingPcm16bit) {
        pcmParams.eNumData = OMX_NumericalDataSigned;
        pcmParams.nBitPerSample = 16;
        err = mOMX->setParameter(
                mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
    }
    return err;
}

status_t ACodec::configureTunneledVideoPlayback(
        int32_t audioHwSync, const sp<ANativeWindow> &nativeWindow) {
    native_handle_t* sidebandHandle;

    status_t err = mOMX->configureVideoTunnelMode(
            mNode, kPortIndexOutput, OMX_TRUE, audioHwSync, &sidebandHandle);
    if (err != OK) {
        ALOGE("configureVideoTunnelMode failed! (err %d).", err);
        return err;
    }

    err = native_window_set_sideband_stream(nativeWindow.get(), sidebandHandle);
    if (err != OK) {
        ALOGE("native_window_set_sideband_stream(%p) failed! (err %d).",
                sidebandHandle, err);
        return err;
    }

    return OK;
}

status_t ACodec::setVideoPortFormatType(
        OMX_U32 portIndex,
        OMX_VIDEO_CODINGTYPE compressionFormat,
        OMX_COLOR_FORMATTYPE colorFormat,
        bool usingNativeBuffers) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = portIndex;
    format.nIndex = 0;
    bool found = false;

    for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        // substitute back flexible color format to codec supported format
        OMX_U32 flexibleEquivalent;
        if (compressionFormat == OMX_VIDEO_CodingUnused
                && isFlexibleColorFormat(
                        mOMX, mNode, format.eColorFormat, usingNativeBuffers, &flexibleEquivalent)
                && colorFormat == flexibleEquivalent) {
            ALOGI("[%s] using color format %#x in place of %#x",
                    mComponentName.c_str(), format.eColorFormat, colorFormat);
            colorFormat = format.eColorFormat;
        }

        // The following assertion is violated by TI's video decoder.
        // CHECK_EQ(format.nIndex, index);

        if (!strcmp("OMX.TI.Video.encoder", mComponentName.c_str())) {
            if (portIndex == kPortIndexInput
                    && colorFormat == format.eColorFormat) {
                // eCompressionFormat does not seem right.
                found = true;
                break;
            }
            if (portIndex == kPortIndexOutput
                    && compressionFormat == format.eCompressionFormat) {
                // eColorFormat does not seem right.
                found = true;
                break;
            }
        }

        if (format.eCompressionFormat == compressionFormat
            && format.eColorFormat == colorFormat) {
            found = true;
            break;
        }

        if (index == kMaxIndicesToCheck) {
            ALOGW("[%s] stopping checking formats after %u: %s(%x)/%s(%x)",
                    mComponentName.c_str(), index,
                    asString(format.eCompressionFormat), format.eCompressionFormat,
                    asString(format.eColorFormat), format.eColorFormat);
        }
    }

    if (!found) {
        return UNKNOWN_ERROR;
    }

    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));

    return err;
}

// Set optimal output format. OMX component lists output formats in the order
// of preference, but this got more complicated since the introduction of flexible
// YUV formats. We support a legacy behavior for applications that do not use
// surface output, do not specify an output format, but expect a "usable" standard
// OMX format. SW readable and standard formats must be flex-YUV.
//
// Suggested preference order:
// - optimal format for texture rendering (mediaplayer behavior)
// - optimal SW readable & texture renderable format (flex-YUV support)
// - optimal SW readable non-renderable format (flex-YUV bytebuffer support)
// - legacy "usable" standard formats
//
// For legacy support, we prefer a standard format, but will settle for a SW readable
// flex-YUV format.
status_t ACodec::setSupportedOutputFormat(bool getLegacyFlexibleFormat) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format, legacyFormat;
    InitOMXParams(&format);
    format.nPortIndex = kPortIndexOutput;

    InitOMXParams(&legacyFormat);
    // this field will change when we find a suitable legacy format
    legacyFormat.eColorFormat = OMX_COLOR_FormatUnused;

    for (OMX_U32 index = 0; ; ++index) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));
        if (err != OK) {
            // no more formats, pick legacy format if found
            if (legacyFormat.eColorFormat != OMX_COLOR_FormatUnused) {
                 memcpy(&format, &legacyFormat, sizeof(format));
                 break;
            }
            return err;
        }
        if (format.eCompressionFormat != OMX_VIDEO_CodingUnused) {
            return OMX_ErrorBadParameter;
        }
        if (!getLegacyFlexibleFormat) {
            break;
        }
        // standard formats that were exposed to users before
        if (format.eColorFormat == OMX_COLOR_FormatYUV420Planar
                || format.eColorFormat == OMX_COLOR_FormatYUV420PackedPlanar
                || format.eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar
                || format.eColorFormat == OMX_COLOR_FormatYUV420PackedSemiPlanar
                || format.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar) {
            break;
        }
        // find best legacy non-standard format
        OMX_U32 flexibleEquivalent;
        if (legacyFormat.eColorFormat == OMX_COLOR_FormatUnused
                && isFlexibleColorFormat(
                        mOMX, mNode, format.eColorFormat, false /* usingNativeBuffers */,
                        &flexibleEquivalent)
                && flexibleEquivalent == OMX_COLOR_FormatYUV420Flexible) {
            memcpy(&legacyFormat, &format, sizeof(format));
        }
    }
    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));
}

static const struct VideoCodingMapEntry {
    const char *mMime;
    OMX_VIDEO_CODINGTYPE mVideoCodingType;
} kVideoCodingMapEntry[] = {
    { MEDIA_MIMETYPE_VIDEO_AVC, OMX_VIDEO_CodingAVC },
    { MEDIA_MIMETYPE_VIDEO_HEVC, OMX_VIDEO_CodingHEVC },
    { MEDIA_MIMETYPE_VIDEO_MPEG4, OMX_VIDEO_CodingMPEG4 },
    { MEDIA_MIMETYPE_VIDEO_MPEG4_DP, OMX_VIDEO_CodingMPEG4 },
    { MEDIA_MIMETYPE_VIDEO_H263, OMX_VIDEO_CodingH263 },
    { MEDIA_MIMETYPE_VIDEO_MPEG2, OMX_VIDEO_CodingMPEG2 },
    { MEDIA_MIMETYPE_VIDEO_VP8, OMX_VIDEO_CodingVP8 },
    { MEDIA_MIMETYPE_VIDEO_VP9, OMX_VIDEO_CodingVP9 },
    { MEDIA_MIMETYPE_VIDEO_DOLBY_VISION, OMX_VIDEO_CodingDolbyVision },
};

status_t ACodec::GetVideoCodingTypeFromMime(
        const char *mime, OMX_VIDEO_CODINGTYPE *codingType) {
    for (size_t i = 0;
         i < sizeof(kVideoCodingMapEntry) / sizeof(kVideoCodingMapEntry[0]);
         ++i) {
        if (!strcasecmp(mime, kVideoCodingMapEntry[i].mMime)) {
            *codingType = kVideoCodingMapEntry[i].mVideoCodingType;
            return OK;
        }
    }

    *codingType = OMX_VIDEO_CodingUnused;

    return ERROR_UNSUPPORTED;
}

static status_t GetMimeTypeForVideoCoding(
        OMX_VIDEO_CODINGTYPE codingType, AString *mime) {
    for (size_t i = 0;
         i < sizeof(kVideoCodingMapEntry) / sizeof(kVideoCodingMapEntry[0]);
         ++i) {
        if (codingType == kVideoCodingMapEntry[i].mVideoCodingType) {
            *mime = kVideoCodingMapEntry[i].mMime;
            return OK;
        }
    }

    mime->clear();

    return ERROR_UNSUPPORTED;
}

status_t ACodec::setPortBufferNum(OMX_U32 portIndex, int bufferNum) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;
    status_t err;
    ALOGD("Setting [%s] %s port buffer number: %d", mComponentName.c_str(),
            portIndex == kPortIndexInput ? "input" : "output", bufferNum);
    err = mOMX->getParameter(
        mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        return err;
    }
    def.nBufferCountActual = bufferNum;
    err = mOMX->setParameter(
        mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        // Component could reject this request.
        ALOGW("Fail to set [%s] %s port buffer number: %d", mComponentName.c_str(),
            portIndex == kPortIndexInput ? "input" : "output", bufferNum);
    }
    return OK;
}

status_t ACodec::setupVideoDecoder(
        const char *mime, const sp<AMessage> &msg, bool haveNativeWindow,
        bool usingSwRenderer, sp<AMessage> &outputFormat) {
    int32_t width, height;
    if (!msg->findInt32("width", &width)
            || !msg->findInt32("height", &height)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CODINGTYPE compressionFormat;
    status_t err = GetVideoCodingTypeFromMime(mime, &compressionFormat);

    err = FFMPEGSoftCodec::setVideoFormat(err,
                    msg, mime, mOMX, mNode, mIsEncoder, &compressionFormat,
                    mComponentName.c_str());
    if (err != OK) {
        return err;
    }

    if (compressionFormat == OMX_VIDEO_CodingVP9) {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE params;
        InitOMXParams(&params);
        params.nPortIndex = kPortIndexInput;
        // Check if VP9 decoder advertises supported profiles.
        params.nProfileIndex = 0;
        status_t err = mOMX->getParameter(
                mNode,
                OMX_IndexParamVideoProfileLevelQuerySupported,
                &params,
                sizeof(params));
        mIsLegacyVP9Decoder = err != OK;
    }

    err = setVideoPortFormatType(
            kPortIndexInput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        return err;
    }

    int32_t tmp;
    if (msg->findInt32("color-format", &tmp)) {
        OMX_COLOR_FORMATTYPE colorFormat =
            static_cast<OMX_COLOR_FORMATTYPE>(tmp);
        err = setVideoPortFormatType(
                kPortIndexOutput, OMX_VIDEO_CodingUnused, colorFormat, haveNativeWindow);
        if (err != OK) {
            ALOGW("[%s] does not support color format %d",
                  mComponentName.c_str(), colorFormat);
            err = setSupportedOutputFormat(!haveNativeWindow /* getLegacyFlexibleFormat */);
        }
    } else {
        err = setSupportedOutputFormat(!haveNativeWindow /* getLegacyFlexibleFormat */);
    }

    if (err != OK) {
        return err;
    }

    // Set the component input buffer number to be |tmp|. If succeed,
    // component will set input port buffer number to be |tmp|. If fail,
    // component will keep the same buffer number as before.
    if (msg->findInt32("android._num-input-buffers", &tmp)) {
        err = setPortBufferNum(kPortIndexInput, tmp);
        if (err != OK)
            return err;
    }

    // Set the component output buffer number to be |tmp|. If succeed,
    // component will set output port buffer number to be |tmp|. If fail,
    // component will keep the same buffer number as before.
    if (msg->findInt32("android._num-output-buffers", &tmp)) {
        err = setPortBufferNum(kPortIndexOutput, tmp);
        if (err != OK)
            return err;
    }

    int32_t frameRateInt;
    float frameRateFloat;
    if (!msg->findFloat("frame-rate", &frameRateFloat)) {
        if (!msg->findInt32("frame-rate", &frameRateInt)) {
            frameRateInt = -1;
        }
        frameRateFloat = (float)frameRateInt;
    }

    err = setVideoFormatOnPort(
            kPortIndexInput, width, height, compressionFormat, frameRateFloat);

    if (err != OK) {
        return err;
    }

    err = setVideoFormatOnPort(
            kPortIndexOutput, width, height, OMX_VIDEO_CodingUnused);

    if (err != OK) {
        return err;
    }

    err = setColorAspectsForVideoDecoder(
            width, height, haveNativeWindow | usingSwRenderer, msg, outputFormat);
    if (err == ERROR_UNSUPPORTED) { // support is optional
        err = OK;
    }

    if (err != OK) {
        return err;
    }

    err = setHDRStaticInfoForVideoCodec(kPortIndexOutput, msg, outputFormat);
    if (err == ERROR_UNSUPPORTED) { // support is optional
        err = OK;
    }
    return err;
}

status_t ACodec::initDescribeColorAspectsIndex() {
    status_t err = mOMX->getExtensionIndex(
            mNode, "OMX.google.android.index.describeColorAspects", &mDescribeColorAspectsIndex);
    if (err != OK) {
        mDescribeColorAspectsIndex = (OMX_INDEXTYPE)0;
    }
    return err;
}

status_t ACodec::setCodecColorAspects(DescribeColorAspectsParams &params, bool verify) {
    status_t err = ERROR_UNSUPPORTED;
    if (mDescribeColorAspectsIndex) {
        err = mOMX->setConfig(mNode, mDescribeColorAspectsIndex, &params, sizeof(params));
    }
    ALOGV("[%s] setting color aspects (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s)) err=%d(%s)",
            mComponentName.c_str(),
            params.sAspects.mRange, asString(params.sAspects.mRange),
            params.sAspects.mPrimaries, asString(params.sAspects.mPrimaries),
            params.sAspects.mMatrixCoeffs, asString(params.sAspects.mMatrixCoeffs),
            params.sAspects.mTransfer, asString(params.sAspects.mTransfer),
            err, asString(err));

    if (verify && err == OK) {
        err = getCodecColorAspects(params);
    }

    ALOGW_IF(err == ERROR_UNSUPPORTED && mDescribeColorAspectsIndex,
            "[%s] setting color aspects failed even though codec advertises support",
            mComponentName.c_str());
    return err;
}

status_t ACodec::setColorAspectsForVideoDecoder(
        int32_t width, int32_t height, bool usingNativeWindow,
        const sp<AMessage> &configFormat, sp<AMessage> &outputFormat) {
    DescribeColorAspectsParams params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    getColorAspectsFromFormat(configFormat, params.sAspects);
    if (usingNativeWindow) {
        setDefaultCodecColorAspectsIfNeeded(params.sAspects, width, height);
        // The default aspects will be set back to the output format during the
        // getFormat phase of configure(). Set non-Unspecified values back into the
        // format, in case component does not support this enumeration.
        setColorAspectsIntoFormat(params.sAspects, outputFormat);
    }

    (void)initDescribeColorAspectsIndex();

    // communicate color aspects to codec
    return setCodecColorAspects(params);
}

status_t ACodec::getCodecColorAspects(DescribeColorAspectsParams &params) {
    status_t err = ERROR_UNSUPPORTED;
    if (mDescribeColorAspectsIndex) {
        err = mOMX->getConfig(mNode, mDescribeColorAspectsIndex, &params, sizeof(params));
    }
    ALOGV("[%s] got color aspects (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s)) err=%d(%s)",
            mComponentName.c_str(),
            params.sAspects.mRange, asString(params.sAspects.mRange),
            params.sAspects.mPrimaries, asString(params.sAspects.mPrimaries),
            params.sAspects.mMatrixCoeffs, asString(params.sAspects.mMatrixCoeffs),
            params.sAspects.mTransfer, asString(params.sAspects.mTransfer),
            err, asString(err));
    if (params.bRequestingDataSpace) {
        ALOGV("for dataspace %#x", params.nDataSpace);
    }
    if (err == ERROR_UNSUPPORTED && mDescribeColorAspectsIndex
            && !params.bRequestingDataSpace && !params.bDataSpaceChanged) {
        ALOGW("[%s] getting color aspects failed even though codec advertises support",
                mComponentName.c_str());
    }
    return err;
}

status_t ACodec::getInputColorAspectsForVideoEncoder(sp<AMessage> &format) {
    DescribeColorAspectsParams params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexInput;
    status_t err = getCodecColorAspects(params);
    if (err == OK) {
        // we only set encoder input aspects if codec supports them
        setColorAspectsIntoFormat(params.sAspects, format, true /* force */);
    }
    return err;
}

status_t ACodec::getDataSpace(
        DescribeColorAspectsParams &params, android_dataspace *dataSpace /* nonnull */,
        bool tryCodec) {
    status_t err = OK;
    if (tryCodec) {
        // request dataspace guidance from codec.
        params.bRequestingDataSpace = OMX_TRUE;
        err = getCodecColorAspects(params);
        params.bRequestingDataSpace = OMX_FALSE;
        if (err == OK && params.nDataSpace != HAL_DATASPACE_UNKNOWN) {
            *dataSpace = (android_dataspace)params.nDataSpace;
            return err;
        } else if (err == ERROR_UNSUPPORTED) {
            // ignore not-implemented error for dataspace requests
            err = OK;
        }
    }

    // this returns legacy versions if available
    *dataSpace = getDataSpaceForColorAspects(params.sAspects, true /* mayexpand */);
    ALOGV("[%s] using color aspects (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s)) "
          "and dataspace %#x",
            mComponentName.c_str(),
            params.sAspects.mRange, asString(params.sAspects.mRange),
            params.sAspects.mPrimaries, asString(params.sAspects.mPrimaries),
            params.sAspects.mMatrixCoeffs, asString(params.sAspects.mMatrixCoeffs),
            params.sAspects.mTransfer, asString(params.sAspects.mTransfer),
            *dataSpace);
    return err;
}


status_t ACodec::getColorAspectsAndDataSpaceForVideoDecoder(
        int32_t width, int32_t height, const sp<AMessage> &configFormat, sp<AMessage> &outputFormat,
        android_dataspace *dataSpace) {
    DescribeColorAspectsParams params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    // reset default format and get resulting format
    getColorAspectsFromFormat(configFormat, params.sAspects);
    if (dataSpace != NULL) {
        setDefaultCodecColorAspectsIfNeeded(params.sAspects, width, height);
    }
    status_t err = setCodecColorAspects(params, true /* readBack */);

    // we always set specified aspects for decoders
    setColorAspectsIntoFormat(params.sAspects, outputFormat);

    if (dataSpace != NULL) {
        status_t res = getDataSpace(params, dataSpace, err == OK /* tryCodec */);
        if (err == OK) {
            err = res;
        }
    }

    return err;
}

// initial video encoder setup for bytebuffer mode
status_t ACodec::setColorAspectsForVideoEncoder(
        const sp<AMessage> &configFormat, sp<AMessage> &outputFormat, sp<AMessage> &inputFormat) {
    // copy config to output format as this is not exposed via getFormat
    copyColorConfig(configFormat, outputFormat);

    DescribeColorAspectsParams params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexInput;
    getColorAspectsFromFormat(configFormat, params.sAspects);

    (void)initDescribeColorAspectsIndex();

    int32_t usingRecorder;
    if (configFormat->findInt32("android._using-recorder", &usingRecorder) && usingRecorder) {
        android_dataspace dataSpace = HAL_DATASPACE_BT709;
        int32_t width, height;
        if (configFormat->findInt32("width", &width)
                && configFormat->findInt32("height", &height)) {
            setDefaultCodecColorAspectsIfNeeded(params.sAspects, width, height);
            status_t err = getDataSpace(
                    params, &dataSpace, mDescribeColorAspectsIndex /* tryCodec */);
            if (err != OK) {
                return err;
            }
            setColorAspectsIntoFormat(params.sAspects, outputFormat);
        }
        inputFormat->setInt32("android._dataspace", (int32_t)dataSpace);
    }

    // communicate color aspects to codec, but do not allow change of the platform aspects
    ColorAspects origAspects = params.sAspects;
    for (int triesLeft = 2; --triesLeft >= 0; ) {
        status_t err = setCodecColorAspects(params, true /* readBack */);
        if (err != OK
                || !ColorUtils::checkIfAspectsChangedAndUnspecifyThem(
                        params.sAspects, origAspects, true /* usePlatformAspects */)) {
            return err;
        }
        ALOGW_IF(triesLeft == 0, "[%s] Codec repeatedly changed requested ColorAspects.",
                mComponentName.c_str());
    }
    return OK;
}

status_t ACodec::setHDRStaticInfoForVideoCodec(
        OMX_U32 portIndex, const sp<AMessage> &configFormat, sp<AMessage> &outputFormat) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    DescribeHDRStaticInfoParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;

    HDRStaticInfo *info = &params.sInfo;
    if (getHDRStaticInfoFromFormat(configFormat, info)) {
        setHDRStaticInfoIntoFormat(params.sInfo, outputFormat);
    }

    (void)initDescribeHDRStaticInfoIndex();

    // communicate HDR static Info to codec
    return setHDRStaticInfo(params);
}

// subsequent initial video encoder setup for surface mode
status_t ACodec::setInitialColorAspectsForVideoEncoderSurfaceAndGetDataSpace(
        android_dataspace *dataSpace /* nonnull */) {
    DescribeColorAspectsParams params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexInput;
    ColorAspects &aspects = params.sAspects;

    // reset default format and store resulting format into both input and output formats
    getColorAspectsFromFormat(mConfigFormat, aspects);
    int32_t width, height;
    if (mInputFormat->findInt32("width", &width) && mInputFormat->findInt32("height", &height)) {
        setDefaultCodecColorAspectsIfNeeded(aspects, width, height);
    }
    setColorAspectsIntoFormat(aspects, mInputFormat);
    setColorAspectsIntoFormat(aspects, mOutputFormat);

    // communicate color aspects to codec, but do not allow any change
    ColorAspects origAspects = aspects;
    status_t err = OK;
    for (int triesLeft = 2; mDescribeColorAspectsIndex && --triesLeft >= 0; ) {
        status_t err = setCodecColorAspects(params, true /* readBack */);
        if (err != OK || !ColorUtils::checkIfAspectsChangedAndUnspecifyThem(aspects, origAspects)) {
            break;
        }
        ALOGW_IF(triesLeft == 0, "[%s] Codec repeatedly changed requested ColorAspects.",
                mComponentName.c_str());
    }

    *dataSpace = HAL_DATASPACE_BT709;
    aspects = origAspects; // restore desired color aspects
    status_t res = getDataSpace(
            params, dataSpace, err == OK && mDescribeColorAspectsIndex /* tryCodec */);
    if (err == OK) {
        err = res;
    }
    mInputFormat->setInt32("android._dataspace", (int32_t)*dataSpace);
    mInputFormat->setBuffer(
            "android._color-aspects", ABuffer::CreateAsCopy(&aspects, sizeof(aspects)));

    // update input format with codec supported color aspects (basically set unsupported
    // aspects to Unspecified)
    if (err == OK) {
        (void)getInputColorAspectsForVideoEncoder(mInputFormat);
    }

    ALOGV("set default color aspects, updated input format to %s, output format to %s",
            mInputFormat->debugString(4).c_str(), mOutputFormat->debugString(4).c_str());

    return err;
}

status_t ACodec::getHDRStaticInfoForVideoCodec(OMX_U32 portIndex, sp<AMessage> &format) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);
    DescribeHDRStaticInfoParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;

    status_t err = getHDRStaticInfo(params);
    if (err == OK) {
        // we only set decodec output HDRStaticInfo if codec supports them
        setHDRStaticInfoIntoFormat(params.sInfo, format);
    }
    return err;
}

status_t ACodec::initDescribeHDRStaticInfoIndex() {
    status_t err = mOMX->getExtensionIndex(
            mNode, "OMX.google.android.index.describeHDRStaticInfo", &mDescribeHDRStaticInfoIndex);
    if (err != OK) {
        mDescribeHDRStaticInfoIndex = (OMX_INDEXTYPE)0;
    }
    return err;
}

status_t ACodec::setHDRStaticInfo(const DescribeHDRStaticInfoParams &params) {
    status_t err = ERROR_UNSUPPORTED;
    if (mDescribeHDRStaticInfoIndex) {
        err = mOMX->setConfig(mNode, mDescribeHDRStaticInfoIndex, &params, sizeof(params));
    }

    const HDRStaticInfo *info = &params.sInfo;
    ALOGV("[%s] setting  HDRStaticInfo (R: %u %u, G: %u %u, B: %u, %u, W: %u, %u, "
            "MaxDispL: %u, MinDispL: %u, MaxContentL: %u, MaxFrameAvgL: %u)",
            mComponentName.c_str(),
            info->sType1.mR.x, info->sType1.mR.y, info->sType1.mG.x, info->sType1.mG.y,
            info->sType1.mB.x, info->sType1.mB.y, info->sType1.mW.x, info->sType1.mW.y,
            info->sType1.mMaxDisplayLuminance, info->sType1.mMinDisplayLuminance,
            info->sType1.mMaxContentLightLevel, info->sType1.mMaxFrameAverageLightLevel);

    ALOGW_IF(err == ERROR_UNSUPPORTED && mDescribeHDRStaticInfoIndex,
            "[%s] setting HDRStaticInfo failed even though codec advertises support",
            mComponentName.c_str());
    return err;
}

status_t ACodec::getHDRStaticInfo(DescribeHDRStaticInfoParams &params) {
    status_t err = ERROR_UNSUPPORTED;
    if (mDescribeHDRStaticInfoIndex) {
        err = mOMX->getConfig(mNode, mDescribeHDRStaticInfoIndex, &params, sizeof(params));
    }

    ALOGW_IF(err == ERROR_UNSUPPORTED && mDescribeHDRStaticInfoIndex,
            "[%s] getting HDRStaticInfo failed even though codec advertises support",
            mComponentName.c_str());
    return err;
}

status_t ACodec::setupVideoEncoder(
        const char *mime, const sp<AMessage> &msg,
        sp<AMessage> &outputFormat, sp<AMessage> &inputFormat) {
    int32_t tmp;
    if (!msg->findInt32("color-format", &tmp)) {
        return INVALID_OPERATION;
    }

    OMX_COLOR_FORMATTYPE colorFormat =
        static_cast<OMX_COLOR_FORMATTYPE>(tmp);

    status_t err = setVideoPortFormatType(
            kPortIndexInput, OMX_VIDEO_CodingUnused, colorFormat);

    if (err != OK) {
        ALOGE("[%s] does not support color format %d",
              mComponentName.c_str(), colorFormat);

        return err;
    }

    /* Input port configuration */

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    int32_t width, height, bitrate;
    if (!msg->findInt32("width", &width)
            || !msg->findInt32("height", &height)
            || !msg->findInt32("bitrate", &bitrate)) {
        return INVALID_OPERATION;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    int32_t stride;
    if (!msg->findInt32("stride", &stride)) {
        stride = width;
    }

    video_def->nStride = stride;

    int32_t sliceHeight;
    if (!msg->findInt32("slice-height", &sliceHeight)) {
        sliceHeight = height;
    }

    video_def->nSliceHeight = sliceHeight;

    def.nBufferSize = (video_def->nStride * video_def->nSliceHeight * 3) / 2;

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
        mTimePerFrameUs = (int64_t) (1000000.0f / frameRate);
    }

    video_def->xFramerate = (OMX_U32)(frameRate * 65536.0f);
    video_def->eCompressionFormat = OMX_VIDEO_CodingUnused;
    // this is redundant as it was already set up in setVideoPortFormatType
    // FIXME for now skip this only for flexible YUV formats
    if (colorFormat != OMX_COLOR_FormatYUV420Flexible) {
        video_def->eColorFormat = colorFormat;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        ALOGE("[%s] failed to set input port definition parameters.",
              mComponentName.c_str());

        return err;
    }

    /* Output port configuration */

    OMX_VIDEO_CODINGTYPE compressionFormat;
    err = GetVideoCodingTypeFromMime(mime, &compressionFormat);

    if (err != OK) {
        return err;
    }

    err = setVideoPortFormatType(
            kPortIndexOutput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        ALOGE("[%s] does not support compression format %d",
             mComponentName.c_str(), compressionFormat);

        return err;
    }

    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->xFramerate = 0;
    video_def->nBitrate = bitrate;
    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        ALOGE("[%s] failed to set output port definition parameters.",
              mComponentName.c_str());

        return err;
    }

    int32_t intraRefreshPeriod = 0;
    if (msg->findInt32("intra-refresh-period", &intraRefreshPeriod)
            && intraRefreshPeriod >= 0) {
        err = setIntraRefreshPeriod((uint32_t)intraRefreshPeriod, true);
        if (err != OK) {
            ALOGI("[%s] failed setIntraRefreshPeriod. Failure is fine since this key is optional",
                    mComponentName.c_str());
            err = OK;
        }
    }

    switch (compressionFormat) {
        case OMX_VIDEO_CodingMPEG4:
            err = setupMPEG4EncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingH263:
            err = setupH263EncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingAVC:
            err = setupAVCEncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingHEVC:
            err = setupHEVCEncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingVP8:
        case OMX_VIDEO_CodingVP9:
            err = setupVPXEncoderParameters(msg, outputFormat);
            break;

        default:
            break;
    }

    if (err != OK) {
        return err;
    }

    // Set up color aspects on input, but propagate them to the output format, as they will
    // not be read back from encoder.
    err = setColorAspectsForVideoEncoder(msg, outputFormat, inputFormat);
    if (err == ERROR_UNSUPPORTED) {
        ALOGI("[%s] cannot encode color aspects. Ignoring.", mComponentName.c_str());
        err = OK;
    }

    if (err != OK) {
        return err;
    }

    err = setHDRStaticInfoForVideoCodec(kPortIndexInput, msg, outputFormat);
    if (err == ERROR_UNSUPPORTED) { // support is optional
        ALOGI("[%s] cannot encode HDR static metadata. Ignoring.", mComponentName.c_str());
        err = OK;
    }

    if (err != OK) {
        return err;
    }

    switch (compressionFormat) {
        case OMX_VIDEO_CodingAVC:
        case OMX_VIDEO_CodingHEVC:
            err = configureTemporalLayers(msg, true /* inConfigure */, outputFormat);
            if (err != OK) {
                err = OK; // ignore failure
            }
            break;

        case OMX_VIDEO_CodingVP8:
        case OMX_VIDEO_CodingVP9:
            // TODO: do we need to support android.generic layering? webrtc layering is
            // already set up in setupVPXEncoderParameters.
            break;

        default:
            break;
    }

    if (err == OK) {
        ALOGI("setupVideoEncoder succeeded");
    }

    return err;
}

status_t ACodec::setCyclicIntraMacroblockRefresh(const sp<AMessage> &msg, int32_t mode) {
    OMX_VIDEO_PARAM_INTRAREFRESHTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    params.eRefreshMode = static_cast<OMX_VIDEO_INTRAREFRESHTYPE>(mode);

    if (params.eRefreshMode == OMX_VIDEO_IntraRefreshCyclic ||
            params.eRefreshMode == OMX_VIDEO_IntraRefreshBoth) {
        int32_t mbs;
        if (!msg->findInt32("intra-refresh-CIR-mbs", &mbs)) {
            return INVALID_OPERATION;
        }
        params.nCirMBs = mbs;
    }

    if (params.eRefreshMode == OMX_VIDEO_IntraRefreshAdaptive ||
            params.eRefreshMode == OMX_VIDEO_IntraRefreshBoth) {
        int32_t mbs;
        if (!msg->findInt32("intra-refresh-AIR-mbs", &mbs)) {
            return INVALID_OPERATION;
        }
        params.nAirMBs = mbs;

        int32_t ref;
        if (!msg->findInt32("intra-refresh-AIR-ref", &ref)) {
            return INVALID_OPERATION;
        }
        params.nAirRef = ref;
    }

    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoIntraRefresh,
            &params, sizeof(params));
    return err;
}

static OMX_U32 setPFramesSpacing(
        float iFramesInterval /* seconds */, int32_t frameRate, uint32_t BFramesSpacing = 0) {
    // BFramesSpacing is the number of B frames between I/P frames
    // PFramesSpacing (the value to be returned) is the number of P frames between I frames
    //
    // keyFrameInterval = ((PFramesSpacing + 1) * BFramesSpacing) + PFramesSpacing + 1
    //                                     ^^^                            ^^^        ^^^
    //                              number of B frames                number of P    I frame
    //
    //                  = (PFramesSpacing + 1) * (BFramesSpacing + 1)
    //
    // E.g.
    //      I   P   I  : I-interval: 8, nPFrames 1, nBFrames 3
    //       BBB BBB

    if (iFramesInterval < 0) { // just 1 key frame
        return 0xFFFFFFFE; // don't use maxint as key-frame-interval calculation will add 1
    } else if (iFramesInterval == 0) { // just key frames
        return 0;
    }

    // round down as key-frame-interval is an upper limit
    uint32_t keyFrameInterval = uint32_t(frameRate * iFramesInterval);
    OMX_U32 ret = keyFrameInterval / (BFramesSpacing + 1);
    return ret > 0 ? ret - 1 : 0;
}

static OMX_VIDEO_CONTROLRATETYPE getBitrateMode(const sp<AMessage> &msg) {
    int32_t tmp;
    if (!msg->findInt32("bitrate-mode", &tmp)) {
        return OMX_Video_ControlRateVariable;
    }

    return static_cast<OMX_VIDEO_CONTROLRATETYPE>(tmp);
}

status_t ACodec::setupMPEG4EncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate;
    float iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findAsFloat("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    OMX_VIDEO_PARAM_MPEG4TYPE mpeg4type;
    InitOMXParams(&mpeg4type);
    mpeg4type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));

    if (err != OK) {
        return err;
    }

    mpeg4type.nSliceHeaderSpacing = 0;
    mpeg4type.bSVH = OMX_FALSE;
    mpeg4type.bGov = OMX_FALSE;

    mpeg4type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    mpeg4type.nBFrames = 0;
    mpeg4type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate, mpeg4type.nBFrames);
    if (mpeg4type.nPFrames == 0) {
        mpeg4type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    mpeg4type.nIDCVLCThreshold = 0;
    mpeg4type.bACPred = OMX_TRUE;
    mpeg4type.nMaxPacketSize = 256;
    mpeg4type.nTimeIncRes = 1000;
    mpeg4type.nHeaderExtension = 0;
    mpeg4type.bReversibleVLC = OMX_FALSE;

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            return err;
        }

        mpeg4type.eProfile = static_cast<OMX_VIDEO_MPEG4PROFILETYPE>(profile);
        mpeg4type.eLevel = static_cast<OMX_VIDEO_MPEG4LEVELTYPE>(level);
    }

    setBFrames(&mpeg4type);
    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));

    if (err != OK) {
        return err;
    }

    err = configureBitrate(bitrate, bitrateMode);

    if (err != OK) {
        return err;
    }

    return setupErrorCorrectionParameters();
}

status_t ACodec::setupH263EncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate;
    float iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findAsFloat("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    OMX_VIDEO_PARAM_H263TYPE h263type;
    InitOMXParams(&h263type);
    h263type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));

    if (err != OK) {
        return err;
    }

    h263type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    h263type.nBFrames = 0;
    h263type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate, h263type.nBFrames);
    if (h263type.nPFrames == 0) {
        h263type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            return err;
        }

        h263type.eProfile = static_cast<OMX_VIDEO_H263PROFILETYPE>(profile);
        h263type.eLevel = static_cast<OMX_VIDEO_H263LEVELTYPE>(level);
    }

    h263type.bPLUSPTYPEAllowed = OMX_FALSE;
    h263type.bForceRoundingTypeToZero = OMX_FALSE;
    h263type.nPictureHeaderRepetition = 0;
    h263type.nGOBHeaderInterval = 0;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));

    if (err != OK) {
        return err;
    }

    err = configureBitrate(bitrate, bitrateMode);

    if (err != OK) {
        return err;
    }

    return setupErrorCorrectionParameters();
}

// static
int /* OMX_VIDEO_AVCLEVELTYPE */ ACodec::getAVCLevelFor(
        int width, int height, int rate, int bitrate,
        OMX_VIDEO_AVCPROFILETYPE profile) {
    // convert bitrate to main/baseline profile kbps equivalent
    switch (profile) {
        case OMX_VIDEO_AVCProfileHigh10:
            bitrate = divUp(bitrate, 3000); break;
        case OMX_VIDEO_AVCProfileHigh:
            bitrate = divUp(bitrate, 1250); break;
        default:
            bitrate = divUp(bitrate, 1000); break;
    }

    // convert size and rate to MBs
    width = divUp(width, 16);
    height = divUp(height, 16);
    int mbs = width * height;
    rate *= mbs;
    int maxDimension = max(width, height);

    static const int limits[][5] = {
        /*   MBps     MB   dim  bitrate        level */
        {    1485,    99,  28,     64, OMX_VIDEO_AVCLevel1  },
        {    1485,    99,  28,    128, OMX_VIDEO_AVCLevel1b },
        {    3000,   396,  56,    192, OMX_VIDEO_AVCLevel11 },
        {    6000,   396,  56,    384, OMX_VIDEO_AVCLevel12 },
        {   11880,   396,  56,    768, OMX_VIDEO_AVCLevel13 },
        {   11880,   396,  56,   2000, OMX_VIDEO_AVCLevel2  },
        {   19800,   792,  79,   4000, OMX_VIDEO_AVCLevel21 },
        {   20250,  1620, 113,   4000, OMX_VIDEO_AVCLevel22 },
        {   40500,  1620, 113,  10000, OMX_VIDEO_AVCLevel3  },
        {  108000,  3600, 169,  14000, OMX_VIDEO_AVCLevel31 },
        {  216000,  5120, 202,  20000, OMX_VIDEO_AVCLevel32 },
        {  245760,  8192, 256,  20000, OMX_VIDEO_AVCLevel4  },
        {  245760,  8192, 256,  50000, OMX_VIDEO_AVCLevel41 },
        {  522240,  8704, 263,  50000, OMX_VIDEO_AVCLevel42 },
        {  589824, 22080, 420, 135000, OMX_VIDEO_AVCLevel5  },
        {  983040, 36864, 543, 240000, OMX_VIDEO_AVCLevel51 },
        { 2073600, 36864, 543, 240000, OMX_VIDEO_AVCLevel52 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
        const int (&limit)[5] = limits[i];
        if (rate <= limit[0] && mbs <= limit[1] && maxDimension <= limit[2]
                && bitrate <= limit[3]) {
            return limit[4];
        }
    }
    return 0;
}

status_t ACodec::setupAVCEncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate;
    float iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findAsFloat("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    status_t err = OK;
    int32_t intraRefreshMode = 0;
    if (msg->findInt32("intra-refresh-mode", &intraRefreshMode)) {
        err = setCyclicIntraMacroblockRefresh(msg, intraRefreshMode);
        if (err != OK) {
            ALOGE("Setting intra macroblock refresh mode (%d) failed: 0x%x",
                    err, intraRefreshMode);
            return err;
        }
    }

    OMX_VIDEO_PARAM_AVCTYPE h264type;
    InitOMXParams(&h264type);
    h264type.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));

    if (err != OK) {
        return err;
    }

    h264type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            ALOGE("%s does not support profile %x @ level %x",
                    mComponentName.c_str(), profile, level);
            return err;
        }

        h264type.eProfile = static_cast<OMX_VIDEO_AVCPROFILETYPE>(profile);
        h264type.eLevel = static_cast<OMX_VIDEO_AVCLEVELTYPE>(level);
    } 
#if 0
      else {
        h264type.eProfile = OMX_VIDEO_AVCProfileBaseline;
#if 0   /* DON'T YET DEFAULT TO HIGHEST PROFILE */
        // Use largest supported profile for AVC recording if profile is not specified.
        for (OMX_VIDEO_AVCPROFILETYPE profile : {
                OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCProfileMain }) {
            if (verifySupportForProfileAndLevel(profile, 0) == OK) {
                h264type.eProfile = profile;
                break;
            }
        }
#endif
    }
#endif

    ALOGI("setupAVCEncoderParameters with [profile: %s] [level: %s]",
            asString(h264type.eProfile), asString(h264type.eLevel));

    if (h264type.eProfile == OMX_VIDEO_AVCProfileBaseline) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 1;
        h264type.nBFrames = 0;
        h264type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate, h264type.nBFrames);
        if (h264type.nPFrames == 0) {
            h264type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
        }
        h264type.nRefIdx10ActiveMinus1 = 0;
        h264type.nRefIdx11ActiveMinus1 = 0;
        h264type.bEntropyCodingCABAC = OMX_FALSE;
        h264type.bWeightedPPrediction = OMX_FALSE;
        h264type.bconstIpred = OMX_FALSE;
        h264type.bDirect8x8Inference = OMX_FALSE;
        h264type.bDirectSpatialTemporal = OMX_FALSE;
        h264type.nCabacInitIdc = 0;
    } else if (h264type.eProfile == OMX_VIDEO_AVCProfileMain ||
            h264type.eProfile == OMX_VIDEO_AVCProfileHigh) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 2;
        h264type.nBFrames = 1;
        h264type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate, h264type.nBFrames);
        h264type.nAllowedPictureTypes =
            OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP | OMX_VIDEO_PictureTypeB;
        h264type.nRefIdx10ActiveMinus1 = 0;
        h264type.nRefIdx11ActiveMinus1 = 0;
        h264type.bEntropyCodingCABAC = OMX_TRUE;
        h264type.bWeightedPPrediction = OMX_TRUE;
        h264type.bconstIpred = OMX_TRUE;
        h264type.bDirect8x8Inference = OMX_TRUE;
        h264type.bDirectSpatialTemporal = OMX_TRUE;
        h264type.nCabacInitIdc = 1;
    }

    setBFrames(&h264type, iFrameInterval, frameRate);
    if (h264type.nBFrames != 0) {
        h264type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
    }

    h264type.bEnableUEP = OMX_FALSE;
    h264type.bEnableFMO = OMX_FALSE;
    h264type.bEnableASO = OMX_FALSE;
    h264type.bEnableRS = OMX_FALSE;
    h264type.bFrameMBsOnly = OMX_TRUE;
    h264type.bMBAFF = OMX_FALSE;
    h264type.eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));

    if (err != OK) {
        return err;
    }

    // TRICKY: if we are enabling temporal layering as well, some codecs may not support layering
    // when B-frames are enabled. Detect this now so we can disable B frames if temporal layering
    // is preferred.
    AString tsSchema;
    int32_t preferBFrames = (int32_t)false;
    if (msg->findString("ts-schema", &tsSchema)
            && (!msg->findInt32("android._prefer-b-frames", &preferBFrames) || !preferBFrames)) {
        OMX_VIDEO_PARAM_ANDROID_TEMPORALLAYERINGTYPE layering;
        InitOMXParams(&layering);
        layering.nPortIndex = kPortIndexOutput;
        if (mOMX->getParameter(
                        mNode, (OMX_INDEXTYPE)OMX_IndexParamAndroidVideoTemporalLayering,
                        &layering, sizeof(layering)) == OK
                && layering.eSupportedPatterns
                && layering.nBLayerCountMax == 0) {
            h264type.nBFrames = 0;
            h264type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate, h264type.nBFrames);
            h264type.nAllowedPictureTypes &= ~OMX_VIDEO_PictureTypeB;
            ALOGI("disabling B-frames");
            err = mOMX->setParameter(
                    mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));

            if (err != OK) {
                return err;
            }
        }
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::setupHEVCEncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate;
    float iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findAsFloat("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    AVUtils::get()->setIntraPeriod(setPFramesSpacing(iFrameInterval, frameRate), 0, mOMX, mNode);

    OMX_VIDEO_PARAM_HEVCTYPE hevcType;
    InitOMXParams(&hevcType);
    hevcType.nPortIndex = kPortIndexOutput;

    status_t err = OK;
    err = mOMX->getParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoHevc, &hevcType, sizeof(hevcType));
    if (err != OK) {
        return err;
    }

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);
        if (err != OK) {
            return err;
        }

        hevcType.eProfile = static_cast<OMX_VIDEO_HEVCPROFILETYPE>(profile);
        hevcType.eLevel = static_cast<OMX_VIDEO_HEVCLEVELTYPE>(level);
    }
    // TODO: finer control?
    hevcType.nKeyFrameInterval = setPFramesSpacing(iFrameInterval, frameRate) + 1;

    err = mOMX->setParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoHevc, &hevcType, sizeof(hevcType));
    if (err != OK) {
        return err;
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::setupVPXEncoderParameters(const sp<AMessage> &msg, sp<AMessage> &outputFormat) {
    int32_t bitrate;
    float iFrameInterval = 0;
    size_t tsLayers = 0;
    OMX_VIDEO_ANDROID_VPXTEMPORALLAYERPATTERNTYPE pattern =
        OMX_VIDEO_VPXTemporalLayerPatternNone;
    static const uint32_t kVp8LayerRateAlloction
        [OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS]
        [OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS] = {
        {100, 100, 100},  // 1 layer
        { 60, 100, 100},  // 2 layers {60%, 40%}
        { 40,  60, 100},  // 3 layers {40%, 20%, 40%}
    };
    if (!msg->findInt32("bitrate", &bitrate)) {
        return INVALID_OPERATION;
    }
    msg->findAsFloat("i-frame-interval", &iFrameInterval);

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    AString tsSchema;
    OMX_VIDEO_ANDROID_TEMPORALLAYERINGPATTERNTYPE tsType =
        OMX_VIDEO_AndroidTemporalLayeringPatternNone;

    if (msg->findString("ts-schema", &tsSchema)) {
        unsigned int numLayers = 0;
        unsigned int numBLayers = 0;
        int tags;
        char dummy;
        if (sscanf(tsSchema.c_str(), "webrtc.vp8.%u-layer%c", &numLayers, &dummy) == 1
                && numLayers > 0) {
            pattern = OMX_VIDEO_VPXTemporalLayerPatternWebRTC;
            tsType = OMX_VIDEO_AndroidTemporalLayeringPatternWebRTC;
            tsLayers = numLayers;
        } else if ((tags = sscanf(tsSchema.c_str(), "android.generic.%u%c%u%c",
                        &numLayers, &dummy, &numBLayers, &dummy))
                && (tags == 1 || (tags == 3 && dummy == '+'))
                && numLayers > 0 && numLayers < UINT32_MAX - numBLayers) {
            pattern = OMX_VIDEO_VPXTemporalLayerPatternWebRTC;
            // VPX does not have a concept of B-frames, so just count all layers
            tsType = OMX_VIDEO_AndroidTemporalLayeringPatternAndroid;
            tsLayers = numLayers + numBLayers;
        } else {
            ALOGW("Ignoring unsupported ts-schema [%s]", tsSchema.c_str());
        }
        tsLayers = min(tsLayers, (size_t)OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS);
    }

    OMX_VIDEO_PARAM_ANDROID_VP8ENCODERTYPE vp8type;
    InitOMXParams(&vp8type);
    vp8type.nPortIndex = kPortIndexOutput;
    status_t err = mOMX->getParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoAndroidVp8Encoder,
            &vp8type, sizeof(vp8type));

    if (err == OK) {
        if (iFrameInterval > 0) {
            vp8type.nKeyFrameInterval = setPFramesSpacing(iFrameInterval, frameRate) + 1;
        }
        vp8type.eTemporalPattern = pattern;
        vp8type.nTemporalLayerCount = tsLayers;
        if (tsLayers > 0) {
            for (size_t i = 0; i < OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS; i++) {
                vp8type.nTemporalLayerBitrateRatio[i] =
                    kVp8LayerRateAlloction[tsLayers - 1][i];
            }
        }
        if (bitrateMode == OMX_Video_ControlRateConstant) {
            vp8type.nMinQuantizer = 2;
            vp8type.nMaxQuantizer = 63;
        }

        err = mOMX->setParameter(
                mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoAndroidVp8Encoder,
                &vp8type, sizeof(vp8type));
        if (err != OK) {
            ALOGW("Extended VP8 parameters set failed: %d", err);
        } else if (tsType == OMX_VIDEO_AndroidTemporalLayeringPatternWebRTC) {
            // advertise even single layer WebRTC layering, as it is defined
            outputFormat->setString("ts-schema", AStringPrintf("webrtc.vp8.%u-layer", tsLayers));
        } else if (tsLayers > 0) {
            // tsType == OMX_VIDEO_AndroidTemporalLayeringPatternAndroid
            outputFormat->setString("ts-schema", AStringPrintf("android.generic.%u", tsLayers));
        }
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::verifySupportForProfileAndLevel(
        int32_t profile, int32_t level) {
    OMX_VIDEO_PARAM_PROFILELEVELTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
        params.nProfileIndex = index;
        status_t err = mOMX->getParameter(
                mNode,
                OMX_IndexParamVideoProfileLevelQuerySupported,
                &params,
                sizeof(params));

        if (err != OK) {
            return err;
        }

        int32_t supportedProfile = static_cast<int32_t>(params.eProfile);
        int32_t supportedLevel = static_cast<int32_t>(params.eLevel);

        if (profile == supportedProfile && level <= supportedLevel) {
            return OK;
        }

        if (index == kMaxIndicesToCheck) {
            ALOGW("[%s] stopping checking profiles after %u: %x/%x",
                    mComponentName.c_str(), index,
                    params.eProfile, params.eLevel);
        }
    }
    return ERROR_UNSUPPORTED;
}

status_t ACodec::configureBitrate(
        int32_t bitrate, OMX_VIDEO_CONTROLRATETYPE bitrateMode) {
    OMX_VIDEO_PARAM_BITRATETYPE bitrateType;
    InitOMXParams(&bitrateType);
    bitrateType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));

    if (err != OK) {
        return err;
    }

    bitrateType.eControlRate = bitrateMode;
    bitrateType.nTargetBitrate = bitrate;

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
}

status_t ACodec::setupErrorCorrectionParameters() {
    OMX_VIDEO_PARAM_ERRORCORRECTIONTYPE errorCorrectionType;
    InitOMXParams(&errorCorrectionType);
    errorCorrectionType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));

    if (err != OK) {
        return OK;  // Optional feature. Ignore this failure
    }

    errorCorrectionType.bEnableHEC = OMX_FALSE;
    errorCorrectionType.bEnableResync = OMX_TRUE;
    errorCorrectionType.nResynchMarkerSpacing = 256;
    errorCorrectionType.bEnableDataPartitioning = OMX_FALSE;
    errorCorrectionType.bEnableRVLC = OMX_FALSE;

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
}

status_t ACodec::setVideoFormatOnPort(
        OMX_U32 portIndex,
        int32_t width, int32_t height, OMX_VIDEO_CODINGTYPE compressionFormat,
        float frameRate) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        return err;
    }

    if (portIndex == kPortIndexInput) {
        // XXX Need a (much) better heuristic to compute input buffer sizes.
        const size_t X = 64 * 1024;
        if (def.nBufferSize < X) {
            def.nBufferSize = X;
        }
    }

    if (def.eDomain != OMX_PortDomainVideo) {
        ALOGE("expected video port, got %s(%d)", asString(def.eDomain), def.eDomain);
        return FAILED_TRANSACTION;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    if (portIndex == kPortIndexInput) {
        video_def->eCompressionFormat = compressionFormat;
        video_def->eColorFormat = OMX_COLOR_FormatUnused;
        if (frameRate >= 0) {
            video_def->xFramerate = (OMX_U32)(frameRate * 65536.0f);
        }
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    return err;
}

status_t ACodec::initNativeWindow() {
    if (mNativeWindow != NULL) {
        return mOMX->enableNativeBuffers(mNode, kPortIndexOutput, OMX_TRUE /* graphic */, OMX_TRUE);
    }

    mOMX->enableNativeBuffers(mNode, kPortIndexOutput, OMX_TRUE /* graphic */, OMX_FALSE);
    return OK;
}

size_t ACodec::countBuffersOwnedByComponent(OMX_U32 portIndex) const {
    size_t n = 0;

    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex].itemAt(i);

        if (info.mStatus == BufferInfo::OWNED_BY_COMPONENT) {
            ++n;
        }
    }

    return n;
}

size_t ACodec::countBuffersOwnedByNativeWindow() const {
    size_t n = 0;

    for (size_t i = 0; i < mBuffers[kPortIndexOutput].size(); ++i) {
        const BufferInfo &info = mBuffers[kPortIndexOutput].itemAt(i);

        if (info.mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
            ++n;
        }
    }

    return n;
}

void ACodec::waitUntilAllPossibleNativeWindowBuffersAreReturnedToUs() {
    if (mNativeWindow == NULL) {
        return;
    }

    while (countBuffersOwnedByNativeWindow() > mNumUndequeuedBuffers
            && dequeueBufferFromNativeWindow() != NULL) {
        // these buffers will be submitted as regular buffers; account for this
        if (storingMetadataInDecodedBuffers() && mMetadataBuffersToSubmit > 0) {
            --mMetadataBuffersToSubmit;
        }
    }
}

bool ACodec::allYourBuffersAreBelongToUs(
        OMX_U32 portIndex) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mStatus != BufferInfo::OWNED_BY_US
                && info->mStatus != BufferInfo::OWNED_BY_NATIVE_WINDOW) {
            ALOGV("[%s] Buffer %u on port %u still has status %d",
                    mComponentName.c_str(),
                    info->mBufferID, portIndex, info->mStatus);
            return false;
        }
    }

    return true;
}

bool ACodec::allYourBuffersAreBelongToUs() {
    return allYourBuffersAreBelongToUs(kPortIndexInput)
        && allYourBuffersAreBelongToUs(kPortIndexOutput);
}

void ACodec::deferMessage(const sp<AMessage> &msg) {
    mDeferredQueue.push_back(msg);
}

void ACodec::processDeferredMessages() {
    List<sp<AMessage> > queue = mDeferredQueue;
    mDeferredQueue.clear();

    List<sp<AMessage> >::iterator it = queue.begin();
    while (it != queue.end()) {
        onMessageReceived(*it++);
    }
}

// static
bool ACodec::describeDefaultColorFormat(DescribeColorFormat2Params &params) {
    MediaImage2 &image = params.sMediaImage;
    memset(&image, 0, sizeof(image));

    image.mType = MediaImage2::MEDIA_IMAGE_TYPE_UNKNOWN;
    image.mNumPlanes = 0;

    const OMX_COLOR_FORMATTYPE fmt = params.eColorFormat;
    image.mWidth = params.nFrameWidth;
    image.mHeight = params.nFrameHeight;

    // only supporting YUV420
    if (fmt != OMX_COLOR_FormatYUV420Planar &&
        fmt != OMX_COLOR_FormatYUV420PackedPlanar &&
        fmt != OMX_COLOR_FormatYUV420SemiPlanar &&
        fmt != OMX_COLOR_FormatYUV420PackedSemiPlanar &&
        fmt != (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YV12) {
        ALOGW("do not know color format 0x%x = %d", fmt, fmt);
        return false;
    }

    // TEMPORARY FIX for some vendors that advertise sliceHeight as 0
    if (params.nStride != 0 && params.nSliceHeight == 0) {
        ALOGW("using sliceHeight=%u instead of what codec advertised (=0)",
                params.nFrameHeight);
        params.nSliceHeight = params.nFrameHeight;
    }

    // we need stride and slice-height to be non-zero and sensible. These values were chosen to
    // prevent integer overflows further down the line, and do not indicate support for
    // 32kx32k video.
    if (params.nStride == 0 || params.nSliceHeight == 0
            || params.nStride > 32768 || params.nSliceHeight > 32768) {
        ALOGW("cannot describe color format 0x%x = %d with stride=%u and sliceHeight=%u",
                fmt, fmt, params.nStride, params.nSliceHeight);
        return false;
    }

    // set-up YUV format
    image.mType = MediaImage2::MEDIA_IMAGE_TYPE_YUV;
    image.mNumPlanes = 3;
    image.mBitDepth = 8;
    image.mBitDepthAllocated = 8;
    image.mPlane[image.Y].mOffset = 0;
    image.mPlane[image.Y].mColInc = 1;
    image.mPlane[image.Y].mRowInc = params.nStride;
    image.mPlane[image.Y].mHorizSubsampling = 1;
    image.mPlane[image.Y].mVertSubsampling = 1;

    switch ((int)fmt) {
        case HAL_PIXEL_FORMAT_YV12:
            if (params.bUsingNativeBuffers) {
                size_t ystride = align(params.nStride, 16);
                size_t cstride = align(params.nStride / 2, 16);
                image.mPlane[image.Y].mRowInc = ystride;

                image.mPlane[image.V].mOffset = ystride * params.nSliceHeight;
                image.mPlane[image.V].mColInc = 1;
                image.mPlane[image.V].mRowInc = cstride;
                image.mPlane[image.V].mHorizSubsampling = 2;
                image.mPlane[image.V].mVertSubsampling = 2;

                image.mPlane[image.U].mOffset = image.mPlane[image.V].mOffset
                        + (cstride * params.nSliceHeight / 2);
                image.mPlane[image.U].mColInc = 1;
                image.mPlane[image.U].mRowInc = cstride;
                image.mPlane[image.U].mHorizSubsampling = 2;
                image.mPlane[image.U].mVertSubsampling = 2;
                break;
            } else {
                // fall through as YV12 is used for YUV420Planar by some codecs
            }

        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420PackedPlanar:
            image.mPlane[image.U].mOffset = params.nStride * params.nSliceHeight;
            image.mPlane[image.U].mColInc = 1;
            image.mPlane[image.U].mRowInc = params.nStride / 2;
            image.mPlane[image.U].mHorizSubsampling = 2;
            image.mPlane[image.U].mVertSubsampling = 2;

            image.mPlane[image.V].mOffset = image.mPlane[image.U].mOffset
                    + (params.nStride * params.nSliceHeight / 4);
            image.mPlane[image.V].mColInc = 1;
            image.mPlane[image.V].mRowInc = params.nStride / 2;
            image.mPlane[image.V].mHorizSubsampling = 2;
            image.mPlane[image.V].mVertSubsampling = 2;
            break;

        case OMX_COLOR_FormatYUV420SemiPlanar:
            // FIXME: NV21 for sw-encoder, NV12 for decoder and hw-encoder
        case OMX_COLOR_FormatYUV420PackedSemiPlanar:
            // NV12
            image.mPlane[image.U].mOffset = params.nStride * params.nSliceHeight;
            image.mPlane[image.U].mColInc = 2;
            image.mPlane[image.U].mRowInc = params.nStride;
            image.mPlane[image.U].mHorizSubsampling = 2;
            image.mPlane[image.U].mVertSubsampling = 2;

            image.mPlane[image.V].mOffset = image.mPlane[image.U].mOffset + 1;
            image.mPlane[image.V].mColInc = 2;
            image.mPlane[image.V].mRowInc = params.nStride;
            image.mPlane[image.V].mHorizSubsampling = 2;
            image.mPlane[image.V].mVertSubsampling = 2;
            break;

        default:
            TRESPASS();
    }
    return true;
}

// static
bool ACodec::describeColorFormat(
        const sp<IOMX> &omx, IOMX::node_id node,
        DescribeColorFormat2Params &describeParams)
{
    OMX_INDEXTYPE describeColorFormatIndex;
    if (omx->getExtensionIndex(
            node, "OMX.google.android.index.describeColorFormat",
            &describeColorFormatIndex) == OK) {
        DescribeColorFormatParams describeParamsV1(describeParams);
        if (omx->getParameter(
                node, describeColorFormatIndex,
                &describeParamsV1, sizeof(describeParamsV1)) == OK) {
            describeParams.initFromV1(describeParamsV1);
            return describeParams.sMediaImage.mType != MediaImage2::MEDIA_IMAGE_TYPE_UNKNOWN;
        }
    } else if (omx->getExtensionIndex(
            node, "OMX.google.android.index.describeColorFormat2", &describeColorFormatIndex) == OK
               && omx->getParameter(
            node, describeColorFormatIndex, &describeParams, sizeof(describeParams)) == OK) {
        return describeParams.sMediaImage.mType != MediaImage2::MEDIA_IMAGE_TYPE_UNKNOWN;
    }

    return describeDefaultColorFormat(describeParams);
}

// static
bool ACodec::isFlexibleColorFormat(
         const sp<IOMX> &omx, IOMX::node_id node,
         uint32_t colorFormat, bool usingNativeBuffers, OMX_U32 *flexibleEquivalent) {
    DescribeColorFormat2Params describeParams;
    InitOMXParams(&describeParams);
    describeParams.eColorFormat = (OMX_COLOR_FORMATTYPE)colorFormat;
    // reasonable dummy values
    describeParams.nFrameWidth = 128;
    describeParams.nFrameHeight = 128;
    describeParams.nStride = 128;
    describeParams.nSliceHeight = 128;
    describeParams.bUsingNativeBuffers = (OMX_BOOL)usingNativeBuffers;

    CHECK(flexibleEquivalent != NULL);

    if (!describeColorFormat(omx, node, describeParams)) {
        return false;
    }

    const MediaImage2 &img = describeParams.sMediaImage;
    if (img.mType == MediaImage2::MEDIA_IMAGE_TYPE_YUV) {
        if (img.mNumPlanes != 3
                || img.mPlane[img.Y].mHorizSubsampling != 1
                || img.mPlane[img.Y].mVertSubsampling != 1) {
            return false;
        }

        // YUV 420
        if (img.mPlane[img.U].mHorizSubsampling == 2
                && img.mPlane[img.U].mVertSubsampling == 2
                && img.mPlane[img.V].mHorizSubsampling == 2
                && img.mPlane[img.V].mVertSubsampling == 2) {
            // possible flexible YUV420 format
            if (img.mBitDepth <= 8) {
               *flexibleEquivalent = OMX_COLOR_FormatYUV420Flexible;
               return true;
            }
        }
    }
    return false;
}

status_t ACodec::getPortFormat(OMX_U32 portIndex, sp<AMessage> &notify) {
    const char *niceIndex = portIndex == kPortIndexInput ? "input" : "output";
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        return err;
    }

    if (def.eDir != (portIndex == kPortIndexOutput ? OMX_DirOutput : OMX_DirInput)) {
        ALOGE("unexpected dir: %s(%d) on %s port", asString(def.eDir), def.eDir, niceIndex);
        return BAD_VALUE;
    }

    switch (def.eDomain) {
        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *videoDef = &def.format.video;
            switch ((int)videoDef->eCompressionFormat) {
                case OMX_VIDEO_CodingUnused:
                {
                    CHECK(mIsEncoder ^ (portIndex == kPortIndexOutput));
                    notify->setString("mime", MEDIA_MIMETYPE_VIDEO_RAW);

                    notify->setInt32("stride", videoDef->nStride);
                    notify->setInt32("slice-height", videoDef->nSliceHeight);
                    notify->setInt32("color-format", videoDef->eColorFormat);

                    if (mNativeWindow == NULL) {
                        DescribeColorFormat2Params describeParams;
                        InitOMXParams(&describeParams);
                        describeParams.eColorFormat = videoDef->eColorFormat;
                        describeParams.nFrameWidth = videoDef->nFrameWidth;
                        describeParams.nFrameHeight = videoDef->nFrameHeight;
                        describeParams.nStride = videoDef->nStride;
                        describeParams.nSliceHeight = videoDef->nSliceHeight;
                        describeParams.bUsingNativeBuffers = OMX_FALSE;

                        if (describeColorFormat(mOMX, mNode, describeParams)) {
                            notify->setBuffer(
                                    "image-data",
                                    ABuffer::CreateAsCopy(
                                            &describeParams.sMediaImage,
                                            sizeof(describeParams.sMediaImage)));

                            MediaImage2 &img = describeParams.sMediaImage;
                            MediaImage2::PlaneInfo *plane = img.mPlane;
                            ALOGV("[%s] MediaImage { F(%ux%u) @%u+%d+%d @%u+%d+%d @%u+%d+%d }",
                                    mComponentName.c_str(), img.mWidth, img.mHeight,
                                    plane[0].mOffset, plane[0].mColInc, plane[0].mRowInc,
                                    plane[1].mOffset, plane[1].mColInc, plane[1].mRowInc,
                                    plane[2].mOffset, plane[2].mColInc, plane[2].mRowInc);
                        }
                    }

                    int32_t width = (int32_t)videoDef->nFrameWidth;
                    int32_t height = (int32_t)videoDef->nFrameHeight;

                    if (portIndex == kPortIndexOutput) {
                        OMX_CONFIG_RECTTYPE rect;
                        InitOMXParams(&rect);
                        rect.nPortIndex = portIndex;

                        if (mOMX->getConfig(
                                    mNode,
                                    (portIndex == kPortIndexOutput ?
                                            OMX_IndexConfigCommonOutputCrop :
                                            OMX_IndexConfigCommonInputCrop),
                                    &rect, sizeof(rect)) != OK) {
                            rect.nLeft = 0;
                            rect.nTop = 0;
                            rect.nWidth = videoDef->nFrameWidth;
                            rect.nHeight = videoDef->nFrameHeight;
                        }

                        if (rect.nLeft < 0 ||
                            rect.nTop < 0 ||
                            rect.nLeft + rect.nWidth > videoDef->nFrameWidth ||
                            rect.nTop + rect.nHeight > videoDef->nFrameHeight) {
                            ALOGE("Wrong cropped rect (%d, %d) - (%u, %u) vs. frame (%u, %u)",
                                    rect.nLeft, rect.nTop,
                                    rect.nLeft + rect.nWidth, rect.nTop + rect.nHeight,
                                    videoDef->nFrameWidth, videoDef->nFrameHeight);
                            return BAD_VALUE;
                        }

                        notify->setRect(
                                "crop",
                                rect.nLeft,
                                rect.nTop,
                                rect.nLeft + rect.nWidth - 1,
                                rect.nTop + rect.nHeight - 1);

                        width = rect.nWidth;
                        height = rect.nHeight;

                        android_dataspace dataSpace = HAL_DATASPACE_UNKNOWN;
                        (void)getColorAspectsAndDataSpaceForVideoDecoder(
                                width, height, mConfigFormat, notify,
                                mUsingNativeWindow ? &dataSpace : NULL);
                        if (mUsingNativeWindow) {
                            notify->setInt32("android._dataspace", dataSpace);
                        }
                        (void)getHDRStaticInfoForVideoCodec(kPortIndexOutput, notify);
                    } else {
                        (void)getInputColorAspectsForVideoEncoder(notify);
                        if (mConfigFormat->contains("hdr-static-info")) {
                            (void)getHDRStaticInfoForVideoCodec(kPortIndexInput, notify);
                        }
                    }

                    break;
                }

                case OMX_VIDEO_CodingVP8:
                case OMX_VIDEO_CodingVP9:
                {
                    OMX_VIDEO_PARAM_ANDROID_VP8ENCODERTYPE vp8type;
                    InitOMXParams(&vp8type);
                    vp8type.nPortIndex = kPortIndexOutput;
                    status_t err = mOMX->getParameter(
                            mNode,
                            (OMX_INDEXTYPE)OMX_IndexParamVideoAndroidVp8Encoder,
                            &vp8type,
                            sizeof(vp8type));

                    if (err == OK) {
                        if (vp8type.eTemporalPattern == OMX_VIDEO_VPXTemporalLayerPatternWebRTC
                                && vp8type.nTemporalLayerCount > 0
                                && vp8type.nTemporalLayerCount
                                        <= OMX_VIDEO_ANDROID_MAXVP8TEMPORALLAYERS) {
                            // advertise as android.generic if we configured for android.generic
                            AString origSchema;
                            if (notify->findString("ts-schema", &origSchema)
                                    && origSchema.startsWith("android.generic")) {
                                notify->setString("ts-schema", AStringPrintf(
                                        "android.generic.%u", vp8type.nTemporalLayerCount));
                            } else {
                                notify->setString("ts-schema", AStringPrintf(
                                        "webrtc.vp8.%u-layer", vp8type.nTemporalLayerCount));
                            }
                        }
                    }
                    // Fall through to set up mime.
                }

                default:
                {
                    if (!mIsEncoder && !strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
                        err = FFMPEGSoftCodec::getVideoPortFormat(portIndex,
                                (int)videoDef->eCompressionFormat, notify, mOMX, mNode);
                        if (err == OK) {
                            break;
                        }
                    }

                    if (mIsEncoder ^ (portIndex == kPortIndexOutput)) {
                        // should be CodingUnused
                        ALOGE("Raw port video compression format is %s(%d)",
                                asString(videoDef->eCompressionFormat),
                                videoDef->eCompressionFormat);
                        return BAD_VALUE;
                    }
                    AString mime;
                    if (GetMimeTypeForVideoCoding(
                        videoDef->eCompressionFormat, &mime) != OK) {
                        notify->setString("mime", "application/octet-stream");
                    } else {
                        notify->setString("mime", mime.c_str());
                    }
                    uint32_t intraRefreshPeriod = 0;
                    if (mIsEncoder && getIntraRefreshPeriod(&intraRefreshPeriod) == OK
                            && intraRefreshPeriod > 0) {
                        notify->setInt32("intra-refresh-period", intraRefreshPeriod);
                    }
                    break;
                }
            }
            notify->setInt32("width", videoDef->nFrameWidth);
            notify->setInt32("height", videoDef->nFrameHeight);
            ALOGV("[%s] %s format is %s", mComponentName.c_str(),
                    portIndex == kPortIndexInput ? "input" : "output",
                    notify->debugString().c_str());

            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audioDef = &def.format.audio;

            switch ((int)audioDef->eEncoding) {
                case OMX_AUDIO_CodingPCM:
                {
                    OMX_AUDIO_PARAM_PCMMODETYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    if (params.nChannels <= 0
                            || (params.nChannels != 1 && !params.bInterleaved)
                            || params.ePCMMode != OMX_AUDIO_PCMModeLinear) {
                        ALOGE("unsupported PCM port: %u channels%s, %u-bit",
                                params.nChannels,
                                params.bInterleaved ? " interleaved" : "",
                                params.nBitPerSample);
                        return FAILED_TRANSACTION;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_RAW);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSamplingRate);

                    AudioEncoding encoding = kAudioEncodingPcm16bit;
                    if (params.eNumData == OMX_NumericalDataUnsigned
                            && params.nBitPerSample == 8u) {
                        encoding = kAudioEncodingPcm8bit;
                    } else if (params.eNumData == OMX_NumericalDataFloat
                            && params.nBitPerSample == 32u) {
                        encoding = kAudioEncodingPcmFloat;
                    } else if (params.eNumData == OMX_NumericalDataSigned
                            && params.nBitPerSample == 24u) {
                        encoding = kAudioEncodingPcm24bitPacked;
                    } else if (params.eNumData == OMX_NumericalDataSigned
                            && params.nBitPerSample == 32u) {
                        encoding = kAudioEncodingPcm32bit;
                    } else if (params.nBitPerSample != 16u
                            || params.eNumData != OMX_NumericalDataSigned) {
                        ALOGE("unsupported PCM port: %s(%d), %s(%d) mode ",
                                asString(params.eNumData), params.eNumData,
                                asString(params.ePCMMode), params.ePCMMode);
                        return FAILED_TRANSACTION;
                    }
                    notify->setInt32("pcm-encoding", encoding);

                    if (mChannelMaskPresent) {
                        notify->setInt32("channel-mask", mChannelMask);
                    }
                    break;
                }

                case OMX_AUDIO_CodingAAC:
                {
                    OMX_AUDIO_PARAM_AACPROFILETYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioAac, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingAMR:
                {
                    OMX_AUDIO_PARAM_AMRTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioAmr, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setInt32("channel-count", 1);
                    if (params.eAMRBandMode >= OMX_AUDIO_AMRBandModeWB0) {
                        notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_WB);
                        notify->setInt32("sample-rate", 16000);
                    } else {
                        notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_NB);
                        notify->setInt32("sample-rate", 8000);
                    }
                    break;
                }

                case OMX_AUDIO_CodingFLAC:
                {
                    if (!mIsEncoder && !strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
                        err = FFMPEGSoftCodec::getAudioPortFormat(portIndex,
                                (int)audioDef->eEncoding, notify, mOMX, mNode);
                        if (err != OK) {
                            return err;
                        }
                    } else {
                    OMX_AUDIO_PARAM_FLACTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioFlac, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_FLAC);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    }
                    break;
                }

                case OMX_AUDIO_CodingMP3:
                {
                    OMX_AUDIO_PARAM_MP3TYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioMp3, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_MPEG);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingVORBIS:
                {
                    OMX_AUDIO_PARAM_VORBISTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, OMX_IndexParamAudioVorbis, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_VORBIS);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingAndroidAC3:
                {
                    OMX_AUDIO_PARAM_ANDROID_AC3TYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
                            &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AC3);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingAndroidEAC3:
                {
                    OMX_AUDIO_PARAM_ANDROID_EAC3TYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidEac3,
                            &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_EAC3);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingAndroidOPUS:
                {
                    OMX_AUDIO_PARAM_ANDROID_OPUSTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidOpus,
                            &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_OPUS);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingG711:
                {
                    OMX_AUDIO_PARAM_PCMMODETYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                            mNode, (OMX_INDEXTYPE)OMX_IndexParamAudioPcm, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    const char *mime = NULL;
                    if (params.ePCMMode == OMX_AUDIO_PCMModeMULaw) {
                        mime = MEDIA_MIMETYPE_AUDIO_G711_MLAW;
                    } else if (params.ePCMMode == OMX_AUDIO_PCMModeALaw) {
                        mime = MEDIA_MIMETYPE_AUDIO_G711_ALAW;
                    } else { // params.ePCMMode == OMX_AUDIO_PCMModeLinear
                        mime = MEDIA_MIMETYPE_AUDIO_RAW;
                    }
                    notify->setString("mime", mime);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSamplingRate);
                    notify->setInt32("pcm-encoding", kAudioEncodingPcm16bit);
                    break;
                }

                case OMX_AUDIO_CodingGSMFR:
                {
                    OMX_AUDIO_PARAM_PCMMODETYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    err = mOMX->getParameter(
                                mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                    if (err != OK) {
                        return err;
                    }

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_MSGSM);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSamplingRate);
                    break;
                }

                default:
                    if (!mIsEncoder && !strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
                        err = FFMPEGSoftCodec::getAudioPortFormat(portIndex,
                                (int)audioDef->eEncoding, notify, mOMX, mNode);
                    }
                    if (err == OK) {
                        break;
                    }

                    ALOGE("Unsupported audio coding: %s(%d)\n",
                            asString(audioDef->eEncoding), audioDef->eEncoding);
                    return BAD_TYPE;
            }
            break;
        }

        default:
            ALOGE("Unsupported domain: %s(%d)", asString(def.eDomain), def.eDomain);
            return BAD_TYPE;
    }

    return OK;
}

void ACodec::onDataSpaceChanged(android_dataspace dataSpace, const ColorAspects &aspects) {
    // aspects are normally communicated in ColorAspects
    int32_t range, standard, transfer;
    convertCodecColorAspectsToPlatformAspects(aspects, &range, &standard, &transfer);

    // if some aspects are unspecified, use dataspace fields
    if (range != 0) {
        range = (dataSpace & HAL_DATASPACE_RANGE_MASK) >> HAL_DATASPACE_RANGE_SHIFT;
    }
    if (standard != 0) {
        standard = (dataSpace & HAL_DATASPACE_STANDARD_MASK) >> HAL_DATASPACE_STANDARD_SHIFT;
    }
    if (transfer != 0) {
        transfer = (dataSpace & HAL_DATASPACE_TRANSFER_MASK) >> HAL_DATASPACE_TRANSFER_SHIFT;
    }

    mOutputFormat = mOutputFormat->dup(); // trigger an output format changed event
    if (range != 0) {
        mOutputFormat->setInt32("color-range", range);
    }
    if (standard != 0) {
        mOutputFormat->setInt32("color-standard", standard);
    }
    if (transfer != 0) {
        mOutputFormat->setInt32("color-transfer", transfer);
    }

    ALOGD("dataspace changed to %#x (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s)) "
          "(R:%d(%s), S:%d(%s), T:%d(%s))",
            dataSpace,
            aspects.mRange, asString(aspects.mRange),
            aspects.mPrimaries, asString(aspects.mPrimaries),
            aspects.mMatrixCoeffs, asString(aspects.mMatrixCoeffs),
            aspects.mTransfer, asString(aspects.mTransfer),
            range, asString((ColorRange)range),
            standard, asString((ColorStandard)standard),
            transfer, asString((ColorTransfer)transfer));
}

void ACodec::onOutputFormatChanged(sp<const AMessage> expectedFormat) {
    // store new output format, at the same time mark that this is no longer the first frame
    mOutputFormat = mBaseOutputFormat->dup();

    if (getPortFormat(kPortIndexOutput, mOutputFormat) != OK) {
        ALOGE("[%s] Failed to get port format to send format change", mComponentName.c_str());
        return;
    }

    if (expectedFormat != NULL) {
        sp<const AMessage> changes = expectedFormat->changesFrom(mOutputFormat);
        sp<const AMessage> to = mOutputFormat->changesFrom(expectedFormat);
        if (changes->countEntries() != 0 || to->countEntries() != 0) {
            ALOGW("[%s] BAD CODEC: Output format changed unexpectedly from (diff) %s to (diff) %s",
                    mComponentName.c_str(),
                    changes->debugString(4).c_str(), to->debugString(4).c_str());
        }
    }

    if (!mIsVideo && !mIsEncoder) {
        AudioEncoding pcmEncoding = kAudioEncodingPcm16bit;
        (void)mConfigFormat->findInt32("pcm-encoding", (int32_t*)&pcmEncoding);
        AudioEncoding codecPcmEncoding = kAudioEncodingPcm16bit;
        (void)mOutputFormat->findInt32("pcm-encoding", (int32_t*)&codecPcmEncoding);

        mConverter[kPortIndexOutput] = AudioConverter::Create(codecPcmEncoding, pcmEncoding);
        if (mConverter[kPortIndexOutput] != NULL) {
            mOutputFormat->setInt32("pcm-encoding", pcmEncoding);
        }
    }

    if (mTunneled) {
        sendFormatChange();
    }
}

void ACodec::addKeyFormatChangesToRenderBufferNotification(sp<AMessage> &notify) {
    AString mime;
    CHECK(mOutputFormat->findString("mime", &mime));

    if (mime == MEDIA_MIMETYPE_VIDEO_RAW && mNativeWindow != NULL) {
        // notify renderer of the crop change and dataspace change
        // NOTE: native window uses extended right-bottom coordinate
        int32_t left, top, right, bottom;
        if (mOutputFormat->findRect("crop", &left, &top, &right, &bottom)) {
            notify->setRect("crop", left, top, right + 1, bottom + 1);
        }

        int32_t dataSpace;
        if (mOutputFormat->findInt32("android._dataspace", &dataSpace)) {
            notify->setInt32("dataspace", dataSpace);
        }
    }
}

void ACodec::sendFormatChange() {
    AString mime;
    CHECK(mOutputFormat->findString("mime", &mime));

    if (mime == MEDIA_MIMETYPE_AUDIO_RAW && (mEncoderDelay || mEncoderPadding)) {
        int32_t channelCount;
        CHECK(mOutputFormat->findInt32("channel-count", &channelCount));
        if (mSkipCutBuffer != NULL) {
            size_t prevbufsize = mSkipCutBuffer->size();
            if (prevbufsize != 0) {
                ALOGW("Replacing SkipCutBuffer holding %zu bytes", prevbufsize);
            }
        }
        mSkipCutBuffer = new SkipCutBuffer(mEncoderDelay, mEncoderPadding, channelCount);
    }

    sp<AMessage> notify = mNotify->dup();
    getVQZIPInfo(mOutputFormat);
    notify->setInt32("what", kWhatOutputFormatChanged);
    notify->setMessage("format", mOutputFormat);

    notify->post();

    // mLastOutputFormat is not used when tunneled; doing this just to stay consistent
    mLastOutputFormat = mOutputFormat;
}

void ACodec::signalError(OMX_ERRORTYPE error, status_t internalError) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatError);
    ALOGE("signalError(omxError %#x, internalError %d)", error, internalError);

    if (internalError == UNKNOWN_ERROR) { // find better error code
        const status_t omxStatus = statusFromOMXError(error);
        if (omxStatus != 0) {
            internalError = omxStatus;
        } else {
            ALOGW("Invalid OMX error %#x", error);
        }
    }

    mFatalError = true;

    notify->setInt32("err", internalError);
    notify->setInt32("actionCode", ACTION_CODE_FATAL); // could translate from OMX error.
    notify->post();
}

////////////////////////////////////////////////////////////////////////////////

ACodec::PortDescription::PortDescription() {
}

status_t ACodec::requestIDRFrame() {
    if (!mIsEncoder) {
        return ERROR_UNSUPPORTED;
    }

    OMX_CONFIG_INTRAREFRESHVOPTYPE params;
    InitOMXParams(&params);

    params.nPortIndex = kPortIndexOutput;
    params.IntraRefreshVOP = OMX_TRUE;

    return mOMX->setConfig(
            mNode,
            OMX_IndexConfigVideoIntraVOPRefresh,
            &params,
            sizeof(params));
}

void ACodec::PortDescription::addBuffer(
        IOMX::buffer_id id, const sp<ABuffer> &buffer,
        const sp<NativeHandle> &handle, const sp<RefBase> &memRef) {
    mBufferIDs.push_back(id);
    mBuffers.push_back(buffer);
    mHandles.push_back(handle);
    mMemRefs.push_back(memRef);
}

size_t ACodec::PortDescription::countBuffers() {
    return mBufferIDs.size();
}

IOMX::buffer_id ACodec::PortDescription::bufferIDAt(size_t index) const {
    return mBufferIDs.itemAt(index);
}

sp<ABuffer> ACodec::PortDescription::bufferAt(size_t index) const {
    return mBuffers.itemAt(index);
}

sp<NativeHandle> ACodec::PortDescription::handleAt(size_t index) const {
    return mHandles.itemAt(index);
}

sp<RefBase> ACodec::PortDescription::memRefAt(size_t index) const {
    return mMemRefs.itemAt(index);
}

////////////////////////////////////////////////////////////////////////////////

ACodec::BaseState::BaseState(ACodec *codec, const sp<AState> &parentState)
    : AState(parentState),
      mCodec(codec) {
}

ACodec::BaseState::PortMode ACodec::BaseState::getPortMode(
        OMX_U32 /* portIndex */) {
    return KEEP_BUFFERS;
}

bool ACodec::BaseState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatInputBufferFilled:
        {
            onInputBufferFilled(msg);
            break;
        }

        case kWhatOutputBufferDrained:
        {
            onOutputBufferDrained(msg);
            break;
        }

        case ACodec::kWhatOMXMessageList:
        {
            return checkOMXMessage(msg) ? onOMXMessageList(msg) : true;
        }

        case ACodec::kWhatOMXMessageItem:
        {
            // no need to check as we already did it for kWhatOMXMessageList
            return onOMXMessage(msg);
        }

        case ACodec::kWhatOMXMessage:
        {
            return checkOMXMessage(msg) ? onOMXMessage(msg) : true;
        }

        case ACodec::kWhatSetSurface:
        {
            sp<AReplyToken> replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            sp<RefBase> obj;
            CHECK(msg->findObject("surface", &obj));

            status_t err = mCodec->handleSetSurface(static_cast<Surface *>(obj.get()));

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);
            response->postReply(replyID);
            break;
        }

        case ACodec::kWhatCreateInputSurface:
        case ACodec::kWhatSetInputSurface:
        case ACodec::kWhatSignalEndOfInputStream:
        {
            // This may result in an app illegal state exception.
            ALOGE("Message 0x%x was not handled", msg->what());
            mCodec->signalError(OMX_ErrorUndefined, INVALID_OPERATION);
            return true;
        }

        case ACodec::kWhatOMXDied:
        {
            // This will result in kFlagSawMediaServerDie handling in MediaCodec.
            ALOGE("OMX/mediaserver died, signalling error!");
            mCodec->signalError(OMX_ErrorResourcesLost, DEAD_OBJECT);
            break;
        }

        case ACodec::kWhatReleaseCodecInstance:
        {
            ALOGI("[%s] forcing the release of codec",
                    mCodec->mComponentName.c_str());
            status_t err = mCodec->mOMX->freeNode(mCodec->mNode);
            mCodec->changeState(mCodec->mUninitializedState);
            ALOGE_IF("[%s] failed to release codec instance: err=%d",
                       mCodec->mComponentName.c_str(), err);
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
            notify->post();
            break;
        }

        default:
            return false;
    }

    return true;
}

bool ACodec::BaseState::checkOMXMessage(const sp<AMessage> &msg) {
    // there is a possibility that this is an outstanding message for a
    // codec that we have already destroyed
    if (mCodec->mNode == 0) {
        ALOGI("ignoring message as already freed component: %s",
                msg->debugString().c_str());
        return false;
    }

    IOMX::node_id nodeID;
    CHECK(msg->findInt32("node", (int32_t*)&nodeID));
    if (nodeID != mCodec->mNode) {
        ALOGE("Unexpected message for nodeID: %u, should have been %u", nodeID, mCodec->mNode);
        return false;
    }
    return true;
}

bool ACodec::BaseState::onOMXMessageList(const sp<AMessage> &msg) {
    sp<RefBase> obj;
    CHECK(msg->findObject("messages", &obj));
    sp<MessageList> msgList = static_cast<MessageList *>(obj.get());

    bool receivedRenderedEvents = false;
    for (std::list<sp<AMessage>>::const_iterator it = msgList->getList().cbegin();
          it != msgList->getList().cend(); ++it) {
        (*it)->setWhat(ACodec::kWhatOMXMessageItem);
        mCodec->handleMessage(*it);
        int32_t type;
        CHECK((*it)->findInt32("type", &type));
        if (type == omx_message::FRAME_RENDERED) {
            receivedRenderedEvents = true;
        }
    }

    if (receivedRenderedEvents) {
        // NOTE: all buffers are rendered in this case
        mCodec->notifyOfRenderedFrames();
    }
    return true;
}

bool ACodec::BaseState::onOMXMessage(const sp<AMessage> &msg) {
    int32_t type;
    CHECK(msg->findInt32("type", &type));

    switch (type) {
        case omx_message::EVENT:
        {
            int32_t event, data1, data2;
            CHECK(msg->findInt32("event", &event));
            CHECK(msg->findInt32("data1", &data1));
            CHECK(msg->findInt32("data2", &data2));

            if (event == OMX_EventCmdComplete
                    && data1 == OMX_CommandFlush
                    && data2 == (int32_t)OMX_ALL) {
                // Use of this notification is not consistent across
                // implementations. We'll drop this notification and rely
                // on flush-complete notifications on the individual port
                // indices instead.

                return true;
            }

            return onOMXEvent(
                    static_cast<OMX_EVENTTYPE>(event),
                    static_cast<OMX_U32>(data1),
                    static_cast<OMX_U32>(data2));
        }

        case omx_message::EMPTY_BUFFER_DONE:
        {
            IOMX::buffer_id bufferID;
            int32_t fenceFd;

            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));
            CHECK(msg->findInt32("fence_fd", &fenceFd));

            return onOMXEmptyBufferDone(bufferID, fenceFd);
        }

        case omx_message::FILL_BUFFER_DONE:
        {
            IOMX::buffer_id bufferID;
            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));

            int32_t rangeOffset, rangeLength, flags, fenceFd;
            int64_t timeUs;

            CHECK(msg->findInt32("range_offset", &rangeOffset));
            CHECK(msg->findInt32("range_length", &rangeLength));
            CHECK(msg->findInt32("flags", &flags));
            CHECK(msg->findInt64("timestamp", &timeUs));
            CHECK(msg->findInt32("fence_fd", &fenceFd));

            return onOMXFillBufferDone(
                    bufferID,
                    (size_t)rangeOffset, (size_t)rangeLength,
                    (OMX_U32)flags,
                    timeUs,
                    fenceFd);
        }

        case omx_message::FRAME_RENDERED:
        {
            int64_t mediaTimeUs, systemNano;

            CHECK(msg->findInt64("media_time_us", &mediaTimeUs));
            CHECK(msg->findInt64("system_nano", &systemNano));

            return onOMXFrameRendered(
                    mediaTimeUs, systemNano);
        }

        default:
            ALOGE("Unexpected message type: %d", type);
            return false;
    }
}

bool ACodec::BaseState::onOMXFrameRendered(
        int64_t mediaTimeUs __unused, nsecs_t systemNano __unused) {
    // ignore outside of Executing and PortSettingsChanged states
    return true;
}

bool ACodec::BaseState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    if (event == OMX_EventDataSpaceChanged) {
        ColorAspects aspects;
        aspects.mRange = (ColorAspects::Range)((data2 >> 24) & 0xFF);
        aspects.mPrimaries = (ColorAspects::Primaries)((data2 >> 16) & 0xFF);
        aspects.mMatrixCoeffs = (ColorAspects::MatrixCoeffs)((data2 >> 8) & 0xFF);
        aspects.mTransfer = (ColorAspects::Transfer)(data2 & 0xFF);

        mCodec->onDataSpaceChanged((android_dataspace)data1, aspects);
        return true;
    }

    if (event != OMX_EventError) {
        ALOGV("[%s] EVENT(%d, 0x%08x, 0x%08x)",
             mCodec->mComponentName.c_str(), event, data1, data2);

        return false;
    }

    ALOGE("[%s] ERROR(0x%08x)", mCodec->mComponentName.c_str(), data1);

    // verify OMX component sends back an error we expect.
    OMX_ERRORTYPE omxError = (OMX_ERRORTYPE)data1;
    if (!isOMXError(omxError)) {
        ALOGW("Invalid OMX error %#x", omxError);
        omxError = OMX_ErrorUndefined;
    }
    mCodec->signalError(omxError);

    return true;
}

bool ACodec::BaseState::onOMXEmptyBufferDone(IOMX::buffer_id bufferID, int fenceFd) {
    ALOGV("[%s] onOMXEmptyBufferDone %u",
         mCodec->mComponentName.c_str(), bufferID);

    BufferInfo *info = mCodec->findBufferByID(kPortIndexInput, bufferID);
    BufferInfo::Status status = BufferInfo::getSafeStatus(info);
    if (status != BufferInfo::OWNED_BY_COMPONENT) {
        ALOGE("Wrong ownership in EBD: %s(%d) buffer #%u", _asString(status), status, bufferID);
        mCodec->dumpBuffers(kPortIndexInput);
        if (fenceFd >= 0) {
            ::close(fenceFd);
        }
        return false;
    }
    info->mStatus = BufferInfo::OWNED_BY_US;

    // input buffers cannot take fences, so wait for any fence now
    (void)mCodec->waitForFence(fenceFd, "onOMXEmptyBufferDone");
    fenceFd = -1;

    // still save fence for completeness
    info->setWriteFence(fenceFd, "onOMXEmptyBufferDone");

    // We're in "store-metadata-in-buffers" mode, the underlying
    // OMX component had access to data that's implicitly refcounted
    // by this "MediaBuffer" object. Now that the OMX component has
    // told us that it's done with the input buffer, we can decrement
    // the mediaBuffer's reference count.
    info->mData->setMediaBufferBase(NULL);

    PortMode mode = getPortMode(kPortIndexInput);

    switch (mode) {
        case KEEP_BUFFERS:
            break;

        case RESUBMIT_BUFFERS:
            postFillThisBuffer(info);
            break;

        case FREE_BUFFERS:
        default:
            ALOGE("SHOULD NOT REACH HERE: cannot free empty output buffers");
            return false;
    }

    return true;
}

void ACodec::BaseState::postFillThisBuffer(BufferInfo *info) {
    if (mCodec->mPortEOS[kPortIndexInput]) {
        return;
    }

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatFillThisBuffer);
    notify->setInt32("buffer-id", info->mBufferID);

    info->mData->meta()->clear();
    notify->setBuffer("buffer", info->mData);

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, mCodec);
    reply->setInt32("buffer-id", info->mBufferID);

    notify->setMessage("reply", reply);

    notify->post();

    info->mStatus = BufferInfo::OWNED_BY_UPSTREAM;
}

void ACodec::BaseState::onInputBufferFilled(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));
    sp<ABuffer> buffer;
    int32_t err = OK;
    bool eos = false;
    PortMode mode = getPortMode(kPortIndexInput);

    if (!msg->findBuffer("buffer", &buffer)) {
        /* these are unfilled buffers returned by client */
        CHECK(msg->findInt32("err", &err));

        if (err == OK) {
            /* buffers with no errors are returned on MediaCodec.flush */
            mode = KEEP_BUFFERS;
        } else {
            ALOGV("[%s] saw error %d instead of an input buffer",
                 mCodec->mComponentName.c_str(), err);
            eos = true;
        }

        buffer.clear();
    }

    int32_t tmp;
    if (buffer != NULL && buffer->meta()->findInt32("eos", &tmp) && tmp) {
        eos = true;
        err = ERROR_END_OF_STREAM;
    }

    BufferInfo *info = mCodec->findBufferByID(kPortIndexInput, bufferID);
    BufferInfo::Status status = BufferInfo::getSafeStatus(info);
    if (status != BufferInfo::OWNED_BY_UPSTREAM) {
        ALOGE("Wrong ownership in IBF: %s(%d) buffer #%u", _asString(status), status, bufferID);
        mCodec->dumpBuffers(kPortIndexInput);
        mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
        return;
    }

    info->mStatus = BufferInfo::OWNED_BY_US;

    switch (mode) {
        case KEEP_BUFFERS:
        {
            if (eos) {
                if (!mCodec->mPortEOS[kPortIndexInput]) {
                    mCodec->mPortEOS[kPortIndexInput] = true;
                    mCodec->mInputEOSResult = err;
                }
            }
            break;
        }

        case RESUBMIT_BUFFERS:
        {
            if (buffer != NULL && !mCodec->mPortEOS[kPortIndexInput]) {
                // Do not send empty input buffer w/o EOS to the component.
                if (buffer->size() == 0 && !eos) {
                    postFillThisBuffer(info);
                    break;
                }

                int64_t timeUs;
                CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

                OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;

                MetadataBufferType metaType = mCodec->mInputMetadataType;
                int32_t isCSD = 0;
                if (buffer->meta()->findInt32("csd", &isCSD) && isCSD != 0) {
                    if (mCodec->mIsLegacyVP9Decoder) {
                        ALOGV("[%s] is legacy VP9 decoder. Ignore %u codec specific data",
                            mCodec->mComponentName.c_str(), bufferID);
                        postFillThisBuffer(info);
                        break;
                    }
                    flags |= OMX_BUFFERFLAG_CODECCONFIG;
                    metaType = kMetadataBufferTypeInvalid;
                }

                if (eos) {
                    flags |= OMX_BUFFERFLAG_EOS;
                }

                if (buffer != info->mCodecData) {
                    ALOGV("[%s] Needs to copy input data for buffer %u. (%p != %p)",
                         mCodec->mComponentName.c_str(),
                         bufferID,
                         buffer.get(), info->mCodecData.get());

                    sp<DataConverter> converter = mCodec->mConverter[kPortIndexInput];
                    if (converter == NULL || isCSD) {
                        converter = getCopyConverter();
                    }
                    status_t err = converter->convert(buffer, info->mCodecData);
                    if (err != OK) {
                        mCodec->signalError(OMX_ErrorUndefined, err);
                        return;
                    }
                }

                if (flags & OMX_BUFFERFLAG_CODECCONFIG) {
                    ALOGV("[%s] calling emptyBuffer %u w/ codec specific data",
                         mCodec->mComponentName.c_str(), bufferID);
                } else if (flags & OMX_BUFFERFLAG_EOS) {
                    ALOGV("[%s] calling emptyBuffer %u w/ EOS",
                         mCodec->mComponentName.c_str(), bufferID);
                } else {
#if TRACK_BUFFER_TIMING
                    ALOGI("[%s] calling emptyBuffer %u w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, (long long)timeUs);
#else
                    ALOGV("[%s] calling emptyBuffer %u w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, (long long)timeUs);
#endif
                }

#if TRACK_BUFFER_TIMING
                ACodec::BufferStats stats;
                stats.mEmptyBufferTimeUs = ALooper::GetNowUs();
                stats.mFillBufferDoneTimeUs = -1ll;
                mCodec->mBufferStats.add(timeUs, stats);
#endif

                if (mCodec->storingMetadataInDecodedBuffers()) {
                    // try to submit an output buffer for each input buffer
                    PortMode outputMode = getPortMode(kPortIndexOutput);

                    ALOGV("MetadataBuffersToSubmit=%u portMode=%s",
                            mCodec->mMetadataBuffersToSubmit,
                            (outputMode == FREE_BUFFERS ? "FREE" :
                             outputMode == KEEP_BUFFERS ? "KEEP" : "RESUBMIT"));
                    if (outputMode == RESUBMIT_BUFFERS) {
                        mCodec->submitOutputMetadataBuffer();
                    }
                }
                info->checkReadFence("onInputBufferFilled");

                status_t err2 = OK;
                switch (metaType) {
                case kMetadataBufferTypeInvalid:
#ifdef CAMCORDER_GRALLOC_SOURCE
                case kMetadataBufferTypeCameraSource:
#endif
                    break;
#ifndef OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
                case kMetadataBufferTypeNativeHandleSource:
                    if (info->mCodecData->size() >= sizeof(VideoNativeHandleMetadata)) {
                        VideoNativeHandleMetadata *vnhmd =
                            (VideoNativeHandleMetadata*)info->mCodecData->base();
                        err2 = mCodec->mOMX->updateNativeHandleInMeta(
                                mCodec->mNode, kPortIndexInput,
                                NativeHandle::create(vnhmd->pHandle, false /* ownsHandle */),
                                bufferID);
                    }
                    break;
                case kMetadataBufferTypeANWBuffer:
                    if (info->mCodecData->size() >= sizeof(VideoNativeMetadata)) {
                        VideoNativeMetadata *vnmd = (VideoNativeMetadata*)info->mCodecData->base();
                        err2 = mCodec->mOMX->updateGraphicBufferInMeta(
                                mCodec->mNode, kPortIndexInput,
                                new GraphicBuffer(vnmd->pBuffer, false /* keepOwnership */),
                                bufferID);
                    }
                    break;
#endif
                default:
                    ALOGW("Can't marshall %s data in %zu sized buffers in %zu-bit mode",
                            asString(metaType), info->mCodecData->size(),
                            sizeof(buffer_handle_t) * 8);
                    err2 = ERROR_UNSUPPORTED;
                    break;
                }

                if (err2 == OK) {
                    err2 = mCodec->mOMX->emptyBuffer(
                        mCodec->mNode,
                        bufferID,
                        0,
                        info->mCodecData->size(),
                        flags,
                        timeUs,
                        info->mFenceFd);
                }
                info->mFenceFd = -1;
                if (err2 != OK) {
                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err2));
                    return;
                }
                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                if (!eos && err == OK) {
                    getMoreInputDataIfPossible();
                } else {
                    ALOGV("[%s] Signalled EOS (%d) on the input port",
                         mCodec->mComponentName.c_str(), err);

                    mCodec->mPortEOS[kPortIndexInput] = true;
                    mCodec->mInputEOSResult = err;
                }
            } else if (!mCodec->mPortEOS[kPortIndexInput]) {
                if (err != OK && err != ERROR_END_OF_STREAM) {
                    ALOGV("[%s] Signalling EOS on the input port due to error %d",
                         mCodec->mComponentName.c_str(), err);
                } else {
                    ALOGV("[%s] Signalling EOS on the input port",
                         mCodec->mComponentName.c_str());
                }

                ALOGV("[%s] calling emptyBuffer %u signalling EOS",
                     mCodec->mComponentName.c_str(), bufferID);

                info->checkReadFence("onInputBufferFilled");
                status_t err2 = mCodec->mOMX->emptyBuffer(
                        mCodec->mNode,
                        bufferID,
                        0,
                        0,
                        OMX_BUFFERFLAG_EOS,
                        0,
                        info->mFenceFd);
                info->mFenceFd = -1;
                if (err2 != OK) {
                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err2));
                    return;
                }
                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                mCodec->mPortEOS[kPortIndexInput] = true;
                mCodec->mInputEOSResult = err;
            }
            break;
        }

        case FREE_BUFFERS:
            break;

        default:
            ALOGE("invalid port mode: %d", mode);
            break;
    }
}

void ACodec::BaseState::getMoreInputDataIfPossible() {
    if (mCodec->mPortEOS[kPortIndexInput]) {
        return;
    }

    BufferInfo *eligible = NULL;

    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(i);

#if 0
        if (info->mStatus == BufferInfo::OWNED_BY_UPSTREAM) {
            // There's already a "read" pending.
            return;
        }
#endif

        if (info->mStatus == BufferInfo::OWNED_BY_US) {
            eligible = info;
        }
    }

    if (eligible == NULL) {
        return;
    }

    postFillThisBuffer(eligible);
}

bool ACodec::BaseState::onOMXFillBufferDone(
        IOMX::buffer_id bufferID,
        size_t rangeOffset, size_t rangeLength,
        OMX_U32 flags,
        int64_t timeUs,
        int fenceFd) {
    ALOGV("[%s] onOMXFillBufferDone %u time %" PRId64 " us, flags = 0x%08x",
         mCodec->mComponentName.c_str(), bufferID, timeUs, flags);

    ssize_t index;
    status_t err= OK;

#if TRACK_BUFFER_TIMING
    index = mCodec->mBufferStats.indexOfKey(timeUs);
    if (index >= 0) {
        ACodec::BufferStats *stats = &mCodec->mBufferStats.editValueAt(index);
        stats->mFillBufferDoneTimeUs = ALooper::GetNowUs();

        ALOGI("frame PTS %lld: %lld",
                timeUs,
                stats->mFillBufferDoneTimeUs - stats->mEmptyBufferTimeUs);

        mCodec->mBufferStats.removeItemsAt(index);
        stats = NULL;
    }
#endif

    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexOutput, bufferID, &index);
    BufferInfo::Status status = BufferInfo::getSafeStatus(info);
    if (status != BufferInfo::OWNED_BY_COMPONENT) {
        ALOGE("Wrong ownership in FBD: %s(%d) buffer #%u", _asString(status), status, bufferID);
        mCodec->dumpBuffers(kPortIndexOutput);
        mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
        if (fenceFd >= 0) {
            ::close(fenceFd);
        }
        return true;
    }

    info->mDequeuedAt = ++mCodec->mDequeueCounter;
    info->mStatus = BufferInfo::OWNED_BY_US;

    if (info->mRenderInfo != NULL) {
        // The fence for an emptied buffer must have signaled, but there still could be queued
        // or out-of-order dequeued buffers in the render queue prior to this buffer. Drop these,
        // as we will soon requeue this buffer to the surface. While in theory we could still keep
        // track of buffers that are requeued to the surface, it is better to add support to the
        // buffer-queue to notify us of released buffers and their fences (in the future).
        mCodec->notifyOfRenderedFrames(true /* dropIncomplete */);
    }

    // byte buffers cannot take fences, so wait for any fence now
    if (mCodec->mNativeWindow == NULL) {
        (void)mCodec->waitForFence(fenceFd, "onOMXFillBufferDone");
        fenceFd = -1;
    }
    info->setReadFence(fenceFd, "onOMXFillBufferDone");

    PortMode mode = getPortMode(kPortIndexOutput);

    switch (mode) {
        case KEEP_BUFFERS:
            break;

        case RESUBMIT_BUFFERS:
        {
            if (rangeLength == 0 && (!(flags & OMX_BUFFERFLAG_EOS)
                    || mCodec->mPortEOS[kPortIndexOutput])) {
                ALOGV("[%s] calling fillBuffer %u",
                     mCodec->mComponentName.c_str(), info->mBufferID);

                err = mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID, info->mFenceFd);
                info->mFenceFd = -1;
                if (err != OK) {
                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                    return true;
                }

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                break;
            }

            sp<AMessage> reply =
                new AMessage(kWhatOutputBufferDrained, mCodec);

            if (mCodec->mOutputFormat != mCodec->mLastOutputFormat && rangeLength > 0) {
                // pretend that output format has changed on the first frame (we used to do this)
                if (mCodec->mBaseOutputFormat == mCodec->mOutputFormat) {
                    mCodec->onOutputFormatChanged(mCodec->mOutputFormat);
                }
                mCodec->addKeyFormatChangesToRenderBufferNotification(reply);
                mCodec->sendFormatChange();
            } else if (rangeLength > 0 && mCodec->mNativeWindow != NULL) {
                // If potentially rendering onto a surface, always save key format data (crop &
                // data space) so that we can set it if and once the buffer is rendered.
                mCodec->addKeyFormatChangesToRenderBufferNotification(reply);
            }

            if (mCodec->usingMetadataOnEncoderOutput()) {
                native_handle_t *handle = NULL;
                VideoNativeHandleMetadata &nativeMeta =
                    *(VideoNativeHandleMetadata *)info->mData->data();
#ifdef CAMCORDER_GRALLOC_SOURCE
                VideoGrallocMetadata &grallocMeta =
                    *(VideoGrallocMetadata *)info->mData->data();
#endif
                if (info->mData->size() >= sizeof(nativeMeta)
                        && nativeMeta.eType == kMetadataBufferTypeNativeHandleSource) {
#ifdef OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
                    // handle is only valid on 32-bit/mediaserver process
                    handle = NULL;
#else
                    handle = (native_handle_t *)nativeMeta.pHandle;
#endif
                }
#ifdef CAMCORDER_GRALLOC_SOURCE
                else if (info->mData->size() >= sizeof(grallocMeta)
                        && grallocMeta.eType == kMetadataBufferTypeGrallocSource) {
                    handle = (native_handle_t *)(uintptr_t)grallocMeta.pHandle;
                }
#endif
                info->mData->meta()->setPointer("handle", handle);
                info->mData->meta()->setInt32("rangeOffset", rangeOffset);
                info->mData->meta()->setInt32("rangeLength", rangeLength);
            } else if (info->mData == info->mCodecData) {
                info->mData->setRange(rangeOffset, rangeLength);
            } else {
                info->mCodecData->setRange(rangeOffset, rangeLength);
                // in this case we know that mConverter is not null
                status_t err = mCodec->mConverter[kPortIndexOutput]->convert(
                        info->mCodecData, info->mData);
                if (err != OK) {
                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                    return true;
                }
            }
#if 0
            if (mCodec->mNativeWindow == NULL) {
                if (IsIDR(info->mData)) {
                    ALOGI("IDR frame");
                }
            }
#endif

            if (mCodec->mSkipCutBuffer != NULL) {
                mCodec->mSkipCutBuffer->submit(info->mData);
            }
            info->mData->meta()->setInt64("timeUs", timeUs);
            info->mData->meta()->setObject("graphic-buffer", info->mGraphicBuffer);

            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatDrainThisBuffer);
            notify->setInt32("buffer-id", info->mBufferID);
            notify->setBuffer("buffer", info->mData);
            notify->setInt32("flags", flags);

            reply->setInt32("buffer-id", info->mBufferID);

            notify->setMessage("reply", reply);

            notify->post();

            info->mStatus = BufferInfo::OWNED_BY_DOWNSTREAM;

            if (flags & OMX_BUFFERFLAG_EOS) {
                ALOGV("[%s] saw output EOS", mCodec->mComponentName.c_str());

                sp<AMessage> notify = mCodec->mNotify->dup();
                notify->setInt32("what", CodecBase::kWhatEOS);
                notify->setInt32("err", mCodec->mInputEOSResult);
                notify->post();

                mCodec->mPortEOS[kPortIndexOutput] = true;
            }
            break;
        }

        case FREE_BUFFERS:
            err = mCodec->freeBuffer(kPortIndexOutput, index);
            if (err != OK) {
                mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                return true;
            }
            break;

        default:
            ALOGE("Invalid port mode: %d", mode);
            return false;
    }

    return true;
}

void ACodec::BaseState::onOutputBufferDrained(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));
    ssize_t index;
    BufferInfo *info = mCodec->findBufferByID(kPortIndexOutput, bufferID, &index);
    BufferInfo::Status status = BufferInfo::getSafeStatus(info);
    if (status != BufferInfo::OWNED_BY_DOWNSTREAM) {
        ALOGE("Wrong ownership in OBD: %s(%d) buffer #%u", _asString(status), status, bufferID);
        mCodec->dumpBuffers(kPortIndexOutput);
        mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
        return;
    }

    android_native_rect_t crop;
    if (msg->findRect("crop", &crop.left, &crop.top, &crop.right, &crop.bottom)
            && memcmp(&crop, &mCodec->mLastNativeWindowCrop, sizeof(crop)) != 0) {
        mCodec->mLastNativeWindowCrop = crop;
        status_t err = native_window_set_crop(mCodec->mNativeWindow.get(), &crop);
        ALOGW_IF(err != NO_ERROR, "failed to set crop: %d", err);
    }

    int32_t dataSpace;
    if (msg->findInt32("dataspace", &dataSpace)
            && dataSpace != mCodec->mLastNativeWindowDataSpace) {
        status_t err = native_window_set_buffers_data_space(
                mCodec->mNativeWindow.get(), (android_dataspace)dataSpace);
        mCodec->mLastNativeWindowDataSpace = dataSpace;
        ALOGW_IF(err != NO_ERROR, "failed to set dataspace: %d", err);
    }

    int32_t render;
    if (mCodec->mNativeWindow != NULL
            && msg->findInt32("render", &render) && render != 0
            && info->mData != NULL && info->mData->size() != 0) {
        ATRACE_NAME("render");
        // The client wants this buffer to be rendered.

        // save buffers sent to the surface so we can get render time when they return
        int64_t mediaTimeUs = -1;
        info->mData->meta()->findInt64("timeUs", &mediaTimeUs);
        if (mediaTimeUs >= 0) {
            mCodec->mRenderTracker.onFrameQueued(
                    mediaTimeUs, info->mGraphicBuffer, new Fence(::dup(info->mFenceFd)));
        }

        int64_t timestampNs = 0;
        if (!msg->findInt64("timestampNs", &timestampNs)) {
            // use media timestamp if client did not request a specific render timestamp
            if (info->mData->meta()->findInt64("timeUs", &timestampNs)) {
                ALOGV("using buffer PTS of %lld", (long long)timestampNs);
                timestampNs *= 1000;
            }
        }

        status_t err;
        err = native_window_set_buffers_timestamp(mCodec->mNativeWindow.get(), timestampNs);
        ALOGW_IF(err != NO_ERROR, "failed to set buffer timestamp: %d", err);

        info->checkReadFence("onOutputBufferDrained before queueBuffer");
        err = mCodec->mNativeWindow->queueBuffer(
                    mCodec->mNativeWindow.get(), info->mGraphicBuffer.get(), info->mFenceFd);
        info->mFenceFd = -1;
        if (err == OK) {
            info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        } else {
            ALOGE("queueBuffer failed in onOutputBufferDrained: %d", err);
            mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
            info->mStatus = BufferInfo::OWNED_BY_US;
            // keeping read fence as write fence to avoid clobbering
            info->mIsReadFence = false;
        }
    } else {
        if (mCodec->mNativeWindow != NULL &&
            (info->mData == NULL || info->mData->size() != 0)) {
            // move read fence into write fence to avoid clobbering
            info->mIsReadFence = false;
            ATRACE_NAME("frame-drop");
        }
        info->mStatus = BufferInfo::OWNED_BY_US;
    }

    PortMode mode = getPortMode(kPortIndexOutput);

    switch (mode) {
        case KEEP_BUFFERS:
        {
            // XXX fishy, revisit!!! What about the FREE_BUFFERS case below?

            if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                // We cannot resubmit the buffer we just rendered, dequeue
                // the spare instead.

                info = mCodec->dequeueBufferFromNativeWindow();
            }
            break;
        }

        case RESUBMIT_BUFFERS:
        {
            if (!mCodec->mPortEOS[kPortIndexOutput]) {
                if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                    // We cannot resubmit the buffer we just rendered, dequeue
                    // the spare instead.

                    info = mCodec->dequeueBufferFromNativeWindow();
                }

                if (info != NULL) {
                    ALOGV("[%s] calling fillBuffer %u",
                         mCodec->mComponentName.c_str(), info->mBufferID);
                    info->checkWriteFence("onOutputBufferDrained::RESUBMIT_BUFFERS");
                    status_t err = mCodec->mOMX->fillBuffer(
                            mCodec->mNode, info->mBufferID, info->mFenceFd);
                    info->mFenceFd = -1;
                    if (err == OK) {
                        info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                    } else {
                        mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                    }
                }
            }
            break;
        }

        case FREE_BUFFERS:
        {
            status_t err = mCodec->freeBuffer(kPortIndexOutput, index);
            if (err != OK) {
                mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
            }
            break;
        }

        default:
            ALOGE("Invalid port mode: %d", mode);
            return;
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::UninitializedState::UninitializedState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::UninitializedState::stateEntered() {
    ALOGV("Now uninitialized");

    if (mDeathNotifier != NULL) {
        mCodec->mNodeBinder->unlinkToDeath(mDeathNotifier);
        mDeathNotifier.clear();
    }

    mCodec->mUsingNativeWindow = false;
    mCodec->mNativeWindow.clear();
    mCodec->mNativeWindowUsageBits = 0;
    mCodec->mNode = 0;
    mCodec->mOMX.clear();
    mCodec->mQuirks = 0;
    mCodec->mFlags = 0;
    mCodec->mInputMetadataType = kMetadataBufferTypeInvalid;
    mCodec->mOutputMetadataType = kMetadataBufferTypeInvalid;
    mCodec->mConverter[0].clear();
    mCodec->mConverter[1].clear();
    mCodec->mComponentName.clear();
}

bool ACodec::UninitializedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case ACodec::kWhatSetup:
        {
            onSetup(msg);

            handled = true;
            break;
        }

        case ACodec::kWhatAllocateComponent:
        {
            onAllocateComponent(msg);
            handled = true;
            break;
        }

        case ACodec::kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));
            ALOGW_IF(keepComponentAllocated,
                     "cannot keep component allocated on shutdown in Uninitialized state");

            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
            notify->post();

            handled = true;
            break;
        }

        case ACodec::kWhatFlush:
        {
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatFlushCompleted);
            notify->post();

            handled = true;
            break;
        }

        case ACodec::kWhatReleaseCodecInstance:
        {
            // nothing to do, as we have already signaled shutdown
            handled = true;
            break;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }

    return handled;
}

void ACodec::UninitializedState::onSetup(
        const sp<AMessage> &msg) {
    if (onAllocateComponent(msg)
            && mCodec->mLoadedState->onConfigureComponent(msg)) {
        mCodec->mLoadedState->onStart();
    }
}

bool ACodec::UninitializedState::onAllocateComponent(const sp<AMessage> &msg) {
    ALOGV("onAllocateComponent");

    CHECK(mCodec->mNode == 0);

    OMXClient client;
    if (client.connect() != OK) {
        mCodec->signalError(OMX_ErrorUndefined, NO_INIT);
        return false;
    }

    sp<IOMX> omx = client.interface();

    sp<AMessage> notify = new AMessage(kWhatOMXDied, mCodec);

    Vector<AString> matchingCodecs;

    AString mime;

    AString componentName;
    uint32_t quirks = 0;
    int32_t encoder = false;
    if (msg->findString("componentName", &componentName)) {
        sp<IMediaCodecList> list = MediaCodecList::getInstance();
        if (list != NULL && list->findCodecByName(componentName.c_str()) >= 0) {
            matchingCodecs.add(componentName);
        }
        //make sure if the component name contains qcom/qti, we add it to matchingCodecs
        //as these components are not present in media_codecs.xml and MediaCodecList won't find
        //these component by findCodecByName
        if (matchingCodecs.size() == 0 && (componentName.find("qcom", 0) > 0 ||
            componentName.find("qti", 0) > 0)) {
            matchingCodecs.add(componentName);
        }

    } else {
        CHECK(msg->findString("mime", &mime));

        if (!msg->findInt32("encoder", &encoder)) {
            encoder = false;
        }

        MediaCodecList::findMatchingCodecs(
                mime.c_str(),
                encoder, // createEncoder
                0,       // flags
                &matchingCodecs);
    }

    sp<CodecObserver> observer = new CodecObserver;
    IOMX::node_id node = 0;

    status_t err = NAME_NOT_FOUND;
    for (size_t matchIndex = 0; matchIndex < matchingCodecs.size();
            ++matchIndex) {
        componentName = matchingCodecs[matchIndex];
        quirks = MediaCodecList::getQuirksFor(componentName.c_str());

        pid_t tid = gettid();
        int prevPriority = androidGetThreadPriority(tid);
        androidSetThreadPriority(tid, ANDROID_PRIORITY_FOREGROUND);
        err = omx->allocateNode(componentName.c_str(), observer, &mCodec->mNodeBinder, &node);
        androidSetThreadPriority(tid, prevPriority);

        if (err == OK) {
            break;
        } else {
            ALOGW("Allocating component '%s' failed, try next one.", componentName.c_str());
        }

        node = 0;
    }

    if (node == 0) {
        if (!mime.empty()) {
            ALOGE("Unable to instantiate a %scoder for type '%s' with err %#x.",
                    encoder ? "en" : "de", mime.c_str(), err);
        } else {
            ALOGE("Unable to instantiate codec '%s' with err %#x.", componentName.c_str(), err);
        }

        mCodec->signalError((OMX_ERRORTYPE)err, makeNoSideEffectStatus(err));
        return false;
    }

    mDeathNotifier = new DeathNotifier(notify);
    if (mCodec->mNodeBinder == NULL ||
            mCodec->mNodeBinder->linkToDeath(mDeathNotifier) != OK) {
        // This was a local binder, if it dies so do we, we won't care
        // about any notifications in the afterlife.
        mDeathNotifier.clear();
    }

    notify = new AMessage(kWhatOMXMessageList, mCodec);
    observer->setNotificationMessage(notify);

    mCodec->mComponentName = componentName;
    mCodec->mRenderTracker.setComponentName(componentName);
    mCodec->mFlags = 0;

    if (componentName.endsWith(".secure")) {
        mCodec->mFlags |= kFlagIsSecure;
        mCodec->mFlags |= kFlagIsGrallocUsageProtected;
        mCodec->mFlags |= kFlagPushBlankBuffersToNativeWindowOnShutdown;
    }

    mCodec->mQuirks = quirks;
    mCodec->mOMX = omx;
    mCodec->mNode = node;

    {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatComponentAllocated);
        notify->setString("componentName", mCodec->mComponentName.c_str());
        notify->post();
    }

    mCodec->changeState(mCodec->mLoadedState);

    return true;
}

////////////////////////////////////////////////////////////////////////////////

ACodec::LoadedState::LoadedState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::LoadedState::stateEntered() {
    ALOGV("[%s] Now Loaded", mCodec->mComponentName.c_str());

    mCodec->mPortEOS[kPortIndexInput] =
        mCodec->mPortEOS[kPortIndexOutput] = false;

    mCodec->mInputEOSResult = OK;

    mCodec->mDequeueCounter = 0;
    mCodec->mMetadataBuffersToSubmit = 0;
    mCodec->mRepeatFrameDelayUs = -1ll;
    mCodec->mInputFormat.clear();
    mCodec->mOutputFormat.clear();
    mCodec->mBaseOutputFormat.clear();

    if (mCodec->mShutdownInProgress) {
        bool keepComponentAllocated = mCodec->mKeepComponentAllocated;

        mCodec->mShutdownInProgress = false;
        mCodec->mKeepComponentAllocated = false;

        onShutdown(keepComponentAllocated);
    }
    mCodec->mExplicitShutdown = false;

    mCodec->processDeferredMessages();
}

void ACodec::LoadedState::onShutdown(bool keepComponentAllocated) {
    if (!keepComponentAllocated) {
        (void)mCodec->mOMX->freeNode(mCodec->mNode);

        mCodec->changeState(mCodec->mUninitializedState);
    }

    if (mCodec->mExplicitShutdown) {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
        notify->post();
        mCodec->mExplicitShutdown = false;
    }
}

bool ACodec::LoadedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case ACodec::kWhatConfigureComponent:
        {
            onConfigureComponent(msg);
            handled = true;
            break;
        }

        case ACodec::kWhatCreateInputSurface:
        {
            onCreateInputSurface(msg);
            handled = true;
            break;
        }

        case ACodec::kWhatSetInputSurface:
        {
            onSetInputSurface(msg);
            handled = true;
            break;
        }

        case ACodec::kWhatStart:
        {
            onStart();
            handled = true;
            break;
        }

        case ACodec::kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));

            mCodec->mExplicitShutdown = true;
            onShutdown(keepComponentAllocated);

            handled = true;
            break;
        }

        case ACodec::kWhatFlush:
        {
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatFlushCompleted);
            notify->post();

            handled = true;
            break;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }

    return handled;
}

bool ACodec::LoadedState::onConfigureComponent(
        const sp<AMessage> &msg) {
    ALOGV("onConfigureComponent");

    CHECK(mCodec->mNode != 0);

    status_t err = OK;
    AString mime;
    if (!msg->findString("mime", &mime)) {
        err = BAD_VALUE;
    } else {
        err = mCodec->configureCodec(mime.c_str(), msg);
    }
    if (err != OK) {
        ALOGE("[%s] configureCodec returning error %d",
              mCodec->mComponentName.c_str(), err);

        int32_t encoder;
        if (!msg->findInt32("encoder", &encoder)) {
            encoder = false;
        }

        if (!encoder && !strncmp(mime.c_str(), "video/", strlen("video/"))) {
            Vector<AString> matchingCodecs;

            MediaCodecList::findMatchingCodecs(
                mime.c_str(),
                encoder, // createEncoder
                0,     // flags
                &matchingCodecs);

            err = mCodec->mOMX->freeNode(mCodec->mNode);

            if (err != OK) {
                ALOGE("Failed to freeNode");
                mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                return false;
            }

            mCodec->mNode = 0;
            AString componentName;
            sp<CodecObserver> observer = new CodecObserver;

            err = NAME_NOT_FOUND;
            for (size_t matchIndex = 0; matchIndex < matchingCodecs.size();
                    ++matchIndex) {
                componentName = matchingCodecs.itemAt(matchIndex);
                if (!strcmp(mCodec->mComponentName.c_str(), componentName.c_str())) {
                    continue;
                }

                pid_t tid = gettid();
                int prevPriority = androidGetThreadPriority(tid);
                androidSetThreadPriority(tid, ANDROID_PRIORITY_FOREGROUND);
                err = mCodec->mOMX->allocateNode(componentName.c_str(), observer,
                        &mCodec->mNodeBinder, &mCodec->mNode);
                androidSetThreadPriority(tid, prevPriority);

                if (err == OK) {
                    break;
                } else {
                    ALOGW("Allocating component '%s' failed, try next one.", componentName.c_str());
                }

                mCodec->mNode = 0;
            }

            if (mCodec->mNode == 0) {
                if (!mime.empty()) {
                    ALOGE("Unable to instantiate a %scoder for type '%s' with err %#x.",
                            encoder ? "en" : "de", mime.c_str(), err);
                } else {
                    ALOGE("Unable to instantiate codec '%s' with err %#x.", componentName.c_str(), err);
                }

                mCodec->signalError((OMX_ERRORTYPE)err, makeNoSideEffectStatus(err));
                return false;
            }

            sp<AMessage> notify = new AMessage(kWhatOMXMessageList, mCodec);
            observer->setNotificationMessage(notify);
            mCodec->mComponentName = componentName;

            err = mCodec->configureCodec(mime.c_str(), msg);
        }

        if (err != OK) {
            mCodec->signalError((OMX_ERRORTYPE)err, makeNoSideEffectStatus(err));
            return false;
        }
    }

    {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatComponentConfigured);
        notify->setString("componentName", mCodec->mComponentName.c_str());
        notify->setMessage("input-format", mCodec->mInputFormat);
        notify->setMessage("output-format", mCodec->mOutputFormat);
        notify->post();
    }

    return true;
}

status_t ACodec::LoadedState::setupInputSurface() {
    status_t err = OK;

    if (mCodec->mRepeatFrameDelayUs > 0ll) {
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode,
                kPortIndexInput,
                IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY,
                &mCodec->mRepeatFrameDelayUs,
                sizeof(mCodec->mRepeatFrameDelayUs));

        if (err != OK) {
            ALOGE("[%s] Unable to configure option to repeat previous "
                  "frames (err %d)",
                  mCodec->mComponentName.c_str(),
                  err);
            return err;
        }
    }

    if (mCodec->mMaxPtsGapUs > 0ll) {
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode,
                kPortIndexInput,
                IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP,
                &mCodec->mMaxPtsGapUs,
                sizeof(mCodec->mMaxPtsGapUs));

        if (err != OK) {
            ALOGE("[%s] Unable to configure max timestamp gap (err %d)",
                    mCodec->mComponentName.c_str(),
                    err);
            return err;
        }
    }

    if (mCodec->mMaxFps > 0) {
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode,
                kPortIndexInput,
                IOMX::INTERNAL_OPTION_MAX_FPS,
                &mCodec->mMaxFps,
                sizeof(mCodec->mMaxFps));

        if (err != OK) {
            ALOGE("[%s] Unable to configure max fps (err %d)",
                    mCodec->mComponentName.c_str(),
                    err);
            return err;
        }
    }

    if (mCodec->mTimePerCaptureUs > 0ll
            && mCodec->mTimePerFrameUs > 0ll) {
        int64_t timeLapse[2];
        timeLapse[0] = mCodec->mTimePerFrameUs;
        timeLapse[1] = mCodec->mTimePerCaptureUs;
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode,
                kPortIndexInput,
                IOMX::INTERNAL_OPTION_TIME_LAPSE,
                &timeLapse[0],
                sizeof(timeLapse));

        if (err != OK) {
            ALOGE("[%s] Unable to configure time lapse (err %d)",
                    mCodec->mComponentName.c_str(),
                    err);
            return err;
        }
    }

    if (mCodec->mCreateInputBuffersSuspended) {
        bool suspend = true;
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode,
                kPortIndexInput,
                IOMX::INTERNAL_OPTION_SUSPEND,
                &suspend,
                sizeof(suspend));

        if (err != OK) {
            ALOGE("[%s] Unable to configure option to suspend (err %d)",
                  mCodec->mComponentName.c_str(),
                  err);
            return err;
        }
    }

    uint32_t usageBits;
    if (mCodec->mOMX->getParameter(
            mCodec->mNode, (OMX_INDEXTYPE)OMX_IndexParamConsumerUsageBits,
            &usageBits, sizeof(usageBits)) == OK) {
        mCodec->mInputFormat->setInt32(
                "using-sw-read-often", !!(usageBits & GRALLOC_USAGE_SW_READ_OFTEN));
    }

    sp<ABuffer> colorAspectsBuffer;
    if (mCodec->mInputFormat->findBuffer("android._color-aspects", &colorAspectsBuffer)) {
        err = mCodec->mOMX->setInternalOption(
                mCodec->mNode, kPortIndexInput, IOMX::INTERNAL_OPTION_COLOR_ASPECTS,
                colorAspectsBuffer->base(), colorAspectsBuffer->capacity());
        if (err != OK) {
            ALOGE("[%s] Unable to configure color aspects (err %d)",
                  mCodec->mComponentName.c_str(), err);
            return err;
        }
    }
    return OK;
}

void ACodec::LoadedState::onCreateInputSurface(
        const sp<AMessage> & /* msg */) {
    ALOGV("onCreateInputSurface");

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatInputSurfaceCreated);

    android_dataspace dataSpace;
    status_t err =
        mCodec->setInitialColorAspectsForVideoEncoderSurfaceAndGetDataSpace(&dataSpace);
    notify->setMessage("input-format", mCodec->mInputFormat);
    notify->setMessage("output-format", mCodec->mOutputFormat);

    sp<IGraphicBufferProducer> bufferProducer;
    if (err == OK) {
        mCodec->mInputMetadataType = kMetadataBufferTypeANWBuffer;
        err = mCodec->mOMX->createInputSurface(
                mCodec->mNode, kPortIndexInput, dataSpace, &bufferProducer,
                &mCodec->mInputMetadataType);
#ifndef CAMCORDER_GRALLOC_SOURCE
        // framework uses ANW buffers internally instead of gralloc handles
        if (mCodec->mInputMetadataType == kMetadataBufferTypeGrallocSource) {
            mCodec->mInputMetadataType = kMetadataBufferTypeANWBuffer;
        }
#endif
    }

    if (err == OK) {
        err = setupInputSurface();
    }

    if (err == OK) {
        notify->setObject("input-surface",
                new BufferProducerWrapper(bufferProducer));
    } else {
        // Can't use mCodec->signalError() here -- MediaCodec won't forward
        // the error through because it's in the "configured" state.  We
        // send a kWhatInputSurfaceCreated with an error value instead.
        ALOGE("[%s] onCreateInputSurface returning error %d",
                mCodec->mComponentName.c_str(), err);
        notify->setInt32("err", err);
    }
    notify->post();
}

void ACodec::LoadedState::onSetInputSurface(
        const sp<AMessage> &msg) {
    ALOGV("onSetInputSurface");

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatInputSurfaceAccepted);

    sp<RefBase> obj;
    CHECK(msg->findObject("input-surface", &obj));
    sp<PersistentSurface> surface = static_cast<PersistentSurface *>(obj.get());

    android_dataspace dataSpace;
    status_t err =
        mCodec->setInitialColorAspectsForVideoEncoderSurfaceAndGetDataSpace(&dataSpace);
    notify->setMessage("input-format", mCodec->mInputFormat);
    notify->setMessage("output-format", mCodec->mOutputFormat);

    if (err == OK) {
        mCodec->mInputMetadataType = kMetadataBufferTypeANWBuffer;
        err = mCodec->mOMX->setInputSurface(
                mCodec->mNode, kPortIndexInput, surface->getBufferConsumer(),
                &mCodec->mInputMetadataType);
#ifndef CAMCORDER_GRALLOC_SOURCE
        // framework uses ANW buffers internally instead of gralloc handles
        if (mCodec->mInputMetadataType == kMetadataBufferTypeGrallocSource) {
            mCodec->mInputMetadataType = kMetadataBufferTypeANWBuffer;
        }
#endif
    }

    if (err == OK) {
        surface->getBufferConsumer()->setDefaultBufferDataSpace(dataSpace);
        err = setupInputSurface();
    }

    if (err != OK) {
        // Can't use mCodec->signalError() here -- MediaCodec won't forward
        // the error through because it's in the "configured" state.  We
        // send a kWhatInputSurfaceAccepted with an error value instead.
        ALOGE("[%s] onSetInputSurface returning error %d",
                mCodec->mComponentName.c_str(), err);
        notify->setInt32("err", err);
    }
    notify->post();
}

void ACodec::LoadedState::onStart() {
    ALOGV("onStart");

    status_t err = mCodec->mOMX->sendCommand(mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle);
    if (err != OK) {
        mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
    } else {
        mCodec->changeState(mCodec->mLoadedToIdleState);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::LoadedToIdleState::LoadedToIdleState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::LoadedToIdleState::stateEntered() {
    ALOGV("[%s] Now Loaded->Idle", mCodec->mComponentName.c_str());

    status_t err;
    if ((err = allocateBuffers()) != OK) {
        ALOGE("Failed to allocate buffers after transitioning to IDLE state "
             "(error 0x%08x)",
             err);

        mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));

        mCodec->mOMX->sendCommand(
                mCodec->mNode, OMX_CommandStateSet, OMX_StateLoaded);
        if (mCodec->allYourBuffersAreBelongToUs(kPortIndexInput)) {
            mCodec->freeBuffersOnPort(kPortIndexInput);
        }
        if (mCodec->allYourBuffersAreBelongToUs(kPortIndexOutput)) {
            mCodec->freeBuffersOnPort(kPortIndexOutput);
        }

        mCodec->changeState(mCodec->mLoadedState);
    }
}

status_t ACodec::LoadedToIdleState::allocateBuffers() {
    status_t err = mCodec->allocateBuffersOnPort(kPortIndexInput);

    if (err != OK) {
        return err;
    }

    return mCodec->allocateBuffersOnPort(kPortIndexOutput);
}

bool ACodec::LoadedToIdleState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSetParameters:
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            return true;
        }

        case kWhatSignalEndOfInputStream:
        {
            mCodec->onSignalEndOfInputStream();
            return true;
        }

        case kWhatResume:
        {
            // We'll be active soon enough.
            return true;
        }

        case kWhatFlush:
        {
            // We haven't even started yet, so we're flushed alright...
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatFlushCompleted);
            notify->post();
            return true;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }
}

bool ACodec::LoadedToIdleState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            status_t err = OK;
            if (data1 != (OMX_U32)OMX_CommandStateSet
                    || data2 != (OMX_U32)OMX_StateIdle) {
                ALOGE("Unexpected command completion in LoadedToIdleState: %s(%u) %s(%u)",
                        asString((OMX_COMMANDTYPE)data1), data1,
                        asString((OMX_STATETYPE)data2), data2);
                err = FAILED_TRANSACTION;
            }

            if (err == OK) {
                err = mCodec->mOMX->sendCommand(
                    mCodec->mNode, OMX_CommandStateSet, OMX_StateExecuting);
            }

            if (err != OK) {
                mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
            } else {
                mCodec->changeState(mCodec->mIdleToExecutingState);
            }

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::IdleToExecutingState::IdleToExecutingState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::IdleToExecutingState::stateEntered() {
    ALOGV("[%s] Now Idle->Executing", mCodec->mComponentName.c_str());
}

bool ACodec::IdleToExecutingState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSetParameters:
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            return true;
        }

        case kWhatResume:
        {
            // We'll be active soon enough.
            return true;
        }

        case kWhatFlush:
        {
            // We haven't even started yet, so we're flushed alright...
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatFlushCompleted);
            notify->post();

            return true;
        }

        case kWhatSignalEndOfInputStream:
        {
            mCodec->onSignalEndOfInputStream();
            return true;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }
}

bool ACodec::IdleToExecutingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 != (OMX_U32)OMX_CommandStateSet
                    || data2 != (OMX_U32)OMX_StateExecuting) {
                ALOGE("Unexpected command completion in IdleToExecutingState: %s(%u) %s(%u)",
                        asString((OMX_COMMANDTYPE)data1), data1,
                        asString((OMX_STATETYPE)data2), data2);
                mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                return true;
            }

            mCodec->mExecutingState->resume();
            mCodec->changeState(mCodec->mExecutingState);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::ExecutingState::ExecutingState(ACodec *codec)
    : BaseState(codec),
      mActive(false) {
}

ACodec::BaseState::PortMode ACodec::ExecutingState::getPortMode(
        OMX_U32 /* portIndex */) {
    return RESUBMIT_BUFFERS;
}

void ACodec::ExecutingState::submitOutputMetaBuffers() {
    // submit as many buffers as there are input buffers with the codec
    // in case we are in port reconfiguring
    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(i);

        if (info->mStatus == BufferInfo::OWNED_BY_COMPONENT) {
            if (mCodec->submitOutputMetadataBuffer() != OK)
                break;
        }
    }

    // *** NOTE: THE FOLLOWING WORKAROUND WILL BE REMOVED ***
    mCodec->signalSubmitOutputMetadataBufferIfEOS_workaround();
}

void ACodec::ExecutingState::submitRegularOutputBuffers() {
    bool failed = false;
    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexOutput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexOutput].editItemAt(i);

        if (mCodec->mNativeWindow != NULL) {
            if (info->mStatus != BufferInfo::OWNED_BY_US
                    && info->mStatus != BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                ALOGE("buffers should be owned by us or the surface");
                failed = true;
                break;
            }

            if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                continue;
            }
        } else {
            if (info->mStatus != BufferInfo::OWNED_BY_US) {
                ALOGE("buffers should be owned by us");
                failed = true;
                break;
            }
        }

        ALOGV("[%s] calling fillBuffer %u", mCodec->mComponentName.c_str(), info->mBufferID);

        info->checkWriteFence("submitRegularOutputBuffers");
        status_t err = mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID, info->mFenceFd);
        info->mFenceFd = -1;
        if (err != OK) {
            failed = true;
            break;
        }

        info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    }

    if (failed) {
        mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
    }
}

void ACodec::ExecutingState::submitOutputBuffers() {
    submitRegularOutputBuffers();
    if (mCodec->storingMetadataInDecodedBuffers()) {
        submitOutputMetaBuffers();
    }
}

void ACodec::ExecutingState::resume() {
    if (mActive) {
        ALOGV("[%s] We're already active, no need to resume.", mCodec->mComponentName.c_str());
        return;
    }

    submitOutputBuffers();

    // Post all available input buffers
    if (mCodec->mBuffers[kPortIndexInput].size() == 0u) {
        ALOGW("[%s] we don't have any input buffers to resume", mCodec->mComponentName.c_str());
    }

    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexInput].size(); i++) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(i);
        if (info->mStatus == BufferInfo::OWNED_BY_US) {
            postFillThisBuffer(info);
        }
    }

    mActive = true;
}

void ACodec::ExecutingState::stateEntered() {
    ALOGV("[%s] Now Executing", mCodec->mComponentName.c_str());

    mCodec->mRenderTracker.clear(systemTime(CLOCK_MONOTONIC));
    mCodec->processDeferredMessages();
}

bool ACodec::ExecutingState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));

            mCodec->mShutdownInProgress = true;
            mCodec->mExplicitShutdown = true;
            mCodec->mKeepComponentAllocated = keepComponentAllocated;

            mActive = false;

            status_t err = mCodec->mOMX->sendCommand(
                    mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle);
            if (err != OK) {
                if (keepComponentAllocated) {
                    mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                }
                // TODO: do some recovery here.
            } else {
                mCodec->changeState(mCodec->mExecutingToIdleState);
            }

            handled = true;
            break;
        }

        case kWhatFlush:
        {
            ALOGV("[%s] ExecutingState flushing now "
                 "(codec owns %zu/%zu input, %zu/%zu output).",
                    mCodec->mComponentName.c_str(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexInput),
                    mCodec->mBuffers[kPortIndexInput].size(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexOutput),
                    mCodec->mBuffers[kPortIndexOutput].size());

            mActive = false;

            status_t err = mCodec->mOMX->sendCommand(mCodec->mNode, OMX_CommandFlush, OMX_ALL);
            if (err != OK) {
                mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
            } else {
                mCodec->changeState(mCodec->mFlushingState);
            }

            handled = true;
            break;
        }

        case kWhatResume:
        {
            resume();

            handled = true;
            break;
        }

        case kWhatRequestIDRFrame:
        {
            status_t err = mCodec->requestIDRFrame();
            if (err != OK) {
                ALOGW("Requesting an IDR frame failed.");
            }

            handled = true;
            break;
        }

        case kWhatSetParameters:
        {
            sp<AMessage> params;
            CHECK(msg->findMessage("params", &params));

            status_t err = mCodec->setParameters(params);

            sp<AMessage> reply;
            if (msg->findMessage("reply", &reply)) {
                reply->setInt32("err", err);
                reply->post();
            }

            handled = true;
            break;
        }

        case ACodec::kWhatSignalEndOfInputStream:
        {
            mCodec->onSignalEndOfInputStream();
            handled = true;
            break;
        }

        // *** NOTE: THE FOLLOWING WORKAROUND WILL BE REMOVED ***
        case kWhatSubmitOutputMetadataBufferIfEOS:
        {
            if (mCodec->mPortEOS[kPortIndexInput] &&
                    !mCodec->mPortEOS[kPortIndexOutput]) {
                status_t err = mCodec->submitOutputMetadataBuffer();
                if (err == OK) {
                    mCodec->signalSubmitOutputMetadataBufferIfEOS_workaround();
                }
            }
            return true;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

status_t ACodec::setParameters(const sp<AMessage> &params) {
    int32_t videoBitrate;
    if (params->findInt32("video-bitrate", &videoBitrate)) {
        OMX_VIDEO_CONFIG_BITRATETYPE configParams;
        InitOMXParams(&configParams);
        configParams.nPortIndex = kPortIndexOutput;
        configParams.nEncodeBitrate = videoBitrate;

        status_t err = mOMX->setConfig(
                mNode,
                OMX_IndexConfigVideoBitrate,
                &configParams,
                sizeof(configParams));

        if (err != OK) {
            ALOGE("setConfig(OMX_IndexConfigVideoBitrate, %d) failed w/ err %d",
                   videoBitrate, err);

            return err;
        }
    }

    int64_t timeOffsetUs;
    if (params->findInt64("time-offset-us", &timeOffsetUs)) {
        status_t err = mOMX->setInternalOption(
            mNode,
            kPortIndexInput,
            IOMX::INTERNAL_OPTION_TIME_OFFSET,
            &timeOffsetUs,
            sizeof(timeOffsetUs));

        if (err != OK) {
            ALOGE("[%s] Unable to set input buffer time offset (err %d)",
                mComponentName.c_str(),
                err);
            return err;
        }
    }

    int64_t skipFramesBeforeUs;
    if (params->findInt64("skip-frames-before", &skipFramesBeforeUs)) {
        status_t err =
            mOMX->setInternalOption(
                     mNode,
                     kPortIndexInput,
                     IOMX::INTERNAL_OPTION_START_TIME,
                     &skipFramesBeforeUs,
                     sizeof(skipFramesBeforeUs));

        if (err != OK) {
            ALOGE("Failed to set parameter 'skip-frames-before' (err %d)", err);
            return err;
        }
    }

    int32_t dropInputFrames;
    if (params->findInt32("drop-input-frames", &dropInputFrames)) {
        bool suspend = dropInputFrames != 0;

        status_t err =
            mOMX->setInternalOption(
                     mNode,
                     kPortIndexInput,
                     IOMX::INTERNAL_OPTION_SUSPEND,
                     &suspend,
                     sizeof(suspend));

        if (err != OK) {
            ALOGE("Failed to set parameter 'drop-input-frames' (err %d)", err);
            return err;
        }
    }

    int32_t dummy;
    if (params->findInt32("request-sync", &dummy)) {
        status_t err = requestIDRFrame();

        if (err != OK) {
            ALOGE("Requesting a sync frame failed w/ err %d", err);
            return err;
        }
    }

    float rate;
    if (params->findFloat("operating-rate", &rate) && rate > 0) {
        status_t err = setOperatingRate(rate, mIsVideo);
        if (err != OK) {
            ALOGE("Failed to set parameter 'operating-rate' (err %d)", err);
            return err;
        }
    }

    int32_t intraRefreshPeriod = 0;
    if (params->findInt32("intra-refresh-period", &intraRefreshPeriod)
            && intraRefreshPeriod > 0) {
        status_t err = setIntraRefreshPeriod(intraRefreshPeriod, false);
        if (err != OK) {
            ALOGI("[%s] failed setIntraRefreshPeriod. Failure is fine since this key is optional",
                    mComponentName.c_str());
            err = OK;
        }
    }

    status_t err = configureTemporalLayers(params, false /* inConfigure */, mOutputFormat);
    if (err != OK) {
        err = OK; // ignore failure
    }

    return err;
}

void ACodec::onSignalEndOfInputStream() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatSignaledInputEOS);

    status_t err = mOMX->signalEndOfInputStream(mNode);
    if (err != OK) {
        notify->setInt32("err", err);
    }
    notify->post();
}

sp<IOMXObserver> ACodec::createObserver() {
    sp<CodecObserver> observer = new CodecObserver;
    sp<AMessage> notify = new AMessage(kWhatOMXMessageList, this);
    observer->setNotificationMessage(notify);
    return observer;
}

bool ACodec::ExecutingState::onOMXFrameRendered(int64_t mediaTimeUs, nsecs_t systemNano) {
    mCodec->onFrameRendered(mediaTimeUs, systemNano);
    return true;
}

bool ACodec::ExecutingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventPortSettingsChanged:
        {
            CHECK_EQ(data1, (OMX_U32)kPortIndexOutput);

            mCodec->onOutputFormatChanged();

            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition) {
                mCodec->mMetadataBuffersToSubmit = 0;
                CHECK_EQ(mCodec->mOMX->sendCommand(
                            mCodec->mNode,
                            OMX_CommandPortDisable, kPortIndexOutput),
                         (status_t)OK);

                mCodec->freeOutputBuffersNotOwnedByComponent();

                mCodec->changeState(mCodec->mOutputPortSettingsChangedState);
            } else if (data2 != OMX_IndexConfigCommonOutputCrop
                    && data2 != OMX_IndexConfigAndroidIntraRefresh) {
                ALOGV("[%s] OMX_EventPortSettingsChanged 0x%08x",
                     mCodec->mComponentName.c_str(), data2);
            }

            return true;
        }

        case OMX_EventBufferFlag:
        {
            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::OutputPortSettingsChangedState::OutputPortSettingsChangedState(
        ACodec *codec)
    : BaseState(codec) {
}

ACodec::BaseState::PortMode ACodec::OutputPortSettingsChangedState::getPortMode(
        OMX_U32 portIndex) {
    if (portIndex == kPortIndexOutput) {
        return FREE_BUFFERS;
    }

    CHECK_EQ(portIndex, (OMX_U32)kPortIndexInput);

    return RESUBMIT_BUFFERS;
}

bool ACodec::OutputPortSettingsChangedState::onMessageReceived(
        const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));

            mCodec->mShutdownInProgress = true;
            mCodec->mExplicitShutdown = true;
            mCodec->mKeepComponentAllocated = keepComponentAllocated;

            status_t err = mCodec->mOMX->sendCommand(
                    mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle);
            if (err != OK) {
                if (keepComponentAllocated) {
                    mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                }
                // TODO: do some recovery here.
            } else {
                // This is technically not correct, but appears to be
                // the only way to free the component instance using
                // ExectingToIdleState.
                mCodec->changeState(mCodec->mExecutingToIdleState);
            }

            handled = true;
            break;
        }
        case kWhatFlush:
        case kWhatResume:
        case kWhatSetParameters:
        {
            if (msg->what() == kWhatResume) {
                ALOGV("[%s] Deferring resume", mCodec->mComponentName.c_str());
            }

            mCodec->deferMessage(msg);
            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void ACodec::OutputPortSettingsChangedState::stateEntered() {
    ALOGV("[%s] Now handling output port settings change",
         mCodec->mComponentName.c_str());
}

bool ACodec::OutputPortSettingsChangedState::onOMXFrameRendered(
        int64_t mediaTimeUs, nsecs_t systemNano) {
    mCodec->onFrameRendered(mediaTimeUs, systemNano);
    return true;
}

bool ACodec::OutputPortSettingsChangedState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 == (OMX_U32)OMX_CommandPortDisable) {
                if (data2 != (OMX_U32)kPortIndexOutput) {
                    ALOGW("ignoring EventCmdComplete CommandPortDisable for port %u", data2);
                    return false;
                }

                ALOGV("[%s] Output port now disabled.", mCodec->mComponentName.c_str());

                status_t err = OK;
                if (!mCodec->mBuffers[kPortIndexOutput].isEmpty()) {
                    ALOGE("disabled port should be empty, but has %zu buffers",
                            mCodec->mBuffers[kPortIndexOutput].size());
                    err = FAILED_TRANSACTION;
                } else {
                    mCodec->mDealer[kPortIndexOutput].clear();
                }

                if (err == OK) {
                    err = mCodec->mOMX->sendCommand(
                            mCodec->mNode, OMX_CommandPortEnable, kPortIndexOutput);
                }

                /* Clear the RenderQueue in which queued GraphicBuffers hold the
                 * actual buffer references in order to free them early.
                 */
                mCodec->mRenderTracker.clear(systemTime(CLOCK_MONOTONIC));

                if (err == OK) {
                    err = mCodec->allocateBuffersOnPort(kPortIndexOutput);
                    ALOGE_IF(err != OK, "Failed to allocate output port buffers after port "
                            "reconfiguration: (%d)", err);
                }

                if (err != OK) {
                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
                }

                return true;
            } else if (data1 == (OMX_U32)OMX_CommandPortEnable) {
                if (data2 != (OMX_U32)kPortIndexOutput) {
                    ALOGW("ignoring EventCmdComplete OMX_CommandPortEnable for port %u", data2);
                    return false;
                }

                ALOGV("[%s] Output port now reenabled.", mCodec->mComponentName.c_str());

                if (mCodec->mExecutingState->active()) {
                    mCodec->mExecutingState->submitOutputBuffers();
                }

                mCodec->changeState(mCodec->mExecutingState);

                return true;
            }

            return false;
        }

        default:
            return false;
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::ExecutingToIdleState::ExecutingToIdleState(ACodec *codec)
    : BaseState(codec),
      mComponentNowIdle(false) {
}

bool ACodec::ExecutingToIdleState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatFlush:
        {
            // Don't send me a flush request if you previously wanted me
            // to shutdown.
            ALOGW("Ignoring flush request in ExecutingToIdleState");
            break;
        }

        case kWhatShutdown:
        {
            // We're already doing that...

            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void ACodec::ExecutingToIdleState::stateEntered() {
    ALOGV("[%s] Now Executing->Idle", mCodec->mComponentName.c_str());

    mComponentNowIdle = false;
    mCodec->mLastOutputFormat.clear();
}

bool ACodec::ExecutingToIdleState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 != (OMX_U32)OMX_CommandStateSet
                    || data2 != (OMX_U32)OMX_StateIdle) {
                ALOGE("Unexpected command completion in ExecutingToIdleState: %s(%u) %s(%u)",
                        asString((OMX_COMMANDTYPE)data1), data1,
                        asString((OMX_STATETYPE)data2), data2);
                mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                return true;
            }

            mComponentNowIdle = true;

            changeStateIfWeOwnAllBuffers();

            return true;
        }

        case OMX_EventPortSettingsChanged:
        case OMX_EventBufferFlag:
        {
            // We're shutting down and don't care about this anymore.
            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

void ACodec::ExecutingToIdleState::changeStateIfWeOwnAllBuffers() {
    if (mComponentNowIdle && mCodec->allYourBuffersAreBelongToUs()) {
        status_t err = mCodec->mOMX->sendCommand(
                mCodec->mNode, OMX_CommandStateSet, OMX_StateLoaded);
        if (err == OK) {
            err = mCodec->freeBuffersOnPort(kPortIndexInput);
            status_t err2 = mCodec->freeBuffersOnPort(kPortIndexOutput);
            if (err == OK) {
                err = err2;
            }
        }

        if ((mCodec->mFlags & kFlagPushBlankBuffersToNativeWindowOnShutdown)
                && mCodec->mNativeWindow != NULL) {
            // We push enough 1x1 blank buffers to ensure that one of
            // them has made it to the display.  This allows the OMX
            // component teardown to zero out any protected buffers
            // without the risk of scanning out one of those buffers.
            pushBlankBuffersToNativeWindow(mCodec->mNativeWindow.get());
        }

        if (err != OK) {
            mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
            return;
        }

        mCodec->changeState(mCodec->mIdleToLoadedState);
    }
}

void ACodec::ExecutingToIdleState::onInputBufferFilled(
        const sp<AMessage> &msg) {
    BaseState::onInputBufferFilled(msg);

    changeStateIfWeOwnAllBuffers();
}

void ACodec::ExecutingToIdleState::onOutputBufferDrained(
        const sp<AMessage> &msg) {
    BaseState::onOutputBufferDrained(msg);

    changeStateIfWeOwnAllBuffers();
}

////////////////////////////////////////////////////////////////////////////////

ACodec::IdleToLoadedState::IdleToLoadedState(ACodec *codec)
    : BaseState(codec) {
}

bool ACodec::IdleToLoadedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            // We're already doing that...

            handled = true;
            break;
        }

        case kWhatFlush:
        {
            // Don't send me a flush request if you previously wanted me
            // to shutdown.
            ALOGE("Got flush request in IdleToLoadedState");
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void ACodec::IdleToLoadedState::stateEntered() {
    ALOGV("[%s] Now Idle->Loaded", mCodec->mComponentName.c_str());
}

bool ACodec::IdleToLoadedState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 != (OMX_U32)OMX_CommandStateSet
                    || data2 != (OMX_U32)OMX_StateLoaded) {
                ALOGE("Unexpected command completion in IdleToLoadedState: %s(%u) %s(%u)",
                        asString((OMX_COMMANDTYPE)data1), data1,
                        asString((OMX_STATETYPE)data2), data2);
                mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                return true;
            }

            mCodec->changeState(mCodec->mLoadedState);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::FlushingState::FlushingState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::FlushingState::stateEntered() {
    ALOGV("[%s] Now Flushing", mCodec->mComponentName.c_str());

    mFlushComplete[kPortIndexInput] = mFlushComplete[kPortIndexOutput] = false;
}

bool ACodec::FlushingState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            break;
        }

        case kWhatFlush:
        {
            // We're already doing this right now.
            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

bool ACodec::FlushingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    ALOGV("[%s] FlushingState onOMXEvent(%u,%d)",
            mCodec->mComponentName.c_str(), event, (OMX_S32)data1);

    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 != (OMX_U32)OMX_CommandFlush) {
                ALOGE("unexpected EventCmdComplete %s(%d) data2:%d in FlushingState",
                        asString((OMX_COMMANDTYPE)data1), data1, data2);
                mCodec->signalError(OMX_ErrorUndefined, FAILED_TRANSACTION);
                return true;
            }

            if (data2 == kPortIndexInput || data2 == kPortIndexOutput) {
                if (mFlushComplete[data2]) {
                    ALOGW("Flush already completed for %s port",
                            data2 == kPortIndexInput ? "input" : "output");
                    return true;
                }
                mFlushComplete[data2] = true;

                if (mFlushComplete[kPortIndexInput] && mFlushComplete[kPortIndexOutput]) {
                    changeStateIfWeOwnAllBuffers();
                }
            } else if (data2 == OMX_ALL) {
                if (!mFlushComplete[kPortIndexInput] || !mFlushComplete[kPortIndexOutput]) {
                    ALOGW("received flush complete event for OMX_ALL before ports have been"
                            "flushed (%d/%d)",
                            mFlushComplete[kPortIndexInput], mFlushComplete[kPortIndexOutput]);
                    return false;
                }

                changeStateIfWeOwnAllBuffers();
            } else {
                ALOGW("data2 not OMX_ALL but %u in EventCmdComplete CommandFlush", data2);
            }

            return true;
        }

        case OMX_EventPortSettingsChanged:
        {
            sp<AMessage> msg = new AMessage(kWhatOMXMessage, mCodec);
            msg->setInt32("type", omx_message::EVENT);
            msg->setInt32("node", mCodec->mNode);
            msg->setInt32("event", event);
            msg->setInt32("data1", data1);
            msg->setInt32("data2", data2);

            ALOGV("[%s] Deferring OMX_EventPortSettingsChanged",
                 mCodec->mComponentName.c_str());

            mCodec->deferMessage(msg);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }

    return true;
}

void ACodec::FlushingState::onOutputBufferDrained(const sp<AMessage> &msg) {
    BaseState::onOutputBufferDrained(msg);

    changeStateIfWeOwnAllBuffers();
}

void ACodec::FlushingState::onInputBufferFilled(const sp<AMessage> &msg) {
    BaseState::onInputBufferFilled(msg);

    changeStateIfWeOwnAllBuffers();
}

void ACodec::FlushingState::changeStateIfWeOwnAllBuffers() {
    if (mFlushComplete[kPortIndexInput]
            && mFlushComplete[kPortIndexOutput]
            && mCodec->allYourBuffersAreBelongToUs()) {
        // We now own all buffers except possibly those still queued with
        // the native window for rendering. Let's get those back as well.
        mCodec->waitUntilAllPossibleNativeWindowBuffersAreReturnedToUs();

        mCodec->mRenderTracker.clear(systemTime(CLOCK_MONOTONIC));

        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatFlushCompleted);
        notify->post();

        mCodec->mPortEOS[kPortIndexInput] =
            mCodec->mPortEOS[kPortIndexOutput] = false;

        mCodec->mInputEOSResult = OK;

        if (mCodec->mSkipCutBuffer != NULL) {
            mCodec->mSkipCutBuffer->clear();
        }

        mCodec->changeState(mCodec->mExecutingState);
    }
}

status_t ACodec::queryCapabilities(
        const AString &name, const AString &mime, bool isEncoder,
        sp<MediaCodecInfo::Capabilities> *caps) {
    (*caps).clear();
    const char *role = getComponentRole(isEncoder, mime.c_str());
    if (role == NULL) {
        return BAD_VALUE;
    }

    OMXClient client;
    status_t err = client.connect();
    if (err != OK) {
        return err;
    }

    sp<IOMX> omx = client.interface();
    sp<CodecObserver> observer = new CodecObserver;
    IOMX::node_id node = 0;

    err = omx->allocateNode(name.c_str(), observer, NULL, &node);
    if (err != OK) {
        client.disconnect();
        return err;
    }

    err = setComponentRole(omx, node, role);
    if (err != OK) {
        omx->freeNode(node);
        client.disconnect();
        return err;
    }

    sp<MediaCodecInfo::CapabilitiesBuilder> builder = new MediaCodecInfo::CapabilitiesBuilder();
    bool isVideo = mime.startsWithIgnoreCase("video/");

    if (isVideo) {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
        InitOMXParams(&param);
        param.nPortIndex = isEncoder ? kPortIndexOutput : kPortIndexInput;

        for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
            param.nProfileIndex = index;
            status_t err = omx->getParameter(
                    node, OMX_IndexParamVideoProfileLevelQuerySupported,
                    &param, sizeof(param));
            if (err != OK) {
                break;
            }
            builder->addProfileLevel(param.eProfile, param.eLevel);

            if (index == kMaxIndicesToCheck) {
                ALOGW("[%s] stopping checking profiles after %u: %x/%x",
                        name.c_str(), index,
                        param.eProfile, param.eLevel);
            }
        }

        // Color format query
        // return colors in the order reported by the OMX component
        // prefix "flexible" standard ones with the flexible equivalent
        OMX_VIDEO_PARAM_PORTFORMATTYPE portFormat;
        InitOMXParams(&portFormat);
        portFormat.nPortIndex = isEncoder ? kPortIndexInput : kPortIndexOutput;
        Vector<uint32_t> supportedColors; // shadow copy to check for duplicates
        for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
            portFormat.nIndex = index;
            status_t err = omx->getParameter(
                    node, OMX_IndexParamVideoPortFormat,
                    &portFormat, sizeof(portFormat));
            if (err != OK) {
                break;
            }

            OMX_U32 flexibleEquivalent;
            if (isFlexibleColorFormat(
                    omx, node, portFormat.eColorFormat, false /* usingNativeWindow */,
                    &flexibleEquivalent)) {
                bool marked = false;
                for (size_t i = 0; i < supportedColors.size(); ++i) {
                    if (supportedColors[i] == flexibleEquivalent) {
                        marked = true;
                        break;
                    }
                }
                if (!marked) {
                    supportedColors.push(flexibleEquivalent);
                    builder->addColorFormat(flexibleEquivalent);
                }
            }
            supportedColors.push(portFormat.eColorFormat);
            builder->addColorFormat(portFormat.eColorFormat);

            if (index == kMaxIndicesToCheck) {
                ALOGW("[%s] stopping checking formats after %u: %s(%x)",
                        name.c_str(), index,
                        asString(portFormat.eColorFormat), portFormat.eColorFormat);
            }
        }
    } else if (mime.equalsIgnoreCase(MEDIA_MIMETYPE_AUDIO_AAC)) {
        // More audio codecs if they have profiles.
        OMX_AUDIO_PARAM_ANDROID_PROFILETYPE param;
        InitOMXParams(&param);
        param.nPortIndex = isEncoder ? kPortIndexOutput : kPortIndexInput;
        for (OMX_U32 index = 0; index <= kMaxIndicesToCheck; ++index) {
            param.nProfileIndex = index;
            status_t err = omx->getParameter(
                    node, (OMX_INDEXTYPE)OMX_IndexParamAudioProfileQuerySupported,
                    &param, sizeof(param));
            if (err != OK) {
                break;
            }
            // For audio, level is ignored.
            builder->addProfileLevel(param.eProfile, 0 /* level */);

            if (index == kMaxIndicesToCheck) {
                ALOGW("[%s] stopping checking profiles after %u: %x",
                        name.c_str(), index,
                        param.eProfile);
            }
        }

        // NOTE: Without Android extensions, OMX does not provide a way to query
        // AAC profile support
        if (param.nProfileIndex == 0) {
            ALOGW("component %s doesn't support profile query.", name.c_str());
        }
    }

    if (isVideo && !isEncoder) {
        native_handle_t *sidebandHandle = NULL;
        if (omx->configureVideoTunnelMode(
                node, kPortIndexOutput, OMX_TRUE, 0, &sidebandHandle) == OK) {
            // tunneled playback includes adaptive playback
            builder->addFlags(MediaCodecInfo::Capabilities::kFlagSupportsAdaptivePlayback
                    | MediaCodecInfo::Capabilities::kFlagSupportsTunneledPlayback);
        } else if (omx->storeMetaDataInBuffers(
                node, kPortIndexOutput, OMX_TRUE) == OK ||
            omx->prepareForAdaptivePlayback(
                node, kPortIndexOutput, OMX_TRUE,
                1280 /* width */, 720 /* height */) == OK) {
            builder->addFlags(MediaCodecInfo::Capabilities::kFlagSupportsAdaptivePlayback);
        }
    }

    if (isVideo && isEncoder) {
        OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE params;
        InitOMXParams(&params);
        params.nPortIndex = kPortIndexOutput;
        // TODO: should we verify if fallback is supported?
        if (omx->getConfig(
                node, (OMX_INDEXTYPE)OMX_IndexConfigAndroidIntraRefresh,
                &params, sizeof(params)) == OK) {
            builder->addFlags(MediaCodecInfo::Capabilities::kFlagSupportsIntraRefresh);
        }
    }

    *caps = builder;
    omx->freeNode(node);
    client.disconnect();
    return OK;
}

// These are supposed be equivalent to the logic in
// "audio_channel_out_mask_from_count".
//static
status_t ACodec::getOMXChannelMapping(size_t numChannels, OMX_AUDIO_CHANNELTYPE map[]) {
    switch (numChannels) {
        case 1:
            map[0] = OMX_AUDIO_ChannelCF;
            break;
        case 2:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            break;
        case 3:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            break;
        case 4:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelLR;
            map[3] = OMX_AUDIO_ChannelRR;
            break;
        case 5:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLR;
            map[4] = OMX_AUDIO_ChannelRR;
            break;
        case 6:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            break;
        case 7:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelCS;
            break;
        case 8:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelLS;
            map[7] = OMX_AUDIO_ChannelRS;
            break;
        default:
            return -EINVAL;
    }

    return OK;
}

void ACodec::setBFrames(
        OMX_VIDEO_PARAM_MPEG4TYPE *mpeg4type) {
    //ignore non QC components
    if (strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
        return;
    }
    if (mpeg4type->eProfile > OMX_VIDEO_MPEG4ProfileSimple) {
        mpeg4type->nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
        mpeg4type->nPFrames = (mpeg4type->nPFrames + kNumBFramesPerPFrame) /
                (kNumBFramesPerPFrame + 1);
        mpeg4type->nBFrames = mpeg4type->nPFrames * kNumBFramesPerPFrame;
    }
    return;
}

void ACodec::setBFrames(
        OMX_VIDEO_PARAM_AVCTYPE *h264type, const int32_t iFramesInterval,
        const int32_t frameRate) {
    //ignore non QC components
    if (strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
        return;
    }
    OMX_U32 val = 0;
    if (iFramesInterval < 0) {
        val =  0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        val = 0;
    } else {
        val  = frameRate * iFramesInterval - 1;
        CHECK(val > 1);
    }

    h264type->nPFrames = val;

    if (h264type->nPFrames == 0) {
        h264type->nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }

    if (h264type->eProfile > OMX_VIDEO_AVCProfileBaseline) {
        h264type->nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
        h264type->nPFrames = (h264type->nPFrames + kNumBFramesPerPFrame) /
                (kNumBFramesPerPFrame + 1);
        h264type->nBFrames = h264type->nPFrames * kNumBFramesPerPFrame;

        //enable CABAC as default entropy mode for High/Main profiles
        h264type->bEntropyCodingCABAC = OMX_TRUE;
        h264type->nCabacInitIdc = 0;
    }
    return;
}

}  // namespace android
