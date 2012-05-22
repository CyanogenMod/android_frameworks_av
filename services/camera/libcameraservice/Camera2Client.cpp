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

#define LOG_TAG "Camera2Client"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <gui/Surface.h>

#include <math.h>

#include "Camera2Client.h"

namespace android {

#define ALOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define ALOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

#define ALOG1_ENTRY        \
    int callingPid = getCallingPid(); \
    ALOG1("%s: E (pid %d, id %d) ", __FUNCTION__, \
            callingPid, mCameraId)

#define ALOG1_EXIT        \
    ALOG1("%s: X (pid %d, id %d) ", __FUNCTION__, \
            callingPid, mCameraId)

static int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static int getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

// Interface used by CameraService

Camera2Client::Camera2Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        int cameraId,
        int cameraFacing,
        int clientPid):
        Client(cameraService, cameraClient,
                cameraId, cameraFacing, clientPid),
        mState(NOT_INITIALIZED),
        mParams(NULL),
        mPreviewStreamId(NO_PREVIEW_STREAM),
        mPreviewRequest(NULL)
{
    ALOG1_ENTRY;

    mDevice = new Camera2Device(cameraId);

    ALOG1_EXIT;
}

status_t Camera2Client::initialize(camera_module_t *module)
{
    ALOG1_ENTRY;
    status_t res;

    res = mDevice->initialize(module);
    if (res != OK) {
        ALOGE("%s: Camera %d: unable to initialize device: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return NO_INIT;
    }

    res = buildDefaultParameters();
    if (res != OK) {
        ALOGE("%s: Camera %d: unable to build defaults: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return NO_INIT;
    }
    if (gLogLevel >= 1) {
        ALOGD("%s: Default parameters converted from camera %d:", __FUNCTION__,
              mCameraId);
        mParams->dump();
    }

    mState = STOPPED;

    ALOG1_EXIT;
    return OK;
}

Camera2Client::~Camera2Client() {
    mDestructionStarted = true;

    if (mParams) delete mParams;

    disconnect();
}

status_t Camera2Client::dump(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("Client2[%d] (%p) PID: %d:\n",
            mCameraId,
            getCameraClient()->asBinder().get(),
            mClientPid);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

// ICamera interface

void Camera2Client::disconnect() {

    if (mDevice == 0) return;

    stopPreview();

    if (mPreviewStreamId != NO_PREVIEW_STREAM) {
        mDevice->deleteStream(mPreviewStreamId);
        mPreviewStreamId = NO_PREVIEW_STREAM;
    }

    CameraService::Client::disconnect();
}

status_t Camera2Client::connect(const sp<ICameraClient>& client) {
    return BAD_VALUE;
}

status_t Camera2Client::lock() {
    return BAD_VALUE;
}

status_t Camera2Client::unlock() {
    return BAD_VALUE;
}

status_t Camera2Client::setPreviewDisplay(
        const sp<Surface>& surface) {
    ALOG1_ENTRY;
    if (mState == PREVIEW) return INVALID_OPERATION;

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surface != 0) {
        binder = surface->asBinder();
        window = surface;
    }

    return setPreviewWindow(binder,window);
}

status_t Camera2Client::setPreviewTexture(
        const sp<ISurfaceTexture>& surfaceTexture) {
    ALOG1_ENTRY;
    if (mState == PREVIEW) return INVALID_OPERATION;

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surfaceTexture != 0) {
        binder = surfaceTexture->asBinder();
        window = new SurfaceTextureClient(surfaceTexture);
    }
    return setPreviewWindow(binder, window);
}

status_t Camera2Client::setPreviewWindow(const sp<IBinder>& binder,
        const sp<ANativeWindow>& window) {
    ALOG1_ENTRY;
    status_t res;

    if (binder == mPreviewSurface) {
        return NO_ERROR;
    }

    if (mPreviewStreamId != NO_PREVIEW_STREAM) {
        res = mDevice->deleteStream(mPreviewStreamId);
        if (res != OK) {
            return res;
        }
    }

    int previewWidth, previewHeight;
    mParams->getPreviewSize(&previewWidth, &previewHeight);

    res = mDevice->createStream(window,
            previewWidth, previewHeight, CAMERA2_HAL_PIXEL_FORMAT_OPAQUE,
            &mPreviewStreamId);
    if (res != OK) {
        return res;
    }

    if (mState == WAITING_FOR_PREVIEW_WINDOW) {
        return startPreview();
    }

    ALOG1_EXIT;
    return OK;
}

void Camera2Client::setPreviewCallbackFlag(int flag) {

}

status_t Camera2Client::startPreview() {
    ALOG1_ENTRY;
    status_t res;
    if (mState == PREVIEW) return INVALID_OPERATION;

    if (mPreviewStreamId == NO_PREVIEW_STREAM) {
        mState = WAITING_FOR_PREVIEW_WINDOW;
        return OK;
    }

    if (mPreviewRequest == NULL) {
        updatePreviewRequest();
    }

    uint8_t outputStream = mPreviewStreamId;

    camera_metadata_entry_t outputStreams;
    res = find_camera_metadata_entry(mPreviewRequest,
            ANDROID_REQUEST_OUTPUT_STREAMS,
            &outputStreams);
    if (res == NAME_NOT_FOUND) {
        res = add_camera_metadata_entry(mPreviewRequest,
                ANDROID_REQUEST_OUTPUT_STREAMS,
                &outputStream, 1);
    } else if (res == OK) {
        res = update_camera_metadata_entry(mPreviewRequest,
                outputStreams.index, &outputStream, 1, NULL);
    }

    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        mState = STOPPED;
        return res;
    }

    res = mDevice->setStreamingRequest(mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview request to start preview: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        mState = STOPPED;
        return res;
    }
    mState = PREVIEW;

    return OK;
}

void Camera2Client::stopPreview() {
    ALOG1_ENTRY;
    if (mState != PREVIEW) return;

    mDevice->setStreamingRequest(NULL);
    mState = STOPPED;
}

bool Camera2Client::previewEnabled() {
    return mState == PREVIEW;
}

status_t Camera2Client::storeMetaDataInBuffers(bool enabled) {
    return BAD_VALUE;
}

status_t Camera2Client::startRecording() {
    return BAD_VALUE;
}

void Camera2Client::stopRecording() {
}

bool Camera2Client::recordingEnabled() {
    return BAD_VALUE;
}

void Camera2Client::releaseRecordingFrame(const sp<IMemory>& mem) {

}

status_t Camera2Client::autoFocus() {
    return OK;
}

status_t Camera2Client::cancelAutoFocus() {
    return OK;
}

status_t Camera2Client::takePicture(int msgType) {
    return BAD_VALUE;
}

status_t Camera2Client::setParameters(const String8& params) {
    return OK;
}

String8 Camera2Client::getParameters() const {
    return mParams->flatten();
}

status_t Camera2Client::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) {
    return OK;
}

// private methods

status_t Camera2Client::buildDefaultParameters() {
    status_t res;
    if (mParams) {
        delete mParams;
    }
    mParams = new CameraParameters;

    camera_metadata_entry_t availableProcessedSizes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
            &availableProcessedSizes);
    if (res != OK) return res;
    if (availableProcessedSizes.count < 2) {
        ALOGE("%s: Camera %d: "
                "Malformed %s entry",
                __FUNCTION__, mCameraId,
                get_camera_metadata_tag_name(
                    ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES));
        return NO_INIT;
    }

    // TODO: Pick more intelligently
    int previewWidth = availableProcessedSizes.data.i32[0];
    int previewHeight = availableProcessedSizes.data.i32[1];

    mParams->setPreviewSize(previewWidth, previewHeight);
    mParams->setVideoSize(previewWidth, previewHeight);
    mParams->set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO,
            String8::format("%dx%d",previewWidth,previewHeight));
    {
        String8 supportedPreviewSizes;
        for (size_t i=0; i < availableProcessedSizes.count; i += 2) {
            if (i != 0) supportedPreviewSizes += ",";
            supportedPreviewSizes += String8::format("%dx%d",
                    availableProcessedSizes.data.i32[i],
                    availableProcessedSizes.data.i32[i+1]);
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                supportedPreviewSizes);
        mParams->set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES,
                supportedPreviewSizes);
    }

    camera_metadata_entry_t availableFpsRanges;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            &availableFpsRanges);
    if (res != OK) return res;
    if (availableFpsRanges.count < 2) {
        ALOGE("%s: Camera %d: "
                "Malformed %s entry",
                __FUNCTION__, mCameraId,
                get_camera_metadata_tag_name(
                    ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES));
        return NO_INIT;
    }

    int previewFpsRangeMin = availableFpsRanges.data.i32[0];
    int previewFpsRangeMax = availableFpsRanges.data.i32[1];

    mParams->set(CameraParameters::KEY_PREVIEW_FPS_RANGE,
            String8::format("%d,%d", previewFpsRangeMin, previewFpsRangeMax));

    {
        String8 supportedPreviewFpsRange;
        for (size_t i=0; i < availableFpsRanges.count; i += 2) {
            if (i != 0) supportedPreviewFpsRange += ",";
            supportedPreviewFpsRange += String8::format("(%d,%d)",
                    availableFpsRanges.data.i32[i],
                    availableFpsRanges.data.i32[i+1]);
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
                supportedPreviewFpsRange);
    }

    mParams->set(CameraParameters::KEY_PREVIEW_FORMAT,
            "yuv420sp"); // NV21

    camera_metadata_entry_t availableFormats;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_SCALER_AVAILABLE_FORMATS,
            &availableFormats);
    {
        String8 supportedPreviewFormats;
        bool addComma = false;
        for (size_t i=0; i < availableFormats.count; i++) {
            if (addComma) supportedPreviewFormats += ",";
            addComma = true;
            switch (availableFormats.data.i32[i]) {
            case HAL_PIXEL_FORMAT_YCbCr_422_SP:
                supportedPreviewFormats += "yuv422sp";
                break;
            case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                supportedPreviewFormats += "yuv420sp";
                break;
            case HAL_PIXEL_FORMAT_YCbCr_422_I:
                supportedPreviewFormats += "yuv422i-yuyv";
                break;
            case HAL_PIXEL_FORMAT_YV12:
                supportedPreviewFormats += "yuv420p";
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                supportedPreviewFormats += "rgb565";
                break;
                // Not advertizing JPEG, RAW_SENSOR, etc, for preview formats
            case HAL_PIXEL_FORMAT_RAW_SENSOR:
                addComma = false;
                break;
            default:
                ALOGW("%s: Camera %d: Unknown preview format: %x",
                        __FUNCTION__, mCameraId, availableFormats.data.i32[i]);
                addComma = false;
                break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                supportedPreviewFormats);
    }

    // PREVIEW_FRAME_RATE / SUPPORTED_PREVIEW_FRAME_RATES are deprecated, but
    // still have to do something sane for them

    mParams->set(CameraParameters::KEY_PREVIEW_FRAME_RATE,
            previewFpsRangeMin);

    {
        String8 supportedPreviewFrameRates;
        for (size_t i=0; i < availableFpsRanges.count; i += 2) {
            if (i != 0) supportedPreviewFrameRates += ",";
            supportedPreviewFrameRates += String8::format("%d",
                    availableFpsRanges.data.i32[i]);
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
                supportedPreviewFrameRates);
    }

    camera_metadata_entry_t availableJpegSizes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
            &availableJpegSizes);
    if (res != OK) return res;
    if (availableJpegSizes.count < 2) {
        ALOGE("%s: Camera %d: "
                "Malformed %s entry",
                __FUNCTION__, mCameraId,
                get_camera_metadata_tag_name(
                    ANDROID_SCALER_AVAILABLE_JPEG_SIZES));
        return NO_INIT;
    }

    // TODO: Pick maximum
    int32_t pictureWidth = availableJpegSizes.data.i32[0];
    int32_t pictureHeight = availableJpegSizes.data.i32[1];

    mParams->setPictureSize(pictureWidth, pictureHeight);

    {
        String8 supportedPictureSizes;
        for (size_t i=0; i < availableJpegSizes.count; i += 2) {
            if (i != 0) supportedPictureSizes += ",";
            supportedPictureSizes += String8::format("%dx%d",
                    availableJpegSizes.data.i32[i],
                    availableJpegSizes.data.i32[i+1]);
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES,
                supportedPictureSizes);
    }

    mParams->setPictureFormat("jpeg");

    mParams->set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS,
            "jpeg");

    camera_metadata_entry_t availableJpegThumbnailSizes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
            &availableJpegThumbnailSizes);
    if (res != OK) return res;
    if (availableJpegThumbnailSizes.count < 2) {
        ALOGE("%s: Camera %d: "
                "Malformed %s entry",
                __FUNCTION__, mCameraId,
                get_camera_metadata_tag_name(
                    ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES));
        return NO_INIT;
    }

    // TODO: Pick default thumbnail size sensibly
    int32_t jpegThumbWidth = availableJpegThumbnailSizes.data.i32[0];
    int32_t jpegThumbHeight = availableJpegThumbnailSizes.data.i32[1];

    mParams->set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,
            jpegThumbWidth);
    mParams->set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,
            jpegThumbHeight);

    {
        String8 supportedJpegThumbSizes;
        for (size_t i=0; i < availableJpegThumbnailSizes.count; i += 2) {
            if (i != 0) supportedJpegThumbSizes += ",";
            supportedJpegThumbSizes += String8::format("%dx%d",
                    availableJpegThumbnailSizes.data.i32[i],
                    availableJpegThumbnailSizes.data.i32[i+1]);
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES,
                supportedJpegThumbSizes);
    }

    mParams->set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY,
            "90");
    mParams->set(CameraParameters::KEY_JPEG_QUALITY,
            "90");
    mParams->set(CameraParameters::KEY_ROTATION,
            "0");
    // Not settting GPS fields

    mParams->set(CameraParameters::KEY_WHITE_BALANCE,
            "auto");

    camera_metadata_entry_t availableWhiteBalanceModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AWB_AVAILABLE_MODES,
            &availableWhiteBalanceModes);
    {
        String8 supportedWhiteBalance;
        bool addComma = false;
        for (size_t i=0; i < availableWhiteBalanceModes.count; i++) {
            if (addComma) supportedWhiteBalance += ",";
            addComma = true;
            switch (availableWhiteBalanceModes.data.u8[i]) {
            case ANDROID_CONTROL_AWB_AUTO:
                supportedWhiteBalance += "auto";
                break;
            case ANDROID_CONTROL_AWB_INCANDESCENT:
                supportedWhiteBalance += "incandescent";
                break;
            case ANDROID_CONTROL_AWB_FLUORESCENT:
                supportedWhiteBalance += "fluorescent";
                break;
            case ANDROID_CONTROL_AWB_WARM_FLUORESCENT:
                supportedWhiteBalance += "warm-fluorescent";
                break;
            case ANDROID_CONTROL_AWB_DAYLIGHT:
                supportedWhiteBalance += "daylight";
                break;
            case ANDROID_CONTROL_AWB_CLOUDY_DAYLIGHT:
                supportedWhiteBalance += "cloudy-daylight";
                break;
            case ANDROID_CONTROL_AWB_TWILIGHT:
                supportedWhiteBalance += "twilight";
                break;
            case ANDROID_CONTROL_AWB_SHADE:
                supportedWhiteBalance += "shade";
                break;
            // Skipping values not mappable to v1 API
            case ANDROID_CONTROL_AWB_OFF:
                addComma = false;
                break;
            default:
                ALOGW("%s: Camera %d: Unknown white balance value: %d",
                        __FUNCTION__, mCameraId,
                        availableWhiteBalanceModes.data.u8[i]);
                addComma = false;
                break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE,
                supportedWhiteBalance);
    }

    mParams->set(CameraParameters::KEY_EFFECT, "none");
    camera_metadata_entry_t availableEffects;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AVAILABLE_EFFECTS,
            &availableEffects);
    if (res != OK) return res;
    {
        String8 supportedEffects;
        bool addComma = false;
        for (size_t i=0; i < availableEffects.count; i++) {
            if (addComma) supportedEffects += ",";
            addComma = true;
            switch (availableEffects.data.u8[i]) {
                case ANDROID_CONTROL_EFFECT_OFF:
                    supportedEffects += "none";
                    break;
                case ANDROID_CONTROL_EFFECT_MONO:
                    supportedEffects += "mono";
                case ANDROID_CONTROL_EFFECT_NEGATIVE:
                    supportedEffects += "negative";
                    break;
                case ANDROID_CONTROL_EFFECT_SOLARIZE:
                    supportedEffects += "solarize";
                    break;
                case ANDROID_CONTROL_EFFECT_SEPIA:
                    supportedEffects += "sepia";
                    break;
                case ANDROID_CONTROL_EFFECT_POSTERIZE:
                    supportedEffects += "posterize";
                    break;
                case ANDROID_CONTROL_EFFECT_WHITEBOARD:
                    supportedEffects += "whiteboard";
                    break;
                case ANDROID_CONTROL_EFFECT_BLACKBOARD:
                    supportedEffects += "blackboard";
                    break;
                case ANDROID_CONTROL_EFFECT_AQUA:
                    supportedEffects += "aqua";
                    break;
                default:
                    ALOGW("%s: Camera %d: Unknown effect value: %d",
                        __FUNCTION__, mCameraId, availableEffects.data.u8[i]);
                    addComma = false;
                    break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_EFFECTS, supportedEffects);
    }

    mParams->set(CameraParameters::KEY_ANTIBANDING, "auto");
    camera_metadata_entry_t availableAntibandingModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
            &availableAntibandingModes);
    if (res != OK) return res;
    {
        String8 supportedAntibanding;
        bool addComma = false;
        for (size_t i=0; i < availableAntibandingModes.count; i++) {
            if (addComma) supportedAntibanding += ",";
            addComma = true;
            switch (availableAntibandingModes.data.u8[i]) {
                case ANDROID_CONTROL_AE_ANTIBANDING_OFF:
                    supportedAntibanding += "off";
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_50HZ:
                    supportedAntibanding += "50hz";
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_60HZ:
                    supportedAntibanding += "60hz";
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_AUTO:
                    supportedAntibanding += "auto";
                    break;
                default:
                    ALOGW("%s: Camera %d: Unknown antibanding value: %d",
                        __FUNCTION__, mCameraId,
                            availableAntibandingModes.data.u8[i]);
                    addComma = false;
                    break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_ANTIBANDING,
                supportedAntibanding);
    }

    mParams->set(CameraParameters::KEY_SCENE_MODE, "auto");
    camera_metadata_entry_t availableSceneModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
            &availableSceneModes);
    if (res != OK) return res;
    {
        String8 supportedSceneModes("auto");
        bool addComma = true;
        bool noSceneModes = false;
        for (size_t i=0; i < availableSceneModes.count; i++) {
            if (addComma) supportedSceneModes += ",";
            addComma = true;
            switch (availableSceneModes.data.u8[i]) {
                case ANDROID_CONTROL_SCENE_MODE_UNSUPPORTED:
                    noSceneModes = true;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY:
                    // Not in old API
                    addComma = false;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_ACTION:
                    supportedSceneModes += "action";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_PORTRAIT:
                    supportedSceneModes += "portrait";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_LANDSCAPE:
                    supportedSceneModes += "landscape";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_NIGHT:
                    supportedSceneModes += "night";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_NIGHT_PORTRAIT:
                    supportedSceneModes += "night-portrait";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_THEATRE:
                    supportedSceneModes += "theatre";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_BEACH:
                    supportedSceneModes += "beach";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SNOW:
                    supportedSceneModes += "snow";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SUNSET:
                    supportedSceneModes += "sunset";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_STEADYPHOTO:
                    supportedSceneModes += "steadyphoto";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_FIREWORKS:
                    supportedSceneModes += "fireworks";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SPORTS:
                    supportedSceneModes += "sports";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_PARTY:
                    supportedSceneModes += "party";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT:
                    supportedSceneModes += "candlelight";
                    break;
                case ANDROID_CONTROL_SCENE_MODE_BARCODE:
                    supportedSceneModes += "barcode";
                    break;
                default:
                    ALOGW("%s: Camera %d: Unknown scene mode value: %d",
                        __FUNCTION__, mCameraId, availableSceneModes.data.u8[i]);
                    addComma = false;
                    break;
            }
        }
        if (!noSceneModes) {
            mParams->set(CameraParameters::KEY_SUPPORTED_SCENE_MODES,
                    supportedSceneModes);
        }
    }

    camera_metadata_entry_t flashAvailable;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_FLASH_AVAILABLE, &flashAvailable);
    if (res != OK) return res;

    camera_metadata_entry_t availableAeModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AE_AVAILABLE_MODES,
            &availableAeModes);
    if (res != OK) return res;

    if (flashAvailable.data.u8[0]) {
        mParams->set(CameraParameters::KEY_FLASH_MODE, "auto");
        String8 supportedFlashModes("off,auto,on,torch");
        for (size_t i=0; i < availableAeModes.count; i++) {
            if (availableAeModes.data.u8[i] ==
                    ANDROID_CONTROL_AE_ON_AUTO_FLASH_REDEYE) {
                supportedFlashModes += ",red-eye";
                break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,
                supportedFlashModes);
    }

    camera_metadata_entry_t minFocusDistance;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_LENS_MINIMUM_FOCUS_DISTANCE,
            &minFocusDistance);
    if (res != OK) return res;
    camera_metadata_entry_t availableAfModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AF_AVAILABLE_MODES,
            &availableAfModes);
    if (res != OK) return res;
    if (minFocusDistance.data.f[0] == 0) {
        // Fixed-focus lens
        mParams->set(CameraParameters::KEY_FOCUS_MODE, "fixed");
        mParams->set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES, "fixed");
    } else {
        mParams->set(CameraParameters::KEY_FOCUS_MODE, "auto");
        String8 supportedFocusModes("fixed,infinity");
        bool addComma = true;
        for (size_t i=0; i < availableAfModes.count; i++) {
            if (addComma) supportedFocusModes += ",";
            addComma = true;
            switch (availableAfModes.data.u8[i]) {
                case ANDROID_CONTROL_AF_AUTO:
                    supportedFocusModes += "auto";
                    break;
                case ANDROID_CONTROL_AF_MACRO:
                    supportedFocusModes += "macro";
                    break;
                case ANDROID_CONTROL_AF_CONTINUOUS_VIDEO:
                    supportedFocusModes += "continuous-video";
                    break;
                case ANDROID_CONTROL_AF_CONTINUOUS_PICTURE:
                    supportedFocusModes += "continuous-picture";
                    break;
                case ANDROID_CONTROL_AF_EDOF:
                    supportedFocusModes += "edof";
                    break;
                // Not supported in v1 API
                case ANDROID_CONTROL_AF_OFF:
                    addComma = false;
                    break;
                default:
                    ALOGW("%s: Camera %d: Unknown AF mode value: %d",
                        __FUNCTION__, mCameraId, availableAfModes.data.u8[i]);
                    addComma = false;
                    break;
            }
        }
        mParams->set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                supportedFocusModes);
    }

    camera_metadata_entry_t max3aRegions;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_MAX_REGIONS, &max3aRegions);
    if (res != OK) return res;

    mParams->set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS,
            max3aRegions.data.i32[0]);
    mParams->set(CameraParameters::KEY_FOCUS_AREAS,
            "(0,0,0,0,0)");

    camera_metadata_entry_t availableFocalLengths;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_LENS_AVAILABLE_FOCAL_LENGTHS,
            &availableFocalLengths);
    if (res != OK) return res;
    float minFocalLength = availableFocalLengths.data.f[0];
    mParams->setFloat(CameraParameters::KEY_FOCAL_LENGTH, minFocalLength);

    camera_metadata_entry_t sensorSize;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_SENSOR_PHYSICAL_SIZE,
            &sensorSize);
    if (res != OK) return res;

    // The fields of view here assume infinity focus, maximum wide angle
    float horizFov = 180 / M_PI *
            2 * atanf(sensorSize.data.f[0] / (2 * minFocalLength));
    float vertFov  = 180 / M_PI *
            2 * atanf(sensorSize.data.f[1] / (2 * minFocalLength));
    mParams->setFloat(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE, horizFov);
    mParams->setFloat(CameraParameters::KEY_VERTICAL_VIEW_ANGLE, vertFov);

    mParams->set(CameraParameters::KEY_EXPOSURE_COMPENSATION, 0);

    camera_metadata_entry_t exposureCompensationRange;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AE_EXP_COMPENSATION_RANGE,
            &exposureCompensationRange);
    if (res != OK) return res;
    mParams->set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION,
            exposureCompensationRange.data.i32[1]);
    mParams->set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION,
            exposureCompensationRange.data.i32[0]);

    camera_metadata_entry_t exposureCompensationStep;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AE_EXP_COMPENSATION_STEP,
            &exposureCompensationStep);
    if (res != OK) return res;
    mParams->setFloat(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP,
            exposureCompensationStep.data.r[0].numerator /
            exposureCompensationStep.data.r[0].denominator);

    mParams->set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK, "false");
    mParams->set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK_SUPPORTED, "true");

    mParams->set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK, "false");
    mParams->set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED, "true");

    mParams->set(CameraParameters::KEY_MAX_NUM_METERING_AREAS,
            max3aRegions.data.i32[0]);
    mParams->set(CameraParameters::KEY_METERING_AREAS,
            "(0,0,0,0,0)");

    mParams->set(CameraParameters::KEY_ZOOM, 0);
    mParams->set(CameraParameters::KEY_MAX_ZOOM, NUM_ZOOM_STEPS - 1);

    camera_metadata_entry_t maxDigitalZoom;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_SCALER_AVAILABLE_MAX_ZOOM, &maxDigitalZoom);
    if (res != OK) return res;

    {
        String8 zoomRatios;
        float zoom = 1.f;
        float zoomIncrement = (maxDigitalZoom.data.f[0] - zoom) /
                (NUM_ZOOM_STEPS-1);
        bool addComma = false;
        for (size_t i=0; i < NUM_ZOOM_STEPS; i++) {
            if (addComma) zoomRatios += ",";
            addComma = true;
            zoomRatios += String8::format("%d", static_cast<int>(zoom * 100));
            zoom += zoomIncrement;
        }
        mParams->set(CameraParameters::KEY_ZOOM_RATIOS, zoomRatios);
    }

    mParams->set(CameraParameters::KEY_ZOOM_SUPPORTED, "true");
    mParams->set(CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED, "true");

    mParams->set(CameraParameters::KEY_FOCUS_DISTANCES,
            "Infinity,Infinity,Infinity");

    camera_metadata_entry_t maxFacesDetected;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_STATS_MAX_FACE_COUNT,
            &maxFacesDetected);
    mParams->set(CameraParameters::KEY_MAX_NUM_DETECTED_FACES_HW,
            maxFacesDetected.data.i32[0]);
    mParams->set(CameraParameters::KEY_MAX_NUM_DETECTED_FACES_SW,
            0);

    mParams->set(CameraParameters::KEY_VIDEO_FRAME_FORMAT,
            "yuv420sp");

    mParams->set(CameraParameters::KEY_RECORDING_HINT,
            "false");

    mParams->set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED,
            "true");

    mParams->set(CameraParameters::KEY_VIDEO_STABILIZATION,
            "false");

    camera_metadata_entry_t availableVideoStabilizationModes;
    res = find_camera_metadata_entry(mDevice->info(),
            ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
            &availableVideoStabilizationModes);
    if (res != OK) return res;
    if (availableVideoStabilizationModes.count > 1) {
        mParams->set(CameraParameters::KEY_VIDEO_STABILIZATION_SUPPORTED,
                "true");
    } else {
        mParams->set(CameraParameters::KEY_VIDEO_STABILIZATION_SUPPORTED,
                "false");
    }

    return OK;
}

status_t Camera2Client::updatePreviewRequest() {
    ALOG1_ENTRY
    status_t res;
    if (mPreviewRequest == NULL) {
        res = mDevice->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
                &mPreviewRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default preview request: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }
    // TODO: Adjust for mParams changes
    ALOG1_EXIT
    return OK;
}

} // namespace android
