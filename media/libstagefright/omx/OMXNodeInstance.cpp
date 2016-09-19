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
#include "GraphicBufferSource.h"

#include <OMX_Component.h>
#include <OMX_IndexExt.h>
#include <OMX_AsString.h>

#include <binder/IMemory.h>
#include <gui/BufferQueue.h>
#include <HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaErrors.h>

#include <utils/misc.h>

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

#define EMPTY_BUFFER(addr, header) "%#x [%u@%p]", \
    (addr), (header)->nAllocLen, (header)->pBuffer
#define FULL_BUFFER(addr, header) "%#" PRIxPTR " [%u@%p (%u..+%u) f=%x ts=%lld]", \
    (intptr_t)(addr), (header)->nAllocLen, (header)->pBuffer, \
    (header)->nOffset, (header)->nFilledLen, (header)->nFlags, (header)->nTimeStamp

#define WITH_STATS_WRAPPER(fmt, ...) fmt " { IN=%zu/%zu OUT=%zu/%zu }", ##__VA_ARGS__, \
    mInputBuffersWithCodec.size(), mNumPortBuffers[kPortIndexInput], \
    mOutputBuffersWithCodec.size(), mNumPortBuffers[kPortIndexOutput]
// TRICKY: this is needed so formatting macros expand before substitution
#define WITH_STATS(fmt, ...) WITH_STATS_WRAPPER(fmt, ##__VA_ARGS__)

template<class T>
static void InitOMXParams(T *params) {
    memset(params, 0, sizeof(T));
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

namespace android {

struct BufferMeta {
    BufferMeta(
            const sp<IMemory> &mem, OMX_U32 portIndex, bool copyToOmx,
            bool copyFromOmx, OMX_U8 *backup)
        : mMem(mem),
          mCopyFromOmx(copyFromOmx),
          mCopyToOmx(copyToOmx),
          mPortIndex(portIndex),
          mBackup(backup) {
    }

    BufferMeta(size_t size, OMX_U32 portIndex)
        : mSize(size),
          mCopyFromOmx(false),
          mCopyToOmx(false),
          mPortIndex(portIndex),
          mBackup(NULL) {
    }

    BufferMeta(const sp<GraphicBuffer> &graphicBuffer, OMX_U32 portIndex)
        : mGraphicBuffer(graphicBuffer),
          mCopyFromOmx(false),
          mCopyToOmx(false),
          mPortIndex(portIndex),
          mBackup(NULL) {
    }

    void CopyFromOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mCopyFromOmx) {
            return;
        }
        size_t bytesToCopy = header->nFlags & OMX_BUFFERFLAG_EXTRADATA ?
            header->nAllocLen - header->nOffset : header->nFilledLen;
        memcpy((OMX_U8 *)mMem->pointer() + header->nOffset,
               header->pBuffer + header->nOffset, bytesToCopy);
    }

    void CopyToOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mCopyToOmx) {
            return;
        }
        size_t bytesToCopy = header->nFlags & OMX_BUFFERFLAG_EXTRADATA ?
            header->nAllocLen - header->nOffset : header->nFilledLen;
        memcpy(header->pBuffer + header->nOffset,
               (const OMX_U8 *)mMem->pointer() + header->nOffset, bytesToCopy);
    }

    void setGraphicBuffer(const sp<GraphicBuffer> &graphicBuffer) {
        mGraphicBuffer = graphicBuffer;
    }

    OMX_U32 getPortIndex() {
        return mPortIndex;
    }

    ~BufferMeta() {
        delete[] mBackup;
    }

private:
    sp<GraphicBuffer> mGraphicBuffer;
    sp<IMemory> mMem;
    size_t mSize;
    bool mCopyFromOmx;
    bool mCopyToOmx;
    OMX_U32 mPortIndex;
    OMX_U8 *mBackup;

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
        case ~0:               return "All";
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
      mSailed(false),
      mQueriedProhibitedExtensions(false),
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
    mUsingMetadata[0] = false;
    mUsingMetadata[1] = false;
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

static status_t StatusFromOMXError(OMX_ERRORTYPE err) {
    switch (err) {
        case OMX_ErrorNone:
            return OK;
        case OMX_ErrorUnsupportedSetting:
        case OMX_ErrorUnsupportedIndex:
            return ERROR_UNSUPPORTED;
        default:
            return UNKNOWN_ERROR;
    }
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
        case OMX_StatePause:
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
    if (cmd == OMX_CommandStateSet && param != OMX_StateIdle) {
        // Normally there are no configurations past first StateSet; however, OMXCodec supports
        // meta configuration past Stateset:Idle.
        mSailed = true;
    }
    const sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
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

bool OMXNodeInstance::isProhibitedIndex_l(OMX_INDEXTYPE index) {
    // these extensions can only be used from OMXNodeInstance, not by clients directly.
    static const char *restricted_extensions[] = {
        "OMX.google.android.index.storeMetaDataInBuffers",
        "OMX.google.android.index.prepareForAdaptivePlayback",
        "OMX.google.android.index.configureVideoTunnelMode",
        "OMX.google.android.index.useAndroidNativeBuffer2",
        "OMX.google.android.index.useAndroidNativeBuffer",
        "OMX.google.android.index.enableAndroidNativeBuffers",
        "OMX.google.android.index.getAndroidNativeBufferUsage",
    };

    if ((index > OMX_IndexComponentStartUnused && index <= OMX_IndexParamStandardComponentRole)
            || (index > OMX_IndexPortStartUnused && index <= OMX_IndexParamCompBufferSupplier)
            || (index > OMX_IndexAudioStartUnused && index <= OMX_IndexConfigAudioChannelVolume)
            || (index > OMX_IndexVideoStartUnused && index <= OMX_IndexConfigVideoNalSize)
            || (index > OMX_IndexCommonStartUnused
                    && index <= OMX_IndexConfigCommonTransitionEffect)
            || (index > (OMX_INDEXTYPE)OMX_IndexExtAudioStartUnused
                    && index <= (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidEac3)
            || (index > (OMX_INDEXTYPE)OMX_IndexExtVideoStartUnused
                    && index <= (OMX_INDEXTYPE)OMX_IndexParamSliceSegments)
            || (index > (OMX_INDEXTYPE)OMX_IndexExtOtherStartUnused
                    && index <= (OMX_INDEXTYPE)OMX_IndexConfigAutoFramerateConversion)) {
        return false;
    }

    if (!mQueriedProhibitedExtensions) {
        for (size_t i = 0; i < NELEM(restricted_extensions); ++i) {
            OMX_INDEXTYPE ext;
            if (OMX_GetExtensionIndex(mHandle, (OMX_STRING)restricted_extensions[i], &ext) == OMX_ErrorNone) {
                mProhibitedExtensions.add(ext);
            }
        }
        mQueriedProhibitedExtensions = true;
    }

    return mProhibitedExtensions.indexOf(index) >= 0;
}

status_t OMXNodeInstance::getParameter(
        OMX_INDEXTYPE index, void *params, size_t /* size */) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

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

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

    OMX_ERRORTYPE err = OMX_SetParameter(
            mHandle, index, const_cast<void *>(params));
    CLOG_IF_ERROR(setParameter, err, "%s(%#x)", asString(extIndex), index);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getConfig(
        OMX_INDEXTYPE index, void *params, size_t /* size */) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

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

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

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

status_t OMXNodeInstance::enableGraphicBuffers(
        OMX_U32 portIndex, OMX_BOOL enable) {
    Mutex::Autolock autoLock(mLock);
    CLOG_CONFIG(enableGraphicBuffers, "%s:%u, %d", portString(portIndex), portIndex, enable);
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.enableAndroidNativeBuffers");

    OMX_INDEXTYPE index;
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR_IF(enable, getExtensionIndex, err, "%s", name);
        return StatusFromOMXError(err);
    }

    EnableAndroidNativeBuffersParams params;
    InitOMXParams(&params);
    params.nPortIndex = portIndex;
    params.enable = enable;

    err = OMX_SetParameter(mHandle, index, &params);
    CLOG_IF_ERROR(setParameter, err, "%s(%#x): %s:%u en=%d", name, index,
            portString(portIndex), portIndex, enable);
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
        OMX_U32 portIndex,
        OMX_BOOL enable) {
    Mutex::Autolock autolock(mLock);
    CLOG_CONFIG(storeMetaDataInBuffers, "%s:%u en:%d", portString(portIndex), portIndex, enable);
    return storeMetaDataInBuffers_l(
            portIndex, enable,
            OMX_FALSE /* useGraphicBuffer */, NULL /* usingGraphicBufferInMetadata */);
}

status_t OMXNodeInstance::storeMetaDataInBuffers_l(
        OMX_U32 portIndex,
        OMX_BOOL enable,
        OMX_BOOL useGraphicBuffer,
        OMX_BOOL *usingGraphicBufferInMetadata) {
    if (mSailed) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }
    if (portIndex != kPortIndexInput && portIndex != kPortIndexOutput) {
        android_errorWriteLog(0x534e4554, "26324358");
        return BAD_VALUE;
    }

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.storeMetaDataInBuffers");

    OMX_STRING graphicBufferName = const_cast<OMX_STRING>(
            "OMX.google.android.index.storeGraphicBufferInMetaData");
    if (usingGraphicBufferInMetadata == NULL) {
        usingGraphicBufferInMetadata = &useGraphicBuffer;
    }

    OMX_ERRORTYPE err =
        (useGraphicBuffer && portIndex == kPortIndexInput)
                ? OMX_GetExtensionIndex(mHandle, graphicBufferName, &index)
                : OMX_ErrorBadParameter;
    if (err == OMX_ErrorNone) {
        *usingGraphicBufferInMetadata = OMX_TRUE;
        name = graphicBufferName;
    } else {
        err = OMX_GetExtensionIndex(mHandle, name, &index);
    }

    OMX_ERRORTYPE xerr = err;
    if (err == OMX_ErrorNone) {
        StoreMetaDataInBuffersParams params;
        InitOMXParams(&params);
        params.nPortIndex = portIndex;
        params.bStoreMetaData = enable;

        err = OMX_SetParameter(mHandle, index, &params);
    }

    // don't log loud error if component does not support metadata mode on the output
    if (err != OMX_ErrorNone) {
        *usingGraphicBufferInMetadata = OMX_FALSE;
        if (enable) {
            mUsingMetadata[portIndex] = false;
        }
        if (err == OMX_ErrorUnsupportedIndex && portIndex == kPortIndexOutput) {
            CLOGW("component does not support metadata mode; using fallback");
        } else if (xerr != OMX_ErrorNone) {
            CLOG_ERROR(getExtensionIndex, xerr, "%s", name);
        } else {
            CLOG_ERROR(setParameter, err, "%s(%#x): %s:%u en=%d GB=%d", name, index,
                    portString(portIndex), portIndex, enable, useGraphicBuffer);
        }
    } else {
        mUsingMetadata[portIndex] = enable;
    }
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::prepareForAdaptivePlayback(
        OMX_U32 portIndex, OMX_BOOL enable, OMX_U32 maxFrameWidth,
        OMX_U32 maxFrameHeight) {
    Mutex::Autolock autolock(mLock);
    if (mSailed) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }
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
    if (mSailed) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }
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
        OMX::buffer_id *buffer, OMX_BOOL crossProcess) {
    Mutex::Autolock autoLock(mLock);
    if (portIndex >= NELEM(mUsingMetadata)) {
        return BAD_VALUE;
    }
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

    // metadata buffers are not connected cross process
    BufferMeta *buffer_meta;
    bool isMeta = mUsingMetadata[portIndex];
    bool useBackup = crossProcess && isMeta; // use a backup buffer instead of the actual buffer
    OMX_U8 *data = static_cast<OMX_U8 *>(params->pointer());
    // allocate backup buffer
    if (useBackup) {
        data = new (std::nothrow) OMX_U8[params->size()];
        if (data == NULL) {
            return NO_MEMORY;
        }
        memset(data, 0, params->size());

        buffer_meta = new BufferMeta(
                params, portIndex, false /* copyToOmx */, false /* copyFromOmx */, data);
    } else {
        buffer_meta = new BufferMeta(
                params, portIndex, false /* copyToOmx */, false /* copyFromOmx */, NULL);
    }

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_UseBuffer(
            mHandle, &header, portIndex, buffer_meta,
            params->size(), data);

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(useBuffer, err, SIMPLE_BUFFER(portIndex, params->size(), params->pointer()));

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
            *buffer, portIndex, "%zu@%p", params->size(), params->pointer()));
    return OK;
}

status_t OMXNodeInstance::useGraphicBuffer2_l(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id *buffer) {

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

status_t OMXNodeInstance::updateGraphicBufferInMeta(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, portIndex);
    if (header == NULL) {
        return BAD_VALUE;
    }
    VideoDecoderOutputMetaData *metadata =
        (VideoDecoderOutputMetaData *)(header->pBuffer);
    BufferMeta *bufferMeta = (BufferMeta *)(header->pAppPrivate);
    bufferMeta->setGraphicBuffer(graphicBuffer);
    metadata->eType = kMetadataBufferTypeGrallocSource;
    metadata->pHandle = graphicBuffer->handle;
    CLOG_BUFFER(updateGraphicBufferInMeta, "%s:%u, %#x := %p",
            portString(portIndex), portIndex, buffer, graphicBuffer->handle);
    return OK;
}

status_t OMXNodeInstance::createInputSurface(
        OMX_U32 portIndex, sp<IGraphicBufferProducer> *bufferProducer) {
    Mutex::Autolock autolock(mLock);
    status_t err;

    // only allow graphic source on input port, when there are no allocated buffers yet
    if (portIndex != kPortIndexInput) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_VALUE;
    } else if (mNumPortBuffers[portIndex] > 0) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }

    const sp<GraphicBufferSource> surfaceCheck = getGraphicBufferSource();
    if (surfaceCheck != NULL) {
        return ALREADY_EXISTS;
    }

    // Input buffers will hold meta-data (gralloc references).
    OMX_BOOL usingGraphicBuffer = OMX_FALSE;
    err = storeMetaDataInBuffers_l(
            portIndex, OMX_TRUE,
            OMX_TRUE /* useGraphicBuffer */, &usingGraphicBuffer);
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

    GraphicBufferSource* bufferSource = new GraphicBufferSource(
            this, def.format.video.nFrameWidth, def.format.video.nFrameHeight,
            def.nBufferCountActual, usingGraphicBuffer);
    if ((err = bufferSource->initCheck()) != OK) {
        delete bufferSource;
        return err;
    }
    setGraphicBufferSource(bufferSource);

    *bufferProducer = bufferSource->getIGraphicBufferProducer();
    return OK;
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

status_t OMXNodeInstance::allocateBuffer(
        OMX_U32 portIndex, size_t size, OMX::buffer_id *buffer,
        void **buffer_data) {
    Mutex::Autolock autoLock(mLock);
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

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
    *buffer_data = header->pBuffer;

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }
    CLOG_BUFFER(allocateBuffer, NEW_BUFFER_FMT(*buffer, portIndex, "%zu@%p", size, *buffer_data));

    return OK;
}

status_t OMXNodeInstance::allocateBufferWithBackup(
        OMX_U32 portIndex, const sp<IMemory> &params,
        OMX::buffer_id *buffer, OMX_BOOL crossProcess) {
    Mutex::Autolock autoLock(mLock);
    if (portIndex >= NELEM(mUsingMetadata)) {
        return BAD_VALUE;
    }
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

    // metadata buffers are not connected cross process
    bool isMeta = mUsingMetadata[portIndex];
    bool copy = !(crossProcess && isMeta);

    BufferMeta *buffer_meta = new BufferMeta(
            params, portIndex,
            (portIndex == kPortIndexInput) && copy /* copyToOmx */,
            (portIndex == kPortIndexOutput) && copy /* copyFromOmx */,
            NULL /* data */);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_AllocateBuffer(
            mHandle, &header, portIndex, buffer_meta, params->size());

    if (err != OMX_ErrorNone) {
        CLOG_ERROR(allocateBufferWithBackup, err,
                SIMPLE_BUFFER(portIndex, params->size(), params->pointer()));
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

    CLOG_BUFFER(allocateBufferWithBackup, NEW_BUFFER_FMT(*buffer, portIndex, "%zu@%p :> %p",
            params->size(), params->pointer(), header->pBuffer));

    return OK;
}

status_t OMXNodeInstance::freeBuffer(
        OMX_U32 portIndex, OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);
    CLOG_BUFFER(freeBuffer, "%s:%u %#x", portString(portIndex), portIndex, buffer);

    removeActiveBuffer(portIndex, buffer);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, portIndex);
    if (header == NULL) {
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

status_t OMXNodeInstance::fillBuffer(OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, kPortIndexOutput);
    if (header == NULL) {
        return BAD_VALUE;
    }
    header->nFilledLen = 0;
    header->nOffset = 0;
    header->nFlags = 0;

    {
        Mutex::Autolock _l(mDebugLock);
        mOutputBuffersWithCodec.add(header);
        CLOG_BUMPED_BUFFER(fillBuffer, WITH_STATS(EMPTY_BUFFER(buffer, header)));
    }

    OMX_ERRORTYPE err = OMX_FillThisBuffer(mHandle, header);
    if (err != OMX_ErrorNone) {
        CLOG_ERROR(fillBuffer, err, EMPTY_BUFFER(buffer, header));
        Mutex::Autolock _l(mDebugLock);
        mOutputBuffersWithCodec.remove(header);
    }
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::emptyBuffer(
        OMX::buffer_id buffer,
        OMX_U32 rangeOffset, OMX_U32 rangeLength,
        OMX_U32 flags, OMX_TICKS timestamp) {
    Mutex::Autolock autoLock(mLock);

    // no emptybuffer if using input surface
    if (getGraphicBufferSource() != NULL) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }

    OMX_BUFFERHEADERTYPE *header = findBufferHeader(buffer, kPortIndexInput);
    // rangeLength and rangeOffset must be a subset of the allocated data in the buffer.
    // corner case: we permit rangeOffset == end-of-buffer with rangeLength == 0.
    if (header == NULL
            || rangeOffset > header->nAllocLen
            || rangeLength > header->nAllocLen - rangeOffset) {
        return BAD_VALUE;
    }
    header->nFilledLen = rangeLength;
    header->nOffset = rangeOffset;

    BufferMeta *buffer_meta =
        static_cast<BufferMeta *>(header->pAppPrivate);
    buffer_meta->CopyToOMX(header);

    return emptyBuffer_l(header, flags, timestamp, (intptr_t)buffer);
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

status_t OMXNodeInstance::emptyBuffer_l(
        OMX_BUFFERHEADERTYPE *header, OMX_U32 flags, OMX_TICKS timestamp, intptr_t debugAddr) {
    header->nFlags = flags;
    header->nTimeStamp = timestamp;

    {
        Mutex::Autolock _l(mDebugLock);
        mInputBuffersWithCodec.add(header);

        // bump internal-state debug level for 2 input frames past a buffer with CSD
        if ((flags & OMX_BUFFERFLAG_CODECCONFIG) != 0) {
            bumpDebugLevel_l(2 /* numInputBuffers */, 0 /* numOutputBuffers */);
        }

        CLOG_BUMPED_BUFFER(emptyBuffer, WITH_STATS(FULL_BUFFER(debugAddr, header)));
    }

    OMX_ERRORTYPE err = OMX_EmptyThisBuffer(mHandle, header);
    CLOG_IF_ERROR(emptyBuffer, err, FULL_BUFFER(debugAddr, header));

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
status_t OMXNodeInstance::emptyDirectBuffer(
        OMX_BUFFERHEADERTYPE *header,
        OMX_U32 rangeOffset, OMX_U32 rangeLength,
        OMX_U32 flags, OMX_TICKS timestamp) {
    Mutex::Autolock autoLock(mLock);

    header->nFilledLen = rangeLength;
    header->nOffset = rangeOffset;

    return emptyBuffer_l(header, flags, timestamp, (intptr_t)header->pBuffer);
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
        case IOMX::INTERNAL_OPTION_START_TIME:        return "START_TIME";
        case IOMX::INTERNAL_OPTION_TIME_LAPSE:        return "TIME_LAPSE";
        default:                                      return def;
    }
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
        case IOMX::INTERNAL_OPTION_START_TIME:
        case IOMX::INTERNAL_OPTION_TIME_LAPSE:
        {
            const sp<GraphicBufferSource> &bufferSource =
                getGraphicBufferSource();

            if (bufferSource == NULL || portIndex != kPortIndexInput) {
                CLOGW("setInternalOption is only for Surface input");
                return ERROR_UNSUPPORTED;
            }

            if (type == IOMX::INTERNAL_OPTION_SUSPEND) {
                if (size != sizeof(bool)) {
                    return INVALID_OPERATION;
                }

                bool suspend = *(bool *)data;
                CLOG_CONFIG(setInternalOption, "suspend=%d", suspend);
                bufferSource->suspend(suspend);
            } else if (type ==
                    IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY){
                if (size != sizeof(int64_t)) {
                    return INVALID_OPERATION;
                }

                int64_t delayUs = *(int64_t *)data;
                CLOG_CONFIG(setInternalOption, "delayUs=%lld", (long long)delayUs);
                return bufferSource->setRepeatPreviousFrameDelayUs(delayUs);
            } else if (type ==
                    IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP){
                if (size != sizeof(int64_t)) {
                    return INVALID_OPERATION;
                }

                int64_t maxGapUs = *(int64_t *)data;
                CLOG_CONFIG(setInternalOption, "gapUs=%lld", (long long)maxGapUs);
                return bufferSource->setMaxTimestampGapUs(maxGapUs);
            } else if (type == IOMX::INTERNAL_OPTION_START_TIME) {
                if (size != sizeof(int64_t)) {
                    return INVALID_OPERATION;
                }

                int64_t skipFramesBeforeUs = *(int64_t *)data;
                CLOG_CONFIG(setInternalOption, "beforeUs=%lld", (long long)skipFramesBeforeUs);
                bufferSource->setSkipFramesBeforeUs(skipFramesBeforeUs);
            } else { // IOMX::INTERNAL_OPTION_TIME_LAPSE
                if (size != sizeof(int64_t) * 2) {
                    return INVALID_OPERATION;
                }

                int64_t timePerFrameUs = ((int64_t *)data)[0];
                int64_t timePerCaptureUs = ((int64_t *)data)[1];
                CLOG_CONFIG(setInternalOption, "perFrameUs=%lld perCaptureUs=%lld",
                        (long long)timePerFrameUs, (long long)timePerCaptureUs);

                bufferSource->setTimeLapseUs((int64_t *)data);
            }

            return OK;
        }

        default:
            return ERROR_UNSUPPORTED;
    }
}

void OMXNodeInstance::onMessage(const omx_message &msg) {
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());

    if (msg.type == omx_message::FILL_BUFFER_DONE) {
        OMX_BUFFERHEADERTYPE *buffer =
            findBufferHeader(msg.u.extended_buffer_data.buffer, kPortIndexOutput);
        if (buffer == NULL) {
            return;
        }

        {
            Mutex::Autolock _l(mDebugLock);
            mOutputBuffersWithCodec.remove(buffer);

            CLOG_BUMPED_BUFFER(
                    FBD, WITH_STATS(FULL_BUFFER(msg.u.extended_buffer_data.buffer, buffer)));

            unbumpDebugLevel_l(kPortIndexOutput);
        }

        BufferMeta *buffer_meta =
            static_cast<BufferMeta *>(buffer->pAppPrivate);

        buffer_meta->CopyFromOMX(buffer);

        if (bufferSource != NULL) {
            // fix up the buffer info (especially timestamp) if needed
            bufferSource->codecBufferFilled(buffer);

            omx_message newMsg = msg;
            newMsg.u.extended_buffer_data.timestamp = buffer->nTimeStamp;
            mObserver->onMessage(newMsg);
            return;
        }
    } else if (msg.type == omx_message::EMPTY_BUFFER_DONE) {
        OMX_BUFFERHEADERTYPE *buffer =
            findBufferHeader(msg.u.buffer_data.buffer, kPortIndexInput);
        if (buffer == NULL) {
            return;
        }

        {
            Mutex::Autolock _l(mDebugLock);
            mInputBuffersWithCodec.remove(buffer);

            CLOG_BUMPED_BUFFER(
                    EBD, WITH_STATS(EMPTY_BUFFER(msg.u.buffer_data.buffer, buffer)));
        }

        if (bufferSource != NULL) {
            // This is one of the buffers used exclusively by
            // GraphicBufferSource.
            // Don't dispatch a message back to ACodec, since it doesn't
            // know that anyone asked to have the buffer emptied and will
            // be very confused.

            OMX_BUFFERHEADERTYPE *buffer =
                findBufferHeader(msg.u.buffer_data.buffer, kPortIndexInput);
            if (buffer == NULL) {
                return;
            }

            bufferSource->codecBufferEmptied(buffer);
            return;
        }
    }

    mObserver->onMessage(msg);
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

    // allow configuration if we return to the loaded state
    if (event == OMX_EventCmdComplete
            && arg1 == OMX_CommandStateSet
            && arg2 == OMX_StateLoaded) {
        mSailed = false;
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
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnEmptyBufferDone(instance->nodeID(),
            instance->findBufferID(pBuffer), pBuffer);
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnFillBufferDone(
        OMX_IN OMX_HANDLETYPE /* hComponent */,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnFillBufferDone(instance->nodeID(),
            instance->findBufferID(pBuffer), pBuffer);
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
    for (size_t i = mActiveBuffers.size(); i--;) {
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
        ALOGW("findBufferHeader: buffer %u not found", buffer);
        return NULL;
    }
    OMX_BUFFERHEADERTYPE *header = mBufferIDToBufferHeader.valueAt(index);
    BufferMeta *buffer_meta =
        static_cast<BufferMeta *>(header->pAppPrivate);
    if (buffer_meta->getPortIndex() != portIndex) {
        ALOGW("findBufferHeader: buffer %u found but with incorrect port index.", buffer);
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
    return mBufferHeaderToBufferID.valueFor(bufferHeader);
}

void OMXNodeInstance::invalidateBufferID(OMX::buffer_id buffer) {
    if (buffer == 0) {
        return;
    }
    Mutex::Autolock autoLock(mBufferIDLock);
    mBufferHeaderToBufferID.removeItem(mBufferIDToBufferHeader.valueFor(buffer));
    mBufferIDToBufferHeader.removeItem(buffer);
}

}  // namespace android
