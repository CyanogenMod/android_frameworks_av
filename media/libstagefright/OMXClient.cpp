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
#define LOG_TAG "OMXClient"

#ifdef __LP64__
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#endif

#include <utils/Log.h>

#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <media/IMediaCodecService.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/OMXClient.h>
#include <cutils/properties.h>
#include <utils/KeyedVector.h>

#include "include/OMX.h"

namespace android {

static bool sCodecProcessEnabled = true;

struct MuxOMX : public IOMX {
    MuxOMX(const sp<IOMX> &mediaServerOMX, const sp<IOMX> &mediaCodecOMX);
    virtual ~MuxOMX();

    // Nobody should be calling this. In case someone does anyway, just
    // return the media server IOMX.
    // TODO: return NULL
    virtual IBinder *onAsBinder() {
        ALOGE("MuxOMX::onAsBinder should not be called");
        return IInterface::asBinder(mMediaServerOMX).get();
    }

    virtual bool livesLocally(node_id node, pid_t pid);

    virtual status_t listNodes(List<ComponentInfo> *list);

    virtual status_t allocateNode(
            const char *name, const sp<IOMXObserver> &observer,
            sp<IBinder> *nodeBinder,
            node_id *node);

    virtual status_t freeNode(node_id node);

    virtual status_t sendCommand(
            node_id node, OMX_COMMANDTYPE cmd, OMX_S32 param);

    virtual status_t getParameter(
            node_id node, OMX_INDEXTYPE index,
            void *params, size_t size);

    virtual status_t setParameter(
            node_id node, OMX_INDEXTYPE index,
            const void *params, size_t size);

    virtual status_t getConfig(
            node_id node, OMX_INDEXTYPE index,
            void *params, size_t size);

    virtual status_t setConfig(
            node_id node, OMX_INDEXTYPE index,
            const void *params, size_t size);

    virtual status_t getState(
            node_id node, OMX_STATETYPE* state);

    virtual status_t storeMetaDataInBuffers(
            node_id node, OMX_U32 port_index, OMX_BOOL enable, MetadataBufferType *type);

    virtual status_t prepareForAdaptivePlayback(
            node_id node, OMX_U32 port_index, OMX_BOOL enable,
            OMX_U32 maxFrameWidth, OMX_U32 maxFrameHeight);

    virtual status_t configureVideoTunnelMode(
            node_id node, OMX_U32 portIndex, OMX_BOOL tunneled,
            OMX_U32 audioHwSync, native_handle_t **sidebandHandle);

    virtual status_t enableNativeBuffers(
            node_id node, OMX_U32 port_index, OMX_BOOL graphic, OMX_BOOL enable);

    virtual status_t getGraphicBufferUsage(
            node_id node, OMX_U32 port_index, OMX_U32* usage);

    virtual status_t useBuffer(
            node_id node, OMX_U32 port_index, const sp<IMemory> &params,
            buffer_id *buffer, OMX_U32 allottedSize);

    virtual status_t useGraphicBuffer(
            node_id node, OMX_U32 port_index,
            const sp<GraphicBuffer> &graphicBuffer, buffer_id *buffer);

    virtual status_t updateGraphicBufferInMeta(
            node_id node, OMX_U32 port_index,
            const sp<GraphicBuffer> &graphicBuffer, buffer_id buffer);

    virtual status_t updateNativeHandleInMeta(
            node_id node, OMX_U32 port_index,
            const sp<NativeHandle> &nativeHandle, buffer_id buffer);

    virtual status_t createInputSurface(
            node_id node, OMX_U32 port_index, android_dataspace dataSpace,
            sp<IGraphicBufferProducer> *bufferProducer, MetadataBufferType *type);

    virtual status_t createPersistentInputSurface(
            sp<IGraphicBufferProducer> *bufferProducer,
            sp<IGraphicBufferConsumer> *bufferConsumer);

    virtual status_t setInputSurface(
            node_id node, OMX_U32 port_index,
            const sp<IGraphicBufferConsumer> &bufferConsumer, MetadataBufferType *type);

    virtual status_t signalEndOfInputStream(node_id node);

    virtual status_t allocateSecureBuffer(
            node_id node, OMX_U32 port_index, size_t size,
            buffer_id *buffer, void **buffer_data, sp<NativeHandle> *native_handle);

    virtual status_t allocateBufferWithBackup(
            node_id node, OMX_U32 port_index, const sp<IMemory> &params,
            buffer_id *buffer, OMX_U32 allottedSize);

    virtual status_t freeBuffer(
            node_id node, OMX_U32 port_index, buffer_id buffer);

    virtual status_t fillBuffer(node_id node, buffer_id buffer, int fenceFd);

    virtual status_t emptyBuffer(
            node_id node,
            buffer_id buffer,
            OMX_U32 range_offset, OMX_U32 range_length,
            OMX_U32 flags, OMX_TICKS timestamp, int fenceFd);

    virtual status_t getExtensionIndex(
            node_id node,
            const char *parameter_name,
            OMX_INDEXTYPE *index);

    virtual status_t setInternalOption(
            node_id node,
            OMX_U32 port_index,
            InternalOptionType type,
            const void *data,
            size_t size);

private:
    mutable Mutex mLock;

    sp<IOMX> mMediaServerOMX;
    sp<IOMX> mMediaCodecOMX;
    sp<IOMX> mLocalOMX;

    typedef enum {
        LOCAL,
        MEDIAPROCESS,
        CODECPROCESS
    } node_location;

    KeyedVector<node_id, node_location> mNodeLocation;

    bool isLocalNode(node_id node) const;
    bool isLocalNode_l(node_id node) const;
    const sp<IOMX> &getOMX(node_id node) const;
    const sp<IOMX> &getOMX_l(node_id node) const;

    static node_location getPreferredCodecLocation(const char *name);

    DISALLOW_EVIL_CONSTRUCTORS(MuxOMX);
};

MuxOMX::MuxOMX(const sp<IOMX> &mediaServerOMX, const sp<IOMX> &mediaCodecOMX)
    : mMediaServerOMX(mediaServerOMX),
      mMediaCodecOMX(mediaCodecOMX) {
    ALOGI("MuxOMX ctor");
}

MuxOMX::~MuxOMX() {
}

bool MuxOMX::isLocalNode(node_id node) const {
    Mutex::Autolock autoLock(mLock);

    return isLocalNode_l(node);
}

bool MuxOMX::isLocalNode_l(node_id node) const {
    return mNodeLocation.valueFor(node) == LOCAL;
}

// static
MuxOMX::node_location MuxOMX::getPreferredCodecLocation(const char *name) {
    if (sCodecProcessEnabled) {
        // all codecs go to codec process unless excluded using system property, in which case
        // all non-secure decoders, OMX.google.* codecs and encoders can go in the codec process
        // (non-OMX.google.* encoders can be excluded using system property.)
        if ((strcasestr(name, "decoder")
                        && strcasestr(name, ".secure") != name + strlen(name) - 7)
                || (strcasestr(name, "encoder")
                        && !property_get_bool("media.stagefright.legacyencoder", false))
                || !property_get_bool("media.stagefright.less-secure", false)
                || !strncasecmp(name, "OMX.google.", 11)) {
            return CODECPROCESS;
        }
        // everything else runs in the media server
        return MEDIAPROCESS;
    } else {
#ifdef __LP64__
        // 64 bit processes always run OMX remote on MediaServer
        return MEDIAPROCESS;
#else
        // 32 bit processes run only OMX.google.* components locally
        if (!strncasecmp(name, "OMX.google.", 11)) {
            return LOCAL;
        }
        return MEDIAPROCESS;
#endif
    }
}

const sp<IOMX> &MuxOMX::getOMX(node_id node) const {
    Mutex::Autolock autoLock(mLock);
    return getOMX_l(node);
}

const sp<IOMX> &MuxOMX::getOMX_l(node_id node) const {
    node_location loc = mNodeLocation.valueFor(node);
    if (loc == LOCAL) {
        return mLocalOMX;
    } else if (loc == MEDIAPROCESS) {
        return mMediaServerOMX;
    } else if (loc == CODECPROCESS) {
        return mMediaCodecOMX;
    }
    ALOGE("Couldn't determine node location for node %d: %d, using local", node, loc);
    return mLocalOMX;
}

bool MuxOMX::livesLocally(node_id node, pid_t pid) {
    return getOMX(node)->livesLocally(node, pid);
}

status_t MuxOMX::listNodes(List<ComponentInfo> *list) {
    Mutex::Autolock autoLock(mLock);

    if (mLocalOMX == NULL) {
        mLocalOMX = new OMX;
    }

    return mLocalOMX->listNodes(list);
}

status_t MuxOMX::allocateNode(
        const char *name, const sp<IOMXObserver> &observer,
        sp<IBinder> *nodeBinder,
        node_id *node) {
    Mutex::Autolock autoLock(mLock);

    sp<IOMX> omx;

    node_location loc = getPreferredCodecLocation(name);
    if (loc == CODECPROCESS) {
        omx = mMediaCodecOMX;
    } else if (loc == MEDIAPROCESS) {
        omx = mMediaServerOMX;
    } else {
        if (mLocalOMX == NULL) {
            mLocalOMX = new OMX;
        }
        omx = mLocalOMX;
    }

    status_t err = omx->allocateNode(name, observer, nodeBinder, node);
    ALOGV("allocated node_id %x on %s OMX", *node, omx == mMediaCodecOMX ? "codecprocess" :
            omx == mMediaServerOMX ? "mediaserver" : "local");


    if (err != OK) {
        return err;
    }

    mNodeLocation.add(*node, loc);

    return OK;
}

status_t MuxOMX::freeNode(node_id node) {
    Mutex::Autolock autoLock(mLock);

    // exit if we have already freed the node
    if (mNodeLocation.indexOfKey(node) < 0) {
        ALOGD("MuxOMX::freeNode: node %d seems to be released already --- ignoring.", node);
        return OK;
    }

    status_t err = getOMX_l(node)->freeNode(node);

    if (err != OK) {
        return err;
    }

    mNodeLocation.removeItem(node);

    return OK;
}

status_t MuxOMX::sendCommand(
        node_id node, OMX_COMMANDTYPE cmd, OMX_S32 param) {
    return getOMX(node)->sendCommand(node, cmd, param);
}

status_t MuxOMX::getParameter(
        node_id node, OMX_INDEXTYPE index,
        void *params, size_t size) {
    return getOMX(node)->getParameter(node, index, params, size);
}

status_t MuxOMX::setParameter(
        node_id node, OMX_INDEXTYPE index,
        const void *params, size_t size) {
    return getOMX(node)->setParameter(node, index, params, size);
}

status_t MuxOMX::getConfig(
        node_id node, OMX_INDEXTYPE index,
        void *params, size_t size) {
    return getOMX(node)->getConfig(node, index, params, size);
}

status_t MuxOMX::setConfig(
        node_id node, OMX_INDEXTYPE index,
        const void *params, size_t size) {
    return getOMX(node)->setConfig(node, index, params, size);
}

status_t MuxOMX::getState(
        node_id node, OMX_STATETYPE* state) {
    return getOMX(node)->getState(node, state);
}

status_t MuxOMX::storeMetaDataInBuffers(
        node_id node, OMX_U32 port_index, OMX_BOOL enable, MetadataBufferType *type) {
    return getOMX(node)->storeMetaDataInBuffers(node, port_index, enable, type);
}

status_t MuxOMX::prepareForAdaptivePlayback(
        node_id node, OMX_U32 port_index, OMX_BOOL enable,
        OMX_U32 maxFrameWidth, OMX_U32 maxFrameHeight) {
    return getOMX(node)->prepareForAdaptivePlayback(
            node, port_index, enable, maxFrameWidth, maxFrameHeight);
}

status_t MuxOMX::configureVideoTunnelMode(
        node_id node, OMX_U32 portIndex, OMX_BOOL enable,
        OMX_U32 audioHwSync, native_handle_t **sidebandHandle) {
    return getOMX(node)->configureVideoTunnelMode(
            node, portIndex, enable, audioHwSync, sidebandHandle);
}

status_t MuxOMX::enableNativeBuffers(
        node_id node, OMX_U32 port_index, OMX_BOOL graphic, OMX_BOOL enable) {
    return getOMX(node)->enableNativeBuffers(node, port_index, graphic, enable);
}

status_t MuxOMX::getGraphicBufferUsage(
        node_id node, OMX_U32 port_index, OMX_U32* usage) {
    return getOMX(node)->getGraphicBufferUsage(node, port_index, usage);
}

status_t MuxOMX::useBuffer(
        node_id node, OMX_U32 port_index, const sp<IMemory> &params,
        buffer_id *buffer, OMX_U32 allottedSize) {
    return getOMX(node)->useBuffer(node, port_index, params, buffer, allottedSize);
}

status_t MuxOMX::useGraphicBuffer(
        node_id node, OMX_U32 port_index,
        const sp<GraphicBuffer> &graphicBuffer, buffer_id *buffer) {
    return getOMX(node)->useGraphicBuffer(
            node, port_index, graphicBuffer, buffer);
}

status_t MuxOMX::updateGraphicBufferInMeta(
        node_id node, OMX_U32 port_index,
        const sp<GraphicBuffer> &graphicBuffer, buffer_id buffer) {
    return getOMX(node)->updateGraphicBufferInMeta(
            node, port_index, graphicBuffer, buffer);
}

status_t MuxOMX::updateNativeHandleInMeta(
        node_id node, OMX_U32 port_index,
        const sp<NativeHandle> &nativeHandle, buffer_id buffer) {
    return getOMX(node)->updateNativeHandleInMeta(
            node, port_index, nativeHandle, buffer);
}

status_t MuxOMX::createInputSurface(
        node_id node, OMX_U32 port_index, android_dataspace dataSpace,
        sp<IGraphicBufferProducer> *bufferProducer, MetadataBufferType *type) {
    status_t err = getOMX(node)->createInputSurface(
            node, port_index, dataSpace, bufferProducer, type);
    return err;
}

status_t MuxOMX::createPersistentInputSurface(
        sp<IGraphicBufferProducer> *bufferProducer,
        sp<IGraphicBufferConsumer> *bufferConsumer) {
    sp<IOMX> omx;
    {
        Mutex::Autolock autoLock(mLock);
        if (property_get_bool("media.stagefright.legacyencoder", false)) {
            omx = mMediaServerOMX;
        } else {
            omx = mMediaCodecOMX;
        }
    }
    return omx->createPersistentInputSurface(
            bufferProducer, bufferConsumer);
}

status_t MuxOMX::setInputSurface(
        node_id node, OMX_U32 port_index,
        const sp<IGraphicBufferConsumer> &bufferConsumer, MetadataBufferType *type) {
    return getOMX(node)->setInputSurface(node, port_index, bufferConsumer, type);
}

status_t MuxOMX::signalEndOfInputStream(node_id node) {
    return getOMX(node)->signalEndOfInputStream(node);
}

status_t MuxOMX::allocateSecureBuffer(
        node_id node, OMX_U32 port_index, size_t size,
        buffer_id *buffer, void **buffer_data, sp<NativeHandle> *native_handle) {
    return getOMX(node)->allocateSecureBuffer(
            node, port_index, size, buffer, buffer_data, native_handle);
}

status_t MuxOMX::allocateBufferWithBackup(
        node_id node, OMX_U32 port_index, const sp<IMemory> &params,
        buffer_id *buffer, OMX_U32 allottedSize) {
    return getOMX(node)->allocateBufferWithBackup(
            node, port_index, params, buffer, allottedSize);
}

status_t MuxOMX::freeBuffer(
        node_id node, OMX_U32 port_index, buffer_id buffer) {
    return getOMX(node)->freeBuffer(node, port_index, buffer);
}

status_t MuxOMX::fillBuffer(node_id node, buffer_id buffer, int fenceFd) {
    return getOMX(node)->fillBuffer(node, buffer, fenceFd);
}

status_t MuxOMX::emptyBuffer(
        node_id node,
        buffer_id buffer,
        OMX_U32 range_offset, OMX_U32 range_length,
        OMX_U32 flags, OMX_TICKS timestamp, int fenceFd) {
    return getOMX(node)->emptyBuffer(
            node, buffer, range_offset, range_length, flags, timestamp, fenceFd);
}

status_t MuxOMX::getExtensionIndex(
        node_id node,
        const char *parameter_name,
        OMX_INDEXTYPE *index) {
    return getOMX(node)->getExtensionIndex(node, parameter_name, index);
}

status_t MuxOMX::setInternalOption(
        node_id node,
        OMX_U32 port_index,
        InternalOptionType type,
        const void *data,
        size_t size) {
    return getOMX(node)->setInternalOption(node, port_index, type, data, size);
}

OMXClient::OMXClient() {
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.codecremote", value, NULL)
            && (!strcmp("0", value) || !strcasecmp("false", value))) {
        sCodecProcessEnabled = false;
    }
}

status_t OMXClient::connect() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> playerbinder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> mediaservice = interface_cast<IMediaPlayerService>(playerbinder);

    if (mediaservice.get() == NULL) {
        ALOGE("Cannot obtain IMediaPlayerService");
        return NO_INIT;
    }

    sp<IOMX> mediaServerOMX = mediaservice->getOMX();
    if (mediaServerOMX.get() == NULL) {
        ALOGE("Cannot obtain mediaserver IOMX");
        return NO_INIT;
    }

    // If we don't want to use the codec process, and the media server OMX
    // is local, use it directly instead of going through MuxOMX
    if (!sCodecProcessEnabled &&
            mediaServerOMX->livesLocally(0 /* node */, getpid())) {
        mOMX = mediaServerOMX;
        return OK;
    }

    sp<IBinder> codecbinder = sm->getService(String16("media.codec"));
    sp<IMediaCodecService> codecservice = interface_cast<IMediaCodecService>(codecbinder);

    if (codecservice.get() == NULL) {
        ALOGE("Cannot obtain IMediaCodecService");
        return NO_INIT;
    }

    sp<IOMX> mediaCodecOMX = codecservice->getOMX();
    if (mediaCodecOMX.get() == NULL) {
        ALOGE("Cannot obtain mediacodec IOMX");
        return NO_INIT;
    }

    mOMX = new MuxOMX(mediaServerOMX, mediaCodecOMX);

    return OK;
}

void OMXClient::disconnect() {
    if (mOMX.get() != NULL) {
        mOMX.clear();
        mOMX = NULL;
    }
}

}  // namespace android
