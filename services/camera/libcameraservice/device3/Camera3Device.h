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
#include <utils/KeyedVector.h>
#include <utils/Timers.h>
#include <hardware/camera3.h>
#include <camera/CaptureResult.h>

#include "common/CameraDeviceBase.h"
#include "device3/StatusTracker.h"
#include "device3/Camera3BufferManager.h"
#include "utils/TagMonitor.h"

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

namespace camera3 {

class Camera3Stream;
class Camera3ZslStream;
class Camera3OutputStreamInterface;
class Camera3StreamInterface;

}

/**
 * CameraDevice for HAL devices with version CAMERA_DEVICE_API_VERSION_3_0 or higher.
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
    virtual status_t initialize(CameraModule *module);
    virtual status_t disconnect();
    virtual status_t dump(int fd, const Vector<String16> &args);
    virtual const CameraMetadata& info() const;

    // Capture and setStreamingRequest will configure streams if currently in
    // idle state
    virtual status_t capture(CameraMetadata &request, int64_t *lastFrameNumber = NULL);
    virtual status_t captureList(const List<const CameraMetadata> &requests,
                                 int64_t *lastFrameNumber = NULL);
    virtual status_t setStreamingRequest(const CameraMetadata &request,
                                         int64_t *lastFrameNumber = NULL);
    virtual status_t setStreamingRequestList(const List<const CameraMetadata> &requests,
                                             int64_t *lastFrameNumber = NULL);
    virtual status_t clearStreamingRequest(int64_t *lastFrameNumber = NULL);

    virtual status_t waitUntilRequestReceived(int32_t requestId, nsecs_t timeout);

    // Actual stream creation/deletion is delayed until first request is submitted
    // If adding streams while actively capturing, will pause device before adding
    // stream, reconfiguring device, and unpausing. If the client create a stream
    // with nullptr consumer surface, the client must then call setConsumer()
    // and finish the stream configuration before starting output streaming.
    virtual status_t createStream(sp<Surface> consumer,
            uint32_t width, uint32_t height, int format,
            android_dataspace dataSpace, camera3_stream_rotation_t rotation, int *id,
            int streamSetId = camera3::CAMERA3_STREAM_SET_ID_INVALID,
            uint32_t consumerUsage = 0);
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
            uint32_t *width, uint32_t *height,
            uint32_t *format, android_dataspace *dataSpace);
    virtual status_t setStreamTransform(int id, int transform);

    virtual status_t deleteStream(int id);
    virtual status_t deleteReprocessStream(int id);

    virtual status_t configureStreams(bool isConstraiedHighSpeed = false);
    virtual status_t getInputBufferProducer(
            sp<IGraphicBufferProducer> *producer);

    virtual status_t createDefaultRequest(int templateId, CameraMetadata *request);

    // Transitions to the idle state on success
    virtual status_t waitUntilDrained();

    virtual status_t setNotifyCallback(wp<NotificationListener> listener);
    virtual bool     willNotify3A();
    virtual status_t waitForNextFrame(nsecs_t timeout);
    virtual status_t getNextResult(CaptureResult *frame);

    virtual status_t triggerAutofocus(uint32_t id);
    virtual status_t triggerCancelAutofocus(uint32_t id);
    virtual status_t triggerPrecaptureMetering(uint32_t id);

    virtual status_t pushReprocessBuffer(int reprocessStreamId,
            buffer_handle_t *buffer, wp<BufferReleasedListener> listener);

    virtual status_t flush(int64_t *lastFrameNumber = NULL);

    virtual status_t prepare(int streamId);

    virtual status_t tearDown(int streamId);

    virtual status_t addBufferListenerForStream(int streamId,
            wp<camera3::Camera3StreamBufferListener> listener);

    virtual status_t prepare(int maxCount, int streamId);

    virtual uint32_t getDeviceVersion();

    virtual ssize_t getJpegBufferSize(uint32_t width, uint32_t height) const;
    ssize_t getPointCloudBufferSize() const;
    ssize_t getRawOpaqueBufferSize(int32_t width, int32_t height) const;

    // Methods called by subclasses
    void             notifyStatus(bool idle); // updates from StatusTracker

    /**
     * Set the deferred consumer surface to the output stream and finish the deferred
     * consumer configuration.
     */
    virtual status_t setConsumerSurface(int streamId, sp<Surface> consumer);

  private:
    static const size_t        kDumpLockAttempts  = 10;
    static const size_t        kDumpSleepDuration = 100000; // 0.10 sec
    static const nsecs_t       kShutdownTimeout   = 5000000000; // 5 sec
    static const nsecs_t       kActiveTimeout     = 500000000;  // 500 ms
    static const size_t        kInFlightWarnLimit = 20;
    static const size_t        kInFlightWarnLimitHighSpeed = 256; // batch size 32 * pipe depth 8
    // SCHED_FIFO priority for request submission thread in HFR mode
    static const int           kRequestThreadPriority = 1;

    struct                     RequestTrigger;
    // minimal jpeg buffer size: 256KB + blob header
    static const ssize_t       kMinJpegBufferSize = 256 * 1024 + sizeof(camera3_jpeg_blob);
    // Constant to use for stream ID when one doesn't exist
    static const int           NO_STREAM = -1;

    // A lock to enforce serialization on the input/configure side
    // of the public interface.
    // Only locked by public methods inherited from CameraDeviceBase.
    // Not locked by methods guarded by mOutputLock, since they may act
    // concurrently to the input/configure side of the interface.
    // Must be locked before mLock if both will be locked by a method
    Mutex                      mInterfaceLock;

    // The main lock on internal state
    Mutex                      mLock;

    // Camera device ID
    const int                  mId;

    // Flag indicating is the current active stream configuration is constrained high speed.
    bool                       mIsConstrainedHighSpeedConfiguration;

    /**** Scope for mLock ****/

    camera3_device_t          *mHal3Device;

    CameraMetadata             mDeviceInfo;

    CameraMetadata             mRequestTemplateCache[CAMERA3_TEMPLATE_COUNT];

    uint32_t                   mDeviceVersion;

    // whether Camera3Device should derive ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST for
    // backward compatibility. Should not be changed after initialization.
    bool                       mDerivePostRawSensKey = false;

    struct Size {
        uint32_t width;
        uint32_t height;
        Size(uint32_t w = 0, uint32_t h = 0) : width(w), height(h){}
    };
    // Map from format to size.
    Vector<Size>               mSupportedOpaqueInputSizes;

    enum Status {
        STATUS_ERROR,
        STATUS_UNINITIALIZED,
        STATUS_UNCONFIGURED,
        STATUS_CONFIGURED,
        STATUS_ACTIVE
    }                          mStatus;

    // Only clear mRecentStatusUpdates, mStatusWaiters from waitUntilStateThenRelock
    Vector<Status>             mRecentStatusUpdates;
    int                        mStatusWaiters;

    Condition                  mStatusChanged;

    // Tracking cause of fatal errors when in STATUS_ERROR
    String8                    mErrorCause;

    // Mapping of stream IDs to stream instances
    typedef KeyedVector<int, sp<camera3::Camera3OutputStreamInterface> >
            StreamSet;

    StreamSet                  mOutputStreams;
    sp<camera3::Camera3Stream> mInputStream;
    int                        mNextStreamId;
    bool                       mNeedConfig;

    int                        mDummyStreamId;

    // Whether to send state updates upstream
    // Pause when doing transparent reconfiguration
    bool                       mPauseStateNotify;

    // Need to hold on to stream references until configure completes.
    Vector<sp<camera3::Camera3StreamInterface> > mDeletedStreams;

    // Whether the HAL will send partial result
    bool                       mUsePartialResult;

    // Number of partial results that will be delivered by the HAL.
    uint32_t                   mNumPartialResults;

    /**** End scope for mLock ****/

    // The offset converting from clock domain of other subsystem
    // (video/hardware composer) to that of camera. Assumption is that this
    // offset won't change during the life cycle of the camera device. In other
    // words, camera device shouldn't be open during CPU suspend.
    nsecs_t                    mTimestampOffset;

    typedef struct AeTriggerCancelOverride {
        bool applyAeLock;
        uint8_t aeLock;
        bool applyAePrecaptureTrigger;
        uint8_t aePrecaptureTrigger;
    } AeTriggerCancelOverride_t;

    class CaptureRequest : public LightRefBase<CaptureRequest> {
      public:
        CameraMetadata                      mSettings;
        sp<camera3::Camera3Stream>          mInputStream;
        camera3_stream_buffer_t             mInputBuffer;
        Vector<sp<camera3::Camera3OutputStreamInterface> >
                                            mOutputStreams;
        CaptureResultExtras                 mResultExtras;
        // Used to cancel AE precapture trigger for devices doesn't support
        // CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL
        AeTriggerCancelOverride_t           mAeTriggerCancelOverride;
        // The number of requests that should be submitted to HAL at a time.
        // For example, if batch size is 8, this request and the following 7
        // requests will be submitted to HAL at a time. The batch size for
        // the following 7 requests will be ignored by the request thread.
        int                                 mBatchSize;
    };
    typedef List<sp<CaptureRequest> > RequestList;

    status_t checkStatusOkToCaptureLocked();

    status_t convertMetadataListToRequestListLocked(
            const List<const CameraMetadata> &metadataList,
            /*out*/
            RequestList *requestList);

    status_t submitRequestsHelper(const List<const CameraMetadata> &requests, bool repeating,
                                  int64_t *lastFrameNumber = NULL);

    /**
     * Get the last request submitted to the hal by the request thread.
     *
     * Takes mLock.
     */
    virtual CameraMetadata getLatestRequestLocked();

    /**
     * Update the current device status and wake all waiting threads.
     *
     * Must be called with mLock held.
     */
    void internalUpdateStatusLocked(Status status);

    /**
     * Pause processing and flush everything, but don't tell the clients.
     * This is for reconfiguring outputs transparently when according to the
     * CameraDeviceBase interface we shouldn't need to.
     * Must be called with mLock and mInterfaceLock both held.
     */
    status_t internalPauseAndWaitLocked();

    /**
     * Resume work after internalPauseAndWaitLocked()
     * Must be called with mLock and mInterfaceLock both held.
     */
    status_t internalResumeLocked();

    /**
     * Wait until status tracker tells us we've transitioned to the target state
     * set, which is either ACTIVE when active==true or IDLE (which is any
     * non-ACTIVE state) when active==false.
     *
     * Needs to be called with mLock and mInterfaceLock held.  This means there
     * can ever only be one waiter at most.
     *
     * During the wait mLock is released.
     *
     */
    status_t waitUntilStateThenRelock(bool active, nsecs_t timeout);

    /**
     * Implementation of waitUntilDrained. On success, will transition to IDLE state.
     *
     * Need to be called with mLock and mInterfaceLock held.
     */
    status_t waitUntilDrainedLocked();

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
     * Cancel stream configuration that did not finish successfully.
     */
    void               cancelStreamsConfigurationLocked();

    /**
     * Add a dummy stream to the current stream set as a workaround for
     * not allowing 0 streams in the camera HAL spec.
     */
    status_t           addDummyStreamLocked();

    /**
     * Remove a dummy stream if the current config includes real streams.
     */
    status_t           tryRemoveDummyStreamLocked();

    /**
     * Set device into an error state due to some fatal failure, and set an
     * error message to indicate why. Only the first call's message will be
     * used. The message is also sent to the log.
     */
    void               setErrorState(const char *fmt, ...);
    void               setErrorStateV(const char *fmt, va_list args);
    void               setErrorStateLocked(const char *fmt, ...);
    void               setErrorStateLockedV(const char *fmt, va_list args);

    /**
     * Debugging trylock/spin method
     * Try to acquire a lock a few times with sleeps between before giving up.
     */
    bool               tryLockSpinRightRound(Mutex& lock);

    /**
     * Helper function to determine if an input size for implementation defined
     * format is supported.
     */
    bool isOpaqueInputSizeSupported(uint32_t width, uint32_t height);

    /**
     * Helper function to get the largest Jpeg resolution (in area)
     * Return Size(0, 0) if static metatdata is invalid
     */
    Size getMaxJpegResolution() const;

    /**
     * Helper function to get the offset between MONOTONIC and BOOTTIME
     * timestamp.
     */
    static nsecs_t getMonoToBoottimeOffset();

    /**
     * Helper function to map between legacy and new dataspace enums
     */
    static android_dataspace mapToLegacyDataspace(android_dataspace dataSpace);

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
                sp<camera3::StatusTracker> statusTracker,
                camera3_device_t *hal3Device,
                bool aeLockAvailable);

        void     setNotificationListener(wp<NotificationListener> listener);

        /**
         * Call after stream (re)-configuration is completed.
         */
        void     configurationComplete(bool isConstrainedHighSpeed);

        /**
         * Set or clear the list of repeating requests. Does not block
         * on either. Use waitUntilPaused to wait until request queue
         * has emptied out.
         */
        status_t setRepeatingRequests(const RequestList& requests,
                                      /*out*/
                                      int64_t *lastFrameNumber = NULL);
        status_t clearRepeatingRequests(/*out*/
                                        int64_t *lastFrameNumber = NULL);

        status_t queueRequestList(List<sp<CaptureRequest> > &requests,
                                  /*out*/
                                  int64_t *lastFrameNumber = NULL);

        /**
         * Remove all queued and repeating requests, and pending triggers
         */
        status_t clear(/*out*/int64_t *lastFrameNumber = NULL);

        /**
         * Flush all pending requests in HAL.
         */
        status_t flush();

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
         * Wait until thread processes the capture request with settings'
         * android.request.id == requestId.
         *
         * Returns TIMED_OUT in case the thread does not process the request
         * within the timeout.
         */
        status_t waitUntilRequestProcessed(int32_t requestId, nsecs_t timeout);

        /**
         * Shut down the thread. Shutdown is asynchronous, so thread may
         * still be running once this method returns.
         */
        virtual void requestExit();

        /**
         * Get the latest request that was sent to the HAL
         * with process_capture_request.
         */
        CameraMetadata getLatestRequest() const;

        /**
         * Returns true if the stream is a target of any queued or repeating
         * capture request
         */
        bool isStreamPending(sp<camera3::Camera3StreamInterface>& stream);

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

        // HAL workaround: Make sure a trigger ID always exists if
        // a trigger does
        status_t          addDummyTriggerIds(const sp<CaptureRequest> &request);

        static const nsecs_t kRequestTimeout = 50e6; // 50 ms

        // Used to prepare a batch of requests.
        struct NextRequest {
            sp<CaptureRequest>              captureRequest;
            camera3_capture_request_t       halRequest;
            Vector<camera3_stream_buffer_t> outputBuffers;
            bool                            submitted;
        };

        // Wait for the next batch of requests and put them in mNextRequests. mNextRequests will
        // be empty if it times out.
        void waitForNextRequestBatch();

        // Waits for a request, or returns NULL if times out. Must be called with mRequestLock hold.
        sp<CaptureRequest> waitForNextRequestLocked();

        // Prepare HAL requests and output buffers in mNextRequests. Return TIMED_OUT if getting any
        // output buffer timed out. If an error is returned, the caller should clean up the pending
        // request batch.
        status_t prepareHalRequests();

        // Return buffers, etc, for requests in mNextRequests that couldn't be fully constructed and
        // send request errors if sendRequestError is true. The buffers will be returned in the
        // ERROR state to mark them as not having valid data. mNextRequests will be cleared.
        void cleanUpFailedRequests(bool sendRequestError);

        // Stop the repeating request if any of its output streams is abandoned.
        void checkAndStopRepeatingRequest();

        // Pause handling
        bool               waitIfPaused();
        void               unpauseForNewRequests();

        // Relay error to parent device object setErrorState
        void               setErrorState(const char *fmt, ...);

        // If the input request is in mRepeatingRequests. Must be called with mRequestLock hold
        bool isRepeatingRequestLocked(const sp<CaptureRequest>);

        // Handle AE precapture trigger cancel for devices <= CAMERA_DEVICE_API_VERSION_3_2.
        void handleAePrecaptureCancelRequest(sp<CaptureRequest> request);

        // Clear repeating requests. Must be called with mRequestLock held.
        status_t clearRepeatingRequestsLocked(/*out*/ int64_t *lastFrameNumber = NULL);

        wp<Camera3Device>  mParent;
        wp<camera3::StatusTracker>  mStatusTracker;
        camera3_device_t  *mHal3Device;

        wp<NotificationListener> mListener;

        const int          mId;       // The camera ID
        int                mStatusId; // The RequestThread's component ID for
                                      // status tracking

        Mutex              mRequestLock;
        Condition          mRequestSignal;
        RequestList        mRequestQueue;
        RequestList        mRepeatingRequests;
        // The next batch of requests being prepped for submission to the HAL, no longer
        // on the request queue. Read-only even with mRequestLock held, outside
        // of threadLoop
        Vector<NextRequest> mNextRequests;

        // To protect flush() and sending a request batch to HAL.
        Mutex              mFlushLock;

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

        mutable Mutex      mLatestRequestMutex;
        Condition          mLatestRequestSignal;
        // android.request.id for latest process_capture_request
        int32_t            mLatestRequestId;
        CameraMetadata     mLatestRequest;

        typedef KeyedVector<uint32_t/*tag*/, RequestTrigger> TriggerMap;
        Mutex              mTriggerMutex;
        TriggerMap         mTriggerMap;
        TriggerMap         mTriggerRemovedMap;
        TriggerMap         mTriggerReplacedMap;
        uint32_t           mCurrentAfTriggerId;
        uint32_t           mCurrentPreCaptureTriggerId;

        int64_t            mRepeatingLastFrameNumber;

        // Whether the device supports AE lock
        bool               mAeLockAvailable;

        // Flag indicating if we should prepare video stream for video requests.
        bool               mPrepareVideoStream;
    };
    sp<RequestThread> mRequestThread;

    /**
     * In-flight queue for tracking completion of capture requests.
     */

    struct InFlightRequest {
        // Set by notify() SHUTTER call.
        nsecs_t shutterTimestamp;
        // Set by process_capture_result().
        nsecs_t sensorTimestamp;
        int     requestStatus;
        // Set by process_capture_result call with valid metadata
        bool    haveResultMetadata;
        // Decremented by calls to process_capture_result with valid output
        // and input buffers
        int     numBuffersLeft;
        CaptureResultExtras resultExtras;
        // If this request has any input buffer
        bool hasInputBuffer;

        // The last metadata that framework receives from HAL and
        // not yet send out because the shutter event hasn't arrived.
        // It's added by process_capture_result and sent when framework
        // receives the shutter event.
        CameraMetadata pendingMetadata;

        // The metadata of the partial results that framework receives from HAL so far
        // and has sent out.
        CameraMetadata collectedPartialResult;

        // Buffers are added by process_capture_result when output buffers
        // return from HAL but framework has not yet received the shutter
        // event. They will be returned to the streams when framework receives
        // the shutter event.
        Vector<camera3_stream_buffer_t> pendingOutputBuffers;

        // Used to cancel AE precapture trigger for devices doesn't support
        // CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL
        AeTriggerCancelOverride_t aeTriggerCancelOverride;

        // Default constructor needed by KeyedVector
        InFlightRequest() :
                shutterTimestamp(0),
                sensorTimestamp(0),
                requestStatus(OK),
                haveResultMetadata(false),
                numBuffersLeft(0),
                hasInputBuffer(false),
                aeTriggerCancelOverride({false, 0, false, 0}){
        }

        InFlightRequest(int numBuffers, CaptureResultExtras extras, bool hasInput,
                AeTriggerCancelOverride aeTriggerCancelOverride) :
                shutterTimestamp(0),
                sensorTimestamp(0),
                requestStatus(OK),
                haveResultMetadata(false),
                numBuffersLeft(numBuffers),
                resultExtras(extras),
                hasInputBuffer(hasInput),
                aeTriggerCancelOverride(aeTriggerCancelOverride){
        }
    };

    // Map from frame number to the in-flight request state
    typedef KeyedVector<uint32_t, InFlightRequest> InFlightMap;

    Mutex                  mInFlightLock; // Protects mInFlightMap
    InFlightMap            mInFlightMap;
    int                    mInFlightStatusId;

    status_t registerInFlight(uint32_t frameNumber,
            int32_t numBuffers, CaptureResultExtras resultExtras, bool hasInput,
            const AeTriggerCancelOverride_t &aeTriggerCancelOverride);

    /**
     * Override result metadata for cancelling AE precapture trigger applied in
     * handleAePrecaptureCancelRequest().
     */
    void overrideResultForPrecaptureCancel(CameraMetadata* result,
            const AeTriggerCancelOverride_t &aeTriggerCancelOverride);

    /**
     * Tracking for idle detection
     */
    sp<camera3::StatusTracker> mStatusTracker;

    /**
     * Graphic buffer manager for output streams. Each device has a buffer manager, which is used
     * by the output streams to get and return buffers if these streams are registered to this
     * buffer manager.
     */
    sp<camera3::Camera3BufferManager> mBufferManager;

    /**
     * Thread for preparing streams
     */
    class PreparerThread : private Thread, public virtual RefBase {
      public:
        PreparerThread();
        ~PreparerThread();

        void setNotificationListener(wp<NotificationListener> listener);

        /**
         * Queue up a stream to be prepared. Streams are processed by a background thread in FIFO
         * order.  Pre-allocate up to maxCount buffers for the stream, or the maximum number needed
         * for the pipeline if maxCount is ALLOCATE_PIPELINE_MAX.
         */
        status_t prepare(int maxCount, sp<camera3::Camera3StreamInterface>& stream);

        /**
         * Cancel all current and pending stream preparation
         */
        status_t clear();

      private:
        Mutex mLock;

        virtual bool threadLoop();

        // Guarded by mLock

        wp<NotificationListener> mListener;
        List<sp<camera3::Camera3StreamInterface> > mPendingStreams;
        bool mActive;
        bool mCancelNow;

        // Only accessed by threadLoop and the destructor

        sp<camera3::Camera3StreamInterface> mCurrentStream;
    };
    sp<PreparerThread> mPreparerThread;

    /**
     * Output result queue and current HAL device 3A state
     */

    // Lock for output side of device
    Mutex                  mOutputLock;

    /**** Scope for mOutputLock ****/
    // the minimal frame number of the next non-reprocess result
    uint32_t               mNextResultFrameNumber;
    // the minimal frame number of the next reprocess result
    uint32_t               mNextReprocessResultFrameNumber;
    // the minimal frame number of the next non-reprocess shutter
    uint32_t               mNextShutterFrameNumber;
    // the minimal frame number of the next reprocess shutter
    uint32_t               mNextReprocessShutterFrameNumber;
    List<CaptureResult>   mResultQueue;
    Condition              mResultSignal;
    wp<NotificationListener>  mListener;

    /**** End scope for mOutputLock ****/

    /**
     * Callback functions from HAL device
     */
    void processCaptureResult(const camera3_capture_result *result);

    void notify(const camera3_notify_msg *msg);

    // Specific notify handlers
    void notifyError(const camera3_error_msg_t &msg,
            sp<NotificationListener> listener);
    void notifyShutter(const camera3_shutter_msg_t &msg,
            sp<NotificationListener> listener);

    // helper function to return the output buffers to the streams.
    void returnOutputBuffers(const camera3_stream_buffer_t *outputBuffers,
            size_t numBuffers, nsecs_t timestamp);

    // Send a partial capture result.
    void sendPartialCaptureResult(const camera_metadata_t * partialResult,
            const CaptureResultExtras &resultExtras, uint32_t frameNumber,
            const AeTriggerCancelOverride_t &aeTriggerCancelOverride);

    // Send a total capture result given the pending metadata and result extras,
    // partial results, and the frame number to the result queue.
    void sendCaptureResult(CameraMetadata &pendingMetadata,
            CaptureResultExtras &resultExtras,
            CameraMetadata &collectedPartialResult, uint32_t frameNumber,
            bool reprocess, const AeTriggerCancelOverride_t &aeTriggerCancelOverride);

    // Insert the result to the result queue after updating frame number and overriding AE
    // trigger cancel.
    // mOutputLock must be held when calling this function.
    void insertResultLocked(CaptureResult *result, uint32_t frameNumber,
            const AeTriggerCancelOverride_t &aeTriggerCancelOverride);

    /**** Scope for mInFlightLock ****/

    // Remove the in-flight request of the given index from mInFlightMap
    // if it's no longer needed. It must only be called with mInFlightLock held.
    void removeInFlightRequestIfReadyLocked(int idx);

    /**** End scope for mInFlightLock ****/

    // Debug tracker for metadata tag value changes
    // - Enabled with the -m <taglist> option to dumpsys, such as
    //   dumpsys -m android.control.aeState,android.control.aeMode
    // - Disabled with -m off
    // - dumpsys -m 3a is a shortcut for ae/af/awbMode, State, and Triggers
    TagMonitor mTagMonitor;

    void monitorMetadata(TagMonitor::eventSource source, int64_t frameNumber,
            nsecs_t timestamp, const CameraMetadata& metadata);

    /**
     * Static callback forwarding methods from HAL to instance
     */
    static callbacks_process_capture_result_t sProcessCaptureResult;

    static callbacks_notify_t sNotify;

}; // class Camera3Device

}; // namespace android

#endif
