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
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <gui/Surface.h>

#include <math.h>

#include "Camera2Client.h"

namespace android {

#define ALOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define ALOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

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
        mPreviewStreamId(NO_PREVIEW_STREAM),
        mPreviewRequest(NULL)
{
    ATRACE_CALL();

    mDevice = new Camera2Device(cameraId);
}

status_t Camera2Client::initialize(camera_module_t *module)
{
    ATRACE_CALL();
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
        ALOGD("%s", mParamsFlattened.string());
    }

    mState = STOPPED;

    return OK;
}

Camera2Client::~Camera2Client() {
    ATRACE_CALL();
    mDestructionStarted = true;

    disconnect();

}

status_t Camera2Client::dump(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("Client2[%d] (%p) PID: %d:\n",
            mCameraId,
            getCameraClient()->asBinder().get(),
            mClientPid);
    result.append("  State: ");
#define CASE_APPEND_ENUM(x) case x: result.append(#x "\n"); break;

    switch (mState) {
        CASE_APPEND_ENUM(NOT_INITIALIZED)
        CASE_APPEND_ENUM(STOPPED)
        CASE_APPEND_ENUM(WAITING_FOR_PREVIEW_WINDOW)
        CASE_APPEND_ENUM(PREVIEW)
        CASE_APPEND_ENUM(RECORD)
        CASE_APPEND_ENUM(STILL_CAPTURE)
        default: result.append("UNKNOWN\n"); break;
    }

    result.append("  Current parameters:\n");
    result.appendFormat("    Preview size: %d x %d\n",
            mParameters.previewWidth, mParameters.previewHeight);
    result.appendFormat("    Preview FPS range: %d - %d\n",
            mParameters.previewFpsRangeMin, mParameters.previewFpsRangeMax);
    result.appendFormat("    Preview HAL pixel format: 0x%x\n",
            mParameters.previewFormat);
    result.appendFormat("    Picture size: %d x %d\n",
            mParameters.pictureWidth, mParameters.pictureHeight);
    result.appendFormat("    Jpeg thumbnail size: %d x %d\n",
            mParameters.jpegThumbWidth, mParameters.jpegThumbHeight);
    result.appendFormat("    Jpeg quality: %d, thumbnail quality: %d\n",
            mParameters.jpegQuality, mParameters.jpegThumbQuality);
    result.appendFormat("    Jpeg rotation: %d\n", mParameters.jpegRotation);
    result.appendFormat("    GPS tags %s\n",
            mParameters.gpsEnabled ? "enabled" : "disabled");
    if (mParameters.gpsEnabled) {
        result.appendFormat("    GPS lat x long x alt: %f x %f x %f\n",
                mParameters.gpsLatitude, mParameters.gpsLongitude,
                mParameters.gpsAltitude);
        result.appendFormat("    GPS timestamp: %lld\n",
                mParameters.gpsTimestamp);
        result.appendFormat("    GPS processing method: %s\n",
                mParameters.gpsProcessingMethod.string());
    }

    result.append("    White balance mode: ");
    switch (mParameters.wbMode) {
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_AUTO)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_INCANDESCENT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_FLUORESCENT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_WARM_FLUORESCENT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_DAYLIGHT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_CLOUDY_DAYLIGHT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_TWILIGHT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AWB_SHADE)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Effect mode: ");
    switch (mParameters.effectMode) {
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_OFF)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_MONO)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_NEGATIVE)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_SOLARIZE)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_SEPIA)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_POSTERIZE)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_WHITEBOARD)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_BLACKBOARD)
        CASE_APPEND_ENUM(ANDROID_CONTROL_EFFECT_AQUA)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Antibanding mode: ");
    switch (mParameters.antibandingMode) {
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_AUTO)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_OFF)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_50HZ)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_60HZ)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Scene mode: ");
    switch (mParameters.sceneMode) {
        case ANDROID_CONTROL_SCENE_MODE_UNSUPPORTED:
            result.append("AUTO\n"); break;
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_ACTION)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_PORTRAIT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_LANDSCAPE)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_NIGHT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_NIGHT_PORTRAIT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_THEATRE)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_BEACH)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_SNOW)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_SUNSET)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_STEADYPHOTO)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_FIREWORKS)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_SPORTS)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_PARTY)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT)
        CASE_APPEND_ENUM(ANDROID_CONTROL_SCENE_MODE_BARCODE)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Flash mode: ");
    switch (mParameters.flashMode) {
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_OFF)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_AUTO)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_ON)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_TORCH)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_RED_EYE)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_INVALID)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Focus mode: ");
    switch (mParameters.focusMode) {
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_AUTO)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_MACRO)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_CONTINUOUS_VIDEO)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_CONTINUOUS_PICTURE)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_EDOF)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_INFINITY)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_FIXED)
        CASE_APPEND_ENUM(Parameters::FOCUS_MODE_INVALID)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Focusing areas:\n");
    for (size_t i = 0; i < mParameters.focusingAreas.size(); i++) {
        result.appendFormat("      [ (%d, %d, %d, %d), weight %d ]\n",
                mParameters.focusingAreas[i].left,
                mParameters.focusingAreas[i].top,
                mParameters.focusingAreas[i].right,
                mParameters.focusingAreas[i].bottom,
                mParameters.focusingAreas[i].weight);
    }

    result.appendFormat("    Exposure compensation index: %d\n",
            mParameters.exposureCompensation);

    result.appendFormat("    AE lock %s, AWB lock %s\n",
            mParameters.autoExposureLock ? "enabled" : "disabled",
            mParameters.autoWhiteBalanceLock ? "enabled" : "disabled" );

    result.appendFormat("    Metering areas:\n");
    for (size_t i = 0; i < mParameters.meteringAreas.size(); i++) {
        result.appendFormat("      [ (%d, %d, %d, %d), weight %d ]\n",
                mParameters.meteringAreas[i].left,
                mParameters.meteringAreas[i].top,
                mParameters.meteringAreas[i].right,
                mParameters.meteringAreas[i].bottom,
                mParameters.meteringAreas[i].weight);
    }

    result.appendFormat("   Zoom index: %d\n", mParameters.zoom);
    result.appendFormat("   Video size: %d x %d\n", mParameters.videoWidth,
            mParameters.videoHeight);

    result.appendFormat("   Recording hint is %s\n",
            mParameters.recordingHint ? "set" : "not set");

    result.appendFormat("   Video stabilization is %s\n",
            mParameters.videoStabilization ? "enabled" : "disabled");

    write(fd, result.string(), result.size());

    // TODO: Dump Camera2Device

#undef CASE_APPEND_ENUM
    return NO_ERROR;
}

// ICamera interface

void Camera2Client::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    if (mDevice == 0) return;

    stopPreviewLocked();

    if (mPreviewStreamId != NO_PREVIEW_STREAM) {
        mDevice->deleteStream(mPreviewStreamId);
        mPreviewStreamId = NO_PREVIEW_STREAM;
    }

    CameraService::Client::disconnect();
}

status_t Camera2Client::connect(const sp<ICameraClient>& client) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    return BAD_VALUE;
}

status_t Camera2Client::lock() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    return BAD_VALUE;
}

status_t Camera2Client::unlock() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    return BAD_VALUE;
}

status_t Camera2Client::setPreviewDisplay(
        const sp<Surface>& surface) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    if (mState >= PREVIEW) return INVALID_OPERATION;

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
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    if (mState >= PREVIEW) return INVALID_OPERATION;

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
    ATRACE_CALL();
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
    res = mDevice->createStream(window,
            mParameters.previewWidth, mParameters.previewHeight,
            CAMERA2_HAL_PIXEL_FORMAT_OPAQUE,
            &mPreviewStreamId);
    if (res != OK) {
        return res;
    }

    if (mState == WAITING_FOR_PREVIEW_WINDOW) {
        return startPreview();
    }

    return OK;
}

void Camera2Client::setPreviewCallbackFlag(int flag) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
}

status_t Camera2Client::startPreview() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

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
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    stopPreviewLocked();
}

void Camera2Client::stopPreviewLocked() {
    ATRACE_CALL();
    if (mState != PREVIEW) return;

    mDevice->setStreamingRequest(NULL);
    mState = STOPPED;
}

bool Camera2Client::previewEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return mState == PREVIEW;
}

status_t Camera2Client::storeMetaDataInBuffers(bool enabled) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return BAD_VALUE;
}

status_t Camera2Client::startRecording() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return BAD_VALUE;
}

void Camera2Client::stopRecording() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
}

bool Camera2Client::recordingEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return BAD_VALUE;
}

void Camera2Client::releaseRecordingFrame(const sp<IMemory>& mem) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
}

status_t Camera2Client::autoFocus() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return OK;
}

status_t Camera2Client::cancelAutoFocus() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return OK;
}

status_t Camera2Client::takePicture(int msgType) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return BAD_VALUE;
}

status_t Camera2Client::setParameters(const String8& params) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return OK;
}

String8 Camera2Client::getParameters() const {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    Mutex::Autolock pl(mParamsLock);

    // TODO: Deal with focus distances
    return mParamsFlattened;
}

status_t Camera2Client::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return OK;
}

/** Device-related methods */

camera_metadata_entry_t Camera2Client::staticInfo(uint32_t tag,
        size_t minCount, size_t maxCount) {
    status_t res;
    camera_metadata_entry_t entry;
    res = find_camera_metadata_entry(mDevice->info(),
            tag,
            &entry);
    if (CC_UNLIKELY( res != OK )) {
        const char* tagSection = get_camera_metadata_section_name(tag);
        if (tagSection == NULL) tagSection = "<unknown>";
        const char* tagName = get_camera_metadata_tag_name(tag);
        if (tagName == NULL) tagName = "<unknown>";

        ALOGE("Error finding static metadata entry '%s.%s' (%x): %s (%d)",
                tagSection, tagName, tag, strerror(-res), res);
        entry.count = 0;
        entry.data.u8 = NULL;
    } else if (CC_UNLIKELY(
            (minCount != 0 && entry.count < minCount) ||
            (maxCount != 0 && entry.count > maxCount) ) ) {
        const char* tagSection = get_camera_metadata_section_name(tag);
        if (tagSection == NULL) tagSection = "<unknown>";
        const char* tagName = get_camera_metadata_tag_name(tag);
        if (tagName == NULL) tagName = "<unknown>";
        ALOGE("Malformed static metadata entry '%s.%s' (%x):"
                "Expected between %d and %d values, but got %d values",
                tagSection, tagName, tag, minCount, maxCount, entry.count);
        entry.count = 0;
        entry.data.u8 = NULL;
    }

    return entry;
}

/** Utility methods */


status_t Camera2Client::buildDefaultParameters() {
    ATRACE_CALL();
    Mutex::Autolock pl(mParamsLock);

    status_t res;
    CameraParameters params;

    camera_metadata_entry_t availableProcessedSizes =
        staticInfo(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, 2);
    if (!availableProcessedSizes.count) return NO_INIT;

    // TODO: Pick more intelligently
    mParameters.previewWidth = availableProcessedSizes.data.i32[0];
    mParameters.previewHeight = availableProcessedSizes.data.i32[1];
    mParameters.videoWidth = mParameters.previewWidth;
    mParameters.videoHeight = mParameters.previewHeight;

    params.setPreviewSize(mParameters.previewWidth, mParameters.previewHeight);
    params.setVideoSize(mParameters.videoWidth, mParameters.videoHeight);
    params.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO,
            String8::format("%dx%d",
                    mParameters.previewWidth, mParameters.previewHeight));
    {
        String8 supportedPreviewSizes;
        for (size_t i=0; i < availableProcessedSizes.count; i += 2) {
            if (i != 0) supportedPreviewSizes += ",";
            supportedPreviewSizes += String8::format("%dx%d",
                    availableProcessedSizes.data.i32[i],
                    availableProcessedSizes.data.i32[i+1]);
        }
        params.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                supportedPreviewSizes);
        params.set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES,
                supportedPreviewSizes);
    }

    camera_metadata_entry_t availableFpsRanges =
        staticInfo(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, 2);
    if (!availableFpsRanges.count) return NO_INIT;

    mParameters.previewFpsRangeMin = availableFpsRanges.data.i32[0];
    mParameters.previewFpsRangeMax = availableFpsRanges.data.i32[1];

    params.set(CameraParameters::KEY_PREVIEW_FPS_RANGE,
            String8::format("%d,%d",
                    mParameters.previewFpsRangeMin,
                    mParameters.previewFpsRangeMax));

    {
        String8 supportedPreviewFpsRange;
        for (size_t i=0; i < availableFpsRanges.count; i += 2) {
            if (i != 0) supportedPreviewFpsRange += ",";
            supportedPreviewFpsRange += String8::format("(%d,%d)",
                    availableFpsRanges.data.i32[i],
                    availableFpsRanges.data.i32[i+1]);
        }
        params.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
                supportedPreviewFpsRange);
    }

    mParameters.previewFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
    params.set(CameraParameters::KEY_PREVIEW_FORMAT,
            formatEnumToString(mParameters.previewFormat)); // NV21

    camera_metadata_entry_t availableFormats =
        staticInfo(ANDROID_SCALER_AVAILABLE_FORMATS);

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
        params.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                supportedPreviewFormats);
    }

    // PREVIEW_FRAME_RATE / SUPPORTED_PREVIEW_FRAME_RATES are deprecated, but
    // still have to do something sane for them

    params.set(CameraParameters::KEY_PREVIEW_FRAME_RATE,
            mParameters.previewFpsRangeMin);

    {
        String8 supportedPreviewFrameRates;
        for (size_t i=0; i < availableFpsRanges.count; i += 2) {
            if (i != 0) supportedPreviewFrameRates += ",";
            supportedPreviewFrameRates += String8::format("%d",
                    availableFpsRanges.data.i32[i]);
        }
        params.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
                supportedPreviewFrameRates);
    }

    camera_metadata_entry_t availableJpegSizes =
        staticInfo(ANDROID_SCALER_AVAILABLE_JPEG_SIZES, 2);
    if (!availableJpegSizes.count) return NO_INIT;

    // TODO: Pick maximum
    mParameters.pictureWidth = availableJpegSizes.data.i32[0];
    mParameters.pictureHeight = availableJpegSizes.data.i32[1];

    params.setPictureSize(mParameters.pictureWidth,
            mParameters.pictureHeight);

    {
        String8 supportedPictureSizes;
        for (size_t i=0; i < availableJpegSizes.count; i += 2) {
            if (i != 0) supportedPictureSizes += ",";
            supportedPictureSizes += String8::format("%dx%d",
                    availableJpegSizes.data.i32[i],
                    availableJpegSizes.data.i32[i+1]);
        }
        params.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES,
                supportedPictureSizes);
    }

    params.setPictureFormat(CameraParameters::PIXEL_FORMAT_JPEG);
    params.set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS,
            CameraParameters::PIXEL_FORMAT_JPEG);

    camera_metadata_entry_t availableJpegThumbnailSizes =
        staticInfo(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, 2);
    if (!availableJpegThumbnailSizes.count) return NO_INIT;

    // TODO: Pick default thumbnail size sensibly
    mParameters.jpegThumbWidth = availableJpegThumbnailSizes.data.i32[0];
    mParameters.jpegThumbHeight = availableJpegThumbnailSizes.data.i32[1];

    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,
            mParameters.jpegThumbWidth);
    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,
            mParameters.jpegThumbHeight);

    {
        String8 supportedJpegThumbSizes;
        for (size_t i=0; i < availableJpegThumbnailSizes.count; i += 2) {
            if (i != 0) supportedJpegThumbSizes += ",";
            supportedJpegThumbSizes += String8::format("%dx%d",
                    availableJpegThumbnailSizes.data.i32[i],
                    availableJpegThumbnailSizes.data.i32[i+1]);
        }
        params.set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES,
                supportedJpegThumbSizes);
    }

    mParameters.jpegThumbQuality = 90;
    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY,
            mParameters.jpegThumbQuality);
    mParameters.jpegQuality = 90;
    params.set(CameraParameters::KEY_JPEG_QUALITY,
            mParameters.jpegQuality);
    mParameters.jpegRotation = 0;
    params.set(CameraParameters::KEY_ROTATION,
            mParameters.jpegRotation);

    mParameters.gpsEnabled = false;
    mParameters.gpsProcessingMethod = "unknown";
    // GPS fields in CameraParameters are not set by implementation

    mParameters.wbMode = ANDROID_CONTROL_AWB_AUTO;
    params.set(CameraParameters::KEY_WHITE_BALANCE,
            CameraParameters::WHITE_BALANCE_AUTO);

    camera_metadata_entry_t availableWhiteBalanceModes =
        staticInfo(ANDROID_CONTROL_AWB_AVAILABLE_MODES);
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
        params.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE,
                supportedWhiteBalance);
    }

    mParameters.effectMode = ANDROID_CONTROL_EFFECT_OFF;
    params.set(CameraParameters::KEY_EFFECT,
            CameraParameters::EFFECT_NONE);

    camera_metadata_entry_t availableEffects =
        staticInfo(ANDROID_CONTROL_AVAILABLE_EFFECTS);
    if (!availableEffects.count) return NO_INIT;
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
        params.set(CameraParameters::KEY_SUPPORTED_EFFECTS, supportedEffects);
    }

    mParameters.antibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_AUTO;
    params.set(CameraParameters::KEY_ANTIBANDING,
            CameraParameters::ANTIBANDING_AUTO);

    camera_metadata_entry_t availableAntibandingModes =
        staticInfo(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES);
    if (!availableAntibandingModes.count) return NO_INIT;
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
        params.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING,
                supportedAntibanding);
    }

    mParameters.sceneMode = ANDROID_CONTROL_OFF;
    params.set(CameraParameters::KEY_SCENE_MODE,
            CameraParameters::SCENE_MODE_AUTO);

    camera_metadata_entry_t availableSceneModes =
        staticInfo(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
    if (!availableSceneModes.count) return NO_INIT;
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
            params.set(CameraParameters::KEY_SUPPORTED_SCENE_MODES,
                    supportedSceneModes);
        }
    }

    camera_metadata_entry_t flashAvailable =
        staticInfo(ANDROID_FLASH_AVAILABLE, 1, 1);
    if (!flashAvailable.count) return NO_INIT;

    camera_metadata_entry_t availableAeModes =
        staticInfo(ANDROID_CONTROL_AE_AVAILABLE_MODES);
    if (!availableAeModes.count) return NO_INIT;

    if (flashAvailable.data.u8[0]) {
        mParameters.flashMode = Parameters::FLASH_MODE_AUTO;
        params.set(CameraParameters::KEY_FLASH_MODE,
                CameraParameters::FLASH_MODE_AUTO);

        String8 supportedFlashModes(CameraParameters::FLASH_MODE_OFF);
        supportedFlashModes = supportedFlashModes +
            "," + CameraParameters::FLASH_MODE_AUTO +
            "," + CameraParameters::FLASH_MODE_ON +
            "," + CameraParameters::FLASH_MODE_TORCH;
        for (size_t i=0; i < availableAeModes.count; i++) {
            if (availableAeModes.data.u8[i] ==
                    ANDROID_CONTROL_AE_ON_AUTO_FLASH_REDEYE) {
                supportedFlashModes = supportedFlashModes + "," +
                    CameraParameters::FLASH_MODE_RED_EYE;
                break;
            }
        }
        params.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,
                supportedFlashModes);
    } else {
        mParameters.flashMode = Parameters::FLASH_MODE_OFF;
        params.set(CameraParameters::KEY_FLASH_MODE,
                CameraParameters::FLASH_MODE_OFF);
        params.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,
                CameraParameters::FLASH_MODE_OFF);
    }

    camera_metadata_entry_t minFocusDistance =
        staticInfo(ANDROID_LENS_MINIMUM_FOCUS_DISTANCE, 1, 1);
    if (!minFocusDistance.count) return NO_INIT;

    camera_metadata_entry_t availableAfModes =
        staticInfo(ANDROID_CONTROL_AF_AVAILABLE_MODES);
    if (!availableAfModes.count) return NO_INIT;

    if (minFocusDistance.data.f[0] == 0) {
        // Fixed-focus lens
        mParameters.focusMode = Parameters::FOCUS_MODE_FIXED;
        params.set(CameraParameters::KEY_FOCUS_MODE,
                CameraParameters::FOCUS_MODE_FIXED);
        params.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                CameraParameters::FOCUS_MODE_FIXED);
    } else {
        mParameters.focusMode = Parameters::FOCUS_MODE_AUTO;
        params.set(CameraParameters::KEY_FOCUS_MODE,
                CameraParameters::FOCUS_MODE_AUTO);
        String8 supportedFocusModes(CameraParameters::FOCUS_MODE_FIXED);
        supportedFocusModes = supportedFocusModes + "," +
            CameraParameters::FOCUS_MODE_INFINITY;
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
        params.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                supportedFocusModes);
    }

    camera_metadata_entry_t max3aRegions =
        staticInfo(ANDROID_CONTROL_MAX_REGIONS, 1, 1);
    if (!max3aRegions.count) return NO_INIT;

    params.set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS,
            max3aRegions.data.i32[0]);
    params.set(CameraParameters::KEY_FOCUS_AREAS,
            "(0,0,0,0,0)");
    mParameters.focusingAreas.clear();
    mParameters.focusingAreas.add(Parameters::Area(0,0,0,0,0));

    camera_metadata_entry_t availableFocalLengths =
        staticInfo(ANDROID_LENS_AVAILABLE_FOCAL_LENGTHS);
    if (!availableFocalLengths.count) return NO_INIT;

    float minFocalLength = availableFocalLengths.data.f[0];
    params.setFloat(CameraParameters::KEY_FOCAL_LENGTH, minFocalLength);

    camera_metadata_entry_t sensorSize =
        staticInfo(ANDROID_SENSOR_PHYSICAL_SIZE, 2, 2);
    if (!sensorSize.count) return NO_INIT;

    // The fields of view here assume infinity focus, maximum wide angle
    float horizFov = 180 / M_PI *
            2 * atanf(sensorSize.data.f[0] / (2 * minFocalLength));
    float vertFov  = 180 / M_PI *
            2 * atanf(sensorSize.data.f[1] / (2 * minFocalLength));
    params.setFloat(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE, horizFov);
    params.setFloat(CameraParameters::KEY_VERTICAL_VIEW_ANGLE, vertFov);

    mParameters.exposureCompensation = 0;
    params.set(CameraParameters::KEY_EXPOSURE_COMPENSATION,
                mParameters.exposureCompensation);

    camera_metadata_entry_t exposureCompensationRange =
        staticInfo(ANDROID_CONTROL_AE_EXP_COMPENSATION_RANGE, 2, 2);
    if (!exposureCompensationRange.count) return NO_INIT;

    params.set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION,
            exposureCompensationRange.data.i32[1]);
    params.set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION,
            exposureCompensationRange.data.i32[0]);

    camera_metadata_entry_t exposureCompensationStep =
        staticInfo(ANDROID_CONTROL_AE_EXP_COMPENSATION_STEP, 1, 1);
    if (!exposureCompensationStep.count) return NO_INIT;

    params.setFloat(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP,
            exposureCompensationStep.data.r[0].numerator /
            exposureCompensationStep.data.r[0].denominator);

    mParameters.autoExposureLock = false;
    params.set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK,
            CameraParameters::FALSE);
    params.set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK_SUPPORTED,
            CameraParameters::TRUE);

    mParameters.autoWhiteBalanceLock = false;
    params.set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK,
            CameraParameters::FALSE);
    params.set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED,
            CameraParameters::TRUE);

    mParameters.meteringAreas.add(Parameters::Area(0, 0, 0, 0, 0));
    params.set(CameraParameters::KEY_MAX_NUM_METERING_AREAS,
            max3aRegions.data.i32[0]);
    params.set(CameraParameters::KEY_METERING_AREAS,
            "(0,0,0,0,0)");

    mParameters.zoom = 0;
    params.set(CameraParameters::KEY_ZOOM, mParameters.zoom);
    params.set(CameraParameters::KEY_MAX_ZOOM, NUM_ZOOM_STEPS - 1);

    camera_metadata_entry_t maxDigitalZoom =
        staticInfo(ANDROID_SCALER_AVAILABLE_MAX_ZOOM, 1, 1);
    if (!maxDigitalZoom.count) return NO_INIT;

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
        params.set(CameraParameters::KEY_ZOOM_RATIOS, zoomRatios);
    }

    params.set(CameraParameters::KEY_ZOOM_SUPPORTED,
            CameraParameters::TRUE);
    params.set(CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED,
            CameraParameters::TRUE);

    params.set(CameraParameters::KEY_FOCUS_DISTANCES,
            "Infinity,Infinity,Infinity");

    camera_metadata_entry_t maxFacesDetected =
        staticInfo(ANDROID_STATS_MAX_FACE_COUNT, 1, 1);
    params.set(CameraParameters::KEY_MAX_NUM_DETECTED_FACES_HW,
            maxFacesDetected.data.i32[0]);
    params.set(CameraParameters::KEY_MAX_NUM_DETECTED_FACES_SW,
            0);

    params.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT,
            formatEnumToString(HAL_PIXEL_FORMAT_YCrCb_420_SP));

    params.set(CameraParameters::KEY_RECORDING_HINT,
            CameraParameters::FALSE);

    params.set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED,
            CameraParameters::TRUE);

    params.set(CameraParameters::KEY_VIDEO_STABILIZATION,
            CameraParameters::FALSE);

    camera_metadata_entry_t availableVideoStabilizationModes =
        staticInfo(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
    if (!availableVideoStabilizationModes.count) return NO_INIT;

    if (availableVideoStabilizationModes.count > 1) {
        params.set(CameraParameters::KEY_VIDEO_STABILIZATION_SUPPORTED,
                CameraParameters::TRUE);
    } else {
        params.set(CameraParameters::KEY_VIDEO_STABILIZATION_SUPPORTED,
                CameraParameters::FALSE);
    }

    mParamsFlattened = params.flatten();

    return OK;
}

status_t Camera2Client::updatePreviewRequest() {
    ATRACE_CALL();
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
    // TODO: Adjust for params changes
    return OK;
}

const char* Camera2Client::formatEnumToString(int format) {
    const char *fmt;
    switch(format) {
        case HAL_PIXEL_FORMAT_YCbCr_422_SP: // NV16
            fmt = CameraParameters::PIXEL_FORMAT_YUV422SP;
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP: // NV21
            fmt = CameraParameters::PIXEL_FORMAT_YUV420SP;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_I: // YUY2
            fmt = CameraParameters::PIXEL_FORMAT_YUV422I;
            break;
        case HAL_PIXEL_FORMAT_YV12:        // YV12
            fmt = CameraParameters::PIXEL_FORMAT_YUV420P;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:     // RGB565
            fmt = CameraParameters::PIXEL_FORMAT_RGB565;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:   // RGBA8888
            fmt = CameraParameters::PIXEL_FORMAT_RGBA8888;
            break;
        case HAL_PIXEL_FORMAT_RAW_SENSOR:
            ALOGW("Raw sensor preview format requested.");
            fmt = CameraParameters::PIXEL_FORMAT_BAYER_RGGB;
            break;
        default:
            ALOGE("%s: Unknown preview format: %x",
                    __FUNCTION__,  format);
            fmt = NULL;
            break;
    }
    return fmt;
}
} // namespace android
