/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "Camera2Device"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include "Camera2Device.h"

namespace android {

Camera2Device::Camera2Device(int id):
        mId(id),
        mDevice(NULL)
{
    ALOGV("%s: E", __FUNCTION__);
}

Camera2Device::~Camera2Device()
{
    ALOGV("%s: E", __FUNCTION__);
    if (mDevice) {
        status_t res;
        res = mDevice->common.close(&mDevice->common);
        if (res != OK) {
            ALOGE("%s: Could not close camera %d: %s (%d)",
                    __FUNCTION__,
                    mId, strerror(-res), res);
        }
        mDevice = NULL;
    }
}

status_t Camera2Device::initialize(camera_module_t *module)
{
    ALOGV("%s: E", __FUNCTION__);

    status_t res;
    char name[10];
    snprintf(name, sizeof(name), "%d", mId);

    res = module->common.methods->open(&module->common, name,
            reinterpret_cast<hw_device_t**>(&mDevice));

    if (res != OK) {
        ALOGE("%s: Could not open camera %d: %s (%d)", __FUNCTION__,
                mId, strerror(-res), res);
        return res;
    }

    if (mDevice->common.version != CAMERA_DEVICE_API_VERSION_2_0) {
        ALOGE("%s: Could not open camera %d: "
                "Camera device is not version %x, reports %x instead",
                __FUNCTION__, mId, CAMERA_DEVICE_API_VERSION_2_0,
                mDevice->common.version);
        return BAD_VALUE;
    }

    camera_info info;
    res = module->get_camera_info(mId, &info);
    if (res != OK ) return res;

    if (info.device_version != mDevice->common.version) {
        ALOGE("%s: HAL reporting mismatched camera_info version (%x)"
                " and device version (%x).", __FUNCTION__,
                mDevice->common.version, info.device_version);
        return BAD_VALUE;
    }

    mDeviceInfo = info.static_camera_characteristics;

    res = mRequestQueue.setConsumerDevice(mDevice);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to connect request queue to device: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }
    res = mFrameQueue.setProducerDevice(mDevice);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to connect frame queue to device: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    res = mDevice->ops->get_metadata_vendor_tag_ops(mDevice, &mVendorTagOps);
    if (res != OK ) {
        ALOGE("%s: Camera %d: Unable to retrieve tag ops from device: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    return OK;
}

camera_metadata_t *Camera2Device::info() {
    ALOGV("%s: E", __FUNCTION__);

    return mDeviceInfo;
}

status_t Camera2Device::setStreamingRequest(camera_metadata_t* request)
{
    ALOGV("%s: E", __FUNCTION__);

    mRequestQueue.setStreamSlot(request);
    return OK;
}

status_t Camera2Device::createStream(sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format, int *id) {
    status_t res;
    ALOGV("%s: E", __FUNCTION__);

    sp<StreamAdapter> stream = new StreamAdapter(mDevice);

    res = stream->connectToDevice(consumer, width, height, format);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to create stream (%d x %d, format %x):"
                "%s (%d)",
                __FUNCTION__, mId, width, height, format, strerror(-res), res);
        return res;
    }

    *id = stream->getId();

    mStreams.push_back(stream);
    return OK;
}

status_t Camera2Device::deleteStream(int id) {
    ALOGV("%s: E", __FUNCTION__);

    bool found = false;
    for (StreamList::iterator streamI = mStreams.begin();
         streamI != mStreams.end(); streamI++) {
        if ((*streamI)->getId() == id) {
            mStreams.erase(streamI);
            found = true;
            break;
        }
    }
    if (!found) {
        ALOGE("%s: Camera %d: Unable to find stream %d to delete",
                __FUNCTION__, mId, id);
        return BAD_VALUE;
    }
    return OK;
}

status_t Camera2Device::createDefaultRequest(int templateId,
        camera_metadata_t **request) {
    ALOGV("%s: E", __FUNCTION__);
    return mDevice->ops->construct_default_request(mDevice, templateId, request);
}

/**
 * Camera2Device::MetadataQueue
 */

Camera2Device::MetadataQueue::MetadataQueue():
            mDevice(NULL),
            mFrameCount(0),
            mCount(0),
            mStreamSlotCount(0),
            mSignalConsumer(true)
{
    camera2_request_queue_src_ops::dequeue_request = consumer_dequeue;
    camera2_request_queue_src_ops::request_count = consumer_buffer_count;
    camera2_request_queue_src_ops::free_request = consumer_free;

    camera2_frame_queue_dst_ops::dequeue_frame = producer_dequeue;
    camera2_frame_queue_dst_ops::cancel_frame = producer_cancel;
    camera2_frame_queue_dst_ops::enqueue_frame = producer_enqueue;
}

Camera2Device::MetadataQueue::~MetadataQueue() {
    Mutex::Autolock l(mMutex);
    freeBuffers(mEntries.begin(), mEntries.end());
    freeBuffers(mStreamSlot.begin(), mStreamSlot.end());
}

// Connect to camera2 HAL as consumer (input requests/reprocessing)
status_t Camera2Device::MetadataQueue::setConsumerDevice(camera2_device_t *d) {
    status_t res;
    res = d->ops->set_request_queue_src_ops(d,
            this);
    if (res != OK) return res;
    mDevice = d;
    return OK;
}

status_t Camera2Device::MetadataQueue::setProducerDevice(camera2_device_t *d) {
    status_t res;
    res = d->ops->set_frame_queue_dst_ops(d,
            this);
    return res;
}

// Real interfaces
status_t Camera2Device::MetadataQueue::enqueue(camera_metadata_t *buf) {
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mMutex);

    mCount++;
    mEntries.push_back(buf);

    return signalConsumerLocked();
}

int Camera2Device::MetadataQueue::getBufferCount() {
    Mutex::Autolock l(mMutex);
    if (mStreamSlotCount > 0) {
        return CAMERA2_REQUEST_QUEUE_IS_BOTTOMLESS;
    }
    return mCount;
}

status_t Camera2Device::MetadataQueue::dequeue(camera_metadata_t **buf,
        bool incrementCount)
{
    ALOGV("%s: E", __FUNCTION__);
    status_t res;
    Mutex::Autolock l(mMutex);

    if (mCount == 0) {
        if (mStreamSlotCount == 0) {
            ALOGV("%s: Empty", __FUNCTION__);
            *buf = NULL;
            mSignalConsumer = true;
            return OK;
        }
        ALOGV("%s: Streaming %d frames to queue", __FUNCTION__,
              mStreamSlotCount);

        for (List<camera_metadata_t*>::iterator slotEntry = mStreamSlot.begin();
                slotEntry != mStreamSlot.end();
                slotEntry++ ) {
            size_t entries = get_camera_metadata_entry_count(*slotEntry);
            size_t dataBytes = get_camera_metadata_data_count(*slotEntry);

            camera_metadata_t *copy =
                    allocate_camera_metadata(entries, dataBytes);
            append_camera_metadata(copy, *slotEntry);
            mEntries.push_back(copy);
        }
        mCount = mStreamSlotCount;
    }
    ALOGV("MetadataQueue: deque (%d buffers)", mCount);
    camera_metadata_t *b = *(mEntries.begin());
    mEntries.erase(mEntries.begin());

    if (incrementCount) {
        camera_metadata_entry_t frameCount;
        res = find_camera_metadata_entry(b,
                ANDROID_REQUEST_FRAME_COUNT,
                &frameCount);
        if (res != OK) {
            ALOGE("%s: Unable to add frame count: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
        } else {
            *frameCount.data.i32 = mFrameCount;
        }
        mFrameCount++;
    }

    *buf = b;
    mCount--;

    return OK;
}

status_t Camera2Device::MetadataQueue::waitForBuffer(nsecs_t timeout)
{
    Mutex::Autolock l(mMutex);
    status_t res;
    while (mCount == 0) {
        res = notEmpty.waitRelative(mMutex,timeout);
        if (res != OK) return res;
    }
    return OK;
}

status_t Camera2Device::MetadataQueue::setStreamSlot(camera_metadata_t *buf)
{
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mMutex);
    if (buf == NULL) {
        freeBuffers(mStreamSlot.begin(), mStreamSlot.end());
        mStreamSlotCount = 0;
        return OK;
    }
    if (mStreamSlotCount > 1) {
        List<camera_metadata_t*>::iterator deleter = ++mStreamSlot.begin();
        freeBuffers(++mStreamSlot.begin(), mStreamSlot.end());
        mStreamSlotCount = 1;
    }
    if (mStreamSlotCount == 1) {
        free_camera_metadata( *(mStreamSlot.begin()) );
        *(mStreamSlot.begin()) = buf;
    } else {
        mStreamSlot.push_front(buf);
        mStreamSlotCount = 1;
    }
    return signalConsumerLocked();
}

status_t Camera2Device::MetadataQueue::setStreamSlot(
        const List<camera_metadata_t*> &bufs)
{
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock l(mMutex);
    if (mStreamSlotCount > 0) {
        freeBuffers(mStreamSlot.begin(), mStreamSlot.end());
    }
    mStreamSlot = bufs;
    mStreamSlotCount = mStreamSlot.size();
    return signalConsumerLocked();
}

status_t Camera2Device::MetadataQueue::signalConsumerLocked() {
    status_t res = OK;
    notEmpty.signal();
    if (mSignalConsumer && mDevice != NULL) {
        mSignalConsumer = false;

        mMutex.unlock();
        ALOGV("%s: Signaling consumer", __FUNCTION__);
        res = mDevice->ops->notify_request_queue_not_empty(mDevice);
        mMutex.lock();
    }
    return res;
}

status_t Camera2Device::MetadataQueue::freeBuffers(
        List<camera_metadata_t*>::iterator start,
        List<camera_metadata_t*>::iterator end)
{
    while (start != end) {
        free_camera_metadata(*start);
        start = mStreamSlot.erase(start);
    }
    return OK;
}

Camera2Device::MetadataQueue* Camera2Device::MetadataQueue::getInstance(
        const camera2_request_queue_src_ops_t *q)
{
    const MetadataQueue* cmq = static_cast<const MetadataQueue*>(q);
    return const_cast<MetadataQueue*>(cmq);
}

Camera2Device::MetadataQueue* Camera2Device::MetadataQueue::getInstance(
        const camera2_frame_queue_dst_ops_t *q)
{
    const MetadataQueue* cmq = static_cast<const MetadataQueue*>(q);
    return const_cast<MetadataQueue*>(cmq);
}

int Camera2Device::MetadataQueue::consumer_buffer_count(
        const camera2_request_queue_src_ops_t *q)
{
    MetadataQueue *queue = getInstance(q);
    return queue->getBufferCount();
}

int Camera2Device::MetadataQueue::consumer_dequeue(
        const camera2_request_queue_src_ops_t *q,
        camera_metadata_t **buffer)
{
    MetadataQueue *queue = getInstance(q);
    return queue->dequeue(buffer, true);
}

int Camera2Device::MetadataQueue::consumer_free(
        const camera2_request_queue_src_ops_t *q,
        camera_metadata_t *old_buffer)
{
    MetadataQueue *queue = getInstance(q);
    free_camera_metadata(old_buffer);
    return OK;
}

int Camera2Device::MetadataQueue::producer_dequeue(
        const camera2_frame_queue_dst_ops_t *q,
        size_t entries, size_t bytes,
        camera_metadata_t **buffer)
{
    camera_metadata_t *new_buffer =
            allocate_camera_metadata(entries, bytes);
    if (new_buffer == NULL) return NO_MEMORY;
    *buffer = new_buffer;
        return OK;
}

int Camera2Device::MetadataQueue::producer_cancel(
        const camera2_frame_queue_dst_ops_t *q,
        camera_metadata_t *old_buffer)
{
    free_camera_metadata(old_buffer);
    return OK;
}

int Camera2Device::MetadataQueue::producer_enqueue(
        const camera2_frame_queue_dst_ops_t *q,
        camera_metadata_t *filled_buffer)
{
    MetadataQueue *queue = getInstance(q);
    return queue->enqueue(filled_buffer);
}

/**
 * Camera2Device::StreamAdapter
 */

#ifndef container_of
#define container_of(ptr, type, member) \
    (type *)((char*)(ptr) - offsetof(type, member))
#endif

Camera2Device::StreamAdapter::StreamAdapter(camera2_device_t *d):
        mState(DISCONNECTED),
        mDevice(d),
        mId(-1),
        mWidth(0), mHeight(0), mFormatRequested(0)
{
    camera2_stream_ops::dequeue_buffer = dequeue_buffer;
    camera2_stream_ops::enqueue_buffer = enqueue_buffer;
    camera2_stream_ops::cancel_buffer = cancel_buffer;
    camera2_stream_ops::set_crop = set_crop;
}

Camera2Device::StreamAdapter::~StreamAdapter() {
    disconnect();
}

status_t Camera2Device::StreamAdapter::connectToDevice(sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format) {
    status_t res;

    if (mState != DISCONNECTED) return INVALID_OPERATION;
    if (consumer == NULL) {
        ALOGE("%s: Null consumer passed to stream adapter", __FUNCTION__);
        return BAD_VALUE;
    }

    mConsumerInterface = consumer;
    mWidth = width;
    mHeight = height;
    mFormatRequested = format;

    // Allocate device-side stream interface

    uint32_t id;
    uint32_t formatActual;
    uint32_t usage;
    uint32_t maxBuffers = 2;
    res = mDevice->ops->allocate_stream(mDevice,
            mWidth, mHeight, mFormatRequested, getStreamOps(),
            &id, &formatActual, &usage, &maxBuffers);
    if (res != OK) {
        ALOGE("%s: Device stream allocation failed: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    mId = id;
    mFormat = formatActual;
    mUsage = usage;
    mMaxProducerBuffers = maxBuffers;

    mState = ALLOCATED;

    // Configure consumer-side ANativeWindow interface
    res = native_window_api_connect(mConsumerInterface.get(),
            NATIVE_WINDOW_API_CAMERA);
    if (res != OK) {
        ALOGE("%s: Unable to connect to native window for stream %d",
                __FUNCTION__, mId);

        return res;
    }

    mState = CONNECTED;

    res = native_window_set_usage(mConsumerInterface.get(), mUsage);
    if (res != OK) {
        ALOGE("%s: Unable to configure usage %08x for stream %d",
                __FUNCTION__, mUsage, mId);
        return res;
    }

    res = native_window_set_buffers_geometry(mConsumerInterface.get(),
            mWidth, mHeight, mFormat);
    if (res != OK) {
        ALOGE("%s: Unable to configure buffer geometry"
                " %d x %d, format 0x%x for stream %d",
                __FUNCTION__, mWidth, mHeight, mFormat, mId);
        return res;
    }

    int maxConsumerBuffers;
    res = mConsumerInterface->query(mConsumerInterface.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &maxConsumerBuffers);
    if (res != OK) {
        ALOGE("%s: Unable to query consumer undequeued"
                " buffer count for stream %d", __FUNCTION__, mId);
        return res;
    }
    mMaxConsumerBuffers = maxConsumerBuffers;

    ALOGV("%s: Producer wants %d buffers, consumer wants %d", __FUNCTION__,
            mMaxProducerBuffers, mMaxConsumerBuffers);

    int totalBuffers = mMaxConsumerBuffers + mMaxProducerBuffers;

    res = native_window_set_buffer_count(mConsumerInterface.get(),
            totalBuffers);
    if (res != OK) {
        ALOGE("%s: Unable to set buffer count for stream %d",
                __FUNCTION__, mId);
        return res;
    }

    // Register allocated buffers with HAL device
    buffer_handle_t *buffers = new buffer_handle_t[totalBuffers];
    ANativeWindowBuffer **anwBuffers = new ANativeWindowBuffer*[totalBuffers];
    int bufferIdx = 0;
    for (; bufferIdx < totalBuffers; bufferIdx++) {
        res = mConsumerInterface->dequeueBuffer(mConsumerInterface.get(),
                &anwBuffers[bufferIdx]);
        if (res != OK) {
            ALOGE("%s: Unable to dequeue buffer %d for initial registration for"
                    "stream %d", __FUNCTION__, bufferIdx, mId);
            goto cleanUpBuffers;
        }

        res = mConsumerInterface->lockBuffer(mConsumerInterface.get(),
                anwBuffers[bufferIdx]);
        if (res != OK) {
            ALOGE("%s: Unable to lock buffer %d for initial registration for"
                    "stream %d", __FUNCTION__, bufferIdx, mId);
            bufferIdx++;
            goto cleanUpBuffers;
        }

        buffers[bufferIdx] = anwBuffers[bufferIdx]->handle;
    }

    res = mDevice->ops->register_stream_buffers(mDevice,
            mId,
            totalBuffers,
            buffers);
    if (res != OK) {
        ALOGE("%s: Unable to register buffers with HAL device for stream %d",
                __FUNCTION__, mId);
    } else {
        mState = ACTIVE;
    }

cleanUpBuffers:
    for (int i = 0; i < bufferIdx; i++) {
        res = mConsumerInterface->cancelBuffer(mConsumerInterface.get(),
                anwBuffers[i]);
        if (res != OK) {
            ALOGE("%s: Unable to cancel buffer %d after registration",
                    __FUNCTION__, i);
        }
    }
    delete anwBuffers;
    delete buffers;

    return res;
}

status_t Camera2Device::StreamAdapter::disconnect() {
    status_t res;
    if (mState >= ALLOCATED) {
        res = mDevice->ops->release_stream(mDevice, mId);
        if (res != OK) {
            ALOGE("%s: Unable to release stream %d",
                    __FUNCTION__, mId);
            return res;
        }
    }
    if (mState >= CONNECTED) {
        res = native_window_api_disconnect(mConsumerInterface.get(),
                NATIVE_WINDOW_API_CAMERA);
        if (res != OK) {
            ALOGE("%s: Unable to disconnect stream %d from native window",
                    __FUNCTION__, mId);
            return res;
        }
    }
    mId = -1;
    mState = DISCONNECTED;
    return OK;
}

int Camera2Device::StreamAdapter::getId() {
    return mId;
}

const camera2_stream_ops *Camera2Device::StreamAdapter::getStreamOps() {
    return static_cast<camera2_stream_ops *>(this);
}

ANativeWindow* Camera2Device::StreamAdapter::toANW(
        const camera2_stream_ops_t *w) {
    return static_cast<const StreamAdapter*>(w)->mConsumerInterface.get();
}

int Camera2Device::StreamAdapter::dequeue_buffer(const camera2_stream_ops_t *w,
        buffer_handle_t** buffer) {
    int res;
    int state = static_cast<const StreamAdapter*>(w)->mState;
    if (state != ACTIVE) {
        ALOGE("%s: Called when in bad state: %d", __FUNCTION__, state);
        return INVALID_OPERATION;
    }

    ANativeWindow *a = toANW(w);
    ANativeWindowBuffer* anb;
    res = a->dequeueBuffer(a, &anb);
    if (res != OK) return res;
    res = a->lockBuffer(a, anb);
    if (res != OK) return res;

    *buffer = &(anb->handle);
    ALOGV("%s: Buffer %p", __FUNCTION__, *buffer);
    return res;
}

int Camera2Device::StreamAdapter::enqueue_buffer(const camera2_stream_ops_t* w,
        int64_t timestamp,
        buffer_handle_t* buffer) {
    ALOGV("%s: Buffer %p captured at %lld ns", __FUNCTION__, buffer, timestamp);
    int state = static_cast<const StreamAdapter*>(w)->mState;
    if (state != ACTIVE) {
        ALOGE("%s: Called when in bad state: %d", __FUNCTION__, state);
        return INVALID_OPERATION;
    }
    ANativeWindow *a = toANW(w);
    status_t err;
    err = native_window_set_buffers_timestamp(a, timestamp);
    if (err != OK) return err;
    return a->queueBuffer(a,
            container_of(buffer, ANativeWindowBuffer, handle));
}

int Camera2Device::StreamAdapter::cancel_buffer(const camera2_stream_ops_t* w,
        buffer_handle_t* buffer) {
    int state = static_cast<const StreamAdapter*>(w)->mState;
    if (state != ACTIVE) {
        ALOGE("%s: Called when in bad state: %d", __FUNCTION__, state);
        return INVALID_OPERATION;
    }
    ANativeWindow *a = toANW(w);
    return a->cancelBuffer(a,
            container_of(buffer, ANativeWindowBuffer, handle));
}

int Camera2Device::StreamAdapter::set_crop(const camera2_stream_ops_t* w,
        int left, int top, int right, int bottom) {
    int state = static_cast<const StreamAdapter*>(w)->mState;
    if (state != ACTIVE) {
        ALOGE("%s: Called when in bad state: %d", __FUNCTION__, state);
        return INVALID_OPERATION;
    }
    ANativeWindow *a = toANW(w);
    android_native_rect_t crop = { left, top, right, bottom };
    return native_window_set_crop(a, &crop);
}


}; // namespace android
