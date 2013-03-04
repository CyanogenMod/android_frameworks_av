/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA3DEVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERA3DEVICE_H

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>

#include "CameraDeviceBase.h"

#include "hardware/camera3.h"

/**
 * Function pointer types with C calling convention to
 * use for HAL callback functions.
 */
extern "C" {
    typedef void (callbacks_process_capture_result_t)(
        const struct camera3_callback_ops *,
        const camera3_capture_result_t *);

    typedef void (callbacks_notify_t)(
        const struct camera3_callback_ops *,
        const camera3_notify_msg_t *);
}

namespace android {

/**
 * CameraDevice for HAL devices with version CAMERA_DEVICE_API_VERSION_3_0
 */
class Camera3Device :
            public CameraDeviceBase,
            private camera3_callback_ops {
  public:
    Camera3Device(int id);

    virtual ~Camera3Device();

    /**
     * CameraDevice interface
     */
    virtual int      getId() const;
    virtual status_t initialize(camera_module_t *module);
    virtual status_t disconnect();
    virtual status_t dump(int fd, const Vector<String16> &args);
    virtual const CameraMetadata& info() const;
    virtual status_t capture(CameraMetadata &request);
    virtual status_t setStreamingRequest(const CameraMetadata &request);
    virtual status_t clearStreamingRequest();
    virtual status_t waitUntilRequestReceived(int32_t requestId, nsecs_t timeout);
    virtual status_t createStream(sp<ANativeWindow> consumer,
            uint32_t width, uint32_t height, int format, size_t size,
            int *id);
    virtual status_t createReprocessStreamFromStream(int outputId, int *id);
    virtual status_t getStreamInfo(int id,
            uint32_t *width, uint32_t *height, uint32_t *format);
    virtual status_t setStreamTransform(int id, int transform);
    virtual status_t deleteStream(int id);
    virtual status_t deleteReprocessStream(int id);
    virtual status_t createDefaultRequest(int templateId, CameraMetadata *request);
    virtual status_t waitUntilDrained();
    virtual status_t setNotifyCallback(NotificationListener *listener);
    virtual status_t waitForNextFrame(nsecs_t timeout);
    virtual status_t getNextFrame(CameraMetadata *frame);
    virtual status_t triggerAutofocus(uint32_t id);
    virtual status_t triggerCancelAutofocus(uint32_t id);
    virtual status_t triggerPrecaptureMetering(uint32_t id);
    virtual status_t pushReprocessBuffer(int reprocessStreamId,
            buffer_handle_t *buffer, wp<BufferReleasedListener> listener);

  private:
    const int              mId;
    camera3_device_t      *mHal3Device;

    CameraMetadata         mDeviceInfo;
    vendor_tag_query_ops_t mVendorTagOps;

    /**
     * Thread for managing capture request submission to HAL device.
     */
    class RequestThread: public Thread {

      public:

        RequestThread(wp<Camera3Device> parent);

      protected:

        virtual bool threadLoop();

      private:

        wp<Camera3Device> mParent;

    };
    sp<RequestThread> requestThread;

    /**
     * Callback functions from HAL device
     */
    void processCaptureResult(const camera3_capture_result *result);

    void notify(const camera3_notify_msg *msg);

    /**
     * Static callback forwarding methods from HAL to instance
     */
    static callbacks_process_capture_result_t sProcessCaptureResult;

    static callbacks_notify_t sNotify;

}; // class Camera3Device

}; // namespace android

#endif
