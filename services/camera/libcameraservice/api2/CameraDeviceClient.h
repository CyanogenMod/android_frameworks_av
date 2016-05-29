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

#ifndef ANDROID_SERVERS_CAMERA_PHOTOGRAPHY_CAMERADEVICECLIENT_H
#define ANDROID_SERVERS_CAMERA_PHOTOGRAPHY_CAMERADEVICECLIENT_H

#include <android/hardware/camera2/BnCameraDeviceUser.h>
#include <android/hardware/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/OutputConfiguration.h>
#include <camera/camera2/SubmitInfo.h>

#include "CameraService.h"
#include "common/FrameProcessorBase.h"
#include "common/Camera2ClientBase.h"

namespace android {

struct CameraDeviceClientBase :
         public CameraService::BasicClient,
         public hardware::camera2::BnCameraDeviceUser
{
    typedef hardware::camera2::ICameraDeviceCallbacks TCamCallbacks;

    const sp<hardware::camera2::ICameraDeviceCallbacks>& getRemoteCallback() {
        return mRemoteCallback;
    }

protected:
    CameraDeviceClientBase(const sp<CameraService>& cameraService,
            const sp<hardware::camera2::ICameraDeviceCallbacks>& remoteCallback,
            const String16& clientPackageName,
            int cameraId,
            int cameraFacing,
            int clientPid,
            uid_t clientUid,
            int servicePid);

    sp<hardware::camera2::ICameraDeviceCallbacks> mRemoteCallback;
};

/**
 * Implements the binder ICameraDeviceUser API,
 * meant for HAL3-public implementation of
 * android.hardware.photography.CameraDevice
 */
class CameraDeviceClient :
        public Camera2ClientBase<CameraDeviceClientBase>,
        public camera2::FrameProcessorBase::FilteredListener
{
public:
    /**
     * ICameraDeviceUser interface (see ICameraDeviceUser for details)
     */

    // Note that the callee gets a copy of the metadata.
    virtual binder::Status submitRequest(
            const hardware::camera2::CaptureRequest& request,
            bool streaming = false,
            /*out*/
            hardware::camera2::utils::SubmitInfo *submitInfo = nullptr);
    // List of requests are copied.
    virtual binder::Status submitRequestList(
            const std::vector<hardware::camera2::CaptureRequest>& requests,
            bool streaming = false,
            /*out*/
            hardware::camera2::utils::SubmitInfo *submitInfo = nullptr);
    virtual binder::Status cancelRequest(int requestId,
            /*out*/
            int64_t* lastFrameNumber = NULL);

    virtual binder::Status beginConfigure();

    virtual binder::Status endConfigure(bool isConstrainedHighSpeed = false);

    // Returns -EBUSY if device is not idle
    virtual binder::Status deleteStream(int streamId);

    virtual binder::Status createStream(
            const hardware::camera2::params::OutputConfiguration &outputConfiguration,
            /*out*/
            int32_t* newStreamId = NULL);

    // Create an input stream of width, height, and format.
    virtual binder::Status createInputStream(int width, int height, int format,
            /*out*/
            int32_t* newStreamId = NULL);

    // Get the buffer producer of the input stream
    virtual binder::Status getInputSurface(
            /*out*/
            view::Surface *inputSurface);

    // Create a request object from a template.
    virtual binder::Status createDefaultRequest(int templateId,
            /*out*/
            hardware::camera2::impl::CameraMetadataNative* request);

    // Get the static metadata for the camera
    // -- Caller owns the newly allocated metadata
    virtual binder::Status getCameraInfo(
            /*out*/
            hardware::camera2::impl::CameraMetadataNative* cameraCharacteristics);

    // Wait until all the submitted requests have finished processing
    virtual binder::Status waitUntilIdle();

    // Flush all active and pending requests as fast as possible
    virtual binder::Status flush(
            /*out*/
            int64_t* lastFrameNumber = NULL);

    // Prepare stream by preallocating its buffers
    virtual binder::Status prepare(int32_t streamId);

    // Tear down stream resources by freeing its unused buffers
    virtual binder::Status tearDown(int32_t streamId);

    // Prepare stream by preallocating up to maxCount of its buffers
    virtual binder::Status prepare2(int32_t maxCount, int32_t streamId);

    // Set the deferred surface for a stream.
    virtual binder::Status setDeferredConfiguration(int32_t streamId,
            const hardware::camera2::params::OutputConfiguration &outputConfiguration);

    /**
     * Interface used by CameraService
     */

    CameraDeviceClient(const sp<CameraService>& cameraService,
            const sp<hardware::camera2::ICameraDeviceCallbacks>& remoteCallback,
            const String16& clientPackageName,
            int cameraId,
            int cameraFacing,
            int clientPid,
            uid_t clientUid,
            int servicePid);
    virtual ~CameraDeviceClient();

    virtual status_t      initialize(CameraModule *module);

    virtual status_t      dump(int fd, const Vector<String16>& args);

    virtual status_t      dumpClient(int fd, const Vector<String16>& args);

    /**
     * Device listener interface
     */

    virtual void notifyIdle();
    virtual void notifyError(int32_t errorCode,
                             const CaptureResultExtras& resultExtras);
    virtual void notifyShutter(const CaptureResultExtras& resultExtras, nsecs_t timestamp);
    virtual void notifyPrepared(int streamId);
    virtual void notifyRepeatingRequestError(long lastFrameNumber);

    /**
     * Interface used by independent components of CameraDeviceClient.
     */
protected:
    /** FilteredListener implementation **/
    virtual void          onResultAvailable(const CaptureResult& result);
    virtual void          detachDevice();

    // Calculate the ANativeWindow transform from android.sensor.orientation
    status_t              getRotationTransformLocked(/*out*/int32_t* transform);

private:
    /** ICameraDeviceUser interface-related private members */

    /** Preview callback related members */
    sp<camera2::FrameProcessorBase> mFrameProcessor;
    static const int32_t FRAME_PROCESSOR_LISTENER_MIN_ID = 0;
    static const int32_t FRAME_PROCESSOR_LISTENER_MAX_ID = 0x7fffffffL;

    /** Utility members */
    binder::Status checkPidStatus(const char* checkLocation);
    bool enforceRequestPermissions(CameraMetadata& metadata);

    // Find the square of the euclidean distance between two points
    static int64_t euclidDistSquare(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

    // Create an output stream with surface deferred for future.
    binder::Status createDeferredSurfaceStreamLocked(
            const hardware::camera2::params::OutputConfiguration &outputConfiguration,
            int* newStreamId = NULL);

    // Set the stream transform flags to automatically rotate the camera stream for preview use
    // cases.
    binder::Status setStreamTransformLocked(int streamId);

    // Find the closest dimensions for a given format in available stream configurations with
    // a width <= ROUNDING_WIDTH_CAP
    static const int32_t ROUNDING_WIDTH_CAP = 1920;
    static bool roundBufferDimensionNearest(int32_t width, int32_t height, int32_t format,
            android_dataspace dataSpace, const CameraMetadata& info,
            /*out*/int32_t* outWidth, /*out*/int32_t* outHeight);

    // IGraphicsBufferProducer binder -> Stream ID for output streams
    KeyedVector<sp<IBinder>, int> mStreamMap;

    struct InputStreamConfiguration {
        bool configured;
        int32_t width;
        int32_t height;
        int32_t format;
        int32_t id;
    } mInputStream;

    // Streaming request ID
    int32_t mStreamingRequestId;
    Mutex mStreamingRequestIdLock;
    static const int32_t REQUEST_ID_NONE = -1;

    int32_t mRequestIdCounter;

    // The list of output streams whose surfaces are deferred. We have to track them separately
    // as there are no surfaces available and can not be put into mStreamMap. Once the deferred
    // Surface is configured, the stream id will be moved to mStreamMap.
    Vector<int32_t> mDeferredStreams;
};

}; // namespace android

#endif
