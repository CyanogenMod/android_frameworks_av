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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2CLIENT_H
#define ANDROID_SERVERS_CAMERA_CAMERA2CLIENT_H

#include "Camera2Device.h"
#include "CameraService.h"
#include "camera2/Parameters.h"
#include "camera2/FrameProcessor.h"
#include "camera2/StreamingProcessor.h"
#include "camera2/JpegProcessor.h"
#include "camera2/ZslProcessor.h"
#include "camera2/CaptureSequencer.h"
#include "camera2/CallbackProcessor.h"

namespace android {

class IMemory;
/**
 * Implements the android.hardware.camera API on top of
 * camera device HAL version 2.
 */
class Camera2Client :
        public CameraService::Client,
        public Camera2Device::NotificationListener
{
public:
    /**
     * ICamera interface (see ICamera for details)
     */

    virtual void            disconnect();
    virtual status_t        connect(const sp<ICameraClient>& client);
    virtual status_t        lock();
    virtual status_t        unlock();
    virtual status_t        setPreviewDisplay(const sp<Surface>& surface);
    virtual status_t        setPreviewTexture(
        const sp<ISurfaceTexture>& surfaceTexture);
    virtual void            setPreviewCallbackFlag(int flag);
    virtual status_t        startPreview();
    virtual void            stopPreview();
    virtual bool            previewEnabled();
    virtual status_t        storeMetaDataInBuffers(bool enabled);
    virtual status_t        startRecording();
    virtual void            stopRecording();
    virtual bool            recordingEnabled();
    virtual void            releaseRecordingFrame(const sp<IMemory>& mem);
    virtual status_t        autoFocus();
    virtual status_t        cancelAutoFocus();
    virtual status_t        takePicture(int msgType);
    virtual status_t        setParameters(const String8& params);
    virtual String8         getParameters() const;
    virtual status_t        sendCommand(int32_t cmd, int32_t arg1, int32_t arg2);

    /**
     * Interface used by CameraService
     */

    Camera2Client(const sp<CameraService>& cameraService,
            const sp<ICameraClient>& cameraClient,
            int cameraId,
            int cameraFacing,
            int clientPid,
            int servicePid);
    virtual ~Camera2Client();

    status_t initialize(camera_module_t *module);

    virtual status_t dump(int fd, const Vector<String16>& args);

    /**
     * Interface used by Camera2Device
     */

    virtual void notifyError(int errorCode, int arg1, int arg2);
    virtual void notifyShutter(int frameNumber, nsecs_t timestamp);
    virtual void notifyAutoFocus(uint8_t newState, int triggerId);
    virtual void notifyAutoExposure(uint8_t newState, int triggerId);
    virtual void notifyAutoWhitebalance(uint8_t newState, int triggerId);

    /**
     * Interface used by independent components of Camera2Client.
     */

    int getCameraId() const;
    const sp<Camera2Device>& getCameraDevice();
    const sp<CameraService>& getCameraService();
    camera2::SharedParameters& getParameters();

    int getPreviewStreamId() const;
    int getCaptureStreamId() const;
    int getCallbackStreamId() const;
    int getRecordingStreamId() const;
    int getZslStreamId() const;

    status_t registerFrameListener(int32_t minId, int32_t maxId,
            wp<camera2::FrameProcessor::FilteredListener> listener);
    status_t removeFrameListener(int32_t minId, int32_t maxId,
            wp<camera2::FrameProcessor::FilteredListener> listener);

    status_t stopStream();

    // Simple class to ensure that access to ICameraClient is serialized by
    // requiring mCameraClientLock to be locked before access to mCameraClient
    // is possible.
    class SharedCameraClient {
      public:
        class Lock {
          public:
            Lock(SharedCameraClient &client);
            ~Lock();
            sp<ICameraClient> &mCameraClient;
          private:
            SharedCameraClient &mSharedClient;
        };
        SharedCameraClient(const sp<ICameraClient>& client);
        SharedCameraClient& operator=(const sp<ICameraClient>& client);
        void clear();
      private:
        sp<ICameraClient> mCameraClient;
        mutable Mutex mCameraClientLock;
    } mSharedCameraClient;

    static size_t calculateBufferSize(int width, int height,
            int format, int stride);

    static const int32_t kPreviewRequestIdStart = 10000000;
    static const int32_t kPreviewRequestIdEnd   = 20000000;

    static const int32_t kRecordingRequestIdStart  = 20000000;
    static const int32_t kRecordingRequestIdEnd    = 30000000;

    static const int32_t kCaptureRequestIdStart = 30000000;
    static const int32_t kCaptureRequestIdEnd   = 40000000;

private:
    /** ICamera interface-related private members */

    // Mutex that must be locked by methods implementing the ICamera interface.
    // Ensures serialization between incoming ICamera calls. All methods below
    // that append 'L' to the name assume that mICameraLock is locked when
    // they're called
    mutable Mutex mICameraLock;

    typedef camera2::Parameters Parameters;
    typedef camera2::CameraMetadata CameraMetadata;

    status_t setPreviewWindowL(const sp<IBinder>& binder,
            sp<ANativeWindow> window);
    status_t startPreviewL(Parameters &params, bool restart);
    void     stopPreviewL();
    status_t startRecordingL(Parameters &params, bool restart);
    bool     recordingEnabledL();

    // Individual commands for sendCommand()
    status_t commandStartSmoothZoomL();
    status_t commandStopSmoothZoomL();
    status_t commandSetDisplayOrientationL(int degrees);
    status_t commandEnableShutterSoundL(bool enable);
    status_t commandPlayRecordingSoundL();
    status_t commandStartFaceDetectionL(int type);
    status_t commandStopFaceDetectionL(Parameters &params);
    status_t commandEnableFocusMoveMsgL(bool enable);
    status_t commandPingL();
    status_t commandSetVideoBufferCountL(size_t count);

    // Current camera device configuration
    camera2::SharedParameters mParameters;

    /** Camera device-related private members */

    void     setPreviewCallbackFlagL(Parameters &params, int flag);
    status_t updateRequests(Parameters &params);

    // Used with stream IDs
    static const int NO_STREAM = -1;

    sp<camera2::FrameProcessor> mFrameProcessor;

    /* Preview/Recording related members */

    sp<IBinder> mPreviewSurface;
    sp<camera2::StreamingProcessor> mStreamingProcessor;

    /** Preview callback related members */

    sp<camera2::CallbackProcessor> mCallbackProcessor;

    /* Still image capture related members */

    sp<camera2::CaptureSequencer> mCaptureSequencer;
    sp<camera2::JpegProcessor> mJpegProcessor;
    sp<camera2::ZslProcessor> mZslProcessor;

    /** Notification-related members */

    bool mAfInMotion;

    /** Camera2Device instance wrapping HAL2 entry */

    sp<Camera2Device> mDevice;

    /** Utility members */

    // Wait until the camera device has received the latest control settings
    status_t syncWithDevice();

    // Verify that caller is the owner of the camera
    status_t checkPid(const char *checkLocation) const;
};

}; // namespace android

#endif
