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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H

#include <utils/RefBase.h>
#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>
#include <utils/Errors.h>
#include "hardware/camera2.h"

namespace android {

class Camera2Device : public virtual RefBase {
  public:
    Camera2Device(int id);

    ~Camera2Device();

    status_t initialize(camera_module_t *module);

    status_t setStreamingRequest(camera_metadata_t* request);

    camera_metadata_t* info() {
        return mDeviceInfo;
    }

  private:

    const int mId;
    camera2_device_t *mDevice;

    camera_metadata_t *mDeviceInfo;
    vendor_tag_query_ops_t *mVendorTagOps;

    /**
     * Queue class for both sending requests to a camera2 device, and for
     * receiving frames from a camera2 device.
     */
    class MetadataQueue: public camera2_request_queue_src_ops_t,
                         public camera2_frame_queue_dst_ops_t {
      public:
        MetadataQueue();
        ~MetadataQueue();

        // Interface to camera2 HAL device, either for requests (device is
        // consumer) or for frames (device is producer)
        const camera2_request_queue_src_ops_t*   getToConsumerInterface();
        void setFromConsumerInterface(camera2_device_t *d);

        const camera2_frame_queue_dst_ops_t* getToProducerInterface();

        // Real interfaces. On enqueue, queue takes ownership of buffer pointer
        // On dequeue, user takes ownership of buffer pointer.
        status_t enqueue(camera_metadata_t *buf);
        status_t dequeue(camera_metadata_t **buf, bool incrementCount = true);
        int      getBufferCount();
        status_t waitForBuffer(nsecs_t timeout);

        // Set repeating buffer(s); if the queue is empty on a dequeue call, the
        // queue copies the contents of the stream slot into the queue, and then
        // dequeues the first new entry.
        status_t setStreamSlot(camera_metadata_t *buf);
        status_t setStreamSlot(const List<camera_metadata_t*> &bufs);

      private:
        status_t freeBuffers(List<camera_metadata_t*>::iterator start,
                List<camera_metadata_t*>::iterator end);

        camera2_device_t *mDevice;

        Mutex mMutex;
        Condition notEmpty;

        int mFrameCount;

        int mCount;
        List<camera_metadata_t*> mEntries;
        int mStreamSlotCount;
        List<camera_metadata_t*> mStreamSlot;

        bool mSignalConsumer;

        static MetadataQueue* getInstance(
            const camera2_frame_queue_dst_ops_t *q);
        static MetadataQueue* getInstance(
            const camera2_request_queue_src_ops_t *q);

        static int consumer_buffer_count(
            const camera2_request_queue_src_ops_t *q);

        static int consumer_dequeue(const camera2_request_queue_src_ops_t *q,
            camera_metadata_t **buffer);

        static int consumer_free(const camera2_request_queue_src_ops_t *q,
                camera_metadata_t *old_buffer);

        static int producer_dequeue(const camera2_frame_queue_dst_ops_t *q,
                size_t entries, size_t bytes,
                camera_metadata_t **buffer);

        static int producer_cancel(const camera2_frame_queue_dst_ops_t *q,
            camera_metadata_t *old_buffer);

        static int producer_enqueue(const camera2_frame_queue_dst_ops_t *q,
                camera_metadata_t *filled_buffer);

    }; // class MetadataQueue

    MetadataQueue mRequestQueue;
    MetadataQueue mFrameQueue;

}; // class Camera2Device

}; // namespace android

#endif
