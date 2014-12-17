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

#include <media/stagefright/ACodec.h>

#include <binder/MemoryDealer.h>

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>

#include <media/stagefright/BufferProducerWrapper.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NativeWindowWrapper.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/ExtendedCodec.h>
#include <media/stagefright/FFMPEGSoftCodec.h>

#include <media/hardware/HardwareAPI.h>

#include <OMX_AudioExt.h>
#include <OMX_VideoExt.h>
#include <OMX_Component.h>
#include <OMX_IndexExt.h>

#include "include/ExtendedUtils.h"

#ifdef USE_SAMSUNG_COLORFORMAT
#include <sec_format.h>
#endif

#include "include/avc_utils.h"

#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMediaDefs.h>
#include <ExtendedUtils.h>
#endif

namespace android {

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

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

struct CodecObserver : public BnOMXObserver {
    CodecObserver() {}

    void setNotificationMessage(const sp<AMessage> &msg) {
        mNotify = msg;
    }

    // from IOMXObserver
    virtual void onMessage(const omx_message &omx_msg) {
        sp<AMessage> msg = mNotify->dup();

        msg->setInt32("type", omx_msg.type);
        msg->setInt32("node", omx_msg.node);

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
                break;
            }

            default:
                TRESPASS();
                break;
        }

        msg->post();
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
    bool onOMXMessage(const sp<AMessage> &msg);

    bool onOMXEmptyBufferDone(IOMX::buffer_id bufferID);

    bool onOMXFillBufferDone(
            IOMX::buffer_id bufferID,
            size_t rangeOffset, size_t rangeLength,
            OMX_U32 flags,
            int64_t timeUs);

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
    void onStart();
    void onShutdown(bool keepComponentAllocated);

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

ACodec::ACodec()
    : mQuirks(0),
      mNode(0),
      mSentFormat(false),
      mIsEncoder(false),
      mUseMetadataOnEncoderOutput(false),
      mShutdownInProgress(false),
      mExplicitShutdown(false),
      mEncoderDelay(0),
      mEncoderPadding(0),
      mRotationDegrees(0),
      mChannelMaskPresent(false),
      mChannelMask(0),
      mDequeueCounter(0),
      mStoreMetaDataInOutputBuffers(false),
      mMetaDataBuffersToSubmit(0),
      mRepeatFrameDelayUs(-1ll),
      mMaxPtsGapUs(-1ll),
      mTimePerFrameUs(-1ll),
      mTimePerCaptureUs(-1ll),
      mCreateInputBuffersSuspended(false),
      mTunneled(false) {
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

    changeState(mUninitializedState);
}

ACodec::~ACodec() {
}

void ACodec::setNotificationMessage(const sp<AMessage> &msg) {
    mNotify = msg;
}

void ACodec::initiateSetup(const sp<AMessage> &msg) {
    msg->setWhat(kWhatSetup);
    msg->setTarget(id());
    msg->post();
}

void ACodec::signalSetParameters(const sp<AMessage> &params) {
    sp<AMessage> msg = new AMessage(kWhatSetParameters, id());
    msg->setMessage("params", params);
    msg->post();
}

void ACodec::initiateAllocateComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatAllocateComponent);
    msg->setTarget(id());
    msg->post();
}

void ACodec::initiateConfigureComponent(const sp<AMessage> &msg) {
    if (mMediaExtendedStats == NULL)
        msg->findObject(MEDIA_EXTENDED_STATS, (sp<RefBase>*)&mMediaExtendedStats);

    msg->setWhat(kWhatConfigureComponent);
    msg->setTarget(id());
    msg->post();
}

void ACodec::initiateCreateInputSurface() {
    (new AMessage(kWhatCreateInputSurface, id()))->post();
}

void ACodec::signalEndOfInputStream() {
    (new AMessage(kWhatSignalEndOfInputStream, id()))->post();
}

void ACodec::initiateStart() {
    (new AMessage(kWhatStart, id()))->post();
}

void ACodec::signalFlush() {
    ALOGV("[%s] signalFlush", mComponentName.c_str());
    (new AMessage(kWhatFlush, id()))->post();
}

void ACodec::signalResume() {
    (new AMessage(kWhatResume, id()))->post();
}

void ACodec::initiateShutdown(bool keepComponentAllocated) {
    sp<AMessage> msg = new AMessage(kWhatShutdown, id());
    msg->setInt32("keepComponentAllocated", keepComponentAllocated);
    msg->post();
    if (!keepComponentAllocated) {
        // ensure shutdown completes in 3 seconds
        (new AMessage(kWhatReleaseCodecInstance, id()))->post(3000000);
    }
}

void ACodec::signalRequestIDRFrame() {
    (new AMessage(kWhatRequestIDRFrame, id()))->post();
}

// *** NOTE: THE FOLLOWING WORKAROUND WILL BE REMOVED ***
// Some codecs may return input buffers before having them processed.
// This causes a halt if we already signaled an EOS on the input
// port.  For now keep submitting an output buffer if there was an
// EOS on the input port, but not yet on the output port.
void ACodec::signalSubmitOutputMetaDataBufferIfEOS_workaround() {
    if (mPortEOS[kPortIndexInput] && !mPortEOS[kPortIndexOutput] &&
            mMetaDataBuffersToSubmit > 0) {
        (new AMessage(kWhatSubmitOutputMetaDataBufferIfEOS, id()))->post();
    }
}

status_t ACodec::allocateBuffersOnPort(OMX_U32 portIndex) {
    ATRACE_NAME(mComponentName.c_str());

    bool isVideo = mComponentName.find("video") != -1;
    const char* portType = portIndex == kPortIndexInput ?
                                        STATS_PROFILE_ALLOCATE_INPUT(isVideo) :
                                        STATS_PROFILE_ALLOCATE_OUTPUT(isVideo);
    ExtendedStats::AutoProfile autoProfile(portType, mMediaExtendedStats);

    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    CHECK(mDealer[portIndex] == NULL);
    CHECK(mBuffers[portIndex].isEmpty());

    status_t err;
    if (mNativeWindow != NULL && portIndex == kPortIndexOutput) {
        if (mStoreMetaDataInOutputBuffers) {
            err = allocateOutputMetaDataBuffers();
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
            ALOGV("[%s] Allocating %u buffers of size %u on %s port",
                    mComponentName.c_str(),
                    def.nBufferCountActual, def.nBufferSize,
                    portIndex == kPortIndexInput ? "input" : "output");

            size_t totalSize = def.nBufferCountActual * def.nBufferSize;
            mDealer[portIndex] = new MemoryDealer(totalSize, "ACodec");

            for (OMX_U32 i = 0; i < def.nBufferCountActual; ++i) {
                sp<IMemory> mem = mDealer[portIndex]->allocate(def.nBufferSize);
                CHECK(mem.get() != NULL);

                BufferInfo info;
                info.mStatus = BufferInfo::OWNED_BY_US;

                uint32_t requiresAllocateBufferBit =
                    (portIndex == kPortIndexInput)
                        ? OMXCodec::kRequiresAllocateBufferOnInputPorts
                        : OMXCodec::kRequiresAllocateBufferOnOutputPorts;

                if ((portIndex == kPortIndexInput && (mFlags & kFlagIsSecure))
                        || mUseMetadataOnEncoderOutput) {
                    mem.clear();

                    void *ptr;
                    err = mOMX->allocateBuffer(
                            mNode, portIndex, def.nBufferSize, &info.mBufferID,
                            &ptr);

                    int32_t bufSize = mUseMetadataOnEncoderOutput ?
                            (4 + sizeof(buffer_handle_t)) : def.nBufferSize;

                    info.mData = new ABuffer(ptr, bufSize);
                } else if (mQuirks & requiresAllocateBufferBit) {
                    err = mOMX->allocateBufferWithBackup(
                            mNode, portIndex, mem, &info.mBufferID);
                } else {
                    err = mOMX->useBuffer(mNode, portIndex, mem, &info.mBufferID);
                }

                if (mem != NULL) {
                    info.mData = new ABuffer(mem->pointer(), def.nBufferSize);
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

        desc->addBuffer(info.mBufferID, info.mData);
    }

    notify->setObject("portDesc", desc);
    notify->post();

    return OK;
}

status_t ACodec::configureOutputBuffersFromNativeWindow(
        OMX_U32 *bufferCount, OMX_U32 *bufferSize,
        OMX_U32 *minUndequeuedBuffers) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

#ifdef USE_SAMSUNG_COLORFORMAT
    OMX_COLOR_FORMATTYPE eNativeColorFormat = def.format.video.eColorFormat;
    setNativeWindowColorFormat(eNativeColorFormat);

    err = native_window_set_buffers_geometry(
    mNativeWindow.get(),
    def.format.video.nFrameWidth,
    def.format.video.nFrameHeight,
    eNativeColorFormat);
#else
    err = native_window_set_buffers_geometry(
            mNativeWindow.get(),
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
            def.format.video.eColorFormat);
#endif

    if (err != 0) {
        ALOGE("native_window_set_buffers_geometry failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    if (mRotationDegrees != 0) {
        uint32_t transform = 0;
        switch (mRotationDegrees) {
            case 0: transform = 0; break;
            case 90: transform = HAL_TRANSFORM_ROT_90; break;
            case 180: transform = HAL_TRANSFORM_ROT_180; break;
            case 270: transform = HAL_TRANSFORM_ROT_270; break;
            default: transform = 0; break;
        }

        if (transform > 0) {
            err = native_window_set_buffers_transform(
                    mNativeWindow.get(), transform);
            if (err != 0) {
                ALOGE("native_window_set_buffers_transform failed: %s (%d)",
                        strerror(-err), -err);
                return err;
            }
        }
    }

    // Set up the native window.
    OMX_U32 usage = 0;
    err = mOMX->getGraphicBufferUsage(mNode, kPortIndexOutput, &usage);
    if (err != 0) {
        ALOGW("querying usage flags from OMX IL component failed: %d", err);
        // XXX: Currently this error is logged, but not fatal.
        usage = 0;
    }

    if (mFlags & (kFlagIsSecure | kFlagIsContentDrmProtected)) {
        usage |= GRALLOC_USAGE_PROTECTED;
    }

    // Make sure to check whether either Stagefright or the video decoder
    // requested protected buffers.
    if (usage & GRALLOC_USAGE_PROTECTED) {
        // Verify that the ANativeWindow sends images directly to
        // SurfaceFlinger.
        int queuesToNativeWindow = 0;
        err = mNativeWindow->query(
                mNativeWindow.get(), NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER,
                &queuesToNativeWindow);
        if (err != 0) {
            ALOGE("error authenticating native window: %d", err);
            return err;
        }
        if (queuesToNativeWindow != 1) {
            ALOGE("native window could not be authenticated");
            return PERMISSION_DENIED;
        }
    }

    err = native_window_set_usage(
            mNativeWindow.get(),
            usage | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);

    if (err != 0) {
        ALOGE("native_window_set_usage failed: %s (%d)", strerror(-err), -err);
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
            &bufferCount, &bufferSize, &minUndequeuedBuffers);
    if (err != 0)
        return err;
    mNumUndequeuedBuffers = minUndequeuedBuffers;

    ALOGV("[%s] Allocating %u buffers from a native window of size %u on "
         "output port",
         mComponentName.c_str(), bufferCount, bufferSize);

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        ANativeWindowBuffer *buf;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
        if (err != 0) {
            ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
            break;
        }

        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_US;
        info.mData = new ABuffer(NULL /* data */, bufferSize /* capacity */);
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
        status_t error = cancelBufferToNativeWindow(info);
        if (err == 0) {
            err = error;
        }
    }

    return err;
}

status_t ACodec::allocateOutputMetaDataBuffers() {
    OMX_U32 bufferCount, bufferSize, minUndequeuedBuffers;
    status_t err = configureOutputBuffersFromNativeWindow(
            &bufferCount, &bufferSize, &minUndequeuedBuffers);
    if (err != 0)
        return err;
    mNumUndequeuedBuffers = minUndequeuedBuffers;

    ALOGV("[%s] Allocating %u meta buffers on output port",
         mComponentName.c_str(), bufferCount);

    size_t totalSize = bufferCount * 8;
    mDealer[kPortIndexOutput] = new MemoryDealer(totalSize, "ACodec");

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        info.mGraphicBuffer = NULL;
        info.mDequeuedAt = mDequeueCounter;

        sp<IMemory> mem = mDealer[kPortIndexOutput]->allocate(
                sizeof(struct VideoDecoderOutputMetaData));
        CHECK(mem.get() != NULL);
        info.mData = new ABuffer(mem->pointer(), mem->size());

        // we use useBuffer for metadata regardless of quirks
        err = mOMX->useBuffer(
                mNode, kPortIndexOutput, mem, &info.mBufferID);

        mBuffers[kPortIndexOutput].push(info);

        ALOGV("[%s] allocated meta buffer with ID %u (pointer = %p)",
             mComponentName.c_str(), info.mBufferID, mem->pointer());
    }

    mMetaDataBuffersToSubmit = bufferCount - minUndequeuedBuffers;
    return err;
}

status_t ACodec::submitOutputMetaDataBuffer() {
    CHECK(mStoreMetaDataInOutputBuffers);
    if (mMetaDataBuffersToSubmit == 0)
        return OK;

    BufferInfo *info = dequeueBufferFromNativeWindow();
    if (info == NULL)
        return ERROR_IO;

    ALOGV("[%s] submitting output meta buffer ID %u for graphic buffer %p",
          mComponentName.c_str(), info->mBufferID, info->mGraphicBuffer.get());

    --mMetaDataBuffersToSubmit;
    CHECK_EQ(mOMX->fillBuffer(mNode, info->mBufferID),
             (status_t)OK);

    info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    return OK;
}

#ifdef USE_SAMSUNG_COLORFORMAT
void ACodec::setNativeWindowColorFormat(OMX_COLOR_FORMATTYPE &eNativeColorFormat)
{
    // In case of Samsung decoders, we set proper native color format for the Native Window
    if (!strcasecmp(mComponentName.c_str(), "OMX.SEC.AVC.Decoder")
        || !strcasecmp(mComponentName.c_str(), "OMX.SEC.FP.AVC.Decoder")
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

    int err = mNativeWindow->cancelBuffer(
        mNativeWindow.get(), info->mGraphicBuffer.get(), -1);

    ALOGW_IF(err != 0, "[%s] can not return buffer %u to native window",
            mComponentName.c_str(), info->mBufferID);

    info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;

    return err;
}

ACodec::BufferInfo *ACodec::dequeueBufferFromNativeWindow() {
    ANativeWindowBuffer *buf;
    int fenceFd = -1;
    CHECK(mNativeWindow.get() != NULL);

    if (mTunneled) {
        ALOGW("dequeueBufferFromNativeWindow() should not be called in tunnel"
              " video playback mode mode!");
        return NULL;
    }

    if (native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf) != 0) {
        ALOGE("dequeueBuffer failed.");
        return NULL;
    }

    BufferInfo *oldest = NULL;
    for (size_t i = mBuffers[kPortIndexOutput].size(); i-- > 0;) {
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);

        if (info->mGraphicBuffer != NULL &&
            info->mGraphicBuffer->handle == buf->handle) {
            CHECK_EQ((int)info->mStatus,
                     (int)BufferInfo::OWNED_BY_NATIVE_WINDOW);

            info->mStatus = BufferInfo::OWNED_BY_US;

            return info;
        }

        if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW &&
            (oldest == NULL ||
             // avoid potential issues from counter rolling over
             mDequeueCounter - info->mDequeuedAt >
                    mDequeueCounter - oldest->mDequeuedAt)) {
            oldest = info;
        }
    }

    if (oldest) {
        CHECK(mStoreMetaDataInOutputBuffers);

        // discard buffer in LRU info and replace with new buffer
        oldest->mGraphicBuffer = new GraphicBuffer(buf, false);
        oldest->mStatus = BufferInfo::OWNED_BY_US;

        mOMX->updateGraphicBufferInMeta(
                mNode, kPortIndexOutput, oldest->mGraphicBuffer,
                oldest->mBufferID);

        VideoDecoderOutputMetaData *metaData =
            reinterpret_cast<VideoDecoderOutputMetaData *>(
                    oldest->mData->base());
        CHECK_EQ(metaData->eType, kMetadataBufferTypeGrallocSource);

        ALOGV("replaced oldest buffer #%u with age %u (%p/%p stored in %p)",
                oldest - &mBuffers[kPortIndexOutput][0],
                mDequeueCounter - oldest->mDequeuedAt,
                metaData->pHandle,
                oldest->mGraphicBuffer->handle, oldest->mData->base());

        return oldest;
    }

    TRESPASS();

    return NULL;
}

status_t ACodec::freeBuffersOnPort(OMX_U32 portIndex) {
    for (size_t i = mBuffers[portIndex].size(); i-- > 0;) {
        CHECK_EQ((status_t)OK, freeBuffer(portIndex, i));
    }

    mDealer[portIndex].clear();

    return OK;
}

status_t ACodec::freeOutputBuffersNotOwnedByComponent() {
    for (size_t i = mBuffers[kPortIndexOutput].size(); i-- > 0;) {
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);

        // At this time some buffers may still be with the component
        // or being drained.
        if (info->mStatus != BufferInfo::OWNED_BY_COMPONENT &&
            info->mStatus != BufferInfo::OWNED_BY_DOWNSTREAM) {
            CHECK_EQ((status_t)OK, freeBuffer(kPortIndexOutput, i));
        }
    }

    return OK;
}

status_t ACodec::freeBuffer(OMX_U32 portIndex, size_t i) {
    BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

    CHECK(info->mStatus == BufferInfo::OWNED_BY_US
            || info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW);

    if (portIndex == kPortIndexOutput && mNativeWindow != NULL
            && info->mStatus == BufferInfo::OWNED_BY_US) {
        cancelBufferToNativeWindow(info);
    }

    CHECK_EQ(mOMX->freeBuffer(
                mNode, portIndex, info->mBufferID),
             (status_t)OK);

    mBuffers[portIndex].removeAt(i);

    return OK;
}

ACodec::BufferInfo *ACodec::findBufferByID(
        uint32_t portIndex, IOMX::buffer_id bufferID,
        ssize_t *index) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mBufferID == bufferID) {
            if (index != NULL) {
                *index = i;
            }
            return info;
        }
    }

    TRESPASS();

    return NULL;
}

status_t ACodec::setComponentRole(
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
#ifdef ENABLE_AV_ENHANCEMENTS
        { MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS,
            "audio_decoder.amrwbplus", "audio_encoder.amrwbplus" },
        { MEDIA_MIMETYPE_AUDIO_EVRC,
            "audio_decoder.evrchw", "audio_encoder.evrc" },
        { MEDIA_MIMETYPE_AUDIO_QCELP,
            "audio_decoder,qcelp13Hw", "audio_encoder.qcelp13" },
#ifdef DOLBY_UDC
        { MEDIA_MIMETYPE_AUDIO_AC3,
            "audio_decoder.ac3", NULL },
        { MEDIA_MIMETYPE_AUDIO_EAC3,
            "audio_decoder.ec3", NULL },
#endif // DOLBY_END
#endif
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
        { MEDIA_MIMETYPE_VIDEO_MPEG4_DP,
            "video_decoder.mpeg4", NULL },
        { MEDIA_MIMETYPE_VIDEO_H263,
            "video_decoder.h263", "video_encoder.h263" },
        { MEDIA_MIMETYPE_VIDEO_VP8,
            "video_decoder.vp8", "video_encoder.vp8" },
        { MEDIA_MIMETYPE_VIDEO_VP9,
            "video_decoder.vp9", "video_encoder.vp9" },
        { MEDIA_MIMETYPE_AUDIO_RAW,
            "audio_decoder.raw", "audio_encoder.raw" },
#ifdef QTI_FLAC_DECODER
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.raw", NULL },
#else
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", "audio_encoder.flac" },
#endif
        { MEDIA_MIMETYPE_AUDIO_MSGSM,
            "audio_decoder.gsm", "audio_encoder.gsm" },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
            "video_decoder.mpeg2", "video_encoder.mpeg2" },
        { MEDIA_MIMETYPE_AUDIO_AC3,
            "audio_decoder.ac3", "audio_encoder.ac3" },
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
        status_t err = BAD_VALUE;
#ifdef ENABLE_AV_ENHANCEMENTS
        if (!strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
            err = ExtendedCodec::setSupportedRole(mOMX, mNode, isEncoder, mime);
        }
#endif
        if (!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
            err = FFMPEGSoftCodec::setSupportedRole(mOMX, mNode, isEncoder, mime);
        }
        return err;
    }

    const char *role =
        isEncoder ? kMimeToRole[i].encoderRole
                  : kMimeToRole[i].decoderRole;

    if (role != NULL) {
        OMX_PARAM_COMPONENTROLETYPE roleParams;
        InitOMXParams(&roleParams);

        strncpy((char *)roleParams.cRole,
                role, OMX_MAX_STRINGNAME_SIZE - 1);

        roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

        status_t err = mOMX->setParameter(
                mNode, OMX_IndexParamStandardComponentRole,
                &roleParams, sizeof(roleParams));

        if (err != OK) {
            ALOGW("[%s] Failed to set standard component role '%s'.",
                 mComponentName.c_str(), role);

            return err;
        }
    }

    return OK;
}

status_t ACodec::configureCodec(
        const char *mime, const sp<AMessage> &msg) {
    ATRACE_NAME("configureCodec");
    int32_t encoder;
    if (!msg->findInt32("encoder", &encoder)) {
        encoder = false;
    }

    bool isVideo = mComponentName.find("video") != -1;
    ExtendedStats::AutoProfile autoProfile(
            STATS_PROFILE_CONFIGURE_CODEC(isVideo), mMediaExtendedStats);

    sp<AMessage> inputFormat = new AMessage();
    sp<AMessage> outputFormat = new AMessage();

    mIsEncoder = encoder;

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

    int32_t storeMeta;
    if (encoder
            && msg->findInt32("store-metadata-in-buffers", &storeMeta)
            && storeMeta != 0) {
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexInput, OMX_TRUE);

        if (err != OK) {
              ALOGE("[%s] storeMetaDataInBuffers (input) failed w/ err %d",
                    mComponentName.c_str(), err);

              return err;
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
    if (encoder && video) {
        OMX_BOOL enable = (OMX_BOOL) (prependSPSPPS
            && msg->findInt32("store-metadata-in-buffers-output", &storeMeta)
            && storeMeta != 0);

        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexOutput, enable);

        if (err != OK) {
            ALOGE("[%s] storeMetaDataInBuffers (output) failed w/ err %d",
                mComponentName.c_str(), err);
            mUseMetadataOnEncoderOutput = 0;
        } else {
            mUseMetadataOnEncoderOutput = enable;
        }

        if (!msg->findInt64(
                    "repeat-previous-frame-after",
                    &mRepeatFrameDelayUs)) {
            mRepeatFrameDelayUs = -1ll;
        }

        if (!msg->findInt64("max-pts-gap-to-encoder", &mMaxPtsGapUs)) {
            mMaxPtsGapUs = -1ll;
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

    sp<RefBase> obj;
    int32_t haveNativeWindow = msg->findObject("native-window", &obj) &&
        obj != NULL;
    mStoreMetaDataInOutputBuffers = false;
    if (video && !encoder) {
        inputFormat->setInt32("adaptive-playback", false);
    }
    if (!encoder && video && haveNativeWindow) {
        sp<NativeWindowWrapper> windowWrapper(
                static_cast<NativeWindowWrapper *>(obj.get()));
        sp<ANativeWindow> nativeWindow = windowWrapper->getNativeWindow();

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

            inputFormat->setInt32("adaptive-playback", true);
        } else {
            ALOGV("Configuring CPU controlled video playback.");
            mTunneled = false;

            bool bAdaptivePlaybackMode = false;
            int32_t preferAdaptive = 0;
            if (msg->findInt32("prefer-adaptive-playback", &preferAdaptive)
                    && preferAdaptive == 1) {
                ALOGI("[%s] Adaptive playback preferred", mComponentName.c_str());
            } else {
                // Always try to enable dynamic output buffers on native surface
                err = mOMX->storeMetaDataInBuffers(
                        mNode, kPortIndexOutput, OMX_TRUE);
                if (err != OK) {
                    ALOGE("[%s] storeMetaDataInBuffers failed w/ err %d",
                            mComponentName.c_str(), err);
                }
            }
            if (err != OK || preferAdaptive) {
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
                    ALOGI("[%s] prepareForAdaptivePlayback(%dx%d)",
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
                    bAdaptivePlaybackMode = (err == OK);
                }
                // if Adaptive mode was tried first and codec failed it, try dynamic mode
                if (err != OK && preferAdaptive) {
                    err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexOutput, OMX_TRUE);
                    if (err != OK) {
                        ALOGE("[%s] storeMetaDataInBuffers failed w/ err %d",
                              mComponentName.c_str(), err);
                    } else {
                        ALOGV("[%s] storeMetaDataInBuffers succeeded", mComponentName.c_str());
                        mStoreMetaDataInOutputBuffers = true;
                        inputFormat->setInt32("adaptive-playback", true);
                    }
                }
                // allow failure
                err = OK;
            } else {
                ALOGV("[%s] storeMetaDataInBuffers succeeded",
                        mComponentName.c_str());
                mStoreMetaDataInOutputBuffers = true;
                inputFormat->setInt32("adaptive-playback", true);
            }

            ALOGI("[%s] DRC Mode: %s", mComponentName.c_str(),
                    (mStoreMetaDataInOutputBuffers ? "Dynamic Buffer Mode" :
                    (bAdaptivePlaybackMode ? "Adaptive Mode" : "Port Reconfig Mode")));
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
        // enforce screen-capture protection if requested by app
        int32_t preventScreenCapture = 0;
        if (msg->findInt32("prevent-screen-capture", &preventScreenCapture)
                && preventScreenCapture == 1) {
            mFlags |= kFlagIsContentDrmProtected;
        }
    }

    if (video) {
        if (encoder) {
            err = setupVideoEncoder(mime, msg);
        } else {
            err = setupVideoDecoder(mime, msg);
#ifdef ENABLE_AV_ENHANCEMENTS
            if (err == OK) {
                const char* componentName = mComponentName.c_str();
                ExtendedCodec::configureVideoDecoder(msg, mime, mOMX, 0, mNode, componentName);
            }
#endif
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
                    numChannels);
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
                    pcmLimiterEnable);
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
            err = setupG711Codec(encoder, numChannels);
        }
    } else if (encoder && !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)) {
        int32_t numChannels, sampleRate, compressionLevel = -1;
        if (!msg->findInt32("channel-count", &numChannels)
                    || !msg->findInt32("sample-rate", &sampleRate)) {
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
            int32_t bitsPerSample = 16;
            msg->findInt32("bits-per-sample", &bitsPerSample);
            err = setupRawAudioFormat(kPortIndexInput, sampleRate, numChannels, bitsPerSample);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AC3)) {
        int32_t numChannels;
        int32_t sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            err = setupAC3Codec(encoder, numChannels, sampleRate);
        }
    } else {
        if (encoder) {
            int32_t numChannels, sampleRate;
            if (msg->findInt32("channel-count", &numChannels)
                  && msg->findInt32("sample-rate", &sampleRate)) {
                setupRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
            }
        }
#ifdef ENABLE_AV_ENHANCEMENTS
        if (!strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
            err = ExtendedCodec::setAudioFormat(
                    msg, mime, mOMX, mNode, mIsEncoder);
        }
#endif
        if(!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
            err = FFMPEGSoftCodec::setAudioFormat(
                    msg, mime, mOMX, mNode, mIsEncoder);
        }
        if (err != OK) {
            return err;
        }
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

    CHECK_EQ(getPortFormat(kPortIndexInput, inputFormat), (status_t)OK);
    CHECK_EQ(getPortFormat(kPortIndexOutput, outputFormat), (status_t)OK);
    mInputFormat = inputFormat;
    mOutputFormat = outputFormat;

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

    CHECK(def.nBufferSize >= size);

    return OK;
}

status_t ACodec::selectAudioPortFormat(
        OMX_U32 portIndex, OMX_AUDIO_CODINGTYPE desiredFormat) {
    OMX_AUDIO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);

    format.nPortIndex = portIndex;
    for (OMX_U32 index = 0;; ++index) {
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
    }

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioPortFormat, &format, sizeof(format));
}

status_t ACodec::setupAACCodec(
        bool encoder, int32_t numChannels, int32_t sampleRate,
        int32_t bitRate, int32_t aacProfile, bool isADTS, int32_t sbrMode,
        int32_t maxOutputChannelCount, const drcParams_t& drc,
        int32_t pcmLimiterEnable) {
    if (encoder && isADTS) {
        return -EINVAL;
    }

    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            sampleRate,
            numChannels);

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
        bool encoder, int32_t numChannels, int32_t sampleRate) {
    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput, sampleRate, numChannels);

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

status_t ACodec::setupG711Codec(bool encoder, int32_t numChannels) {
    CHECK(!encoder);  // XXX TODO

    return setupRawAudioFormat(
            kPortIndexInput, 8000 /* sampleRate */, numChannels);
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
        OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels, int32_t bitsPerSample) {
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
    pcmParams.eNumData = OMX_NumericalDataSigned;
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nBitPerSample = bitsPerSample;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    if (getOMXChannelMapping(numChannels, pcmParams.eChannelMapping) != OK) {
        return OMX_ErrorNone;
    }

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
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
        OMX_COLOR_FORMATTYPE colorFormat) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = portIndex;
    format.nIndex = 0;
    bool found = false;

    OMX_U32 index = 0;
    for (;;) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        // substitute back flexible color format to codec supported format
        OMX_U32 flexibleEquivalent;
        if (compressionFormat == OMX_VIDEO_CodingUnused &&
                isFlexibleColorFormat(
                        mOMX, mNode, format.eColorFormat, &flexibleEquivalent) &&
                colorFormat == flexibleEquivalent) {
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

        ++index;
    }

    if (!found) {
        return UNKNOWN_ERROR;
    }

    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));

    return err;
}

status_t ACodec::setSupportedOutputFormat() {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = kPortIndexOutput;
    format.nIndex = 0;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));
    CHECK_EQ(err, (status_t)OK);
    CHECK_EQ((int)format.eCompressionFormat, (int)OMX_VIDEO_CodingUnused);

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
};

static status_t GetVideoCodingTypeFromMime(
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

status_t ACodec::setupVideoDecoder(
        const char *mime, const sp<AMessage> &msg) {
    int32_t width, height;
    if (!msg->findInt32("width", &width)
            || !msg->findInt32("height", &height)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CODINGTYPE compressionFormat;
    status_t err = GetVideoCodingTypeFromMime(mime, &compressionFormat);

    if (err != OK) {
#ifdef ENABLE_AV_ENHANCEMENTS
        if (!strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
            err = ExtendedCodec::setVideoFormat(msg, mime, &compressionFormat);
        }
#endif
        if (!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
            err = FFMPEGSoftCodec::setVideoFormat(
                    msg, mime, mOMX, mNode, mIsEncoder, &compressionFormat);
        }
        if (err != OK) {
            return err;
        }
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
                kPortIndexOutput, OMX_VIDEO_CodingUnused, colorFormat);
        if (err != OK) {
            ALOGW("[%s] does not support color format %d",
                  mComponentName.c_str(), colorFormat);
            err = setSupportedOutputFormat();
        }
    } else {
        err = setSupportedOutputFormat();
    }

    if (err != OK) {
        return err;
    }

    err = setVideoFormatOnPort(
            kPortIndexInput, width, height, compressionFormat);

    if (err != OK) {
        return err;
    }

    err = setVideoFormatOnPort(
            kPortIndexOutput, width, height, OMX_VIDEO_CodingUnused);

    if (err != OK) {
        return err;
    }

    return OK;
}

status_t ACodec::setupVideoEncoder(const char *mime, const sp<AMessage> &msg) {
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
    video_def->eColorFormat = colorFormat;

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
#ifdef ENABLE_AV_ENHANCEMENTS
        if (!strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
            err = ExtendedCodec::setVideoFormat(msg, mime, &compressionFormat);
        }
#endif
        if (!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
            err = FFMPEGSoftCodec::setVideoFormat(
                    msg, mime, mOMX, mNode, mIsEncoder, &compressionFormat);
        }
        if (err != OK) {
            ALOGE("Not a supported video mime type: %s", mime);
            return err;
        }
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
            err = setupVPXEncoderParameters(msg);
            break;

        default:
            break;
    }

    ALOGI("[%s] setupVideoEncoder %s", mComponentName.c_str(),
            err == OK ? "succeeded" : "failed");

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

static OMX_U32 setPFramesSpacing(int32_t iFramesInterval, int32_t frameRate) {
    if (iFramesInterval < 0) {
        return 0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        return 0;
    }
    OMX_U32 ret = frameRate * iFramesInterval;
    return ret;
}

static OMX_VIDEO_CONTROLRATETYPE getBitrateMode(const sp<AMessage> &msg) {
    int32_t tmp;
    if (!msg->findInt32("bitrate-mode", &tmp)) {
        return OMX_Video_ControlRateVariable;
    }

    return static_cast<OMX_VIDEO_CONTROLRATETYPE>(tmp);
}

status_t ACodec::setupMPEG4EncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
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

    mpeg4type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
    if (mpeg4type.nPFrames == 0) {
        mpeg4type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    mpeg4type.nBFrames = 0;
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
    ExtendedUtils::setBFrames(mpeg4type, mComponentName.c_str());

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
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
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

    h263type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
    if (h263type.nPFrames == 0) {
        h263type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    h263type.nBFrames = 0;

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
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
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

    // XXX
    // Allow higher profiles to be set since the encoder seems to support
#if 0
    if (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline) {
        ALOGW("Use baseline profile instead of %d for AVC recording",
            h264type.eProfile);
        h264type.eProfile = OMX_VIDEO_AVCProfileBaseline;
    }
#endif

    if (h264type.eProfile == OMX_VIDEO_AVCProfileBaseline) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 1;
        h264type.nBFrames = 0;
        h264type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
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
    }
    ExtendedUtils::setBFrames(h264type, iFrameInterval,
            frameRate, mComponentName.c_str());

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

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::setupHEVCEncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
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

    // TODO: Need OMX structure definition for setting iFrameInterval

    err = mOMX->setParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoHevc, &hevcType, sizeof(hevcType));
    if (err != OK) {
        return err;
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::setupVPXEncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate;
    int32_t iFrameInterval = 0;
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
    msg->findInt32("i-frame-interval", &iFrameInterval);

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
    if (msg->findString("ts-schema", &tsSchema)) {
        if (tsSchema == "webrtc.vp8.1-layer") {
            pattern = OMX_VIDEO_VPXTemporalLayerPatternWebRTC;
            tsLayers = 1;
        } else if (tsSchema == "webrtc.vp8.2-layer") {
            pattern = OMX_VIDEO_VPXTemporalLayerPatternWebRTC;
            tsLayers = 2;
        } else if (tsSchema == "webrtc.vp8.3-layer") {
            pattern = OMX_VIDEO_VPXTemporalLayerPatternWebRTC;
            tsLayers = 3;
        } else {
            ALOGW("Unsupported ts-schema [%s]", tsSchema.c_str());
        }
    }

    OMX_VIDEO_PARAM_ANDROID_VP8ENCODERTYPE vp8type;
    InitOMXParams(&vp8type);
    vp8type.nPortIndex = kPortIndexOutput;
    status_t err = mOMX->getParameter(
            mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoAndroidVp8Encoder,
            &vp8type, sizeof(vp8type));

    if (err == OK) {
        if (iFrameInterval > 0) {
            vp8type.nKeyFrameInterval = setPFramesSpacing(iFrameInterval, frameRate);
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
        }
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t ACodec::verifySupportForProfileAndLevel(
        int32_t profile, int32_t level) {
    OMX_VIDEO_PARAM_PROFILELEVELTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    for (params.nProfileIndex = 0;; ++params.nProfileIndex) {
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
    }
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
    errorCorrectionType.nResynchMarkerSpacing = 0;
    errorCorrectionType.bEnableDataPartitioning = OMX_FALSE;
    errorCorrectionType.bEnableRVLC = OMX_FALSE;

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
}

status_t ACodec::setVideoFormatOnPort(
        OMX_U32 portIndex,
        int32_t width, int32_t height, OMX_VIDEO_CODINGTYPE compressionFormat) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

    if (portIndex == kPortIndexInput) {
        // XXX Need a (much) better heuristic to compute input buffer sizes.
        const size_t X = 64 * 1024;
        if (def.nBufferSize < X) {
            def.nBufferSize = X;
        }
    }

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    if (portIndex == kPortIndexInput) {
        video_def->eCompressionFormat = compressionFormat;
        video_def->eColorFormat = OMX_COLOR_FormatUnused;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    return err;
}

status_t ACodec::initNativeWindow() {
    if (mNativeWindow != NULL) {
        return mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_TRUE);
    }

    mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_FALSE);
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
        if (mStoreMetaDataInOutputBuffers && mMetaDataBuffersToSubmit > 0) {
            --mMetaDataBuffersToSubmit;
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
    bool wasEmptyBefore = mDeferredQueue.empty();
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
bool ACodec::describeDefaultColorFormat(DescribeColorFormatParams &params) {
    MediaImage &image = params.sMediaImage;
    memset(&image, 0, sizeof(image));

    image.mType = MediaImage::MEDIA_IMAGE_TYPE_UNKNOWN;
    image.mNumPlanes = 0;

    const OMX_COLOR_FORMATTYPE fmt = params.eColorFormat;
    image.mWidth = params.nFrameWidth;
    image.mHeight = params.nFrameHeight;

    // only supporting YUV420
    if (fmt != OMX_COLOR_FormatYUV420Planar &&
        fmt != OMX_COLOR_FormatYUV420PackedPlanar &&
        fmt != OMX_COLOR_FormatYUV420SemiPlanar &&
        fmt != OMX_COLOR_FormatYUV420PackedSemiPlanar) {
        ALOGW("do not know color format 0x%x = %d", fmt, fmt);
        return false;
    }

    // TEMPORARY FIX for some vendors that advertise sliceHeight as 0
    if (params.nStride != 0 && params.nSliceHeight == 0) {
        ALOGW("using sliceHeight=%u instead of what codec advertised (=0)",
                params.nFrameHeight);
        params.nSliceHeight = params.nFrameHeight;
    }

    // we need stride and slice-height to be non-zero
    if (params.nStride == 0 || params.nSliceHeight == 0) {
        ALOGW("cannot describe color format 0x%x = %d with stride=%u and sliceHeight=%u",
                fmt, fmt, params.nStride, params.nSliceHeight);
        return false;
    }

    // set-up YUV format
    image.mType = MediaImage::MEDIA_IMAGE_TYPE_YUV;
    image.mNumPlanes = 3;
    image.mBitDepth = 8;
    image.mPlane[image.Y].mOffset = 0;
    image.mPlane[image.Y].mColInc = 1;
    image.mPlane[image.Y].mRowInc = params.nStride;
    image.mPlane[image.Y].mHorizSubsampling = 1;
    image.mPlane[image.Y].mVertSubsampling = 1;

    switch (fmt) {
        case OMX_COLOR_FormatYUV420Planar: // used for YV12
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
        DescribeColorFormatParams &describeParams)
{
    OMX_INDEXTYPE describeColorFormatIndex;
    if (omx->getExtensionIndex(
            node, "OMX.google.android.index.describeColorFormat",
            &describeColorFormatIndex) != OK ||
        omx->getParameter(
            node, describeColorFormatIndex,
            &describeParams, sizeof(describeParams)) != OK) {
        return describeDefaultColorFormat(describeParams);
    }
    return describeParams.sMediaImage.mType !=
            MediaImage::MEDIA_IMAGE_TYPE_UNKNOWN;
}

// static
bool ACodec::isFlexibleColorFormat(
         const sp<IOMX> &omx, IOMX::node_id node,
         uint32_t colorFormat, OMX_U32 *flexibleEquivalent) {
    DescribeColorFormatParams describeParams;
    InitOMXParams(&describeParams);
    describeParams.eColorFormat = (OMX_COLOR_FORMATTYPE)colorFormat;
    // reasonable dummy values
    describeParams.nFrameWidth = 128;
    describeParams.nFrameHeight = 128;
    describeParams.nStride = 128;
    describeParams.nSliceHeight = 128;

    CHECK(flexibleEquivalent != NULL);

    if (!describeColorFormat(omx, node, describeParams)) {
        return false;
    }

    const MediaImage &img = describeParams.sMediaImage;
    if (img.mType == MediaImage::MEDIA_IMAGE_TYPE_YUV) {
        if (img.mNumPlanes != 3 ||
            img.mPlane[img.Y].mHorizSubsampling != 1 ||
            img.mPlane[img.Y].mVertSubsampling != 1) {
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
    // TODO: catch errors an return them instead of using CHECK
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    CHECK_EQ(mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def)),
             (status_t)OK);

    CHECK_EQ((int)def.eDir,
            (int)(portIndex == kPortIndexOutput ? OMX_DirOutput : OMX_DirInput));

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

                    DescribeColorFormatParams describeParams;
                    InitOMXParams(&describeParams);
                    describeParams.eColorFormat = videoDef->eColorFormat;
                    describeParams.nFrameWidth = videoDef->nFrameWidth;
                    describeParams.nFrameHeight = videoDef->nFrameHeight;
                    describeParams.nStride = videoDef->nStride;
                    describeParams.nSliceHeight = videoDef->nSliceHeight;

                    if (describeColorFormat(mOMX, mNode, describeParams)) {
                        notify->setBuffer(
                                "image-data",
                                ABuffer::CreateAsCopy(
                                        &describeParams.sMediaImage,
                                        sizeof(describeParams.sMediaImage)));
                    }

                    if (portIndex != kPortIndexOutput) {
                        // TODO: also get input crop
                        break;
                    }

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

                    CHECK_GE(rect.nLeft, 0);
                    CHECK_GE(rect.nTop, 0);
                    CHECK_GE(rect.nWidth, 0u);
                    CHECK_GE(rect.nHeight, 0u);
                    CHECK_LE(rect.nLeft + rect.nWidth - 1, videoDef->nFrameWidth);
                    CHECK_LE(rect.nTop + rect.nHeight - 1, videoDef->nFrameHeight);

                    notify->setRect(
                            "crop",
                            rect.nLeft,
                            rect.nTop,
                            rect.nLeft + rect.nWidth - 1,
                            rect.nTop + rect.nHeight - 1);

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
                        AString tsSchema = "none";
                        if (vp8type.eTemporalPattern
                                == OMX_VIDEO_VPXTemporalLayerPatternWebRTC) {
                            switch (vp8type.nTemporalLayerCount) {
                                case 1:
                                {
                                    tsSchema = "webrtc.vp8.1-layer";
                                    break;
                                }
                                case 2:
                                {
                                    tsSchema = "webrtc.vp8.2-layer";
                                    break;
                                }
                                case 3:
                                {
                                    tsSchema = "webrtc.vp8.3-layer";
                                    break;
                                }
                                default:
                                {
                                    break;
                                }
                            }
                        }
                        notify->setString("ts-schema", tsSchema);
                    }
                    // Fall through to set up mime.
                }

                default:
                {
                    CHECK(mIsEncoder ^ (portIndex == kPortIndexInput));
                    AString mime;
                    if (GetMimeTypeForVideoCoding(
                        videoDef->eCompressionFormat, &mime) != OK) {
                        notify->setString("mime", "application/octet-stream");
                    } else {
                        notify->setString("mime", mime.c_str());
                    }
                    break;
                }
            }

            notify->setInt32("width", videoDef->nFrameWidth);
            notify->setInt32("height", videoDef->nFrameHeight);
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

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioPcm,
                                &params, sizeof(params)),
                             (status_t)OK);

                    CHECK_GT(params.nChannels, 0);
                    CHECK(params.nChannels == 1 || params.bInterleaved);
                    CHECK_EQ((int)params.eNumData,
                             (int)OMX_NumericalDataSigned);

                    CHECK_EQ((int)params.ePCMMode,
                             (int)OMX_AUDIO_PCMModeLinear);

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_RAW);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSamplingRate);

                    CHECK(params.nBitPerSample == 16u ||
                          params.nBitPerSample == 24u ||
                          params.nBitPerSample == 32u);
                    notify->setInt32("bits-per-sample", params.nBitPerSample);

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

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioAac,
                                &params, sizeof(params)),
                             (status_t)OK);

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

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioAmr,
                                &params, sizeof(params)),
                             (status_t)OK);

                    notify->setInt32("channel-count", 1);
                    if (params.eAMRBandMode >= OMX_AUDIO_AMRBandModeWB0) {
                        notify->setString(
                                "mime", MEDIA_MIMETYPE_AUDIO_AMR_WB);

                        notify->setInt32("sample-rate", 16000);
                    } else {
                        notify->setString(
                                "mime", MEDIA_MIMETYPE_AUDIO_AMR_NB);

                        notify->setInt32("sample-rate", 8000);
                    }
                    break;
                }

                case OMX_AUDIO_CodingFLAC:
                {
                    OMX_AUDIO_PARAM_FLACTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioFlac,
                                &params, sizeof(params)),
                             (status_t)OK);

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_FLAC);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingMP3:
                {
                    OMX_AUDIO_PARAM_MP3TYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioMp3,
                                &params, sizeof(params)),
                             (status_t)OK);

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

                    CHECK_EQ(mOMX->getParameter(
                                mNode, OMX_IndexParamAudioVorbis,
                                &params, sizeof(params)),
                             (status_t)OK);

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

                    CHECK_EQ((status_t)OK, mOMX->getParameter(
                            mNode,
                            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
                            &params,
                            sizeof(params)));

                    notify->setString("mime", MEDIA_MIMETYPE_AUDIO_AC3);
                    notify->setInt32("channel-count", params.nChannels);
                    notify->setInt32("sample-rate", params.nSampleRate);
                    break;
                }

                case OMX_AUDIO_CodingAndroidOPUS:
                {
                    OMX_AUDIO_PARAM_ANDROID_OPUSTYPE params;
                    InitOMXParams(&params);
                    params.nPortIndex = portIndex;

                    CHECK_EQ((status_t)OK, mOMX->getParameter(
                            mNode,
                            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidOpus,
                            &params,
                            sizeof(params)));

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

                    CHECK_EQ((status_t)OK, mOMX->getParameter(
                            mNode,
                            (OMX_INDEXTYPE)OMX_IndexParamAudioPcm,
                            &params,
                            sizeof(params)));

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
                    break;
                }

                default:
                {
                    AString mimeType;
                    status_t err = ERROR_UNSUPPORTED;
#ifdef ENABLE_AV_ENHANCEMENTS
                    if (!strncmp(mComponentName.c_str(), "OMX.qcom.", 9)) {
                        err = ExtendedCodec::handleSupportedAudioFormats(
                            audioDef->eEncoding, &mimeType);
                    }
#endif
                    if (!strncmp(mComponentName.c_str(), "OMX.ffmpeg.", 11)) {
                        err = FFMPEGSoftCodec::handleSupportedAudioFormats(
                                audioDef->eEncoding, &mimeType);
                    }
                    if (err == OK) {
                        int channelCount = 0;
                        int sampleRate = 0;
#ifdef ENABLE_AV_ENHANCEMENTS
                        err = ExtendedCodec::getSupportedAudioFormatInfo(
                                      &mimeType,
                                      mOMX,
                                      mNode,
                                      portIndex,
                                      &channelCount,
                                      &sampleRate);
#endif
                        notify->setString("mime", mimeType.c_str());
                        notify->setInt32("channel-count", channelCount);
                        notify->setInt32("sample-rate", sampleRate);
                        break;
                    }
                    ALOGE("UNKNOWN AUDIO CODING: %d\n", audioDef->eEncoding);
                    TRESPASS();
                }
            }
            break;
        }

        default:
            TRESPASS();
    }

    return OK;
}

void ACodec::sendFormatChange(const sp<AMessage> &reply) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatOutputFormatChanged);

    CHECK_EQ(getPortFormat(kPortIndexOutput, notify), (status_t)OK);

    AString mime;
    CHECK(notify->findString("mime", &mime));

    int32_t left, top, right, bottom;
    if (mime == MEDIA_MIMETYPE_VIDEO_RAW &&
        mNativeWindow != NULL &&
        notify->findRect("crop", &left, &top, &right, &bottom)) {
        // notify renderer of the crop change
        // NOTE: native window uses extended right-bottom coordinate
        reply->setRect("crop", left, top, right + 1, bottom + 1);
    } else if (mime == MEDIA_MIMETYPE_AUDIO_RAW &&
               (mEncoderDelay || mEncoderPadding)) {
        int32_t channelCount;
        CHECK(notify->findInt32("channel-count", &channelCount));
        size_t frameSize = channelCount * sizeof(int16_t);
        if (mSkipCutBuffer != NULL) {
            size_t prevbufsize = mSkipCutBuffer->size();
            if (prevbufsize != 0) {
                ALOGW("Replacing SkipCutBuffer holding %d "
                      "bytes",
                      prevbufsize);
            }
        }
        mSkipCutBuffer = new SkipCutBuffer(
                mEncoderDelay * frameSize,
                mEncoderPadding * frameSize);
    }

    notify->post();

    mSentFormat = true;
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
    notify->setInt32("err", internalError);
    notify->setInt32("actionCode", ACTION_CODE_FATAL); // could translate from OMX error.
    notify->post();
}

status_t ACodec::pushBlankBuffersToNativeWindow() {
    status_t err = NO_ERROR;
    ANativeWindowBuffer* anb = NULL;
    int numBufs = 0;
    int minUndequeuedBufs = 0;

    // We need to reconnect to the ANativeWindow as a CPU client to ensure that
    // no frames get dropped by SurfaceFlinger assuming that these are video
    // frames.
    err = native_window_api_disconnect(mNativeWindow.get(),
            NATIVE_WINDOW_API_MEDIA);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: api_disconnect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_api_connect(mNativeWindow.get(),
            NATIVE_WINDOW_API_CPU);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: api_connect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_set_buffers_geometry(mNativeWindow.get(), 1, 1,
            HAL_PIXEL_FORMAT_RGBX_8888);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_buffers_geometry failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = native_window_set_scaling_mode(mNativeWindow.get(),
                NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank_frames: set_scaling_mode failed: %s (%d)",
              strerror(-err), -err);
        goto error;
    }

    err = native_window_set_usage(mNativeWindow.get(),
            GRALLOC_USAGE_SW_WRITE_OFTEN);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_usage failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = mNativeWindow->query(mNativeWindow.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: MIN_UNDEQUEUED_BUFFERS query "
                "failed: %s (%d)", strerror(-err), -err);
        goto error;
    }

    numBufs = minUndequeuedBufs + 1;
    err = native_window_set_buffer_count(mNativeWindow.get(), numBufs);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_buffer_count failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    // We  push numBufs + 1 buffers to ensure that we've drawn into the same
    // buffer twice.  This should guarantee that the buffer has been displayed
    // on the screen and then been replaced, so an previous video frames are
    // guaranteed NOT to be currently displayed.
    for (int i = 0; i < numBufs + 1; i++) {
        int fenceFd = -1;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &anb);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: dequeueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        sp<GraphicBuffer> buf(new GraphicBuffer(anb, false));

        // Fill the buffer with the a 1x1 checkerboard pattern ;)
        uint32_t* img = NULL;
        err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&img));
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: lock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        *img = 0;

        err = buf->unlock();
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: unlock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        err = mNativeWindow->queueBuffer(mNativeWindow.get(),
                buf->getNativeBuffer(), -1);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: queueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        anb = NULL;
    }

error:

    if (err != NO_ERROR) {
        // Clean up after an error.
        if (anb != NULL) {
            mNativeWindow->cancelBuffer(mNativeWindow.get(), anb, -1);
        }

        native_window_api_disconnect(mNativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        native_window_api_connect(mNativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);

        return err;
    } else {
        // Clean up after success.
        err = native_window_api_disconnect(mNativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: api_disconnect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        err = native_window_api_connect(mNativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: api_connect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        return NO_ERROR;
    }
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
        IOMX::buffer_id id, const sp<ABuffer> &buffer) {
    mBufferIDs.push_back(id);
    mBuffers.push_back(buffer);
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

        case ACodec::kWhatOMXMessage:
        {
            return onOMXMessage(msg);
        }

        case ACodec::kWhatCreateInputSurface:
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

bool ACodec::BaseState::onOMXMessage(const sp<AMessage> &msg) {
    int32_t type;
    CHECK(msg->findInt32("type", &type));

    // there is a possibility that this is an outstanding message for a
    // codec that we have already destroyed
    if (mCodec->mNode == NULL) {
        ALOGI("ignoring message as already freed component: %s",
                msg->debugString().c_str());
        return true;
    }

    IOMX::node_id nodeID;
    CHECK(msg->findInt32("node", (int32_t*)&nodeID));
    CHECK_EQ(nodeID, mCodec->mNode);

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
            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));

            return onOMXEmptyBufferDone(bufferID);
        }

        case omx_message::FILL_BUFFER_DONE:
        {
            IOMX::buffer_id bufferID;
            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));

            int32_t rangeOffset, rangeLength, flags;
            int64_t timeUs;

            CHECK(msg->findInt32("range_offset", &rangeOffset));
            CHECK(msg->findInt32("range_length", &rangeLength));
            CHECK(msg->findInt32("flags", &flags));
            CHECK(msg->findInt64("timestamp", &timeUs));

            return onOMXFillBufferDone(
                    bufferID,
                    (size_t)rangeOffset, (size_t)rangeLength,
                    (OMX_U32)flags,
                    timeUs);
        }

        default:
            TRESPASS();
            break;
    }
}

bool ACodec::BaseState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    if (event != OMX_EventError) {
        ALOGV("[%s] EVENT(%d, 0x%08lx, 0x%08lx)",
             mCodec->mComponentName.c_str(), event, data1, data2);

        return false;
    }

    ALOGE("[%s] ERROR(0x%08lx)", mCodec->mComponentName.c_str(), data1);

    // verify OMX component sends back an error we expect.
    OMX_ERRORTYPE omxError = (OMX_ERRORTYPE)data1;
    if (!isOMXError(omxError)) {
        ALOGW("Invalid OMX error %#x", omxError);
        omxError = OMX_ErrorUndefined;
    }
    mCodec->signalError(omxError);

    return true;
}

bool ACodec::BaseState::onOMXEmptyBufferDone(IOMX::buffer_id bufferID) {
    ALOGV("[%s] onOMXEmptyBufferDone %p",
         mCodec->mComponentName.c_str(), bufferID);

    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexInput, bufferID);

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_COMPONENT);
    info->mStatus = BufferInfo::OWNED_BY_US;

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

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);
            TRESPASS();  // Not currently used
            break;
        }
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

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, mCodec->id());
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
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_UPSTREAM);

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
                int64_t timeUs;
                CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

                OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;

                int32_t isCSD;
                if (buffer->meta()->findInt32("csd", &isCSD) && isCSD != 0) {
                    flags |= OMX_BUFFERFLAG_CODECCONFIG;
                }

                if (eos) {
                    flags |= OMX_BUFFERFLAG_EOS;
                }

                if (buffer != info->mData) {
                    ALOGV("[%s] Needs to copy input data for buffer %p. (%p != %p)",
                         mCodec->mComponentName.c_str(),
                         bufferID,
                         buffer.get(), info->mData.get());

                    CHECK_LE(buffer->size(), info->mData->capacity());
                    memcpy(info->mData->data(), buffer->data(), buffer->size());
                }

                if (flags & OMX_BUFFERFLAG_CODECCONFIG) {
                    ALOGV("[%s] calling emptyBuffer %p w/ codec specific data",
                         mCodec->mComponentName.c_str(), bufferID);
                } else if (flags & OMX_BUFFERFLAG_EOS) {
                    ALOGV("[%s] calling emptyBuffer %p w/ EOS",
                         mCodec->mComponentName.c_str(), bufferID);
                } else {
#if TRACK_BUFFER_TIMING
                    ALOGI("[%s] calling emptyBuffer %p w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, timeUs);
#else
                    ALOGV("[%s] calling emptyBuffer %p w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, timeUs);
#endif
                }

#if TRACK_BUFFER_TIMING
                ACodec::BufferStats stats;
                stats.mEmptyBufferTimeUs = ALooper::GetNowUs();
                stats.mFillBufferDoneTimeUs = -1ll;
                mCodec->mBufferStats.add(timeUs, stats);
#endif

                if (mCodec->mStoreMetaDataInOutputBuffers) {
                    // try to submit an output buffer for each input buffer
                    PortMode outputMode = getPortMode(kPortIndexOutput);

                    ALOGV("MetaDataBuffersToSubmit=%u portMode=%s",
                            mCodec->mMetaDataBuffersToSubmit,
                            (outputMode == FREE_BUFFERS ? "FREE" :
                             outputMode == KEEP_BUFFERS ? "KEEP" : "RESUBMIT"));
                    if (outputMode == RESUBMIT_BUFFERS) {
                        mCodec->submitOutputMetaDataBuffer();
                    }
                }

                if (mCodec->mMediaExtendedStats != NULL) {
                    bool isVideo = mCodec->mComponentName.find("video") != -1;
                    mCodec->mMediaExtendedStats->profileStartOnce(
                            STATS_PROFILE_FIRST_BUFFER(isVideo));
                }

                CHECK_EQ(mCodec->mOMX->emptyBuffer(
                            mCodec->mNode,
                            bufferID,
                            0,
                            buffer->size(),
                            flags,
                            timeUs),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                if (!eos) {
                    getMoreInputDataIfPossible();
                } else {
                    ALOGV("[%s] Signalled EOS on the input port",
                         mCodec->mComponentName.c_str());

                    mCodec->mPortEOS[kPortIndexInput] = true;
                    mCodec->mInputEOSResult = err;
                }
            } else if (!mCodec->mPortEOS[kPortIndexInput]) {
                if (err != ERROR_END_OF_STREAM) {
                    ALOGV("[%s] Signalling EOS on the input port "
                         "due to error %d",
                         mCodec->mComponentName.c_str(), err);
                } else {
                    ALOGV("[%s] Signalling EOS on the input port",
                         mCodec->mComponentName.c_str());
                }

                ALOGV("[%s] calling emptyBuffer %p signalling EOS",
                     mCodec->mComponentName.c_str(), bufferID);

                CHECK_EQ(mCodec->mOMX->emptyBuffer(
                            mCodec->mNode,
                            bufferID,
                            0,
                            0,
                            OMX_BUFFERFLAG_EOS,
                            0),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                mCodec->mPortEOS[kPortIndexInput] = true;
                mCodec->mInputEOSResult = err;
            }
            break;
        }

        default:
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);
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
        int64_t timeUs) {
    ALOGV("[%s] onOMXFillBufferDone %u time %" PRId64 " us, flags = 0x%08x",
         mCodec->mComponentName.c_str(), bufferID, timeUs, flags);

    ssize_t index;

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

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_COMPONENT);

    info->mDequeuedAt = ++mCodec->mDequeueCounter;
    info->mStatus = BufferInfo::OWNED_BY_US;

    PortMode mode = getPortMode(kPortIndexOutput);

    switch (mode) {
        case KEEP_BUFFERS:
            break;

        case RESUBMIT_BUFFERS:
        {
            if (rangeLength == 0 && !(flags & OMX_BUFFERFLAG_EOS)) {
                ALOGV("[%s] calling fillBuffer %u",
                     mCodec->mComponentName.c_str(), info->mBufferID);

                CHECK_EQ(mCodec->mOMX->fillBuffer(
                            mCodec->mNode, info->mBufferID),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                break;
            }

            sp<AMessage> reply =
                new AMessage(kWhatOutputBufferDrained, mCodec->id());

            if (!mCodec->mSentFormat && rangeLength > 0) {
                mCodec->sendFormatChange(reply);
            }

            if (mCodec->mUseMetadataOnEncoderOutput) {
                native_handle_t* handle =
                        *(native_handle_t**)(info->mData->data() + 4);
                info->mData->meta()->setPointer("handle", handle);
                info->mData->meta()->setInt32("rangeOffset", rangeOffset);
                info->mData->meta()->setInt32("rangeLength", rangeLength);
            } else {
                info->mData->setRange(rangeOffset, rangeLength);
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

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);

            CHECK_EQ((status_t)OK,
                     mCodec->freeBuffer(kPortIndexOutput, index));
            break;
        }
    }

    return true;
}

void ACodec::BaseState::onOutputBufferDrained(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));
    ssize_t index;
    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexOutput, bufferID, &index);
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_DOWNSTREAM);

    android_native_rect_t crop;
    if (msg->findRect("crop",
            &crop.left, &crop.top, &crop.right, &crop.bottom)) {
        CHECK_EQ(0, native_window_set_crop(
                mCodec->mNativeWindow.get(), &crop));
    }

    int32_t render;
    if (mCodec->mNativeWindow != NULL
            && msg->findInt32("render", &render) && render != 0
            && info->mData != NULL && info->mData->size() != 0) {
        ATRACE_NAME("render");
        // The client wants this buffer to be rendered.

        if (mCodec->mMediaExtendedStats != NULL) {
            bool isVideo = mCodec->mComponentName.find("video") != -1;
            mCodec->mMediaExtendedStats->profileStop(
                STATS_PROFILE_FIRST_BUFFER(isVideo));
        }

        int64_t timestampNs = 0;
        if (!msg->findInt64("timestampNs", &timestampNs)) {
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

        status_t err;
        err = native_window_set_buffers_timestamp(mCodec->mNativeWindow.get(), timestampNs);
        if (err != OK) {
            ALOGW("failed to set buffer timestamp: %d", err);
        }

        if ((err = mCodec->mNativeWindow->queueBuffer(
                    mCodec->mNativeWindow.get(),
                    info->mGraphicBuffer.get(), -1)) == OK) {
            info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        } else {
            mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
            info->mStatus = BufferInfo::OWNED_BY_US;
        }
    } else {
        if (mCodec->mNativeWindow != NULL &&
            (info->mData == NULL || info->mData->size() != 0)) {
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

                    CHECK_EQ(mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID),
                             (status_t)OK);

                    info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                }
            }
            break;
        }

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);

            CHECK_EQ((status_t)OK,
                     mCodec->freeBuffer(kPortIndexOutput, index));
            break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

ACodec::UninitializedState::UninitializedState(ACodec *codec)
    : BaseState(codec) {
}

void ACodec::UninitializedState::stateEntered() {
    ALOGV("Now uninitialized");

    if (mDeathNotifier != NULL) {
        mCodec->mOMX->asBinder()->unlinkToDeath(mDeathNotifier);
        mDeathNotifier.clear();
    }

    mCodec->mNativeWindow.clear();
    mCodec->mNode = NULL;
    mCodec->mOMX.clear();
    mCodec->mQuirks = 0;
    mCodec->mFlags = 0;
    mCodec->mUseMetadataOnEncoderOutput = 0;
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
    ATRACE_NAME("onAllocatecomponent");
    CHECK(mCodec->mNode == NULL);

    OMXClient client;
    CHECK_EQ(client.connect(), (status_t)OK);

    sp<IOMX> omx = client.interface();

    sp<AMessage> notify = new AMessage(kWhatOMXDied, mCodec->id());

    mDeathNotifier = new DeathNotifier(notify);
    if (omx->asBinder()->linkToDeath(mDeathNotifier) != OK) {
        // This was a local binder, if it dies so do we, we won't care
        // about any notifications in the afterlife.
        mDeathNotifier.clear();
    }

    Vector<OMXCodec::CodecNameAndQuirks> matchingCodecs;

    AString mime;

    AString componentName;
    uint32_t quirks = 0;
    int32_t encoder = false;
    if (msg->findString("componentName", &componentName)) {
        ssize_t index = matchingCodecs.add();
        OMXCodec::CodecNameAndQuirks *entry = &matchingCodecs.editItemAt(index);
        entry->mName = String8(componentName.c_str());

        if (!OMXCodec::findCodecQuirks(
                    componentName.c_str(), &entry->mQuirks)) {
            entry->mQuirks = 0;
        }
    } else {
        CHECK(msg->findString("mime", &mime));

        if (!msg->findInt32("encoder", &encoder)) {
            encoder = false;
        }

        ALOGV("onAllocateComponent %s %d", mime.c_str(), encoder);
#ifdef ENABLE_AV_ENHANCEMENTS
    // Call UseQCHWAACEncoder with no arguments to get the correct state since
    // MediaCodecSource does not pass the output format details when calling
    // kInit leading to msg passed not having enough details
    if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)
        && ExtendedUtils::UseQCHWAACEncoder()) {
        //use hw aac encoder
        ALOGD("use QCOM HW AAC encoder");
        OMXCodec::findMatchingCodecs(
                mime.c_str(),
                encoder, // createEncoder
                "OMX.qcom.audio.encoder.aac",  // OMX.qcom.audio.encoder.aac
                0,     // flags
                &matchingCodecs);
#ifdef QTI_FLAC_DECODER
    } else if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_FLAC) && !encoder) {
        //use google's raw decoder
        OMXCodec::findMatchingCodecs(
                MEDIA_MIMETYPE_AUDIO_RAW,
                encoder, //createEncoder
                "OMX.google.raw.decoder",
                0, //flags
                &matchingCodecs);
#endif
    } else
#endif
        OMXCodec::findMatchingCodecs(
                mime.c_str(),
                encoder, // createEncoder
                NULL,  // matchComponentName
                0,     // flags
                &matchingCodecs);
    }

    sp<CodecObserver> observer = new CodecObserver;
    IOMX::node_id node = NULL;

    for (size_t matchIndex = 0; matchIndex < matchingCodecs.size();
            ++matchIndex) {
        componentName = matchingCodecs.itemAt(matchIndex).mName.string();
        quirks = matchingCodecs.itemAt(matchIndex).mQuirks;
        // this doesn't work here as the format isn't passed in the message
        //ExtendedCodec::overrideComponentName(quirks, msg, &componentName, &mime, encoder);

        pid_t tid = androidGetTid();
        int prevPriority = androidGetThreadPriority(tid);
        androidSetThreadPriority(tid, ANDROID_PRIORITY_FOREGROUND);
        status_t err = omx->allocateNode(componentName.c_str(), observer, &node);
        androidSetThreadPriority(tid, prevPriority);

        if (err == OK) {
            break;
        } else {
            ALOGW("Allocating component '%s' failed, try next one.", componentName.c_str());
        }

        node = NULL;
    }

    if (node == NULL) {
        if (!mime.empty()) {
            ALOGE("Unable to instantiate a %scoder for type '%s'.",
                    encoder ? "en" : "de", mime.c_str());
        } else {
            ALOGE("Unable to instantiate codec '%s'.", componentName.c_str());
        }

        mCodec->signalError(OMX_ErrorComponentNotFound);
        return false;
    }

    notify = new AMessage(kWhatOMXMessage, mCodec->id());
    observer->setNotificationMessage(notify);

    mCodec->mComponentName = componentName;
    mCodec->mFlags = 0;

    if (componentName.endsWith(".secure")) {
        mCodec->mFlags |= kFlagIsSecure;
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
    mCodec->mMetaDataBuffersToSubmit = 0;
    mCodec->mRepeatFrameDelayUs = -1ll;
    mCodec->mInputFormat.clear();
    mCodec->mOutputFormat.clear();

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
        CHECK_EQ(mCodec->mOMX->freeNode(mCodec->mNode), (status_t)OK);

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

    CHECK(mCodec->mNode != NULL);

    AString mime;
    CHECK(msg->findString("mime", &mime));

    status_t err = mCodec->configureCodec(mime.c_str(), msg);

    if (err != OK) {
        ALOGE("[%s] configureCodec returning error %d",
              mCodec->mComponentName.c_str(), err);

        mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));
        return false;
    }

    sp<RefBase> obj;
    if (msg->findObject("native-window", &obj)
            && strncmp("OMX.google.", mCodec->mComponentName.c_str(), 11)
            && strncmp("OMX.ffmpeg.", mCodec->mComponentName.c_str(), 11)) {
        sp<NativeWindowWrapper> nativeWindow(
                static_cast<NativeWindowWrapper *>(obj.get()));
        CHECK(nativeWindow != NULL);
        mCodec->mNativeWindow = nativeWindow->getNativeWindow();

        native_window_set_scaling_mode(
                mCodec->mNativeWindow.get(),
                NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    }
    CHECK_EQ((status_t)OK, mCodec->initNativeWindow());

    {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatComponentConfigured);
        notify->setMessage("input-format", mCodec->mInputFormat);
        notify->setMessage("output-format", mCodec->mOutputFormat);
        notify->post();
    }

    return true;
}

void ACodec::LoadedState::onCreateInputSurface(
        const sp<AMessage> & /* msg */) {
    ALOGV("onCreateInputSurface");

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatInputSurfaceCreated);

    sp<IGraphicBufferProducer> bufferProducer;
    status_t err;

    err = mCodec->mOMX->createInputSurface(mCodec->mNode, kPortIndexInput,
            &bufferProducer);

    if (err == OK && mCodec->mRepeatFrameDelayUs > 0ll) {
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
        }
    }

    if (err == OK && mCodec->mMaxPtsGapUs > 0ll) {
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
        }
    }

    if (err == OK && mCodec->mTimePerCaptureUs > 0ll
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
        }
    }

    if (err == OK && mCodec->mCreateInputBuffersSuspended) {
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
        }
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

void ACodec::LoadedState::onStart() {
    ALOGV("onStart");

    CHECK_EQ(mCodec->mOMX->sendCommand(
                mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle),
             (status_t)OK);

    mCodec->changeState(mCodec->mLoadedToIdleState);
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
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateIdle);

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandStateSet, OMX_StateExecuting),
                     (status_t)OK);

            mCodec->changeState(mCodec->mIdleToExecutingState);

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
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateExecuting);

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
            if (mCodec->submitOutputMetaDataBuffer() != OK)
                break;
        }
    }

    // *** NOTE: THE FOLLOWING WORKAROUND WILL BE REMOVED ***
    mCodec->signalSubmitOutputMetaDataBufferIfEOS_workaround();
}

void ACodec::ExecutingState::submitRegularOutputBuffers() {
    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexOutput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexOutput].editItemAt(i);

        if (mCodec->mNativeWindow != NULL) {
            CHECK(info->mStatus == BufferInfo::OWNED_BY_US
                    || info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW);

            if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                continue;
            }
        } else {
            CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);
        }

        ALOGV("[%s] calling fillBuffer %p",
             mCodec->mComponentName.c_str(), info->mBufferID);

        CHECK_EQ(mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID),
                 (status_t)OK);

        info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    }
}

void ACodec::ExecutingState::submitOutputBuffers() {
    submitRegularOutputBuffers();
    if (mCodec->mStoreMetaDataInOutputBuffers) {
        submitOutputMetaBuffers();
    }
}

void ACodec::ExecutingState::resume() {
    if (mActive) {
        ALOGV("[%s] We're already active, no need to resume.",
             mCodec->mComponentName.c_str());

        return;
    }

    submitOutputBuffers();

    // Post all available input buffers
    CHECK_GT(mCodec->mBuffers[kPortIndexInput].size(), 0u);
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

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle),
                     (status_t)OK);

            mCodec->changeState(mCodec->mExecutingToIdleState);

            handled = true;
            break;
        }

        case kWhatFlush:
        {
            ALOGV("[%s] ExecutingState flushing now "
                 "(codec owns %d/%d input, %d/%d output).",
                    mCodec->mComponentName.c_str(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexInput),
                    mCodec->mBuffers[kPortIndexInput].size(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexOutput),
                    mCodec->mBuffers[kPortIndexOutput].size());

            mActive = false;

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandFlush, OMX_ALL),
                     (status_t)OK);

            mCodec->changeState(mCodec->mFlushingState);
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
        case kWhatSubmitOutputMetaDataBufferIfEOS:
        {
            if (mCodec->mPortEOS[kPortIndexInput] &&
                    !mCodec->mPortEOS[kPortIndexOutput]) {
                status_t err = mCodec->submitOutputMetaDataBuffer();
                if (err == OK) {
                    mCodec->signalSubmitOutputMetaDataBufferIfEOS_workaround();
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

    return OK;
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

bool ACodec::ExecutingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventPortSettingsChanged:
        {
            CHECK_EQ(data1, (OMX_U32)kPortIndexOutput);

            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition) {
                mCodec->mMetaDataBuffersToSubmit = 0;
                CHECK_EQ(mCodec->mOMX->sendCommand(
                            mCodec->mNode,
                            OMX_CommandPortDisable, kPortIndexOutput),
                         (status_t)OK);

                mCodec->freeOutputBuffersNotOwnedByComponent();

                mCodec->changeState(mCodec->mOutputPortSettingsChangedState);

                bool isVideo = mCodec->mComponentName.find("video") != -1;
                if (isVideo) {
                    CODEC_PLAYER_STATS(profileStart, STATS_PROFILE_RECONFIGURE);
                }
            } else if (data2 == OMX_IndexConfigCommonOutputCrop) {
                mCodec->mSentFormat = false;
            } else {
                ALOGV("[%s] OMX_EventPortSettingsChanged 0x%08lx",
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
        case kWhatFlush:
        case kWhatShutdown:
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

bool ACodec::OutputPortSettingsChangedState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 == (OMX_U32)OMX_CommandPortDisable) {
                CHECK_EQ(data2, (OMX_U32)kPortIndexOutput);

                ALOGV("[%s] Output port now disabled.",
                        mCodec->mComponentName.c_str());

                CHECK(mCodec->mBuffers[kPortIndexOutput].isEmpty());
                mCodec->mDealer[kPortIndexOutput].clear();

                CHECK_EQ(mCodec->mOMX->sendCommand(
                            mCodec->mNode, OMX_CommandPortEnable, kPortIndexOutput),
                         (status_t)OK);

                status_t err;
                if ((err = mCodec->allocateBuffersOnPort(
                                kPortIndexOutput)) != OK) {
                    ALOGE("Failed to allocate output port buffers after "
                         "port reconfiguration (error 0x%08x)",
                         err);

                    mCodec->signalError(OMX_ErrorUndefined, makeNoSideEffectStatus(err));

                    // This is technically not correct, but appears to be
                    // the only way to free the component instance.
                    // Controlled transitioning from excecuting->idle
                    // and idle->loaded seem impossible probably because
                    // the output port never finishes re-enabling.
                    mCodec->mShutdownInProgress = true;
                    mCodec->mKeepComponentAllocated = false;
                    mCodec->changeState(mCodec->mLoadedState);
                }

                return true;
            } else if (data1 == (OMX_U32)OMX_CommandPortEnable) {
                CHECK_EQ(data2, (OMX_U32)kPortIndexOutput);

                mCodec->mSentFormat = false;

                ALOGV("[%s] Output port now reenabled.",
                        mCodec->mComponentName.c_str());

                if (mCodec->mExecutingState->active()) {
                    mCodec->mExecutingState->submitOutputBuffers();
                }

                mCodec->changeState(mCodec->mExecutingState);

                bool isVideo = mCodec->mComponentName.find("video") != -1;
                if (isVideo) {
                    CODEC_PLAYER_STATS(profileStop, STATS_PROFILE_RECONFIGURE);
                }

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
            TRESPASS();
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
    mCodec->mSentFormat = false;
}

bool ACodec::ExecutingToIdleState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateIdle);

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
        CHECK_EQ(mCodec->mOMX->sendCommand(
                    mCodec->mNode, OMX_CommandStateSet, OMX_StateLoaded),
                 (status_t)OK);

        CHECK_EQ(mCodec->freeBuffersOnPort(kPortIndexInput), (status_t)OK);
        CHECK_EQ(mCodec->freeBuffersOnPort(kPortIndexOutput), (status_t)OK);

        if ((mCodec->mFlags & kFlagPushBlankBuffersToNativeWindowOnShutdown)
                && mCodec->mNativeWindow != NULL) {
            // We push enough 1x1 blank buffers to ensure that one of
            // them has made it to the display.  This allows the OMX
            // component teardown to zero out any protected buffers
            // without the risk of scanning out one of those buffers.
            mCodec->pushBlankBuffersToNativeWindow();
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
            TRESPASS();
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
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateLoaded);

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
    ALOGV("[%s] FlushingState onOMXEvent(%d,%ld)",
            mCodec->mComponentName.c_str(), event, data1);

    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandFlush);

            if (data2 == kPortIndexInput || data2 == kPortIndexOutput) {
                CHECK(!mFlushComplete[data2]);
                mFlushComplete[data2] = true;

                if (mFlushComplete[kPortIndexInput]
                        && mFlushComplete[kPortIndexOutput]) {
                    changeStateIfWeOwnAllBuffers();
                }
            } else {
                CHECK_EQ(data2, OMX_ALL);
                CHECK(mFlushComplete[kPortIndexInput]);
                CHECK(mFlushComplete[kPortIndexOutput]);

                changeStateIfWeOwnAllBuffers();
            }

            return true;
        }

        case OMX_EventPortSettingsChanged:
        {
            sp<AMessage> msg = new AMessage(kWhatOMXMessage, mCodec->id());
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

}  // namespace android
