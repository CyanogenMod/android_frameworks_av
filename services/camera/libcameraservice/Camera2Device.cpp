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

}

Camera2Device::~Camera2Device()
{
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

    res = mDevice->ops->set_request_queue_src_ops(mDevice,
            mRequestQueue.getToConsumerInterface());
    if (res != OK) return res;

    res = mDevice->ops->set_frame_queue_dst_ops(mDevice,
            mFrameQueue.getToProducerInterface());
    if (res != OK) return res;

    res = mDevice->ops->get_metadata_vendor_tag_ops(mDevice, &mVendorTagOps);
    if (res != OK ) return res;

    return OK;
}

status_t Camera2Device::setStreamingRequest(camera_metadata_t* request)
{
    mRequestQueue.setStreamSlot(request);
    return OK;
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

// Interface to camera2 HAL as consumer (input requests/reprocessing)
const camera2_request_queue_src_ops_t*
Camera2Device::MetadataQueue::getToConsumerInterface() {
    return static_cast<camera2_request_queue_src_ops_t*>(this);
}

void Camera2Device::MetadataQueue::setFromConsumerInterface(camera2_device_t *d) {
    Mutex::Autolock l(mMutex);
    mDevice = d;
}

const camera2_frame_queue_dst_ops_t*
Camera2Device::MetadataQueue::getToProducerInterface() {
    return static_cast<camera2_frame_queue_dst_ops_t*>(this);
}

// Real interfaces
status_t Camera2Device::MetadataQueue::enqueue(camera_metadata_t *buf) {
    Mutex::Autolock l(mMutex);

    mCount++;
    mEntries.push_back(buf);
    notEmpty.signal();

    if (mSignalConsumer && mDevice != NULL) {
        mSignalConsumer = false;

        mMutex.unlock();
        ALOGV("%s: Signaling consumer", __FUNCTION__);
        mDevice->ops->notify_request_queue_not_empty(mDevice);
        mMutex.lock();
    }
    return OK;
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
        add_camera_metadata_entry(b,
                ANDROID_REQUEST_FRAME_COUNT,
                (void**)&mFrameCount, 1);
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
    return OK;
}

status_t Camera2Device::MetadataQueue::setStreamSlot(
        const List<camera_metadata_t*> &bufs)
{
    Mutex::Autolock l(mMutex);
    if (mStreamSlotCount > 0) {
        freeBuffers(mStreamSlot.begin(), mStreamSlot.end());
    }
    mStreamSlot = bufs;
    mStreamSlotCount = mStreamSlot.size();

    return OK;
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


}; // namespace android
