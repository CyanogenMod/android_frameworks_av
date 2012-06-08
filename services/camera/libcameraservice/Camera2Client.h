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
    enum {
        NOT_INITIALIZED,
        STOPPED,
        WAITING_FOR_PREVIEW_WINDOW,
        PREVIEW,
        RECORD,
        STILL_CAPTURE
    } mState;

    /** ICamera interface-related private members */

    // Mutex that must be locked by methods implementing the ICamera interface.
    // Ensures serialization between incoming ICamera calls
    mutable Mutex mICameraLock;

    status_t setPreviewWindow(const sp<IBinder>& binder,
            const sp<ANativeWindow>& window);
    void stopPreviewLocked();

    // Mutex that must be locked before accessing mParams, mParamsFlattened
    mutable Mutex mParamsLock;
    String8 mParamsFlattened;
    // Current camera state; this is the contents of the CameraParameters object
    // in a more-efficient format. The enum values are mostly based off the
    // corresponding camera2 enums, not the camera1 strings. A few are defined
    // here if they don't cleanly map to camera2 values.
    struct Parameters {
        int previewWidth, previewHeight;
        int previewFpsRangeMin, previewFpsRangeMax;
        int previewFps; // deprecated, here only for tracking changes
        int previewFormat;

        int pictureWidth, pictureHeight;

        int jpegThumbWidth, jpegThumbHeight;
        int jpegQuality, jpegThumbQuality;
        int jpegRotation;

        bool gpsEnabled;
        double gpsLatitude;
        double gpsLongitude;
        double gpsAltitude;
        int64_t gpsTimestamp;
        String8 gpsProcessingMethod;

        int wbMode;
        int effectMode;
        int antibandingMode;
        int sceneMode;

        enum flashMode_t {
            FLASH_MODE_OFF = 0,
            FLASH_MODE_AUTO,
            FLASH_MODE_ON,
            FLASH_MODE_TORCH,
            FLASH_MODE_RED_EYE = ANDROID_CONTROL_AE_ON_AUTO_FLASH_REDEYE,
            FLASH_MODE_INVALID = -1
        } flashMode;

        enum focusMode_t {
            FOCUS_MODE_AUTO = ANDROID_CONTROL_AF_AUTO,
            FOCUS_MODE_MACRO = ANDROID_CONTROL_AF_MACRO,
            FOCUS_MODE_CONTINUOUS_VIDEO = ANDROID_CONTROL_AF_CONTINUOUS_VIDEO,
            FOCUS_MODE_CONTINUOUS_PICTURE =
                ANDROID_CONTROL_AF_CONTINUOUS_PICTURE,
            FOCUS_MODE_EDOF = ANDROID_CONTROL_AF_EDOF,
            FOCUS_MODE_INFINITY,
            FOCUS_MODE_FIXED,
            FOCUS_MODE_INVALID = -1
        } focusMode;

        struct Area {
            int left, top, right, bottom;
            int weight;
            Area() {}
            Area(int left, int top, int right, int bottom, int weight):
                    left(left), top(top), right(right), bottom(bottom),
                    weight(weight) {}
        };
        Vector<Area> focusingAreas;

        int exposureCompensation;
        bool autoExposureLock;
        bool autoWhiteBalanceLock;

        Vector<Area> meteringAreas;

        int zoom;

        int videoWidth, videoHeight;

        bool recordingHint;
        bool videoStabilization;
    } mParameters;

    /** Camera device-related private members */

    // Number of zoom steps to simulate
    static const unsigned int NUM_ZOOM_STEPS = 10;
    // Used with mPreviewStreamId
    static const int NO_PREVIEW_STREAM = -1;

    sp<IBinder> mPreviewSurface;
    int mPreviewStreamId;
    camera_metadata_t *mPreviewRequest;

    camera_metadata_t *mCaptureRequest;

    sp<Camera2Device> mDevice;


    // Get values for static camera info entry. min/maxCount are used for error
    // checking the number of values in the entry. 0 for max/minCount means to
    // do no bounds check in that direction. In case of error, the entry data
    // pointer is null and the count is 0.
    camera_metadata_entry_t staticInfo(uint32_t tag,
            size_t minCount=0, size_t maxCount=0);

    /** Utility methods */

    // Convert static camera info from a camera2 device to the
    // old API parameter map.
    status_t buildDefaultParameters();

    // Update preview request based on mParams
    status_t updatePreviewRequest();

    // Convert camera1 preview format string to camera2 enum
    static int formatStringToEnum(const char *format);
    static const char *formatEnumToString(int format);

    static int wbModeStringToEnum(const char *wbMode);
    static int effectModeStringToEnum(const char *effectMode);
    static int abModeStringToEnum(const char *abMode);
    static int sceneModeStringToEnum(const char *sceneMode);
    static Parameters::flashMode_t flashModeStringToEnum(const char *flashMode);
    static Parameters::focusMode_t focusModeStringToEnum(const char *focusMode);
    static status_t parseAreas(const char *areasCStr,
            Vector<Parameters::Area> *areas);
    static status_t validateAreas(const Vector<Parameters::Area> &areas,
                                  size_t maxRegions);
    static bool boolFromString(const char *boolStr);
};

}; // namespace android

#endif
