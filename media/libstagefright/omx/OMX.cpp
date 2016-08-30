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

#include <inttypes.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "OMX"
#include <utils/Log.h>

#include <dlfcn.h>

#include "../include/OMX.h"

#include "../include/OMXNodeInstance.h"

#include <binder/IMemory.h>
#include <media/stagefright/foundation/ADebug.h>
#include <utils/threads.h>

#include "OMXMaster.h"
#include "OMXUtils.h"

#include <OMX_AsString.h>
#include <OMX_Component.h>
#include <OMX_VideoExt.h>

namespace android {

// node ids are created by concatenating the pid with a 16-bit counter
static size_t kMaxNodeInstances = (1 << 16);

////////////////////////////////////////////////////////////////////////////////

// This provides the underlying Thread used by CallbackDispatcher.
// Note that deriving CallbackDispatcher from Thread does not work.

struct OMX::CallbackDispatcherThread : public Thread {
    CallbackDispatcherThread(CallbackDispatcher *dispatcher)
        : mDispatcher(dispatcher) {
    }

private:
    CallbackDispatcher *mDispatcher;

    bool threadLoop();

    CallbackDispatcherThread(const CallbackDispatcherThread &);
    CallbackDispatcherThread &operator=(const CallbackDispatcherThread &);
};

////////////////////////////////////////////////////////////////////////////////

struct OMX::CallbackDispatcher : public RefBase {
    CallbackDispatcher(OMXNodeInstance *owner);

    // Posts |msg| to the listener's queue. If |realTime| is true, the listener thread is notified
    // that a new message is available on the queue. Otherwise, the message stays on the queue, but
    // the listener is not notified of it. It will process this message when a subsequent message
    // is posted with |realTime| set to true.
    void post(const omx_message &msg, bool realTime = true);

    bool loop();

protected:
    virtual ~CallbackDispatcher();

private:
    Mutex mLock;

    OMXNodeInstance *mOwner;
    bool mDone;
    Condition mQueueChanged;
    std::list<omx_message> mQueue;

    sp<CallbackDispatcherThread> mThread;

    void dispatch(std::list<omx_message> &messages);

    CallbackDispatcher(const CallbackDispatcher &);
    CallbackDispatcher &operator=(const CallbackDispatcher &);
};

OMX::CallbackDispatcher::CallbackDispatcher(OMXNodeInstance *owner)
    : mOwner(owner),
      mDone(false) {
    mThread = new CallbackDispatcherThread(this);
    mThread->run("OMXCallbackDisp", ANDROID_PRIORITY_FOREGROUND);
}

OMX::CallbackDispatcher::~CallbackDispatcher() {
    {
        Mutex::Autolock autoLock(mLock);

        mDone = true;
        mQueueChanged.signal();
    }

    // A join on self can happen if the last ref to CallbackDispatcher
    // is released within the CallbackDispatcherThread loop
    status_t status = mThread->join();
    if (status != WOULD_BLOCK) {
        // Other than join to self, the only other error return codes are
        // whatever readyToRun() returns, and we don't override that
        CHECK_EQ(status, (status_t)NO_ERROR);
    }
}

void OMX::CallbackDispatcher::post(const omx_message &msg, bool realTime) {
    Mutex::Autolock autoLock(mLock);

    mQueue.push_back(msg);
    if (realTime) {
        mQueueChanged.signal();
    }
}

void OMX::CallbackDispatcher::dispatch(std::list<omx_message> &messages) {
    if (mOwner == NULL) {
        ALOGV("Would have dispatched a message to a node that's already gone.");
        return;
    }
    mOwner->onMessages(messages);
}

bool OMX::CallbackDispatcher::loop() {
    for (;;) {
        std::list<omx_message> messages;

        {
            Mutex::Autolock autoLock(mLock);
            while (!mDone && mQueue.empty()) {
                mQueueChanged.wait(mLock);
            }

            if (mDone) {
                break;
            }

            messages.swap(mQueue);
        }

        dispatch(messages);
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////

bool OMX::CallbackDispatcherThread::threadLoop() {
    return mDispatcher->loop();
}

////////////////////////////////////////////////////////////////////////////////

OMX::OMX()
    : mMaster(new OMXMaster),
      mNodeCounter(0) {
}

OMX::~OMX() {
    delete mMaster;
    mMaster = NULL;
}

void OMX::binderDied(const wp<IBinder> &the_late_who) {
    OMXNodeInstance *instance;

    {
        Mutex::Autolock autoLock(mLock);

        ssize_t index = mLiveNodes.indexOfKey(the_late_who);

        if (index < 0) {
            ALOGE("b/27597103, nonexistent observer on binderDied");
            android_errorWriteLog(0x534e4554, "27597103");
            return;
        }

        instance = mLiveNodes.editValueAt(index);
        mLiveNodes.removeItemsAt(index);

        index = mDispatchers.indexOfKey(instance->nodeID());
        CHECK(index >= 0);
        mDispatchers.removeItemsAt(index);

        invalidateNodeID_l(instance->nodeID());
    }

    instance->onObserverDied(mMaster);
}

bool OMX::isSecure(node_id node) {
    OMXNodeInstance *instance = findInstance(node);
    return (instance == NULL ? false : instance->isSecure());
}

bool OMX::livesLocally(node_id /* node */, pid_t pid) {
    return pid == getpid();
}

status_t OMX::listNodes(List<ComponentInfo> *list) {
    list->clear();

    OMX_U32 index = 0;
    char componentName[256];
    while (mMaster->enumerateComponents(
                componentName, sizeof(componentName), index) == OMX_ErrorNone) {
        list->push_back(ComponentInfo());
        ComponentInfo &info = *--list->end();

        info.mName = componentName;

        Vector<String8> roles;
        OMX_ERRORTYPE err =
            mMaster->getRolesOfComponent(componentName, &roles);

        if (err == OMX_ErrorNone) {
            for (OMX_U32 i = 0; i < roles.size(); ++i) {
                info.mRoles.push_back(roles[i]);
            }
        }

        ++index;
    }

    return OK;
}

status_t OMX::allocateNode(
        const char *name, const sp<IOMXObserver> &observer,
        sp<IBinder> *nodeBinder, node_id *node) {
    Mutex::Autolock autoLock(mLock);

    *node = 0;
    if (nodeBinder != NULL) {
        *nodeBinder = NULL;
    }

    if (mNodeIDToInstance.size() == kMaxNodeInstances) {
        // all possible node IDs are in use
        return NO_MEMORY;
    }

    OMXNodeInstance *instance = new OMXNodeInstance(this, observer, name);

    OMX_COMPONENTTYPE *handle;
    OMX_ERRORTYPE err = mMaster->makeComponentInstance(
            name, &OMXNodeInstance::kCallbacks,
            instance, &handle);

    if (err != OMX_ErrorNone) {
        ALOGE("FAILED to allocate omx component '%s' err=%s(%#x)", name, asString(err), err);

        instance->onGetHandleFailed();

        return StatusFromOMXError(err);
    }

    *node = makeNodeID_l(instance);
    mDispatchers.add(*node, new CallbackDispatcher(instance));

    instance->setHandle(*node, handle);

    mLiveNodes.add(IInterface::asBinder(observer), instance);
    IInterface::asBinder(observer)->linkToDeath(this);

    return OK;
}

status_t OMX::freeNode(node_id node) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return OK;
    }

    {
        Mutex::Autolock autoLock(mLock);
        ssize_t index = mLiveNodes.indexOfKey(IInterface::asBinder(instance->observer()));
        if (index < 0) {
            // This could conceivably happen if the observer dies at roughly the
            // same time that a client attempts to free the node explicitly.
            return OK;
        }
        mLiveNodes.removeItemsAt(index);
    }

    IInterface::asBinder(instance->observer())->unlinkToDeath(this);

    status_t err = instance->freeNode(mMaster);

    {
        Mutex::Autolock autoLock(mLock);
        ssize_t index = mDispatchers.indexOfKey(node);
        CHECK(index >= 0);
        mDispatchers.removeItemsAt(index);
    }

    return err;
}

status_t OMX::sendCommand(
        node_id node, OMX_COMMANDTYPE cmd, OMX_S32 param) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->sendCommand(cmd, param);
}

status_t OMX::getParameter(
        node_id node, OMX_INDEXTYPE index,
        void *params, size_t size) {
    ALOGV("getParameter(%u %#x %p %zd)", node, index, params, size);
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->getParameter(
            index, params, size);
}

status_t OMX::setParameter(
        node_id node, OMX_INDEXTYPE index,
        const void *params, size_t size) {
    ALOGV("setParameter(%u %#x %p %zd)", node, index, params, size);
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->setParameter(
            index, params, size);
}

status_t OMX::getConfig(
        node_id node, OMX_INDEXTYPE index,
        void *params, size_t size) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->getConfig(
            index, params, size);
}

status_t OMX::setConfig(
        node_id node, OMX_INDEXTYPE index,
        const void *params, size_t size) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->setConfig(
            index, params, size);
}

status_t OMX::getState(
        node_id node, OMX_STATETYPE* state) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->getState(
            state);
}

status_t OMX::enableNativeBuffers(
        node_id node, OMX_U32 port_index, OMX_BOOL graphic, OMX_BOOL enable) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->enableNativeBuffers(port_index, graphic, enable);
}

status_t OMX::getGraphicBufferUsage(
        node_id node, OMX_U32 port_index, OMX_U32* usage) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->getGraphicBufferUsage(port_index, usage);
}

status_t OMX::storeMetaDataInBuffers(
        node_id node, OMX_U32 port_index, OMX_BOOL enable, MetadataBufferType *type) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->storeMetaDataInBuffers(port_index, enable, type);
}

status_t OMX::prepareForAdaptivePlayback(
        node_id node, OMX_U32 portIndex, OMX_BOOL enable,
        OMX_U32 maxFrameWidth, OMX_U32 maxFrameHeight) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->prepareForAdaptivePlayback(
            portIndex, enable, maxFrameWidth, maxFrameHeight);
}

status_t OMX::configureVideoTunnelMode(
        node_id node, OMX_U32 portIndex, OMX_BOOL tunneled,
        OMX_U32 audioHwSync, native_handle_t **sidebandHandle) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->configureVideoTunnelMode(
            portIndex, tunneled, audioHwSync, sidebandHandle);
}

status_t OMX::useBuffer(
        node_id node, OMX_U32 port_index, const sp<IMemory> &params,
        buffer_id *buffer, OMX_U32 allottedSize) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->useBuffer(
            port_index, params, buffer, allottedSize);
}

status_t OMX::useGraphicBuffer(
        node_id node, OMX_U32 port_index,
        const sp<GraphicBuffer> &graphicBuffer, buffer_id *buffer) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->useGraphicBuffer(
            port_index, graphicBuffer, buffer);
}

status_t OMX::updateGraphicBufferInMeta(
        node_id node, OMX_U32 port_index,
        const sp<GraphicBuffer> &graphicBuffer, buffer_id buffer) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->updateGraphicBufferInMeta(
            port_index, graphicBuffer, buffer);
}

status_t OMX::updateNativeHandleInMeta(
        node_id node, OMX_U32 port_index,
        const sp<NativeHandle> &nativeHandle, buffer_id buffer) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->updateNativeHandleInMeta(
            port_index, nativeHandle, buffer);
}

status_t OMX::createInputSurface(
        node_id node, OMX_U32 port_index, android_dataspace dataSpace,
        sp<IGraphicBufferProducer> *bufferProducer, MetadataBufferType *type) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->createInputSurface(
            port_index, dataSpace, bufferProducer, type);
}

status_t OMX::createPersistentInputSurface(
        sp<IGraphicBufferProducer> *bufferProducer,
        sp<IGraphicBufferConsumer> *bufferConsumer) {
    return OMXNodeInstance::createPersistentInputSurface(
            bufferProducer, bufferConsumer);
}

status_t OMX::setInputSurface(
        node_id node, OMX_U32 port_index,
        const sp<IGraphicBufferConsumer> &bufferConsumer, MetadataBufferType *type) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->setInputSurface(port_index, bufferConsumer, type);
}


status_t OMX::signalEndOfInputStream(node_id node) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->signalEndOfInputStream();
}

status_t OMX::allocateSecureBuffer(
        node_id node, OMX_U32 port_index, size_t size,
        buffer_id *buffer, void **buffer_data, sp<NativeHandle> *native_handle) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->allocateSecureBuffer(
            port_index, size, buffer, buffer_data, native_handle);
}

status_t OMX::allocateBufferWithBackup(
        node_id node, OMX_U32 port_index, const sp<IMemory> &params,
        buffer_id *buffer, OMX_U32 allottedSize) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->allocateBufferWithBackup(
            port_index, params, buffer, allottedSize);
}

status_t OMX::freeBuffer(node_id node, OMX_U32 port_index, buffer_id buffer) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->freeBuffer(
            port_index, buffer);
}

status_t OMX::fillBuffer(node_id node, buffer_id buffer, int fenceFd) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->fillBuffer(buffer, fenceFd);
}

status_t OMX::emptyBuffer(
        node_id node,
        buffer_id buffer,
        OMX_U32 range_offset, OMX_U32 range_length,
        OMX_U32 flags, OMX_TICKS timestamp, int fenceFd) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->emptyBuffer(
            buffer, range_offset, range_length, flags, timestamp, fenceFd);
}

status_t OMX::getExtensionIndex(
        node_id node,
        const char *parameter_name,
        OMX_INDEXTYPE *index) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->getExtensionIndex(
            parameter_name, index);
}

status_t OMX::setInternalOption(
        node_id node,
        OMX_U32 port_index,
        InternalOptionType type,
        const void *data,
        size_t size) {
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return NAME_NOT_FOUND;
    }

    return instance->setInternalOption(port_index, type, data, size);
}

OMX_ERRORTYPE OMX::OnEvent(
        node_id node,
        OMX_IN OMX_EVENTTYPE eEvent,
        OMX_IN OMX_U32 nData1,
        OMX_IN OMX_U32 nData2,
        OMX_IN OMX_PTR pEventData) {
    ALOGV("OnEvent(%d, %" PRIu32", %" PRIu32 ")", eEvent, nData1, nData2);
    OMXNodeInstance *instance = findInstance(node);

    if (instance == NULL) {
        return OMX_ErrorComponentNotFound;
    }

    // Forward to OMXNodeInstance.
    instance->onEvent(eEvent, nData1, nData2);

    sp<OMX::CallbackDispatcher> dispatcher = findDispatcher(node);
    if (dispatcher == NULL) {
       ALOGW("OnEvent Callback dispatcher NULL, skip post");
       return OMX_ErrorNone;
    }

    // output rendered events are not processed as regular events until they hit the observer
    if (eEvent == OMX_EventOutputRendered) {
        if (pEventData == NULL) {
            return OMX_ErrorBadParameter;
        }

        // process data from array
        OMX_VIDEO_RENDEREVENTTYPE *renderData = (OMX_VIDEO_RENDEREVENTTYPE *)pEventData;
        for (size_t i = 0; i < nData1; ++i) {
            omx_message msg;
            msg.type = omx_message::FRAME_RENDERED;
            msg.node = node;
            msg.fenceFd = -1;
            msg.u.render_data.timestamp = renderData[i].nMediaTimeUs;
            msg.u.render_data.nanoTime = renderData[i].nSystemTimeNs;

            dispatcher->post(msg, false /* realTime */);
        }
        return OMX_ErrorNone;
    }

    omx_message msg;
    msg.type = omx_message::EVENT;
    msg.node = node;
    msg.fenceFd = -1;
    msg.u.event_data.event = eEvent;
    msg.u.event_data.data1 = nData1;
    msg.u.event_data.data2 = nData2;

    dispatcher->post(msg, true /* realTime */);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX::OnEmptyBufferDone(
        node_id node, buffer_id buffer, OMX_IN OMX_BUFFERHEADERTYPE *pBuffer, int fenceFd) {
    ALOGV("OnEmptyBufferDone buffer=%p", pBuffer);

    omx_message msg;
    msg.type = omx_message::EMPTY_BUFFER_DONE;
    msg.node = node;
    msg.fenceFd = fenceFd;
    msg.u.buffer_data.buffer = buffer;

    sp<OMX::CallbackDispatcher> callbackDispatcher = findDispatcher(node);
    if (callbackDispatcher != NULL) {
        callbackDispatcher->post(msg);
    } else {
        ALOGW("OnEmptyBufferDone Callback dispatcher NULL, skip post");
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX::OnFillBufferDone(
        node_id node, buffer_id buffer, OMX_IN OMX_BUFFERHEADERTYPE *pBuffer, int fenceFd) {
    ALOGV("OnFillBufferDone buffer=%p", pBuffer);

    omx_message msg;
    msg.type = omx_message::FILL_BUFFER_DONE;
    msg.node = node;
    msg.fenceFd = fenceFd;
    msg.u.extended_buffer_data.buffer = buffer;
    msg.u.extended_buffer_data.range_offset = pBuffer->nOffset;
    msg.u.extended_buffer_data.range_length = pBuffer->nFilledLen;
    msg.u.extended_buffer_data.flags = pBuffer->nFlags;
    msg.u.extended_buffer_data.timestamp = pBuffer->nTimeStamp;

    sp<OMX::CallbackDispatcher> callbackDispatcher = findDispatcher(node);
    if (callbackDispatcher != NULL) {
        callbackDispatcher->post(msg);
    } else {
        ALOGW("OnFillBufferDone Callback dispatcher NULL, skip post");
    }

    return OMX_ErrorNone;
}

OMX::node_id OMX::makeNodeID_l(OMXNodeInstance *instance) {
    // mLock is already held.

    node_id prefix = node_id(getpid() << 16);
    node_id node = 0;
    do  {
        if (++mNodeCounter >= kMaxNodeInstances) {
            mNodeCounter = 0; // OK to use because we're combining with the pid
        }
        node = node_id(prefix | mNodeCounter);
    } while (mNodeIDToInstance.indexOfKey(node) >= 0);
    mNodeIDToInstance.add(node, instance);

    return node;
}

OMXNodeInstance *OMX::findInstance(node_id node) {
    Mutex::Autolock autoLock(mLock);

    ssize_t index = mNodeIDToInstance.indexOfKey(node);

    return index < 0 ? NULL : mNodeIDToInstance.valueAt(index);
}

sp<OMX::CallbackDispatcher> OMX::findDispatcher(node_id node) {
    Mutex::Autolock autoLock(mLock);

    ssize_t index = mDispatchers.indexOfKey(node);

    return index < 0 ? NULL : mDispatchers.valueAt(index);
}

void OMX::invalidateNodeID(node_id node) {
    Mutex::Autolock autoLock(mLock);
    invalidateNodeID_l(node);
}

void OMX::invalidateNodeID_l(node_id node) {
    // mLock is held.
    mNodeIDToInstance.removeItem(node);
}

}  // namespace android
