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

#ifndef OMX_NODE_INSTANCE_H_

#define OMX_NODE_INSTANCE_H_

#include "OMX.h"

#include <utils/RefBase.h>
#include <utils/SortedVector.h>
#include <utils/threads.h>

namespace android {

class IOMXObserver;
struct OMXMaster;
class GraphicBufferSource;

struct OMXNodeInstance {
    OMXNodeInstance(
            OMX *owner, const sp<IOMXObserver> &observer, const char *name);

    void setHandle(OMX::node_id node_id, OMX_HANDLETYPE handle);

    OMX *owner();
    sp<IOMXObserver> observer();
    OMX::node_id nodeID();

    status_t freeNode(OMXMaster *master);

    status_t sendCommand(OMX_COMMANDTYPE cmd, OMX_S32 param);
    status_t getParameter(OMX_INDEXTYPE index, void *params, size_t size);

    status_t setParameter(
            OMX_INDEXTYPE index, const void *params, size_t size);

    status_t getConfig(OMX_INDEXTYPE index, void *params, size_t size);
    status_t setConfig(OMX_INDEXTYPE index, const void *params, size_t size);

    status_t getState(OMX_STATETYPE* state);

    status_t enableNativeBuffers(OMX_U32 portIndex, OMX_BOOL graphic, OMX_BOOL enable);

    status_t getGraphicBufferUsage(OMX_U32 portIndex, OMX_U32* usage);

    status_t storeMetaDataInBuffers(
            OMX_U32 portIndex, OMX_BOOL enable, MetadataBufferType *type);

    status_t prepareForAdaptivePlayback(
            OMX_U32 portIndex, OMX_BOOL enable,
            OMX_U32 maxFrameWidth, OMX_U32 maxFrameHeight);

    status_t configureVideoTunnelMode(
            OMX_U32 portIndex, OMX_BOOL tunneled,
            OMX_U32 audioHwSync, native_handle_t **sidebandHandle);

    status_t useBuffer(
            OMX_U32 portIndex, const sp<IMemory> &params,
            OMX::buffer_id *buffer, OMX_U32 allottedSize);

    status_t useGraphicBuffer(
            OMX_U32 portIndex, const sp<GraphicBuffer> &graphicBuffer,
            OMX::buffer_id *buffer);

    status_t updateGraphicBufferInMeta(
            OMX_U32 portIndex, const sp<GraphicBuffer> &graphicBuffer,
            OMX::buffer_id buffer);

    status_t updateNativeHandleInMeta(
            OMX_U32 portIndex, const sp<NativeHandle> &nativeHandle,
            OMX::buffer_id buffer);

    status_t createInputSurface(
            OMX_U32 portIndex, android_dataspace dataSpace,
            sp<IGraphicBufferProducer> *bufferProducer,
            MetadataBufferType *type);

    static status_t createPersistentInputSurface(
            sp<IGraphicBufferProducer> *bufferProducer,
            sp<IGraphicBufferConsumer> *bufferConsumer);

    status_t setInputSurface(
            OMX_U32 portIndex, const sp<IGraphicBufferConsumer> &bufferConsumer,
            MetadataBufferType *type);

    status_t signalEndOfInputStream();

    void signalEvent(OMX_EVENTTYPE event, OMX_U32 arg1, OMX_U32 arg2);

    status_t allocateSecureBuffer(
            OMX_U32 portIndex, size_t size, OMX::buffer_id *buffer,
            void **buffer_data, sp<NativeHandle> *native_handle);

    status_t allocateBufferWithBackup(
            OMX_U32 portIndex, const sp<IMemory> &params,
            OMX::buffer_id *buffer, OMX_U32 allottedSize);

    status_t freeBuffer(OMX_U32 portIndex, OMX::buffer_id buffer);

    status_t fillBuffer(OMX::buffer_id buffer, int fenceFd);

    status_t emptyBuffer(
            OMX::buffer_id buffer,
            OMX_U32 rangeOffset, OMX_U32 rangeLength,
            OMX_U32 flags, OMX_TICKS timestamp, int fenceFd);

    status_t emptyGraphicBuffer(
            OMX_BUFFERHEADERTYPE *header, const sp<GraphicBuffer> &buffer,
            OMX_U32 flags, OMX_TICKS timestamp, int fenceFd);

    status_t getExtensionIndex(
            const char *parameterName, OMX_INDEXTYPE *index);

    status_t setInternalOption(
            OMX_U32 portIndex,
            IOMX::InternalOptionType type,
            const void *data,
            size_t size);

    bool isSecure() const {
        return mIsSecure;
    }

    // handles messages and removes them from the list
    void onMessages(std::list<omx_message> &messages);
    void onMessage(const omx_message &msg);
    void onObserverDied(OMXMaster *master);
    void onGetHandleFailed();
    void onEvent(OMX_EVENTTYPE event, OMX_U32 arg1, OMX_U32 arg2);

    static OMX_CALLBACKTYPE kCallbacks;

private:
    Mutex mLock;

    OMX *mOwner;
    OMX::node_id mNodeID;
    OMX_HANDLETYPE mHandle;
    sp<IOMXObserver> mObserver;
    atomic_bool mDying;
    bool mSailed;  // configuration is set (no more meta-mode changes)
    bool mQueriedProhibitedExtensions;
    SortedVector<OMX_INDEXTYPE> mProhibitedExtensions;
    bool mIsSecure;

    // Lock only covers mGraphicBufferSource.  We can't always use mLock
    // because of rare instances where we'd end up locking it recursively.
    Mutex mGraphicBufferSourceLock;
    // Access this through getGraphicBufferSource().
    sp<GraphicBufferSource> mGraphicBufferSource;


    struct ActiveBuffer {
        OMX_U32 mPortIndex;
        OMX::buffer_id mID;
    };
    Vector<ActiveBuffer> mActiveBuffers;
    // for buffer ptr to buffer id translation
    Mutex mBufferIDLock;
    uint32_t mBufferIDCount;
    KeyedVector<OMX::buffer_id, OMX_BUFFERHEADERTYPE *> mBufferIDToBufferHeader;
    KeyedVector<OMX_BUFFERHEADERTYPE *, OMX::buffer_id> mBufferHeaderToBufferID;

    // metadata and secure buffer type tracking
    MetadataBufferType mMetadataType[2];
    enum SecureBufferType {
        kSecureBufferTypeUnknown,
        kSecureBufferTypeOpaque,
        kSecureBufferTypeNativeHandle,
    };
    SecureBufferType mSecureBufferType[2];

    // For debug support
    char *mName;
    int DEBUG;
    size_t mNumPortBuffers[2];  // modified under mLock, read outside for debug
    Mutex mDebugLock;
    // following are modified and read under mDebugLock
    int DEBUG_BUMP;
    SortedVector<OMX_BUFFERHEADERTYPE *> mInputBuffersWithCodec, mOutputBuffersWithCodec;
    size_t mDebugLevelBumpPendingBuffers[2];
    void bumpDebugLevel_l(size_t numInputBuffers, size_t numOutputBuffers);
    void unbumpDebugLevel_l(size_t portIndex);

    ~OMXNodeInstance();

    void addActiveBuffer(OMX_U32 portIndex, OMX::buffer_id id);
    void removeActiveBuffer(OMX_U32 portIndex, OMX::buffer_id id);
    void freeActiveBuffers();

    // For buffer id management
    OMX::buffer_id makeBufferID(OMX_BUFFERHEADERTYPE *bufferHeader);
    OMX_BUFFERHEADERTYPE *findBufferHeader(OMX::buffer_id buffer, OMX_U32 portIndex);
    OMX::buffer_id findBufferID(OMX_BUFFERHEADERTYPE *bufferHeader);
    void invalidateBufferID(OMX::buffer_id buffer);

    bool isProhibitedIndex_l(OMX_INDEXTYPE index);

    status_t useGraphicBuffer2_l(
            OMX_U32 portIndex, const sp<GraphicBuffer> &graphicBuffer,
            OMX::buffer_id *buffer);
    static OMX_ERRORTYPE OnEvent(
            OMX_IN OMX_HANDLETYPE hComponent,
            OMX_IN OMX_PTR pAppData,
            OMX_IN OMX_EVENTTYPE eEvent,
            OMX_IN OMX_U32 nData1,
            OMX_IN OMX_U32 nData2,
            OMX_IN OMX_PTR pEventData);

    static OMX_ERRORTYPE OnEmptyBufferDone(
            OMX_IN OMX_HANDLETYPE hComponent,
            OMX_IN OMX_PTR pAppData,
            OMX_IN OMX_BUFFERHEADERTYPE *pBuffer);

    static OMX_ERRORTYPE OnFillBufferDone(
            OMX_IN OMX_HANDLETYPE hComponent,
            OMX_IN OMX_PTR pAppData,
            OMX_IN OMX_BUFFERHEADERTYPE *pBuffer);

    status_t storeMetaDataInBuffers_l(
            OMX_U32 portIndex, OMX_BOOL enable, MetadataBufferType *type);

    // Stores fence into buffer if it is ANWBuffer type and has enough space.
    // otherwise, waits for the fence to signal.  Takes ownership of |fenceFd|.
    status_t storeFenceInMeta_l(
            OMX_BUFFERHEADERTYPE *header, int fenceFd, OMX_U32 portIndex);

    // Retrieves the fence from buffer if ANWBuffer type and has enough space. Otherwise, returns -1
    int retrieveFenceFromMeta_l(
            OMX_BUFFERHEADERTYPE *header, OMX_U32 portIndex);

    status_t emptyBuffer_l(
            OMX_BUFFERHEADERTYPE *header,
            OMX_U32 flags, OMX_TICKS timestamp, intptr_t debugAddr, int fenceFd);

    // Updates the graphic buffer handle in the metadata buffer for |buffer| and |header| to
    // |graphicBuffer|'s handle. If |updateCodecBuffer| is true, the update will happen in
    // the actual codec buffer (use this if not using emptyBuffer (with no _l) later to
    // pass the buffer to the codec, as only emptyBuffer copies the backup buffer to the codec
    // buffer.)
    status_t updateGraphicBufferInMeta_l(
            OMX_U32 portIndex, const sp<GraphicBuffer> &graphicBuffer,
            OMX::buffer_id buffer, OMX_BUFFERHEADERTYPE *header, bool updateCodecBuffer);

    status_t createGraphicBufferSource(
            OMX_U32 portIndex, sp<IGraphicBufferConsumer> consumer /* nullable */,
            MetadataBufferType *type);
    sp<GraphicBufferSource> getGraphicBufferSource();
    void setGraphicBufferSource(const sp<GraphicBufferSource>& bufferSource);

    // Handles |msg|, and may modify it. Returns true iff completely handled it and
    // |msg| does not need to be sent to the event listener.
    bool handleMessage(omx_message &msg);

    OMXNodeInstance(const OMXNodeInstance &);
    OMXNodeInstance &operator=(const OMXNodeInstance &);
};

}  // namespace android

#endif  // OMX_NODE_INSTANCE_H_
