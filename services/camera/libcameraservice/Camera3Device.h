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

#ifndef ANDROID_SERVERS_CAMERA3DEVICE_H
#define ANDROID_SERVERS_CAMERA3DEVICE_H

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>

#include "CameraDeviceBase.h"
#include "camera3/Camera3Stream.h"
#include "camera3/Camera3OutputStream.h"
#include "camera3/Camera3ZslStream.h"

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
     * CameraDeviceBase interface
     */

    virtual int      getId() const;

    // Transitions to idle state on success.
    virtual status_t initialize(camera_module_t *module);
    virtual status_t disconnect();
    virtual status_t dump(int fd, const Vector<String16> &args);
    virtual const CameraMetadata& info() const;

    // Capture and setStreamingRequest will configure streams if currently in
    // idle state
    virtual status_t capture(CameraMetadata &request);
    virtual status_t setStreamingRequest(const CameraMetadata &request);
    virtual status_t clearStreamingRequest();

    virtual status_t waitUntilRequestReceived(int32_t requestId, nsecs_t timeout);

    // Actual stream creation/deletion is delayed until first request is submitted
    // If adding streams while actively capturing, will pause device before adding
    // stream, reconfiguring device, and unpausing.
    virtual status_t createStream(sp<ANativeWindow> consumer,
            uint32_t width, uint32_t height, int format, size_t size,
            int *id);
    virtual status_t createInputStream(
            uint32_t width, uint32_t height, int format,
            int *id);
    virtual status_t createZslStream(
            uint32_t width, uint32_t height,
            int depth,
            /*out*/
            int *id,
            sp<camera3::Camera3ZslStream>* zslStream);
    virtual status_t createReprocessStreamFromStream(int outputId, int *id);

    virtual status_t getStreamInfo(int id,
            uint32_t *width, uint32_t *height, uint32_t *format);
    virtual status_t setStreamTransform(int id, int transform);

    virtual status_t deleteStream(int id);
    virtual status_t deleteReprocessStream(int id);

    virtual status_t createDefaultRequest(int templateId, CameraMetadata *request);

    // Transitions to the idle state on success
    virtual status_t waitUntilDrained();

    virtual status_t setNotifyCallback(NotificationListener *listener);
    virtual bool     willNotify3A();
    virtual status_t waitForNextFrame(nsecs_t timeout);
    virtual status_t getNextFrame(CameraMetadata *frame);

    virtual status_t triggerAutofocus(uint32_t id);
    virtual status_t triggerCancelAutofocus(uint32_t id);
    virtual status_t triggerPrecaptureMetering(uint32_t id);

    virtual status_t pushReprocessBuffer(int reprocessStreamId,
            buffer_handle_t *buffer, wp<BufferReleasedListener> listener);

  private:
    static const size_t        kInFlightWarnLimit = 20;
    static const nsecs_t       kShutdownTimeout   = 5000000000; // 5 sec
    struct                     RequestTrigger;

    Mutex                      mLock;

    /**** Scope for mLock ****/

    const int                  mId;
    camera3_device_t          *mHal3Device;

    CameraMetadata             mDeviceInfo;
    vendor_tag_query_ops_t     mVendorTagOps;

    enum {
        STATUS_ERROR,
        STATUS_UNINITIALIZED,
        STATUS_IDLE,
        STATUS_ACTIVE
    }                          mStatus;

    // Tracking cause of fatal errors when in STATUS_ERROR
    String8                    mErrorCause;

    // Mapping of stream IDs to stream instances
    typedef KeyedVector<int, sp<camera3::Camera3OutputStreamInterface> >
            StreamSet;

    StreamSet                  mOutputStreams;
    sp<camera3::Camera3Stream> mInputStream;
    int                        mNextStreamId;
    bool                       mNeedConfig;

    // Need to hold on to stream references until configure completes.
    Vector<sp<camera3::Camera3StreamInterface> > mDeletedStreams;

    /**** End scope for mLock ****/

    class CaptureRequest : public LightRefBase<CaptureRequest> {
      public:
        CameraMetadata                      mSettings;
        sp<camera3::Camera3Stream>          mInputStream;
        Vector<sp<camera3::Camera3OutputStreamInterface> >
                                            mOutputStreams;
    };
    typedef List<sp<CaptureRequest> > RequestList;

    /**
     * Lock-held version of waitUntilDrained. Will transition to IDLE on
     * success.
     */
    status_t           waitUntilDrainedLocked();

    /**
     * Do common work for setting up a streaming or single capture request.
     * On success, will transition to ACTIVE if in IDLE.
     */
    sp<CaptureRequest> setUpRequestLocked(const CameraMetadata &request);

    /**
     * Build a CaptureRequest request from the CameraDeviceBase request
     * settings.
     */
    sp<CaptureRequest> createCaptureRequest(const CameraMetadata &request);

    /**
     * Take the currently-defined set of streams and configure the HAL to use
     * them. This is a long-running operation (may be several hundered ms).
     */
    status_t           configureStreamsLocked();

    /**
     * Set device into an error state due to some fatal failure, and set an
     * error message to indicate why. Only the first call's message will be
     * used. The message is also sent to the log.
     */
    void               setErrorState(const char *fmt, ...);
    void               setErrorStateV(const char *fmt, va_list args);
    void               setErrorStateLocked(const char *fmt, ...);
    void               setErrorStateLockedV(const char *fmt, va_list args);

    struct RequestTrigger {
        // Metadata tag number, e.g. android.control.aePrecaptureTrigger
        uint32_t metadataTag;
        // Metadata value, e.g. 'START' or the trigger ID
        int32_t entryValue;

        // The last part of the fully qualified path, e.g. afTrigger
        const char *getTagName() const {
            return get_camera_metadata_tag_name(metadataTag) ?: "NULL";
        }

        // e.g. TYPE_BYTE, TYPE_INT32, etc.
        int getTagType() const {
            return get_camera_metadata_tag_type(metadataTag);
        }
    };

    /**
     * Thread for managing capture request submission to HAL device.
     */
    class RequestThread : public Thread {

      public:

        RequestThread(wp<Camera3Device> parent,
                camera3_device_t *hal3Device);

        /**
         * Call after stream (re)-configuration is completed.
         */
        void     configurationComplete();

        /**
         * Set or clear the list of repeating requests. Does not block
         * on either. Use waitUntilPaused to wait until request queue
         * has emptied out.
         */
        status_t setRepeatingRequests(const RequestList& requests);
        status_t clearRepeatingRequests();

        status_t queueRequest(sp<CaptureRequest> request);

        /**
         * Queue a trigger to be dispatched with the next outgoing
         * process_capture_request. The settings for that request only
         * will be temporarily rewritten to add the trigger tag/value.
         * Subsequent requests will not be rewritten (for this tag).
         */
        status_t queueTrigger(RequestTrigger trigger[], size_t count);

        /**
         * Pause/unpause the capture thread. Doesn't block, so use
         * waitUntilPaused to wait until the thread is paused.
         */
        void     setPaused(bool paused);

        /**
         * Wait until thread is paused, either due to setPaused(true)
         * or due to lack of input requests. Returns TIMED_OUT in case
         * the thread does not pause within the timeout.
         */
        status_t waitUntilPaused(nsecs_t timeout);

        /**
         * Wait until thread processes the capture request with settings'
         * android.request.id == requestId.
         *
         * Returns TIMED_OUT in case the thread does not process the request
         * within the timeout.
         */
        status_t waitUntilRequestProcessed(int32_t requestId, nsecs_t timeout);

      protected:

        virtual bool threadLoop();

      private:
        static int         getId(const wp<Camera3Device> &device);

        status_t           queueTriggerLocked(RequestTrigger trigger);
        // Mix-in queued triggers into this request
        int32_t            insertTriggers(const sp<CaptureRequest> &request);
        // Purge the queued triggers from this request,
        //  restoring the old field values for those tags.
        status_t           removeTriggers(const sp<CaptureRequest> &request);

        static const nsecs_t kRequestTimeout = 50e6; // 50 ms

        // Waits for a request, or returns NULL if times out.
        sp<CaptureRequest> waitForNextRequest();

        // Return buffers, etc, for a request that couldn't be fully
        // constructed. The buffers will be returned in the ERROR state
        // to mark them as not having valid data.
        // All arguments will be modified.
        void cleanUpFailedRequest(camera3_capture_request_t &request,
                sp<CaptureRequest> &nextRequest,
                Vector<camera3_stream_buffer_t> &outputBuffers);

        // Pause handling
        bool               waitIfPaused();

        // Relay error to parent device object setErrorState
        void               setErrorState(const char *fmt, ...);

        wp<Camera3Device>  mParent;
        camera3_device_t  *mHal3Device;

        const int          mId;

        Mutex              mRequestLock;
        Condition          mRequestSignal;
        RequestList        mRequestQueue;
        RequestList        mRepeatingRequests;

        bool               mReconfigured;

        // Used by waitIfPaused, waitForNextRequest, and waitUntilPaused
        Mutex              mPauseLock;
        bool               mDoPause;
        Condition          mDoPauseSignal;
        bool               mPaused;
        Condition          mPausedSignal;

        sp<CaptureRequest> mPrevRequest;
        int32_t            mPrevTriggers;

        uint32_t           mFrameNumber;

        Mutex              mLatestRequestMutex;
        Condition          mLatestRequestSignal;
        // android.request.id for latest process_capture_request
        int32_t            mLatestRequestId;

        typedef KeyedVector<uint32_t/*tag*/, RequestTrigger> TriggerMap;
        Mutex              mTriggerMutex;
        TriggerMap         mTriggerMap;
        TriggerMap         mTriggerRemovedMap;
        TriggerMap         mTriggerReplacedMap;
    };
    sp<RequestThread> mRequestThread;

    /**
     * In-flight queue for tracking completion of capture requests.
     */

    struct InFlightRequest {
        // Set by notify() SHUTTER call.
        nsecs_t captureTimestamp;
        // Set by process_capture_result call with valid metadata
        bool    haveResultMetadata;
        // Decremented by calls to process_capture_result with valid output
        // buffers
        int     numBuffersLeft;

        InFlightRequest() :
                captureTimestamp(0),
                haveResultMetadata(false),
                numBuffersLeft(0) {
        }

        explicit InFlightRequest(int numBuffers) :
                captureTimestamp(0),
                haveResultMetadata(false),
                numBuffersLeft(numBuffers) {
        }
    };
    // Map from frame number to the in-flight request state
    typedef KeyedVector<uint32_t, InFlightRequest> InFlightMap;

    Mutex                  mInFlightLock; // Protects mInFlightMap
    InFlightMap            mInFlightMap;

    status_t registerInFlight(int32_t frameNumber, int32_t numBuffers);

    /**
     * Output result queue and current HAL device 3A state
     */

    // Lock for output side of device
    Mutex                  mOutputLock;

    /**** Scope for mOutputLock ****/

    uint32_t               mNextResultFrameNumber;
    uint32_t               mNextShutterFrameNumber;
    List<CameraMetadata>   mResultQueue;
    Condition              mResultSignal;
    NotificationListener  *mListener;

    /**** End scope for mOutputLock ****/

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
