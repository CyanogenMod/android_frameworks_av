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
#include "camera/CameraParameters.h"

namespace android {

/**
 * Implements the android.hardware.camera API on top of
 * camera device HAL version 2.
 */
class Camera2Client : public CameraService::Client
{
public:
    // ICamera interface (see ICamera for details)
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

    // Interface used by CameraService
    Camera2Client(const sp<CameraService>& cameraService,
            const sp<ICameraClient>& cameraClient,
            int cameraId,
            int cameraFacing,
            int clientPid);
    ~Camera2Client();

    status_t initialize(camera_module_t *module);

    virtual status_t dump(int fd, const Vector<String16>& args);

private:
    // Number of zoom steps to simulate
    static const unsigned int NUM_ZOOM_STEPS = 10;
    // Used with mPreviewStreamId
    static const int NO_PREVIEW_STREAM = -1;

    enum {
        NOT_INITIALIZED,
        STOPPED,
        WAITING_FOR_PREVIEW_WINDOW,
        PREVIEW
    } mState;

    sp<Camera2Device> mDevice;

    CameraParameters *mParams;

    sp<IBinder> mPreviewSurface;
    int mPreviewStreamId;
    camera_metadata_t *mPreviewRequest;

    status_t setPreviewWindow(const sp<IBinder>& binder,
            const sp<ANativeWindow>& window);

    // Convert static camera info from a camera2 device to the
    // old API parameter map.
    status_t buildDefaultParameters();

    // Update preview request based on mParams
    status_t updatePreviewRequest();
};

}; // namespace android

#endif
