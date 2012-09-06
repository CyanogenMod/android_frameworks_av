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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2PARAMETERS_H
#define ANDROID_SERVERS_CAMERA_CAMERA2PARAMETERS_H

#include <system/graphics.h>

#include <utils/Errors.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <utils/Vector.h>

#include "CameraMetadata.h"

namespace android {
namespace camera2 {

/**
 * Current camera state; this is the full state of the Camera under the old
 * camera API (contents of the CameraParameters object in a more-efficient
 * format, plus other state). The enum values are mostly based off the
 * corresponding camera2 enums, not the camera1 strings. A few are defined here
 * if they don't cleanly map to camera2 values.
 */
struct Parameters {
    /**
     * Parameters and other state
     */
    int cameraId;
    int cameraFacing;

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

    uint8_t wbMode;
    uint8_t effectMode;
    uint8_t antibandingMode;
    uint8_t sceneMode;

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
        FOCUS_MODE_CONTINUOUS_PICTURE = ANDROID_CONTROL_AF_CONTINUOUS_PICTURE,
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

    // These parameters are also part of the camera API-visible state, but not
    // directly listed in Camera.Parameters
    bool storeMetadataInBuffers;
    bool playShutterSound;
    bool enableFaceDetect;

    bool enableFocusMoveMessages;
    int afTriggerCounter;
    int currentAfTriggerId;
    bool afInMotion;

    int precaptureTriggerCounter;

    uint32_t previewCallbackFlags;
    bool previewCallbackOneShot;

    bool zslMode;

    // Overall camera state
    enum State {
        DISCONNECTED,
        STOPPED,
        WAITING_FOR_PREVIEW_WINDOW,
        PREVIEW,
        RECORD,
        STILL_CAPTURE,
        VIDEO_SNAPSHOT
    } state;

    // Number of zoom steps to simulate
    static const unsigned int NUM_ZOOM_STEPS = 10;

    // Full static camera info, object owned by someone else, such as
    // Camera2Device.
    const CameraMetadata *info;

    // Fast-access static device information; this is a subset of the
    // information available through the staticInfo() method, used for
    // frequently-accessed values or values that have to be calculated from the
    // static information.
    struct DeviceInfo {
        int32_t arrayWidth;
        int32_t arrayHeight;
        uint8_t bestFaceDetectMode;
        int32_t maxFaces;
    } fastInfo;

    /**
     * Parameter manipulation and setup methods
     */

    Parameters(int cameraId, int cameraFacing);
    ~Parameters();

    // Sets up default parameters
    status_t initialize(const CameraMetadata *info);

    // Build fast device info
    status_t buildFastInfo();

    // Get entry from camera static characteristics information. min/maxCount
    // are used for error checking the number of values in the entry. 0 for
    // max/minCount means to do no bounds check in that direction. In case of
    // error, the entry data pointer is null and the count is 0.
    camera_metadata_ro_entry_t staticInfo(uint32_t tag,
            size_t minCount=0, size_t maxCount=0) const;

    // Validate and update camera parameters based on new settings
    status_t set(const String8 &params);

    // Update passed-in request for common parameters
    status_t updateRequest(CameraMetadata *request) const;

    // Static methods for debugging and converting between camera1 and camera2
    // parameters

    static const char *getStateName(State state);

    static int formatStringToEnum(const char *format);
    static const char *formatEnumToString(int format);

    static int wbModeStringToEnum(const char *wbMode);
    static int effectModeStringToEnum(const char *effectMode);
    static int abModeStringToEnum(const char *abMode);
    static int sceneModeStringToEnum(const char *sceneMode);
    static flashMode_t flashModeStringToEnum(const char *flashMode);
    static focusMode_t focusModeStringToEnum(const char *focusMode);
    static status_t parseAreas(const char *areasCStr,
            Vector<Area> *areas);
    static status_t validateAreas(const Vector<Area> &areas,
                                  size_t maxRegions);
    static bool boolFromString(const char *boolStr);

    // Map from camera orientation + facing to gralloc transform enum
    static int degToTransform(int degrees, bool mirror);

    // Transform between (-1000,-1000)-(1000,1000) normalized coords from camera
    // API and HAL2 (0,0)-(activePixelArray.width/height) coordinates
    int arrayXToNormalized(int width) const;
    int arrayYToNormalized(int height) const;
    int normalizedXToArray(int x) const;
    int normalizedYToArray(int y) const;

};

// This class encapsulates the Parameters class so that it can only be accessed
// by constructing a Lock object, which locks the SharedParameter's mutex.
class SharedParameters {
  public:
    SharedParameters(int cameraId, int cameraFacing):
            mParameters(cameraId, cameraFacing) {
    }

    template<typename S, typename P>
    class BaseLock {
      public:
        BaseLock(S &p):
                mParameters(p.mParameters),
                mSharedParameters(p) {
            mSharedParameters.mLock.lock();
        }

        ~BaseLock() {
            mSharedParameters.mLock.unlock();
        }
        P &mParameters;
      private:
        // Disallow copying, default construction
        BaseLock();
        BaseLock(const BaseLock &);
        BaseLock &operator=(const BaseLock &);
        S &mSharedParameters;
    };
    typedef BaseLock<SharedParameters, Parameters> Lock;
    typedef BaseLock<const SharedParameters, const Parameters> ReadLock;

    // Access static info, read-only and immutable, so no lock needed
    camera_metadata_ro_entry_t staticInfo(uint32_t tag,
            size_t minCount=0, size_t maxCount=0) const {
        return mParameters.staticInfo(tag, minCount, maxCount);
    }

    // Only use for dumping or other debugging
    const Parameters &unsafeAccess() {
        return mParameters;
    }
  private:
    Parameters mParameters;
    mutable Mutex mLock;
};


}; // namespace camera2
}; // namespace android

#endif
