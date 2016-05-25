/*
 * Copyright (C) 2009 The Android Open Source Project
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
#define LOG_TAG "OMXNodeInstance"
#include <utils/Log.h>

#include <inttypes.h>

#include "../include/OMXNodeInstance.h"
#include "OMXMaster.h"
#include "OMXUtils.h"
#include "GraphicBufferSource.h"

#include <OMX_Component.h>
#include <OMX_IndexExt.h>
#include <OMX_AsString.h>

#include <binder/IMemory.h>
#include <cutils/properties.h>
#include <gui/BufferQueue.h>
#include <HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaErrors.h>
#include <utils/misc.h>
#include <utils/NativeHandle.h>

static const OMX_U32 kPortIndexInput = 0;
static const OMX_U32 kPortIndexOutput = 1;

#define CLOGW(fmt, ...) ALOGW("[%x:%s] " fmt, mNodeID, mName, ##__VA_ARGS__)

#define CLOG_ERROR_IF(cond, fn, err, fmt, ...) \
    ALOGE_IF(cond, #fn "(%x:%s, " fmt ") ERROR: %s(%#x)", \
    mNodeID, mName, ##__VA_ARGS__, asString(err), err)
#define CLOG_ERROR(fn, err, fmt, ...) CLOG_ERROR_IF(true, fn, err, fmt, ##__VA_ARGS__)
#define CLOG_IF_ERROR(fn, err, fmt, ...) \
    CLOG_ERROR_IF((err) != OMX_ErrorNone, fn, err, fmt, ##__VA_ARGS__)

#define CLOGI_(level, fn, fmt, ...) \
    ALOGI_IF(DEBUG >= (level), #fn "(%x:%s, " fmt ")", mNodeID, mName, ##__VA_ARGS__)
#define CLOGD_(level, fn, fmt, ...) \
    ALOGD_IF(DEBUG >= (level), #fn "(%x:%s, " fmt ")", mNodeID, mName, ##__VA_ARGS__)

#define CLOG_LIFE(fn, fmt, ...)     CLOGI_(ADebug::kDebugLifeCycle,     fn, fmt, ##__VA_ARGS__)
#define CLOG_STATE(fn, fmt, ...)    CLOGI_(ADebug::kDebugState,         fn, fmt, ##__VA_ARGS__)
#define CLOG_CONFIG(fn, fmt, ...)   CLOGI_(ADebug::kDebugConfig,        fn, fmt, ##__VA_ARGS__)
#define CLOG_INTERNAL(fn, fmt, ...) CLOGD_(ADebug::kDebugInternalState, fn, fmt, ##__VA_ARGS__)

#define CLOG_DEBUG_IF(cond, fn, fmt, ...) \
    ALOGD_IF(cond, #fn "(%x, " fmt ")", mNodeID, ##__VA_ARGS__)

#define CLOG_BUFFER(fn, fmt, ...) \
    CLOG_DEBUG_IF(DEBUG >= ADebug::kDebugAll, fn, fmt, ##__VA_ARGS__)
#define CLOG_BUMPED_BUFFER(fn, fmt, ...) \
    CLOG_DEBUG_IF(DEBUG_BUMP >= ADebug::kDebugAll, fn, fmt, ##__VA_ARGS__)

/* buffer formatting */
#define BUFFER_FMT(port, fmt, ...) "%s:%u " fmt, portString(port), (port), ##__VA_ARGS__
#define NEW_BUFFER_FMT(buffer_id, port, fmt, ...) \
    BUFFER_FMT(port, fmt ") (#%zu => %#x", ##__VA_ARGS__, mActiveBuffers.size(), (buffer_id))

#define SIMPLE_BUFFER(port, size, data) BUFFER_FMT(port, "%zu@%p", (size), (data))
#define SIMPLE_NEW_BUFFER(buffer_id, port, size, data) \
    NEW_BUFFER_FMT(buffer_id, port, "%zu@%p", (size), (data))

#define EMPTY_BUFFER(addr, header, fenceFd) "%#x [%u@%p fc=%d]", \
    (addr), (header)->nAllocLen, (header)->pBuffer, (fenceFd)
#define FULL_BUFFER(addr, header, fenceFd) "%#" PRIxPTR " [%u@%p (%u..+%u) f=%x ts=%lld fc=%d]", \
    (intptr_t)(addr), (header)->nAllocLen, (header)->pBuffer, \
    (header)->nOffset, (header)->nFilledLen, (header)->nFlags, (header)->nTimeStamp, (fenceFd)

#define WITH_STATS_WRAPPER(fmt, ...) fmt " { IN=%zu/%zu OUT=%zu/%zu }", ##__VA_ARGS__, \
    mInputBuffersWithCodec.size(), mNumPortBuffers[kPortIndexInput], \
    mOutputBuffersWithCodec.size(), mNumPortBuffers[kPortIndexOutput]
// TRICKY: this is needed so formatting macros expand before substitution
#define WITH_STATS(fmt, ...) WITH_STATS_WRAPPER(fmt, ##__VA_ARGS__)

namespace android {

struct BufferMeta {
    BufferMeta(const sp<IMemory> &mem, OMX_U32 portIndex, bool is_backup = false)
        : mMem(mem),
          mIsBackup(is_backup),
          mPortIndex(portIndex) {
    }

    BufferMeta(size_t size, OMX_U32 portIndex)
        : mSize(size),
          mIsBackup(false),
          mPortIndex(portIndex) {
    }

    BufferMeta(const sp<GraphicBuffer> &graphicBuffer, OMX_U32 portIndex)
        : mGraphicBuffer(graphicBuffer),
          mIsBackup(false),
          mPortIndex(portIndex) {
    }

    void CopyFromOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mIsBackup) {
            return;
        }

        // check component returns proper range
        sp<ABuffer> codec = getBuffer(header, false /* backup */, true /* limit */);

        memcpy((OMX_U8 *)mMem->pointer() + header->nOffset, codec->data(), codec->size());
    }

    void CopyToOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mIsBackup) {
            return;
        }

        memcpy(header->pBuffer + header->nOffset,
                (const OMX_U8 *)mMem->pointer() + header->nOffset,
                header->nFilledLen);
    }

    // return either the codec or the backup buffer
    sp<ABuffer> getBuffer(const OMX_BUFFERHEADERTYPE *header, bool backup, bool limit) {
        sp<ABuffer> buf;
        if (backup && mMem != NULL) {
            buf = new ABuffer(mMem->pointer(), mMem->size());
        } else {
            buf = new ABuffer(header->pBuffer, header->nAllocLen);
        }
        if (limit) {
            if (header->nOffset + header->nFilledLen > header->nOffset
                    && header->nOffset + header->nFilledLen <= header->nAllocLen) {
                buf->setRange(header->nOffset, header->nFilledLen);
            } else {
                buf->setRange(0, 0);
            }
        }
        return buf;
    }

    void setGraphicBuffer(const sp<GraphicBuffer> &graphicBuffer) {
        mGraphicBuffer = graphicBuffer;
    }

    void setNativeHandle(const sp<NativeHandle> &nativeHandle) {
        mNativeHandle = nativeHandle;
    }

    OMX_U32 getPortIndex() {
        return mPortIndex;
    }

private:
    sp<GraphicBuffer> mGraphicBuffer;
    sp<NativeHandle> mNativeHandle;
    sp<IMemory> mMem;
    size_t mSize;
    bool mIsBackup;
    OMX_U32 mPortIndex;

    BufferMeta(const BufferMeta &);
    BufferMeta &operator=(const BufferMeta &);
};

// static
OMX_CALLBACKTYPE OMXNodeInstance::kCallbacks = {
    &OnEvent, &OnEmptyBufferDone, &OnFillBufferDone
};

static inline const char *portString(OMX_U32 portIndex) {
    switch (portIndex) {
        case kPortIndexInput:  return "Input";
        case kPortIndexOutput: return "Output";
        case ~0U:              return "All";
        default:               return "port";
    }
}

OMXNodeInstance::OMXNodeInstance(
        OMX *owner, const sp<IOMXObserver> &observer, const char *name)
    : mOwner(owner),
      mNodeID(0),
      mHandle(NULL),
      mObserver(observer),
      mDying(false),
      mBufferIDCount(0)
{
    mName = ADebug::GetDebugName(name);
    DEBUG = ADebug::GetDebugLevelFromProperty(name, "debug.stagefright.omx-debug");
    ALOGV("debug level for %s is %d", name, DEBUG);
    DEBUG_BUMP = DEBUG;
    mNumPortBuffers[0] = 0;
    mNumPortBuffers[1] = 0;
    mDebugLevelBumpPendingBuffers[0] = 0;
    mDebugLevelBumpPendingBuffers[1] = 0;
    mMetadataType[0] = kMetadataBufferTypeInvalid;
    mMetadataType[1] = kMetadataBufferTypeInvalid;
    mSecureBufferType[0] = kSecureBufferTypeUnknown;
    mSecureBufferType[1] = kSecureBufferTypeUnknown;
    mIsSecure = AString(name).endsWith(".secure");
}

OMXNodeInstance::~OMXNodeInstance() {
    free(mName);
    CHECK(mHandle == NULL);
}

void OMXNodeInstance::setHandle(OMX::node_id node_id, OMX_HANDLETYPE handle) {
    mNodeID = node_id;
    CLOG_LIFE(allocateNode, "handle=%p", handle);
    CHECK(mHandle == NULL);
    mHandle = handle;
}

sp<GraphicBufferSource> OMXNodeInstance::getGraphicBufferSource() {
    Mutex::Autolock autoLock(mGraphicBufferSourceLock);
    return mGraphicBufferSource;
}

void OMXNodeInstance::setGraphicBufferSource(
        const sp<GraphicBufferSource>& bufferSource) {
    Mutex::Autolock autoLock(mGraphicBufferSourceLock);
    CLOG_INTERNAL(setGraphicBufferSource, "%p", bufferSource.get());
    mGraphicBufferSource = bufferSource;
}

OMX *OMXNodeInstance::owner() {
    return mOwner;
}

sp<IOMXObserver> OMXNodeInstance::observer() {
    return mObserver;
}

OMX::node_id OMXNodeInstance::nodeID() {
    return mNodeID;
}

status_t OMXNodeInstance::freeNode(OMXMaster *master) {
    CLOG_LIFE(freeNode, "handle=%p", mHandle);
    static int32_t kMaxNumIterations = 10;

    // exit if we have already freed the node
    if (mHandle == NULL) {
        return OK;
    }

    // Transition the node from its current state all the way down
    // to "Loaded".
    // This ensures that all active buffers are properly freed even
    // for components that don't do this themselves on a call to
    // "FreeHandle".

    // The code below may trigger some more events to be dispatched
    // by the OMX component - we want to ignore them as our client
    // does not expect them.
    mDying = true;

    OMX_STATETYPE state;
    CHECK_EQ(OMX_GetState(mHandle, &state), OMX_ErrorNone);
    switch (state) {
        case OMX_StateExecuting:
        {
            ALOGV("forcing Executing->Idle");
            sendCommand(OMX_CommandStateSet, OMX_StateIdle);
            OMX_ERRORTYPE err;
            int32_t iteration = 0;
            while ((err = OMX_GetState(mHandle, &state)) == OMX_ErrorNone
                    && state != OMX_StateIdle
                    && state != OMX_StateInvalid) {
                if (++iteration > kMaxNumIterations) {
                    CLOGW("failed to enter Idle state (now %s(%d), aborting.",
                            asString(state), state);
                    state = OMX_StateInvalid;
                    break;
                }

                usleep(100000);
            }
            CHECK_EQ(err, OMX_ErrorNone);

            if (state == OMX_StateInvalid) {
                break;
            }

            // fall through
        }

        case OMX_StateIdle:
        {
            ALOGV("forcing Idle->Loaded");
            sendCommand(OMX_CommandStateSet, OMX_StateLoaded);

            freeActiveBuffers();

            OMX_ERRORTYPE err;
            int32_t iteration = 0;
            while ((err = OMX_GetState(mHandle, &state)) == OMX_ErrorNone
                    && state != OMX_StateLoaded
                    && state != OMX_StateInvalid) {
                if (++iteration > kMaxNumIterations) {
                    CLOGW("failed to enter Loaded state (now %s(%d), aborting.",
                            asString(state), state);
                    state = OMX_StateInvalid;
                    break;
                }

                ALOGV("waiting for Loaded state...");
                usleep(100000);
            }
            CHECK_EQ(err, OMX_ErrorNone);

            // fall through
        }

        case OMX_StateLoaded:
        case OMX_StateInvalid:
            break;

        default:
            LOG_ALWAYS_FATAL("unknown state %s(%#x).", asString(state), state);
            break;
    }

    ALOGV("[%x:%s] calling destroyComponentInstance", mNodeID, mName);
    OMX_ERRORTYPE err = master->destroyComponentInstance(
            static_cast<OMX_COMPONENTTYPE *>(mHandle));

    mHandle = NULL;
    CLOG_IF_ERROR(freeNode, err, "");
    free(mName);
    mName = NULL;

    mOwner->invalidateNodeID(mNodeID);
    mNodeID = 0;

    ALOGV("OMXNodeInstance going away.");
    delete this;

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::sendCommand(
        OMX_COMMANDTYPE cmd, OMX_S32 param) {
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && cmd == OMX_CommandStateSet) {
        if (param == OMX_StateIdle) {
            // Initiating transition from Executing -> Idle
            // ACodec is waiting for all buffers to be returned, do NOT
            // submit any more buffers to the codec.
            bufferSource->omxIdle();
        } else if (param == OMX_StateLoaded) {
            // Initiating transition from Idle/Executing -> Loaded
            // Buffers are about to be freed.
            bufferSource->omxLoaded();
            setGraphicBufferSource(NULL);
        }

        // fall through
    }

    Mutex::Autolock autoLock(mLock);

    // bump internal-state debug level for 2 input and output frames past a command
    {
        Mutex::Autolock _l(mDebugLock);
        bumpDebugLevel_l(2 /* numInputBuffers */, 2 /* numOutputBuffers */);
    }

    const char *paramString =
        cmd == OMX_CommandStateSet ? asString((OMX_STATETYPE)param) : portString(param);
    CLOG_STATE(sendCommand, "%s(%d), %s(%d)", asString(cmd), cmd, paramString, param);
    OMX_ERRORTYPE err = OMX_SendCommand(mHandle, cmd, param, NULL);
    CLOG_IF_ERROR(sendCommand, err, "%s(%d), %s(%d)", asString(cmd), cmd, paramString, param);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getParameter(
        OMX_INDEXTYPE index, void *params, size_t /* size */) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetParameter(mHandle, index, params);
    OMX_INDEXEXTTYPE extIndex = (OMX_INDEXEXTTYPE)index;
    // some errors are expected for getParameter
    if (err != OMX_ErrorNoMore) {
        CLOG_IF_ERROR(getParameter, err, "%s(%#x)", asString(extIndex), index);
    }
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::setParameter(
        OMX_INDEXTYPE index, const void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);
    OMX_INDEXEXTTYPE extIndex = (OMX_INDEXEXTTYPE)index;
    CLOG_CONFIG(setParameter, "%s(%#x), %zu@%p)", asString(extIndex), index, size, params);

    OMX_ERRORTYPE err = OMX_SetParameter(
            mHandle, index, const_cast<void *>(params));
    CLOG_IF_ERROR(setParameter, err, "%s(%#x)", asString(extIndex), index);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getConfig(
        OMX_INDEXTYPE index, void *params, size_t /* size */) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetConfig(mHandle, index, params);
    OMX_INDEXEXTTYPE extIndex = (OMX_INDEXEXTTYPE)index;
    // some errors are expected for getConfig
    if (err != OMX_ErrorNoMore) {
        CLOG_IF_ERROR(getConfig, err, "%s(%#x)", asString(extIndex), index);
    }
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::setConfig(
        OMX_INDEXTYPE index, const void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);
    OMX_INDEXEXTTYPE extIndex = (OMX_INDEXEXTTYPE)index;
    CLOG_CONFIG(setConfig, "%s(%#x), %zu@%p)", asString(extIndex), index, size, params);

    OMX_ERRORTYPE err = OMX_SetConfig(
            mHandle, index, const_cast<void *>(params));
    CLOG_IF_ERROR(setConfig, err, "%s(%#x)", asString(extIndex), index);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getState(OMX_STATETYPE* state) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetState(mHandle, state);
    CLOG_IF_ERROR(getState, err, "");
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::enableNativeBuffers(
        OMX_U32 portIndex, OMX_BOOL graphic, OMX_BOOL enable) {
    Mutex::Autolock autoLock(mLock);
    CLOG_CONFIG(enableNativeBuffers, "%s:%u%s, %d", portString(portIndex), portIndex,
                graphic ? ", graphic" : "", enable);
    OMX_STRING name = const_cast<OMX_STRING>(
            graphic ? "OMX.google.android.index.enableAndroidNativeBuffers"
                    : "OMX.google.android.index.allocateNativeHandle");

    OMX_INDEXTYPE index;
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err == OMX_ErrorNone) {
        EnableAndroidNativeBuffersParams params;
        InitOMXParams(&params);
        params.nPortIndex = portIndex;
        params.enable = enable;

        err = OMX_SetParameter(mHandle, index, &params);
        CLOG_IF_ERROR(setParameter, err, "%s(%#x): %s:%u en=%d", name, index,
                      portString(portIndex), portIndex, enable);
        if (!graphic) {
            if (err == OMX_ErrorNone) {
                mSecureBufferType[portIndex] =
                    enable ? kSecureBufferTypeNativeHandle : kSecureBufferTypeOpaque;
            } else if (mSecureBufferType[portIndex] == kSecureBufferTypeUnknown) {
                mSecureBufferType[portIndex] = kSecureBufferTypeOpaque;
            }
        }
    } else {
        CLOG_ERROR_IF(enable, getExtensionIndex, err, "%s", name);
        if (!graphic) {
            // Extension not supported, check for manual override with system property
            // This is a temporary workaround until partners support the OMX extension
            char value[PROPERTY_VALUE_MAX];
            if (property_get("media.mediadrmservice.enable", value, NULL)
                && (!strcmp("1", value) || !strcasecmp("true", value))) {
                CLOG_CONFIG(enableNativeBuffers, "system property override: using native-handles");
                mSecureBufferType[portIndex] = kSecureBufferTypeNativeHandle;
            } else if (mSecureBufferType[portIndex] == kSecureBufferTypeUnknown) {
                mSecureBufferType[portIndex] = kSecureBufferTypeOpaque;
            }
            err = OMX_ErrorNone;
        }
    }

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getGraphicBufferUsage(
        OMX_U32 portIndex, OMX_U32* usage) {
    Mutex::Autolock autoLock(mLock);

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.getAndroidNativeBufferUsage");
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(getExtensionIndex, err, "%s", name);
        return StatusFromOMXError(err);
    }

    GetAndroidNativeBufferUsageParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;

    err = OMX_GetParameter(mHandle, index, &params);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(getParameter, err, "%s(%#x): %s:%u", name, index,
                portString(portIndex), portIndex);
        return StatusFromOMXError(err);
    }

    *usage = params.nUsage;

    return OK;
}

status_t OMXNodeInstance::storeMetaDataInBuffers(
        OMX_U32 portIndex, OMX_BOOL enable, MetadataBufferType *type) {
    Mutex::Autolock autolock(mLock);
    CLOG_CONFIG(storeMetaDataInBuffers, "%s:%u en:%d", portString(portIndex), portIndex, enable);
    return storeMetaDataInBuffers_l(portIndex, enable, type);
}

status_t OMXNodeInstance::storeMetaDataInBuffers_l(
        OMX_U32 portIndex, OMX_BOOL enable, MetadataBufferType *type) {
    if (portIndex != kPortIndexInput && portIndex != kPortIndexOutput) {
        android_errorWriteLog(0x534e4554, "26324358");
        if (type != NULL) {
            *type = kMetadataBufferTypeInvalid;
        }
        return BAD_VALUE;
    }

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.storeMetaDataInBuffers");

    OMX_STRING nativeBufferName = const_cast<OMX_STRING>(
            "OMX.google.android.index.storeANWBufferInMetadata");
    MetadataBufferType negotiatedType;
    MetadataBufferType requestedType = type != NULL ? *type : kMetadataBufferTypeANWBuffer;

    StoreMetaDataInBuffersParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;
    params.bStoreMetaData = enable;

    OMX_ERRORTYPE err =
        requestedType == kMetadataBufferTypeANWBuffer
                ? OMX_GetExtensionIndex(mHandle, nativeBufferName, &index)
                : OMX_ErrorUnsupportedIndex;
    OMX_ERRORTYPE xerr = err;
    if (err == OMX_ErrorNone) {
        err = OMX_SetParameter(mHandle, index, &params);
        if (err == OMX_ErrorNone) {
            name = nativeBufferName; // set name for debugging
            negotiatedType = requestedType;
        }
    }
    if (err != OMX_ErrorNone) {
        err = OMX_GetExtensionIndex(mHandle, name, &index);
        xerr = err;
        if (err == OMX_ErrorNone) {
            negotiatedType =
                requestedType == kMetadataBufferTypeANWBuffer
                        ? kMetadataBufferTypeGrallocSource : requestedType;
            err = OMX_SetParameter(mHandle, index, &params);
        }
    }

    // don't log loud error if component does not support metadata mode on the output
    if (err != OMX_ErrorNone) {
        if (err == OMX_ErrorUnsupportedIndex && portIndex == kPortIndexOutput) {
            CLOGW("component does not support metadata mode; using fallback");
        } else if (xerr != OMX_ErrorNone) {
            CLOG_ERROR(getExtensionIndex, xerr, "%s", name);
        } else {
            CLOG_ERROR(setParameter, err, "%s(%#x): %s:%u en=%d type=%d", name, index,
                    portString(portIndex), portIndex, enable, negotiatedType);
        }
        negotiatedType = mMetadataType[portIndex];
    } else {
        if (!enable) {
            negotiatedType = kMetadataBufferTypeInvalid;
        }
        mMetadataType[portIndex] = negotiatedType;
    }
    CLOG_CONFIG(storeMetaDataInBuffers, "%s:%u %srequested %s:%d negotiated %s:%d",
            portString(portIndex), portIndex, enable ? "" : "UN",
            asString(requestedType), requestedType, asString(negotiatedType), negotiatedType);

    if (type != NULL) {
        *type = negotiatedType;
    }

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::prepareForAdaptivePlayback(
        OMX_U32 portIndex, OMX_BOOL enable, OMX_U32 maxFrameWidth,
        OMX_U32 maxFrameHeight) {
    Mutex::Autolock autolock(mLock);
    CLOG_CONFIG(prepareForAdaptivePlayback, "%s:%u en=%d max=%ux%u",
            portString(portIndex), portIndex, enable, maxFrameWidth, maxFrameHeight);

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.prepareForAdaptivePlayback");

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR_IF(enable, getExtensionIndex, err, "%s", name);
        return StatusFromOMXError(err);
    }

    PrepareForAdaptivePlaybackParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;
    params.bEnable = enable;
    params.nMaxFrameWidth = maxFrameWidth;
    params.nMaxFrameHeight = maxFrameHeight;

    err = OMX_SetParameter(mHandle, index, &params);
    CLOG_IF_ERROR(setParameter, err, "%s(%#x): %s:%u en=%d max=%ux%u", name, index,
            portString(portIndex), portIndex, enable, maxFrameWidth, maxFrameHeight);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::configureVideoTunnelMode(
        OMX_U32 portIndex, OMX_BOOL tunneled, OMX_U32 audioHwSync,
        native_handle_t **sidebandHandle) {
    Mutex::Autolock autolock(mLock);
    CLOG_CONFIG(configureVideoTunnelMode, "%s:%u tun=%d sync=%u",
            portString(portIndex), portIndex, tunneled, audioHwSync);

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.configureVideoTunnelMode");

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR_IF(tunneled, getExtensionIndex, err, "%s", name);
        return StatusFromOMXError(err);
    }

    ConfigureVideoTunnelModeParams tunnelParams;
    InitOMXParams(&tunnelParams);
    tunnelParams.nPortIndex = portIndex;
    tunnelParams.bTunneled = tunneled;
    tunnelParams.nAudioHwSync = audioHwSync;
    err = OMX_SetParameter(mHandle, index, &tunnelParams);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(setParameter, err, "%s(%#x): %s:%u tun=%d sync=%u", name, index,
                portString(portIndex), portIndex, tunneled, audioHwSync);
        return StatusFromOMXError(err);
    }

    err = OMX_GetParameter(mHandle, index, &tunnelParams);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(getParameter, err, "%s(%#x): %s:%u tun=%d sync=%u", name, index,
                portString(portIndex), portIndex, tunneled, audioHwSync);
        return StatusFromOMXError(err);
    }
    if (sidebandHandle) {
        *sidebandHandle = (native_handle_t*)tunnelParams.pSidebandWindow;
    }

    return OK;
}

status_t OMXNodeInstance::useBuffer(
        OMX_U32 portIndex, const sp<IMemory> &params,
        OMX::buffer_id *buffer, OMX_U32 allottedSize) {
    if (params == NULL || buffer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    Mutex::Autolock autoLock(mLock);
    if (allottedSize > params->size()) {
        return BAD_VALUE;
    }

    BufferMeta *buffer_meta = new BufferMeta(params, portIndex);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_UseBuffer(
            mHandle, &header, portIndex, buffer_meta,
            allottedSize, static_cast<OMX_U8 *>(params->pointer()));

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(useBuffer, err, SIMPLE_BUFFER(
                portIndex, (size_t)allottedSize, params->pointer()));

        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return StatusFromOMXError(err);
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);

    *buffer = makeBufferID(header);

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }

    CLOG_BUFFER(useBuffer, NEW_BUFFER_FMT(
            *buffer, portIndex, "%u(%zu)@%p", allottedSize, params->size(), params->pointer()));
    return OK;
}

status_t OMXNodeInstance::useGraphicBuffer2_l(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id *buffer) {
    if (graphicBuffer == NULL || buffer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    // port definition
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;
    OMX_ERRORTYPE err = OMX_GetParameter(mHandle, OMX_IndexParamPortDefinition, &def);
    if (err != OMX_ErrorNone) {
        OMX_INDEXTYPE index = OMX_IndexParamPortDefinition;
        CLOG_ERROR(getParameter, err, "%s(%#x): %s:%u",
                asString(index), index, portString(portIndex), portIndex);
        return UNKNOWN_ERROR;
    }

    BufferMeta *bufferMeta = new BufferMeta(graphicBuffer, portIndex);

    OMX_BUFFERHEADERTYPE *header = NULL;
    OMX_U8* bufferHandle = const_cast<OMX_U8*>(
            reinterpret_cast<const OMX_U8*>(graphicBuffer->handle));

    err = OMX_UseBuffer(
            mHandle,
            &header,
            portIndex,
            bufferMeta,
            def.nBufferSize,
            bufferHandle);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(useBuffer, err, BUFFER_FMT(portIndex, "%u@%p", def.nBufferSize, bufferHandle));
        delete bufferMeta;
        bufferMeta = NULL;
        *buffer = 0;
        return StatusFromOMXError(err);
    }

    CHECK_EQ(header->pBuffer, bufferHandle);
    CHECK_EQ(header->pAppPrivate, bufferMeta);

    *buffer = makeBufferID(header);

    addActiveBuffer(portIndex, *buffer);
    CLOG_BUFFER(useGraphicBuffer2, NEW_BUFFER_FMT(
            *buffer, portIndex, "%u@%p", def.nBufferSize, bufferHandle));
    return OK;
}

// XXX: This function is here for backwards compatibility.  Once the OMX
// implementations have been updated this can be removed and useGraphicBuffer2
// can be renamed to useGraphicBuffer.
status_t OMXNodeInstance::useGraphicBuffer(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id *buffer) {
    if (graphicBuffer == NULL || buffer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }
    Mutex::Autolock autoLock(mLock);

    // See if the newer version of the extension is present.
    OMX_INDEXTYPE index;
    if (OMX_GetExtensionIndex(
            mHandle,
            const_cast<OMX_STRING>("OMX.google.android.index.useAndroidNativeBuffer2"),
            &index) == OMX_ErrorNone) {
        return useGraphicBuffer2_l(portIndex, graphicBuffer, buffer);
    }

    OMX_STRING name = const_cast<OMX_STRING>(
        "OMX.google.android.index.useAndroidNativeBuffer");
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(getExtensionIndex, err, "%s", name);
        return StatusFromOMXError(err);
    }

    BufferMeta *bufferMeta = new BufferMeta(graphicBuffer, portIndex);

    OMX_BUFFERHEADERTYPE *header;

    OMX_VERSIONTYPE ver;
    ver.s.nVersionMajor = 1;
    ver.s.nVersionMinor = 0;
    ver.s.nRevision = 0;
    ver.s.nStep = 0;
    UseAndroidNativeBufferParams params = {
        sizeof(UseAndroidNativeBufferParams), ver, portIndex, bufferMeta,
        &header, graphicBuffer,
    };

    err = OMX_SetParameter(mHandle, index, &params);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(setParameter, err, "%s(%#x): %s:%u meta=%p GB=%p", name, index,
                portString(portIndex), portIndex, bufferMeta, graphicBuffer->handle);

        delete bufferMeta;
        bufferMeta = NULL;

        *buffer = 0;

        return StatusFromOMXError(err);
    }

    CHECK_EQ(header->pAppPrivate, bufferMeta);

    *buffer = makeBufferID(header);

    addActiveBuffer(portIndex, *buffer);
    CLOG_BUFFER(useGraphicBuffer, NEW_BUFFER_FMT(
            *buffer, portIndex, "GB=%p", graphicBuffer->handle));
    return OK;
}

status_t OMXNodeInstance::updateGraphicBufferInMeta_l(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id buffer, OMX_BUFFERHEADERTYPE *header, bool updateCodecBuffer) {
    // No need to check |graphicBuffer| since NULL is valid for it as below.
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    if (portIndex != kPortIndexInput && portIndex != kPortIndexOutput) {
        return BAD_VALUE;
    }

    BufferMeta *bufferMeta = (BufferMeta *)(header->pAppPrivate);
    sp<ABuffer> data = bufferMeta->getBuffer(
            header, !updateCodecBuffer /* backup */, false /* limit */);
    bufferMeta->setGraphicBuffer(graphicBuffer);
    MetadataBufferType metaType = mMetadataType[portIndex];
    // we use gralloc source only in the codec buffers
    if (metaType == kMetadataBufferTypeGrallocSource && !updateCodecBuffer) {
        metaType = kMetadataBufferTypeANWBuffer;
    }
    if (metaType == kMetadataBufferTypeGrallocSource
            && data->capacity() >= sizeof(VideoGrallocMetadata)) {
        VideoGrallocMetadata &metadata = *(VideoGrallocMetadata *)(data->data());
        metadata.eType = kMetadataBufferTypeGrallocSource;
        metadata.pHandle = graphicBuffer == NULL ? NULL : graphicBuffer->handle;
    } else if (metaType == kMetadataBufferTypeANWBuffer
            && data->capacity() >= sizeof(VideoNativeMetadata)) {
        VideoNativeMetadata &metadata = *(VideoNativeMetadata *)(data->data());
        metadata.eType = kMetadataBufferTypeANWBuffer;
        metadata.pBuffer = graphicBuffer == NULL ? NULL : graphicBuffer->getNativeBuffer();
        metadata.nFenceFd = -1;
    } else {
        CLOG_ERROR(updateGraphicBufferInMeta, BAD_VALUE, "%s:%u, %#x bad type (%d) or size (%u)",
            portString(portIndex), portIndex, buffer, mMetadataType[portIndex], header->nAllocLen);
        return BAD_VALUE;
    }

    CLOG_BUFFER(updateGraphicBufferInMeta, "%s:%u, %#x := %p",
            portString(portIndex), portIndex, buffer,
            graphicBuffer == NULL ? NULL : graphicBuffer->handle);
    return OK;
}

status_t OMXNodeInstance::updateGraphicBufferInMeta(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);
    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, portIndex);
    // update backup buffer for input, codec buffer for output
    return updateGraphicBufferInMeta_l(
            portIndex, graphicBuffer, buffer, header,
            portIndex == kPortIndexOutput /* updateCodecBuffer */);
}

status_t OMXNodeInstance::updateNativeHandleInMeta(
        OMX_U32 portIndex, const sp<NativeHandle>& nativeHandle, OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);
    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, portIndex);
    // No need to check |nativeHandle| since NULL is valid for it as below.
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    if (portIndex != kPortIndexInput && portIndex != kPortIndexOutput) {
        return BAD_VALUE;
    }

    BufferMeta *bufferMeta = (BufferMeta *)(header->pAppPrivate);
    // update backup buffer for input, codec buffer for output
    sp<ABuffer> data = bufferMeta->getBuffer(
            header, portIndex == kPortIndexInput /* backup */, false /* limit */);
    bufferMeta->setNativeHandle(nativeHandle);
    if (mMetadataType[portIndex] == kMetadataBufferTypeNativeHandleSource
            && data->capacity() >= sizeof(VideoNativeHandleMetadata)) {
        VideoNativeHandleMetadata &metadata = *(VideoNativeHandleMetadata *)(data->data());
        metadata.eType = mMetadataType[portIndex];
        metadata.pHandle =
            nativeHandle == NULL ? NULL : const_cast<native_handle*>(nativeHandle->handle());
    } else {
        CLOG_ERROR(updateNativeHandleInMeta, BAD_VALUE, "%s:%u, %#x bad type (%d) or size (%zu)",
            portString(portIndex), portIndex, buffer, mMetadataType[portIndex], data->capacity());
        return BAD_VALUE;
    }

    CLOG_BUFFER(updateNativeHandleInMeta, "%s:%u, %#x := %p",
            portString(portIndex), portIndex, buffer,
            nativeHandle == NULL ? NULL : nativeHandle->handle());
    return OK;
}

status_t OMXNodeInstance::createGraphicBufferSource(
        OMX_U32 portIndex, sp<IGraphicBufferConsumer> bufferConsumer, MetadataBufferType *type) {
    status_t err;

    const sp<GraphicBufferSource>& surfaceCheck = getGraphicBufferSource();
    if (surfaceCheck != NULL) {
        if (portIndex < NELEM(mMetadataType) && type != NULL) {
            *type = mMetadataType[portIndex];
        }
        return ALREADY_EXISTS;
    }

    // Input buffers will hold meta-data (ANativeWindowBuffer references).
    if (type != NULL) {
        *type = kMetadataBufferTypeANWBuffer;
    }
    err = storeMetaDataInBuffers_l(portIndex, OMX_TRUE, type);
    if (err != OK) {
        return err;
    }

    // Retrieve the width and height of the graphic buffer, set when the
    // codec was configured.
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;
    OMX_ERRORTYPE oerr = OMX_GetParameter(
            mHandle, OMX_IndexParamPortDefinition, &def);
    if (oerr != OMX_ErrorNone) {
        OMX_INDEXTYPE index = OMX_IndexParamPortDefinition;
        CLOG_ERROR(getParameter, oerr, "%s(%#x): %s:%u",
                asString(index), index, portString(portIndex), portIndex);
        return UNKNOWN_ERROR;
    }

    if (def.format.video.eColorFormat != OMX_COLOR_FormatAndroidOpaque) {
        CLOGW("createInputSurface requires COLOR_FormatSurface "
                "(AndroidOpaque) color format instead of %s(%#x)",
                asString(def.format.video.eColorFormat), def.format.video.eColorFormat);
        return INVALID_OPERATION;
    }

    uint32_t usageBits;
    oerr = OMX_GetParameter(
            mHandle, (OMX_INDEXTYPE)OMX_IndexParamConsumerUsageBits, &usageBits);
    if (oerr != OMX_ErrorNone) {
        usageBits = 0;
    }

    sp<GraphicBufferSource> bufferSource = new GraphicBufferSource(this,
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
            def.nBufferCountActual,
            usageBits,
            bufferConsumer);

    if ((err = bufferSource->initCheck()) != OK) {
        return err;
    }
    setGraphicBufferSource(bufferSource);

    return OK;
}

status_t OMXNodeInstance::createInputSurface(
        OMX_U32 portIndex, android_dataspace dataSpace,
        sp<IGraphicBufferProducer> *bufferProducer, MetadataBufferType *type) {
    if (bufferProducer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    Mutex::Autolock autolock(mLock);
    status_t err = createGraphicBufferSource(portIndex, NULL /* bufferConsumer */, type);

    if (err != OK) {
        return err;
    }

    mGraphicBufferSource->setDefaultDataSpace(dataSpace);

    *bufferProducer = mGraphicBufferSource->getIGraphicBufferProducer();
    return OK;
}

//static
status_t OMXNodeInstance::createPersistentInputSurface(
        sp<IGraphicBufferProducer> *bufferProducer,
        sp<IGraphicBufferConsumer> *bufferConsumer) {
    if (bufferProducer == NULL || bufferConsumer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }
    String8 name("GraphicBufferSource");

    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    consumer->setConsumerName(name);
    consumer->setConsumerUsageBits(GRALLOC_USAGE_HW_VIDEO_ENCODER);

    sp<BufferQueue::ProxyConsumerListener> proxy =
        new BufferQueue::ProxyConsumerListener(NULL);
    status_t err = consumer->consumerConnect(proxy, false);
    if (err != NO_ERROR) {
        ALOGE("Error connecting to BufferQueue: %s (%d)",
                strerror(-err), err);
        return err;
    }

    *bufferProducer = producer;
    *bufferConsumer = consumer;

    return OK;
}

status_t OMXNodeInstance::setInputSurface(
        OMX_U32 portIndex, const sp<IGraphicBufferConsumer> &bufferConsumer,
        MetadataBufferType *type) {
    Mutex::Autolock autolock(mLock);
    return createGraphicBufferSource(portIndex, bufferConsumer, type);
}

void OMXNodeInstance::signalEvent(OMX_EVENTTYPE event, OMX_U32 arg1, OMX_U32 arg2) {
    mOwner->OnEvent(mNodeID, event, arg1, arg2, NULL);
}

status_t OMXNodeInstance::signalEndOfInputStream() {
    // For non-Surface input, the MediaCodec should convert the call to a
    // pair of requests (dequeue input buffer, queue input buffer with EOS
    // flag set).  Seems easier than doing the equivalent from here.
    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource == NULL) {
        CLOGW("signalEndOfInputStream can only be used with Surface input");
        return INVALID_OPERATION;
    }
    return bufferSource->signalEndOfInputStream();
}

status_t OMXNodeInstance::allocateSecureBuffer(
        OMX_U32 portIndex, size_t size, OMX::buffer_id *buffer,
        void **buffer_data, sp<NativeHandle> *native_handle) {
    if (buffer == NULL || buffer_data == NULL || native_handle == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    Mutex::Autolock autoLock(mLock);

    BufferMeta *buffer_meta = new BufferMeta(size, portIndex);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_AllocateBuffer(
            mHandle, &header, portIndex, buffer_meta, size);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(allocateBuffer, err, BUFFER_FMT(portIndex, "%zu@", size));
        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return StatusFromOMXError(err);
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);

    *buffer = makeBufferID(header);
    if (mSecureBufferType[portIndex] == kSecureBufferTypeNativeHandle) {
        *buffer_data = NULL;
        *native_handle = NativeHandle::create(
                (native_handle_t *)header->pBuffer, false /* ownsHandle */);
    } else {
        *buffer_data = header->pBuffer;
        *native_handle = NULL;
    }

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }
    CLOG_BUFFER(allocateSecureBuffer, NEW_BUFFER_FMT(
            *buffer, portIndex, "%zu@%p:%p", size, *buffer_data,
            *native_handle == NULL ? NULL : (*native_handle)->handle()));

    return OK;
}

status_t OMXNodeInstance::allocateBufferWithBackup(
        OMX_U32 portIndex, const sp<IMemory> &params,
        OMX::buffer_id *buffer, OMX_U32 allottedSize) {
    if (params == NULL || buffer == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    Mutex::Autolock autoLock(mLock);
    if (allottedSize > params->size()) {
        return BAD_VALUE;
    }

    BufferMeta *buffer_meta = new BufferMeta(params, portIndex, true);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_AllocateBuffer(
            mHandle, &header, portIndex, buffer_meta, allottedSize);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(allocateBufferWithBackup, err,
                SIMPLE_BUFFER(portIndex, (size_t)allottedSize, params->pointer()));
        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return StatusFromOMXError(err);
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);

    *buffer = makeBufferID(header);

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }

    CLOG_BUFFER(allocateBufferWithBackup, NEW_BUFFER_FMT(*buffer, portIndex, "%zu@%p :> %u@%p",
            params->size(), params->pointer(), allottedSize, header->pBuffer));

    return OK;
}

status_t OMXNodeInstance::freeBuffer(
        OMX_U32 portIndex, OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);
    CLOG_BUFFER(freeBuffer, "%s:%u %#x", portString(portIndex), portIndex, buffer);

    removeActiveBuffer(portIndex, buffer);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, portIndex);
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }
    BufferMeta *buffer_meta = static_cast<BufferMeta *>(header->pAppPrivate);

    OMX_ERRORTYPE err = OMX_FreeBuffer(mHandle, portIndex, header);
    CLOG_IF_ERROR(freeBuffer, err, "%s:%u %#x", portString(portIndex), portIndex, buffer);

    delete buffer_meta;
    buffer_meta = NULL;
    invalidateBufferID(buffer);

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::fillBuffer(OMX::buffer_id buffer, int fenceFd) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, kPortIndexOutput);
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }
    header->nFilledLen = 0;
    header->nOffset = 0;
    header->nFlags = 0;

    // meta now owns fenceFd
    status_t res = storeFenceInMeta_l(header, fenceFd, kPortIndexOutput);
    if (res != OK) {
        CLOG_ERROR(fillBuffer::storeFenceInMeta, res, EMPTY_BUFFER(buffer, header, fenceFd));
        return res;
    }

    {
        Mutex::Autolock _l(mDebugLock);
        mOutputBuffersWithCodec.add(header);
        CLOG_BUMPED_BUFFER(fillBuffer, WITH_STATS(EMPTY_BUFFER(buffer, header, fenceFd)));
    }

    OMX_ERRORTYPE err = OMX_FillThisBuffer(mHandle, header);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(fillBuffer, err, EMPTY_BUFFER(buffer, header, fenceFd));
        Mutex::Autolock _l(mDebugLock);
        mOutputBuffersWithCodec.remove(header);
    }
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::emptyBuffer(
        OMX::buffer_id buffer,
        OMX_U32 rangeOffset, OMX_U32 rangeLength,
        OMX_U32 flags, OMX_TICKS timestamp, int fenceFd) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, kPortIndexInput);
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }
    BufferMeta *buffer_meta =
        static_cast<BufferMeta *>(header->pAppPrivate);
    sp<ABuffer> backup = buffer_meta->getBuffer(header, true /* backup */, false /* limit */);
    sp<ABuffer> codec = buffer_meta->getBuffer(header, false /* backup */, false /* limit */);

    // convert incoming ANW meta buffers if component is configured for gralloc metadata mode
    // ignore rangeOffset in this case
    if (mMetadataType[kPortIndexInput] == kMetadataBufferTypeGrallocSource
            && backup->capacity() >= sizeof(VideoNativeMetadata)
            && codec->capacity() >= sizeof(VideoGrallocMetadata)
            && ((VideoNativeMetadata *)backup->base())->eType
                    == kMetadataBufferTypeANWBuffer) {
        VideoNativeMetadata &backupMeta = *(VideoNativeMetadata *)backup->base();
        VideoGrallocMetadata &codecMeta = *(VideoGrallocMetadata *)codec->base();
        CLOG_BUFFER(emptyBuffer, "converting ANWB %p to handle %p",
                backupMeta.pBuffer, backupMeta.pBuffer->handle);
        codecMeta.pHandle = backupMeta.pBuffer != NULL ? backupMeta.pBuffer->handle : NULL;
        codecMeta.eType = kMetadataBufferTypeGrallocSource;
        header->nFilledLen = rangeLength ? sizeof(codecMeta) : 0;
        header->nOffset = 0;
    } else {
        // rangeLength and rangeOffset must be a subset of the allocated data in the buffer.
        // corner case: we permit rangeOffset == end-of-buffer with rangeLength == 0.
        if (rangeOffset > header->nAllocLen
                || rangeLength > header->nAllocLen - rangeOffset) {
            CLOG_ERROR(emptyBuffer, OMX_ErrorBadParameter, FULL_BUFFER(NULL, header, fenceFd));
            if (fenceFd >= 0) {
                ::close(fenceFd);
            }
            return BAD_VALUE;
        }
        header->nFilledLen = rangeLength;
        header->nOffset = rangeOffset;

        buffer_meta->CopyToOMX(header);
    }

    return emptyBuffer_l(header, flags, timestamp, (intptr_t)buffer, fenceFd);
}

// log queued buffer activity for the next few input and/or output frames
// if logging at internal state level
void OMXNodeInstance::bumpDebugLevel_l(size_t numInputBuffers, size_t numOutputBuffers) {
    if (DEBUG == ADebug::kDebugInternalState) {
        DEBUG_BUMP = ADebug::kDebugAll;
        if (numInputBuffers > 0) {
            mDebugLevelBumpPendingBuffers[kPortIndexInput] = numInputBuffers;
        }
        if (numOutputBuffers > 0) {
            mDebugLevelBumpPendingBuffers[kPortIndexOutput] = numOutputBuffers;
        }
    }
}

void OMXNodeInstance::unbumpDebugLevel_l(size_t portIndex) {
    if (mDebugLevelBumpPendingBuffers[portIndex]) {
        --mDebugLevelBumpPendingBuffers[portIndex];
    }
    if (!mDebugLevelBumpPendingBuffers[0]
            && !mDebugLevelBumpPendingBuffers[1]) {
        DEBUG_BUMP = DEBUG;
    }
}

status_t OMXNodeInstance::storeFenceInMeta_l(
        OMX_BUFFERHEADERTYPE *header, int fenceFd, OMX_U32 portIndex) {
    // propagate fence if component supports it; wait for it otherwise
    OMX_U32 metaSize = portIndex == kPortIndexInput ? header->nFilledLen : header->nAllocLen;
    if (mMetadataType[portIndex] == kMetadataBufferTypeANWBuffer
            && metaSize >= sizeof(VideoNativeMetadata)) {
        VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)(header->pBuffer);
        if (nativeMeta.nFenceFd >= 0) {
            ALOGE("fence (%d) already exists in meta", nativeMeta.nFenceFd);
            if (fenceFd >= 0) {
                ::close(fenceFd);
            }
            return ALREADY_EXISTS;
        }
        nativeMeta.nFenceFd = fenceFd;
    } else if (fenceFd >= 0) {
        CLOG_BUFFER(storeFenceInMeta, "waiting for fence %d", fenceFd);
        sp<Fence> fence = new Fence(fenceFd);
        return fence->wait(IOMX::kFenceTimeoutMs);
    }
    return OK;
}

int OMXNodeInstance::retrieveFenceFromMeta_l(
        OMX_BUFFERHEADERTYPE *header, OMX_U32 portIndex) {
    OMX_U32 metaSize = portIndex == kPortIndexInput ? header->nAllocLen : header->nFilledLen;
    int fenceFd = -1;
    if (mMetadataType[portIndex] == kMetadataBufferTypeANWBuffer
            && header->nAllocLen >= sizeof(VideoNativeMetadata)) {
        VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)(header->pBuffer);
        if (nativeMeta.eType == kMetadataBufferTypeANWBuffer) {
            fenceFd = nativeMeta.nFenceFd;
            nativeMeta.nFenceFd = -1;
        }
        if (metaSize < sizeof(nativeMeta) && fenceFd >= 0) {
            CLOG_ERROR(foundFenceInEmptyMeta, BAD_VALUE, FULL_BUFFER(
                    NULL, header, nativeMeta.nFenceFd));
            fenceFd = -1;
        }
    }
    return fenceFd;
}

status_t OMXNodeInstance::emptyBuffer_l(
        OMX_BUFFERHEADERTYPE *header, OMX_U32 flags, OMX_TICKS timestamp,
        intptr_t debugAddr, int fenceFd) {
    header->nFlags = flags;
    header->nTimeStamp = timestamp;

    status_t res = storeFenceInMeta_l(header, fenceFd, kPortIndexInput);
    if (res != OK) {
        CLOG_ERROR(emptyBuffer::storeFenceInMeta, res, WITH_STATS(
                FULL_BUFFER(debugAddr, header, fenceFd)));
        return res;
    }

    {
        Mutex::Autolock _l(mDebugLock);
        mInputBuffersWithCodec.add(header);

        // bump internal-state debug level for 2 input frames past a buffer with CSD
        if ((flags & OMX_BUFFERFLAG_CODECCONFIG) != 0) {
            bumpDebugLevel_l(2 /* numInputBuffers */, 0 /* numOutputBuffers */);
        }

        CLOG_BUMPED_BUFFER(emptyBuffer, WITH_STATS(FULL_BUFFER(debugAddr, header, fenceFd)));
    }

    OMX_ERRORTYPE err = OMX_EmptyThisBuffer(mHandle, header);
    CLOG_IF_ERROR(emptyBuffer, err, FULL_BUFFER(debugAddr, header, fenceFd));

    {
        Mutex::Autolock _l(mDebugLock);
        if (err != OMX_ErrorNone) {
            mInputBuffersWithCodec.remove(header);
        } else if (!(flags & OMX_BUFFERFLAG_CODECCONFIG)) {
            unbumpDebugLevel_l(kPortIndexInput);
        }
    }

    return StatusFromOMXError(err);
}

// like emptyBuffer, but the data is already in header->pBuffer
status_t OMXNodeInstance::emptyGraphicBuffer(
        OMX_BUFFERHEADERTYPE *header, const sp<GraphicBuffer> &graphicBuffer,
        OMX_U32 flags, OMX_TICKS timestamp, int fenceFd) {
    if (header == NULL) {
        ALOGE("b/25884056");
        return BAD_VALUE;
    }

    Mutex::Autolock autoLock(mLock);
    OMX::buffer_id buffer = findBufferID(header);
    status_t err = updateGraphicBufferInMeta_l(
            kPortIndexInput, graphicBuffer, buffer, header,
            true /* updateCodecBuffer */);
    if (err != OK) {
        CLOG_ERROR(emptyGraphicBuffer, err, FULL_BUFFER(
                (intptr_t)header->pBuffer, header, fenceFd));
        return err;
    }

    header->nOffset = 0;
    if (graphicBuffer == NULL) {
        header->nFilledLen = 0;
    } else if (mMetadataType[kPortIndexInput] == kMetadataBufferTypeGrallocSource) {
        header->nFilledLen = sizeof(VideoGrallocMetadata);
    } else {
        header->nFilledLen = sizeof(VideoNativeMetadata);
    }
    return emptyBuffer_l(header, flags, timestamp, (intptr_t)header->pBuffer, fenceFd);
}

status_t OMXNodeInstance::getExtensionIndex(
        const char *parameterName, OMX_INDEXTYPE *index) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(
            mHandle, const_cast<char *>(parameterName), index);

    return StatusFromOMXError(err);
}

inline static const char *asString(IOMX::InternalOptionType i, const char *def = "??") {
    switch (i) {
        case IOMX::INTERNAL_OPTION_SUSPEND:           return "SUSPEND";
        case IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY:
            return "REPEAT_PREVIOUS_FRAME_DELAY";
        case IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP: return "MAX_TIMESTAMP_GAP";
        case IOMX::INTERNAL_OPTION_MAX_FPS:           return "MAX_FPS";
        case IOMX::INTERNAL_OPTION_START_TIME:        return "START_TIME";
        case IOMX::INTERNAL_OPTION_TIME_LAPSE:        return "TIME_LAPSE";
        default:                                      return def;
    }
}

template<typename T>
static bool getInternalOption(
        const void *data, size_t size, T *out) {
    if (size != sizeof(T)) {
        return false;
    }
    *out = *(T*)data;
    return true;
}

status_t OMXNodeInstance::setInternalOption(
        OMX_U32 portIndex,
        IOMX::InternalOptionType type,
        const void *data,
        size_t size) {
    CLOG_CONFIG(setInternalOption, "%s(%d): %s:%u %zu@%p",
            asString(type), type, portString(portIndex), portIndex, size, data);
    switch (type) {
        case IOMX::INTERNAL_OPTION_SUSPEND:
        case IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY:
        case IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP:
        case IOMX::INTERNAL_OPTION_MAX_FPS:
        case IOMX::INTERNAL_OPTION_START_TIME:
        case IOMX::INTERNAL_OPTION_TIME_LAPSE:
        case IOMX::INTERNAL_OPTION_COLOR_ASPECTS:
        {
            const sp<GraphicBufferSource> &bufferSource =
                getGraphicBufferSource();

            if (bufferSource == NULL || portIndex != kPortIndexInput) {
                CLOGW("setInternalOption is only for Surface input");
                return ERROR_UNSUPPORTED;
            }

            if (type == IOMX::INTERNAL_OPTION_SUSPEND) {
                bool suspend;
                if (!getInternalOption(data, size, &suspend)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "suspend=%d", suspend);
                bufferSource->suspend(suspend);
            } else if (type == IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY) {
                int64_t delayUs;
                if (!getInternalOption(data, size, &delayUs)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "delayUs=%lld", (long long)delayUs);
                return bufferSource->setRepeatPreviousFrameDelayUs(delayUs);
            } else if (type == IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP) {
                int64_t maxGapUs;
                if (!getInternalOption(data, size, &maxGapUs)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "gapUs=%lld", (long long)maxGapUs);
                return bufferSource->setMaxTimestampGapUs(maxGapUs);
            } else if (type == IOMX::INTERNAL_OPTION_MAX_FPS) {
                float maxFps;
                if (!getInternalOption(data, size, &maxFps)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "maxFps=%f", maxFps);
                return bufferSource->setMaxFps(maxFps);
            } else if (type == IOMX::INTERNAL_OPTION_START_TIME) {
                int64_t skipFramesBeforeUs;
                if (!getInternalOption(data, size, &skipFramesBeforeUs)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "beforeUs=%lld", (long long)skipFramesBeforeUs);
                bufferSource->setSkipFramesBeforeUs(skipFramesBeforeUs);
            } else if (type == IOMX::INTERNAL_OPTION_TIME_LAPSE) {
                GraphicBufferSource::TimeLapseConfig config;
                if (!getInternalOption(data, size, &config)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "perFrameUs=%lld perCaptureUs=%lld",
                        (long long)config.mTimePerFrameUs, (long long)config.mTimePerCaptureUs);

                return bufferSource->setTimeLapseConfig(config);
            } else if (type == IOMX::INTERNAL_OPTION_COLOR_ASPECTS) {
                ColorAspects aspects;
                if (!getInternalOption(data, size, &aspects)) {
                    return INVALID_OPERATION;
                }

                CLOG_CONFIG(setInternalOption, "setting color aspects");
                bufferSource->setColorAspects(aspects);
            }

            return OK;
        }

        default:
            return ERROR_UNSUPPORTED;
    }
}

bool OMXNodeInstance::handleMessage(omx_message &msg) {
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());

    if (msg.type == omx_message::FILL_BUFFER_DONE) {
        OMX_BUFFERHEADERTYPE *buffer =
            findBufferHeader(msg.u.extended_buffer_data.buffer, kPortIndexOutput);
        if (buffer == NULL) {
            ALOGE("b/25884056");
            return false;
        }

        {
            Mutex::Autolock _l(mDebugLock);
            mOutputBuffersWithCodec.remove(buffer);

            CLOG_BUMPED_BUFFER(
                    FBD, WITH_STATS(FULL_BUFFER(
                            msg.u.extended_buffer_data.buffer, buffer, msg.fenceFd)));

            unbumpDebugLevel_l(kPortIndexOutput);
        }

        BufferMeta *buffer_meta =
            static_cast<BufferMeta *>(buffer->pAppPrivate);

        if (buffer->nOffset + buffer->nFilledLen < buffer->nOffset
                || buffer->nOffset + buffer->nFilledLen > buffer->nAllocLen) {
            CLOG_ERROR(onFillBufferDone, OMX_ErrorBadParameter,
                    FULL_BUFFER(NULL, buffer, msg.fenceFd));
        }
        buffer_meta->CopyFromOMX(buffer);

        if (bufferSource != NULL) {
            // fix up the buffer info (especially timestamp) if needed
            bufferSource->codecBufferFilled(buffer);

            msg.u.extended_buffer_data.timestamp = buffer->nTimeStamp;
        }
    } else if (msg.type == omx_message::EMPTY_BUFFER_DONE) {
        OMX_BUFFERHEADERTYPE *buffer =
            findBufferHeader(msg.u.buffer_data.buffer, kPortIndexInput);
        if (buffer == NULL) {
            return false;
        }

        {
            Mutex::Autolock _l(mDebugLock);
            mInputBuffersWithCodec.remove(buffer);

            CLOG_BUMPED_BUFFER(
                    EBD, WITH_STATS(EMPTY_BUFFER(msg.u.buffer_data.buffer, buffer, msg.fenceFd)));
        }

        if (bufferSource != NULL) {
            // This is one of the buffers used exclusively by
            // GraphicBufferSource.
            // Don't dispatch a message back to ACodec, since it doesn't
            // know that anyone asked to have the buffer emptied and will
            // be very confused.
            bufferSource->codecBufferEmptied(buffer, msg.fenceFd);
            return true;
        }
    }

    return false;
}

void OMXNodeInstance::onMessages(std::list<omx_message> &messages) {
    for (std::list<omx_message>::iterator it = messages.begin(); it != messages.end(); ) {
        if (handleMessage(*it)) {
            messages.erase(it++);
        } else {
            ++it;
        }
    }

    if (!messages.empty()) {
        mObserver->onMessages(messages);
    }
}

void OMXNodeInstance::onObserverDied(OMXMaster *master) {
    ALOGE("!!! Observer died. Quickly, do something, ... anything...");

    // Try to force shutdown of the node and hope for the best.
    freeNode(master);
}

void OMXNodeInstance::onGetHandleFailed() {
    delete this;
}

// OMXNodeInstance::OnEvent calls OMX::OnEvent, which then calls here.
// Don't try to acquire mLock here -- in rare circumstances this will hang.
void OMXNodeInstance::onEvent(
        OMX_EVENTTYPE event, OMX_U32 arg1, OMX_U32 arg2) {
    const char *arg1String = "??";
    const char *arg2String = "??";
    ADebug::Level level = ADebug::kDebugInternalState;

    switch (event) {
        case OMX_EventCmdComplete:
            arg1String = asString((OMX_COMMANDTYPE)arg1);
            switch (arg1) {
                case OMX_CommandStateSet:
                    arg2String = asString((OMX_STATETYPE)arg2);
                    level = ADebug::kDebugState;
                    break;
                case OMX_CommandFlush:
                case OMX_CommandPortEnable:
                {
                    // bump internal-state debug level for 2 input and output frames
                    Mutex::Autolock _l(mDebugLock);
                    bumpDebugLevel_l(2 /* numInputBuffers */, 2 /* numOutputBuffers */);
                }
                // fall through
                default:
                    arg2String = portString(arg2);
            }
            break;
        case OMX_EventError:
            arg1String = asString((OMX_ERRORTYPE)arg1);
            level = ADebug::kDebugLifeCycle;
            break;
        case OMX_EventPortSettingsChanged:
            arg2String = asString((OMX_INDEXEXTTYPE)arg2);
            // fall through
        default:
            arg1String = portString(arg1);
    }

    CLOGI_(level, onEvent, "%s(%x), %s(%x), %s(%x)",
            asString(event), event, arg1String, arg1, arg2String, arg2);
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());

    if (bufferSource != NULL
            && event == OMX_EventCmdComplete
            && arg1 == OMX_CommandStateSet
            && arg2 == OMX_StateExecuting) {
        bufferSource->omxExecuting();
    }
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnEvent(
        OMX_IN OMX_HANDLETYPE /* hComponent */,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_EVENTTYPE eEvent,
        OMX_IN OMX_U32 nData1,
        OMX_IN OMX_U32 nData2,
        OMX_IN OMX_PTR pEventData) {
    if (pAppData == NULL) {
        ALOGE("b/25884056");
        return OMX_ErrorBadParameter;
    }
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnEvent(
            instance->nodeID(), eEvent, nData1, nData2, pEventData);
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnEmptyBufferDone(
        OMX_IN OMX_HANDLETYPE /* hComponent */,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
    if (pAppData == NULL) {
        ALOGE("b/25884056");
        return OMX_ErrorBadParameter;
    }
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    int fenceFd = instance->retrieveFenceFromMeta_l(pBuffer, kPortIndexOutput);
    return instance->owner()->OnEmptyBufferDone(instance->nodeID(),
            instance->findBufferID(pBuffer), pBuffer, fenceFd);
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnFillBufferDone(
        OMX_IN OMX_HANDLETYPE /* hComponent */,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
    if (pAppData == NULL) {
        ALOGE("b/25884056");
        return OMX_ErrorBadParameter;
    }
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    int fenceFd = instance->retrieveFenceFromMeta_l(pBuffer, kPortIndexOutput);
    return instance->owner()->OnFillBufferDone(instance->nodeID(),
            instance->findBufferID(pBuffer), pBuffer, fenceFd);
}

void OMXNodeInstance::addActiveBuffer(OMX_U32 portIndex, OMX::buffer_id id) {
    ActiveBuffer active;
    active.mPortIndex = portIndex;
    active.mID = id;
    mActiveBuffers.push(active);

    if (portIndex < NELEM(mNumPortBuffers)) {
        ++mNumPortBuffers[portIndex];
    }
}

void OMXNodeInstance::removeActiveBuffer(
        OMX_U32 portIndex, OMX::buffer_id id) {
    for (size_t i = 0; i < mActiveBuffers.size(); ++i) {
        if (mActiveBuffers[i].mPortIndex == portIndex
                && mActiveBuffers[i].mID == id) {
            mActiveBuffers.removeItemsAt(i);

            if (portIndex < NELEM(mNumPortBuffers)) {
                --mNumPortBuffers[portIndex];
            }
            return;
        }
    }

     CLOGW("Attempt to remove an active buffer [%#x] we know nothing about...", id);
}

void OMXNodeInstance::freeActiveBuffers() {
    // Make sure to count down here, as freeBuffer will in turn remove
    // the active buffer from the vector...
    for (size_t i = mActiveBuffers.size(); i > 0;) {
        i--;
        freeBuffer(mActiveBuffers[i].mPortIndex, mActiveBuffers[i].mID);
    }
}

OMX::buffer_id OMXNodeInstance::makeBufferID(OMX_BUFFERHEADERTYPE *bufferHeader) {
    if (bufferHeader == NULL) {
        return 0;
    }
    Mutex::Autolock autoLock(mBufferIDLock);
    OMX::buffer_id buffer;
    do { // handle the very unlikely case of ID overflow
        if (++mBufferIDCount == 0) {
            ++mBufferIDCount;
        }
        buffer = (OMX::buffer_id)mBufferIDCount;
    } while (mBufferIDToBufferHeader.indexOfKey(buffer) >= 0);
    mBufferIDToBufferHeader.add(buffer, bufferHeader);
    mBufferHeaderToBufferID.add(bufferHeader, buffer);
    return buffer;
}

OMX_BUFFERHEADERTYPE *OMXNodeInstance::findBufferHeader(
        OMX::buffer_id buffer, OMX_U32 portIndex) {
    if (buffer == 0) {
        return NULL;
    }
    Mutex::Autolock autoLock(mBufferIDLock);
    ssize_t index = mBufferIDToBufferHeader.indexOfKey(buffer);
    if (index < 0) {
        CLOGW("findBufferHeader: buffer %u not found", buffer);
        return NULL;
    }
    OMX_BUFFERHEADERTYPE *header = mBufferIDToBufferHeader.valueAt(index);
    BufferMeta *buffer_meta =
        static_cast<BufferMeta *>(header->pAppPrivate);
    if (buffer_meta->getPortIndex() != portIndex) {
        CLOGW("findBufferHeader: buffer %u found but with incorrect port index.", buffer);
        android_errorWriteLog(0x534e4554, "28816827");
        return NULL;
    }
    return header;
}

OMX::buffer_id OMXNodeInstance::findBufferID(OMX_BUFFERHEADERTYPE *bufferHeader) {
    if (bufferHeader == NULL) {
        return 0;
    }
    Mutex::Autolock autoLock(mBufferIDLock);
    ssize_t index = mBufferHeaderToBufferID.indexOfKey(bufferHeader);
    if (index < 0) {
        CLOGW("findBufferID: bufferHeader %p not found", bufferHeader);
        return 0;
    }
    return mBufferHeaderToBufferID.valueAt(index);
}

void OMXNodeInstance::invalidateBufferID(OMX::buffer_id buffer) {
    if (buffer == 0) {
        return;
    }
    Mutex::Autolock autoLock(mBufferIDLock);
    ssize_t index = mBufferIDToBufferHeader.indexOfKey(buffer);
    if (index < 0) {
        CLOGW("invalidateBufferID: buffer %u not found", buffer);
        return;
    }
    mBufferHeaderToBufferID.removeItem(mBufferIDToBufferHeader.valueAt(index));
    mBufferIDToBufferHeader.removeItemsAt(index);
}

}  // namespace android
