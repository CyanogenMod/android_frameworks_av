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
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <gui/CpuConsumer.h>
#include "MediaConsumer.h"

namespace android {

/**
 * Implements the android.hardware.camera API on top of
 * camera device HAL version 2.
 */
class Camera2Client : public CameraService::Client,
                      public Camera2Device::NotificationListener
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
    virtual ~Camera2Client();

    status_t initialize(camera_module_t *module);

    virtual status_t dump(int fd, const Vector<String16>& args);

    // Interface used by CameraDevice

    virtual void notifyError(int errorCode, int arg1, int arg2);
    virtual void notifyShutter(int frameNumber, nsecs_t timestamp);
    virtual void notifyAutoFocus(uint8_t newState, int triggerId);
    virtual void notifyAutoExposure(uint8_t newState, int triggerId);
    virtual void notifyAutoWhitebalance(uint8_t newState, int triggerId);

private:
    enum State {
        DISCONNECTED,
        STOPPED,
        WAITING_FOR_PREVIEW_WINDOW,
        PREVIEW,
        RECORD,
        STILL_CAPTURE,
        VIDEO_SNAPSHOT
    } mState;

    static const char *getStateName(State state);

    /** ICamera interface-related private members */

    // Mutex that must be locked by methods implementing the ICamera interface.
    // Ensures serialization between incoming ICamera calls. All methods below
    // that append 'L' to the name assume that mICameraLock is locked when
    // they're called
    mutable Mutex mICameraLock;

    status_t setPreviewWindowL(const sp<IBinder>& binder,
            sp<ANativeWindow> window);

    void stopPreviewL();
    status_t startPreviewL();

    bool recordingEnabledL();

    // Individual commands for sendCommand()
    status_t commandStartSmoothZoomL();
    status_t commandStopSmoothZoomL();
    status_t commandSetDisplayOrientationL(int degrees);
    status_t commandEnableShutterSoundL(bool enable);
    status_t commandPlayRecordingSoundL();
    status_t commandStartFaceDetectionL(int type);
    status_t commandStopFaceDetectionL();
    status_t commandEnableFocusMoveMsgL(bool enable);
    status_t commandPingL();
    status_t commandSetVideoBufferCountL(size_t count);

    // Current camera state; this is the contents of the CameraParameters object
    // in a more-efficient format. The enum values are mostly based off the
    // corresponding camera2 enums, not the camera1 strings. A few are defined
    // here if they don't cleanly map to camera2 values.
    struct Parameters {
        int previewWidth, previewHeight;
        int32_t previewFpsRange[2];
        int previewFps; // deprecated, here only for tracking changes
        int previewFormat;

        int previewTransform; // set by CAMERA_CMD_SET_DISPLAY_ORIENTATION

        int pictureWidth, pictureHeight;

        int32_t jpegThumbSize[2];
        int32_t jpegQuality, jpegThumbQuality;
        int32_t jpegRotation;

        bool gpsEnabled;
        double gpsCoordinates[3];
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

        int32_t exposureCompensation;
        bool autoExposureLock;
        bool autoWhiteBalanceLock;

        Vector<Area> meteringAreas;

        int zoom;

        int videoWidth, videoHeight;

        bool recordingHint;
        bool videoStabilization;

        String8 paramsFlattened;

        // These parameters are also part of the camera API-visible state, but not directly
        // listed in Camera.Parameters
        bool storeMetadataInBuffers;
        bool playShutterSound;
        bool enableFocusMoveMessages;

        int afTriggerCounter;
        int currentAfTriggerId;
        bool afInMotion;
    };

    class LockedParameters {
      public:
        class Key {
          public:
            Key(LockedParameters &p):
                    mParameters(p.mParameters),
                    mLockedParameters(p) {
                mLockedParameters.mLock.lock();
            }

            ~Key() {
                mLockedParameters.mLock.unlock();
            }
            Parameters &mParameters;
          private:
            // Disallow copying, default construction
            Key();
            Key(const Key &);
            Key &operator=(const Key &);
            LockedParameters &mLockedParameters;
        };
        class ReadKey {
          public:
            ReadKey(const LockedParameters &p):
                    mParameters(p.mParameters),
                    mLockedParameters(p) {
                mLockedParameters.mLock.lock();
            }

            ~ReadKey() {
                mLockedParameters.mLock.unlock();
            }
            const Parameters &mParameters;
          private:
            // Disallow copying, default construction
            ReadKey();
            ReadKey(const ReadKey &);
            ReadKey &operator=(const ReadKey &);
            const LockedParameters &mLockedParameters;
        };

        // Only use for dumping or other debugging
        const Parameters &unsafeUnlock() {
            return mParameters;
        }
      private:
        Parameters mParameters;
        mutable Mutex mLock;

    } mParameters;

    /** Camera device-related private members */

    class Camera2Heap;

    // Number of zoom steps to simulate
    static const unsigned int NUM_ZOOM_STEPS = 10;
    // Used with stream IDs
    static const int NO_STREAM = -1;

    /* Preview related members */

    int mPreviewStreamId;
    camera_metadata_t *mPreviewRequest;
    sp<IBinder> mPreviewSurface;
    sp<ANativeWindow> mPreviewWindow;

    status_t updatePreviewRequest(const Parameters &params);
    status_t updatePreviewStream(const Parameters &params);

    /* Still image capture related members */

    int mCaptureStreamId;
    sp<CpuConsumer>    mCaptureConsumer;
    sp<ANativeWindow>  mCaptureWindow;
    // Simple listener that forwards frame available notifications from
    // a CPU consumer to the capture notification
    class CaptureWaiter: public CpuConsumer::FrameAvailableListener {
      public:
        CaptureWaiter(Camera2Client *parent) : mParent(parent) {}
        void onFrameAvailable() { mParent->onCaptureAvailable(); }
      private:
        Camera2Client *mParent;
    };
    sp<CaptureWaiter>  mCaptureWaiter;
    camera_metadata_t *mCaptureRequest;
    sp<Camera2Heap>    mCaptureHeap;
    // Handle captured image buffers
    void onCaptureAvailable();

    status_t updateCaptureRequest(const Parameters &params);
    status_t updateCaptureStream(const Parameters &params);

    /* Recording related members */

    int mRecordingStreamId;
    sp<MediaConsumer>    mRecordingConsumer;
    sp<ANativeWindow>  mRecordingWindow;
    // Simple listener that forwards frame available notifications from
    // a CPU consumer to the recording notification
    class RecordingWaiter: public MediaConsumer::FrameAvailableListener {
      public:
        RecordingWaiter(Camera2Client *parent) : mParent(parent) {}
        void onFrameAvailable() { mParent->onRecordingFrameAvailable(); }
      private:
        Camera2Client *mParent;
    };
    sp<RecordingWaiter>  mRecordingWaiter;
    camera_metadata_t *mRecordingRequest;
    sp<Camera2Heap> mRecordingHeap;

    static const size_t kDefaultRecordingHeapCount = 8;
    size_t mRecordingHeapCount;
    size_t mRecordingHeapHead, mRecordingHeapFree;
    // Handle new recording image buffers
    void onRecordingFrameAvailable();

    status_t updateRecordingRequest(const Parameters &params);
    status_t updateRecordingStream(const Parameters &params);

    /** Notification-related members */

    bool mAfInMotion;

    /** Camera2Device instance wrapping HAL2 entry */

    sp<Camera2Device> mDevice;

    /** Utility members */

    // Verify that caller is the owner of the camera
    status_t checkPid(const char *checkLocation) const;

    // Utility class for managing a set of IMemory blocks
    class Camera2Heap : public RefBase {
    public:
        Camera2Heap(size_t buf_size, uint_t num_buffers = 1,
                const char *name = NULL) :
                         mBufSize(buf_size),
                         mNumBufs(num_buffers) {
            mHeap = new MemoryHeapBase(buf_size * num_buffers, 0, name);
            mBuffers = new sp<MemoryBase>[mNumBufs];
            for (uint_t i = 0; i < mNumBufs; i++)
                mBuffers[i] = new MemoryBase(mHeap,
                                             i * mBufSize,
                                             mBufSize);
        }

        virtual ~Camera2Heap()
        {
            delete [] mBuffers;
        }

        size_t mBufSize;
        uint_t mNumBufs;
        sp<MemoryHeapBase> mHeap;
        sp<MemoryBase> *mBuffers;
    };

    // Get values for static camera info entry. min/maxCount are used for error
    // checking the number of values in the entry. 0 for max/minCount means to
    // do no bounds check in that direction. In case of error, the entry data
    // pointer is null and the count is 0.
    camera_metadata_entry_t staticInfo(uint32_t tag,
            size_t minCount=0, size_t maxCount=0);

    // Convert static camera info from a camera2 device to the
    // old API parameter map.
    status_t buildDefaultParameters();

    // Update parameters all requests use, based on mParameters
    status_t updateRequestCommon(camera_metadata_t *request, const Parameters &params);

    // Update specific metadata entry with new values. Adds entry if it does not
    // exist, which will invalidate sorting
    static status_t updateEntry(camera_metadata_t *buffer,
            uint32_t tag, const void *data, size_t data_count);

    // Remove metadata entry. Will invalidate sorting. If entry does not exist,
    // does nothing.
    static status_t deleteEntry(camera_metadata_t *buffer,
            uint32_t tag);

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

    // Map from camera orientation + facing to gralloc transform enum
    static int degToTransform(int degrees, bool mirror);

};

}; // namespace android

#endif
