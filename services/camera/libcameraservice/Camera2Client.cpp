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
#include <media/hardware/MetadataBufferType.h>

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
        mPreviewStreamId(NO_STREAM),
        mPreviewRequest(NULL),
        mCaptureStreamId(NO_STREAM),
        mCaptureRequest(NULL),
        mRecordingStreamId(NO_STREAM),
        mRecordingRequest(NULL)
{
    ATRACE_CALL();

    mDevice = new Camera2Device(cameraId);
}

status_t Camera2Client::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
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
        LockedParameters::Key k(mParameters);
        ALOGD("%s: Default parameters converted from camera %d:", __FUNCTION__,
              mCameraId);
        ALOGD("%s", k.mParameters.paramsFlattened.string());
    }

    mState = STOPPED;

    return OK;
}

Camera2Client::~Camera2Client() {
    ATRACE_CALL();
    ALOGV("%s: Camera %d: Shutting down", __FUNCTION__, mCameraId);

    mDestructionStarted = true;

    disconnect();

}

status_t Camera2Client::dump(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("Client2[%d] (%p) PID: %d, dump:\n",
            mCameraId,
            getCameraClient()->asBinder().get(),
            mClientPid);
    result.append("  State: ");
#define CASE_APPEND_ENUM(x) case x: result.append(#x "\n"); break;

    const Parameters& p = mParameters.unsafeUnlock();

    result.append(getStateName(mState));

    result.append("\n  Current parameters:\n");
    result.appendFormat("    Preview size: %d x %d\n",
            p.previewWidth, p.previewHeight);
    result.appendFormat("    Preview FPS range: %d - %d\n",
            p.previewFpsRange[0], p.previewFpsRange[1]);
    result.appendFormat("    Preview HAL pixel format: 0x%x\n",
            p.previewFormat);
    result.appendFormat("    Preview transform: %x\n",
            p.previewTransform);
    result.appendFormat("    Picture size: %d x %d\n",
            p.pictureWidth, p.pictureHeight);
    result.appendFormat("    Jpeg thumbnail size: %d x %d\n",
            p.jpegThumbSize[0], p.jpegThumbSize[1]);
    result.appendFormat("    Jpeg quality: %d, thumbnail quality: %d\n",
            p.jpegQuality, p.jpegThumbQuality);
    result.appendFormat("    Jpeg rotation: %d\n", p.jpegRotation);
    result.appendFormat("    GPS tags %s\n",
            p.gpsEnabled ? "enabled" : "disabled");
    if (p.gpsEnabled) {
        result.appendFormat("    GPS lat x long x alt: %f x %f x %f\n",
                p.gpsCoordinates[0], p.gpsCoordinates[1],
                p.gpsCoordinates[2]);
        result.appendFormat("    GPS timestamp: %lld\n",
                p.gpsTimestamp);
        result.appendFormat("    GPS processing method: %s\n",
                p.gpsProcessingMethod.string());
    }

    result.append("    White balance mode: ");
    switch (p.wbMode) {
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
    switch (p.effectMode) {
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
    switch (p.antibandingMode) {
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_AUTO)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_OFF)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_50HZ)
        CASE_APPEND_ENUM(ANDROID_CONTROL_AE_ANTIBANDING_60HZ)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Scene mode: ");
    switch (p.sceneMode) {
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
    switch (p.flashMode) {
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_OFF)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_AUTO)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_ON)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_TORCH)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_RED_EYE)
        CASE_APPEND_ENUM(Parameters::FLASH_MODE_INVALID)
        default: result.append("UNKNOWN\n");
    }

    result.append("    Focus mode: ");
    switch (p.focusMode) {
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
    for (size_t i = 0; i < p.focusingAreas.size(); i++) {
        result.appendFormat("      [ (%d, %d, %d, %d), weight %d ]\n",
                p.focusingAreas[i].left,
                p.focusingAreas[i].top,
                p.focusingAreas[i].right,
                p.focusingAreas[i].bottom,
                p.focusingAreas[i].weight);
    }

    result.appendFormat("    Exposure compensation index: %d\n",
            p.exposureCompensation);

    result.appendFormat("    AE lock %s, AWB lock %s\n",
            p.autoExposureLock ? "enabled" : "disabled",
            p.autoWhiteBalanceLock ? "enabled" : "disabled" );

    result.appendFormat("    Metering areas:\n");
    for (size_t i = 0; i < p.meteringAreas.size(); i++) {
        result.appendFormat("      [ (%d, %d, %d, %d), weight %d ]\n",
                p.meteringAreas[i].left,
                p.meteringAreas[i].top,
                p.meteringAreas[i].right,
                p.meteringAreas[i].bottom,
                p.meteringAreas[i].weight);
    }

    result.appendFormat("    Zoom index: %d\n", p.zoom);
    result.appendFormat("    Video size: %d x %d\n", p.videoWidth,
            p.videoHeight);

    result.appendFormat("    Recording hint is %s\n",
            p.recordingHint ? "set" : "not set");

    result.appendFormat("    Video stabilization is %s\n",
            p.videoStabilization ? "enabled" : "disabled");

    result.append("  Current streams:\n");
    result.appendFormat("    Preview stream ID: %d\n", mPreviewStreamId);
    result.appendFormat("    Capture stream ID: %d\n", mCaptureStreamId);
    result.appendFormat("    Recording stream ID: %d\n", mRecordingStreamId);

    result.append("  Current requests:\n");
    if (mPreviewRequest != NULL) {
        result.append("    Preview request:\n");
        write(fd, result.string(), result.size());
        dump_camera_metadata(mPreviewRequest, fd, 2);
    } else {
        result.append("    Preview request: undefined\n");
        write(fd, result.string(), result.size());
    }

    if (mCaptureRequest != NULL) {
        result = "    Capture request:\n";
        write(fd, result.string(), result.size());
        dump_camera_metadata(mCaptureRequest, fd, 2);
    } else {
        result = "    Capture request: undefined\n";
        write(fd, result.string(), result.size());
    }

    result = "  Device dump:\n";
    write(fd, result.string(), result.size());

    status_t res = mDevice->dump(fd, args);
    if (res != OK) {
        result = String8::format("   Error dumping device: %s (%d)",
                strerror(-res), res);
        write(fd, result.string(), result.size());
    }

#undef CASE_APPEND_ENUM
    return NO_ERROR;
}

const char* Camera2Client::getStateName(State state) {
#define CASE_ENUM_TO_CHAR(x) case x: return(#x); break;
    switch(state) {
        CASE_ENUM_TO_CHAR(NOT_INITIALIZED)
        CASE_ENUM_TO_CHAR(STOPPED)
        CASE_ENUM_TO_CHAR(WAITING_FOR_PREVIEW_WINDOW)
        CASE_ENUM_TO_CHAR(PREVIEW)
        CASE_ENUM_TO_CHAR(RECORD)
        CASE_ENUM_TO_CHAR(STILL_CAPTURE)
        CASE_ENUM_TO_CHAR(VIDEO_SNAPSHOT)
        default:
            return "Unknown state!";
            break;
    }
#undef CASE_ENUM_TO_CHAR
}

// ICamera interface

void Camera2Client::disconnect() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);

    if (mDevice == 0) return;

    stopPreviewLocked();

    mDevice->waitUntilDrained();

    if (mPreviewStreamId != NO_STREAM) {
        mDevice->deleteStream(mPreviewStreamId);
        mPreviewStreamId = NO_STREAM;
    }

    if (mCaptureStreamId != NO_STREAM) {
        mDevice->deleteStream(mCaptureStreamId);
        mCaptureStreamId = NO_STREAM;
    }

    if (mRecordingStreamId != NO_STREAM) {
        mDevice->deleteStream(mRecordingStreamId);
        mRecordingStreamId = NO_STREAM;
    }

    CameraService::Client::disconnect();
}

status_t Camera2Client::connect(const sp<ICameraClient>& client) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);

    if (mClientPid != 0 && getCallingPid() != mClientPid) {
        ALOGE("%s: Camera %d: Connection attempt from pid %d; "
                "current locked to pid %d", __FUNCTION__,
                mCameraId, getCallingPid(), mClientPid);
        return BAD_VALUE;
    }

    mClientPid = getCallingPid();
    mCameraClient = client;

    return OK;
}

status_t Camera2Client::lock() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    ALOGV("%s: Camera %d: Lock call from pid %d; current client pid %d",
            __FUNCTION__, mCameraId, getCallingPid(), mClientPid);

    if (mClientPid == 0) {
        mClientPid = getCallingPid();
        return OK;
    }

    if (mClientPid != getCallingPid()) {
        ALOGE("%s: Camera %d: Lock call from pid %d; currently locked to pid %d",
                __FUNCTION__, mCameraId, getCallingPid(), mClientPid);
        return EBUSY;
    }

    return OK;
}

status_t Camera2Client::unlock() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    ALOGV("%s: Camera %d: Unlock call from pid %d; current client pid %d",
            __FUNCTION__, mCameraId, getCallingPid(), mClientPid);

    // TODO: Check for uninterruptable conditions

    if (mClientPid == getCallingPid()) {
        mClientPid = 0;
        mCameraClient.clear();
        return OK;
    }

    ALOGE("%s: Camera %d: Unlock call from pid %d; currently locked to pid %d",
            __FUNCTION__, mCameraId, getCallingPid(), mClientPid);
    return EBUSY;
}

status_t Camera2Client::setPreviewDisplay(
        const sp<Surface>& surface) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surface != 0) {
        binder = surface->asBinder();
        window = surface;
    }

    return setPreviewWindowLocked(binder,window);
}

status_t Camera2Client::setPreviewTexture(
        const sp<ISurfaceTexture>& surfaceTexture) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surfaceTexture != 0) {
        binder = surfaceTexture->asBinder();
        window = new SurfaceTextureClient(surfaceTexture);
    }
    return setPreviewWindowLocked(binder, window);
}

status_t Camera2Client::setPreviewWindowLocked(const sp<IBinder>& binder,
        sp<ANativeWindow> window) {
    ATRACE_CALL();
    status_t res;

    if (binder == mPreviewSurface) {
        ALOGV("%s: Camera %d: New window is same as old window",
                __FUNCTION__, mCameraId);
        return NO_ERROR;
    }

    switch (mState) {
        case NOT_INITIALIZED:
        case RECORD:
        case STILL_CAPTURE:
        case VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Cannot set preview display while in state %s",
                    __FUNCTION__, mCameraId, getStateName(mState));
            return INVALID_OPERATION;
        case STOPPED:
        case WAITING_FOR_PREVIEW_WINDOW:
            // OK
            break;
        case PREVIEW:
            // Already running preview - need to stop and create a new stream
            // TODO: Optimize this so that we don't wait for old stream to drain
            // before spinning up new stream
            mDevice->setStreamingRequest(NULL);
            mState = WAITING_FOR_PREVIEW_WINDOW;
            break;
    }

    if (mPreviewStreamId != NO_STREAM) {
        res = mDevice->waitUntilDrained();
        if (res != OK) {
            ALOGE("%s: Error waiting for preview to drain: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        res = mDevice->deleteStream(mPreviewStreamId);
        if (res != OK) {
            ALOGE("%s: Unable to delete old preview stream: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        mPreviewStreamId = NO_STREAM;
    }

    mPreviewSurface = binder;
    mPreviewWindow = window;

    if (mState == WAITING_FOR_PREVIEW_WINDOW) {
        return startPreviewLocked();
    }

    return OK;
}

void Camera2Client::setPreviewCallbackFlag(int flag) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
}

status_t Camera2Client::startPreview() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    return startPreviewLocked();
}

status_t Camera2Client::startPreviewLocked() {
    ATRACE_CALL();
    status_t res;
    if (mState >= PREVIEW) {
        ALOGE("%s: Can't start preview in state %s",
                __FUNCTION__, getStateName(mState));
        return INVALID_OPERATION;
    }

    if (mPreviewWindow == 0) {
        mState = WAITING_FOR_PREVIEW_WINDOW;
        return OK;
    }
    mState = STOPPED;

    LockedParameters::Key k(mParameters);

    res = updatePreviewStream(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update preview stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    if (mPreviewRequest == NULL) {
        res = updatePreviewRequest(k.mParameters);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create preview request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = updateEntry(mPreviewRequest,
            ANDROID_REQUEST_OUTPUT_STREAMS,
            &mPreviewStreamId, 1);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = sort_camera_metadata(mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Error sorting preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    res = mDevice->setStreamingRequest(mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview request to start preview: "
                "%s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    mState = PREVIEW;

    return OK;
}

void Camera2Client::stopPreview() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    stopPreviewLocked();
}

void Camera2Client::stopPreviewLocked() {
    ATRACE_CALL();
    switch (mState) {
        case NOT_INITIALIZED:
            ALOGE("%s: Camera %d: Call before initialized",
                    __FUNCTION__, mCameraId);
            break;
        case STOPPED:
            break;
        case STILL_CAPTURE:
            ALOGE("%s: Camera %d: Cannot stop preview during still capture.",
                    __FUNCTION__, mCameraId);
            break;
        case RECORD:
            // TODO: Handle record stop here
        case PREVIEW:
            mDevice->setStreamingRequest(NULL);
            mDevice->waitUntilDrained();
        case WAITING_FOR_PREVIEW_WINDOW:
            mState = STOPPED;
            break;
        default:
            ALOGE("%s: Camera %d: Unknown state %d", __FUNCTION__, mCameraId,
                    mState);
    }
}

bool Camera2Client::previewEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return mState == PREVIEW;
}

status_t Camera2Client::storeMetaDataInBuffers(bool enabled) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    switch (mState) {
        case RECORD:
        case VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Can't be called in state %s",
                    __FUNCTION__, mCameraId, getStateName(mState));
            return INVALID_OPERATION;
        default:
            // OK
            break;
    }
    LockedParameters::Key k(mParameters);

    k.mParameters.storeMetadataInBuffers = enabled;

    return OK;
}

status_t Camera2Client::startRecording() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    switch (mState) {
        case STOPPED:
            res = startPreviewLocked();
            if (res != OK) return res;
            break;
        case PREVIEW:
            // Ready to go
            break;
        case RECORD:
        case VIDEO_SNAPSHOT:
            // OK to call this when recording is already on
            return OK;
            break;
        default:
            ALOGE("%s: Camera %d: Can't start recording in state %s",
                    __FUNCTION__, mCameraId, getStateName(mState));
            return INVALID_OPERATION;
    };

    LockedParameters::Key k(mParameters);

    if (!k.mParameters.storeMetadataInBuffers) {
        ALOGE("%s: Camera %d: Recording only supported in metadata mode, but "
                "non-metadata recording mode requested!", __FUNCTION__,
                mCameraId);
        return INVALID_OPERATION;
    }

    res = updateRecordingStream(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update recording stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    if (mRecordingRequest == NULL) {
        res = updateRecordingRequest(k.mParameters);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create recording request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    uint8_t outputStreams[2] = { mPreviewStreamId, mRecordingStreamId };
    res = updateEntry(mRecordingRequest,
            ANDROID_REQUEST_OUTPUT_STREAMS,
            outputStreams, 2);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up recording request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = sort_camera_metadata(mRecordingRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Error sorting recording request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    res = mDevice->setStreamingRequest(mRecordingRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set recording request to start "
                "recording: %s (%d)", __FUNCTION__, mCameraId,
                strerror(-res), res);
        return res;
    }
    mState = RECORD;

    return OK;
}

void Camera2Client::stopRecording() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    switch (mState) {
        case RECORD:
            // OK to stop
            break;
        case STOPPED:
        case PREVIEW:
        case STILL_CAPTURE:
        case VIDEO_SNAPSHOT:
        default:
            ALOGE("%s: Camera %d: Can't stop recording in state %s",
                    __FUNCTION__, mCameraId, getStateName(mState));
            return;
    };

    // Back to preview. Since record can only be reached through preview,
    // all preview stream setup should be up to date.
    res = mDevice->setStreamingRequest(mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to switch back to preview request: "
                "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
        return;
    }

    // TODO: Should recording heap be freed? Can't do it yet since requests
    // could still be in flight.

    mState = PREVIEW;
}

bool Camera2Client::recordingEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    return (mState == RECORD || mState == VIDEO_SNAPSHOT);
}

void Camera2Client::releaseRecordingFrame(const sp<IMemory>& mem) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    // Make sure this is for the current heap
    ssize_t offset;
    size_t size;
    status_t res;
    sp<IMemoryHeap> heap = mem->getMemory(&offset, &size);
    if (heap->getHeapID() != mRecordingHeap->mHeap->getHeapID()) {
        ALOGW("%s: Camera %d: Mismatched heap ID, ignoring release "
                "(got %x, expected %x)", __FUNCTION__, mCameraId,
                heap->getHeapID(), mRecordingHeap->mHeap->getHeapID());
        return;
    }
    uint8_t *data = (uint8_t*)heap->getBase() + offset;
    uint32_t type = *(uint32_t*)data;
    if (type != kMetadataBufferTypeGrallocSource) {
        ALOGE("%s: Camera %d: Recording frame type invalid (got %x, expected %x)",
                __FUNCTION__, mCameraId, type, kMetadataBufferTypeGrallocSource);
        return;
    }
    buffer_handle_t imgBuffer = *(buffer_handle_t*)(data + 4);
    ALOGV("%s: Camera %d: Freeing buffer_handle_t %p", __FUNCTION__, mCameraId,
            imgBuffer);
    res = mRecordingConsumer->freeBuffer(imgBuffer);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to free recording frame (buffer_handle_t: %p):"
                "%s (%d)",
                __FUNCTION__, mCameraId, imgBuffer, strerror(-res), res);
        return;
    }

    mRecordingHeapFree++;
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
    status_t res;

    switch (mState) {
        case NOT_INITIALIZED:
        case STOPPED:
        case WAITING_FOR_PREVIEW_WINDOW:
            ALOGE("%s: Camera %d: Cannot take picture without preview enabled",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
        case PREVIEW:
        case RECORD:
            // Good to go for takePicture
            break;
        case STILL_CAPTURE:
        case VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Already taking a picture",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
    }

    LockedParameters::Key k(mParameters);

    res = updateCaptureStream(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Can't set up still image stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    if (mCaptureRequest == NULL) {
        res = updateCaptureRequest(k.mParameters);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create still image capture request: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    camera_metadata_entry_t outputStreams;
    if (mState == PREVIEW) {
        uint8_t streamIds[2] = { mPreviewStreamId, mCaptureStreamId };
        res = updateEntry(mCaptureRequest, ANDROID_REQUEST_OUTPUT_STREAMS,
                &streamIds, 2);
    } else if (mState == RECORD) {
        uint8_t streamIds[3] = { mPreviewStreamId, mRecordingStreamId,
                                 mCaptureStreamId };
        res = updateEntry(mCaptureRequest, ANDROID_REQUEST_OUTPUT_STREAMS,
                &streamIds, 3);
    }

    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up still image capture request: "
                "%s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = sort_camera_metadata(mCaptureRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to sort capture request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    camera_metadata_t *captureCopy = clone_camera_metadata(mCaptureRequest);
    if (captureCopy == NULL) {
        ALOGE("%s: Camera %d: Unable to copy capture request for HAL device",
                __FUNCTION__, mCameraId);
        return NO_MEMORY;
    }

    if (mState == PREVIEW) {
        res = mDevice->setStreamingRequest(NULL);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to stop preview for still capture: "
                    "%s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }
    // TODO: Capture should be atomic with setStreamingRequest here
    res = mDevice->capture(captureCopy);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to submit still image capture request: "
                "%s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    switch (mState) {
        case PREVIEW:
            mState = STILL_CAPTURE;
            break;
        case RECORD:
            mState = VIDEO_SNAPSHOT;
            break;
        default:
            ALOGE("%s: Camera %d: Unknown state for still capture!",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
    }

    return OK;
}

status_t Camera2Client::setParameters(const String8& params) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    LockedParameters::Key k(mParameters);
    status_t res;

    CameraParameters newParams(params);

    // TODO: Currently ignoring any changes to supposedly read-only
    // parameters such as supported preview sizes, etc. Should probably
    // produce an error if they're changed.

    /** Extract and verify new parameters */

    size_t i;

    // PREVIEW_SIZE
    int previewWidth, previewHeight;
    newParams.getPreviewSize(&previewWidth, &previewHeight);

    if (previewWidth != k.mParameters.previewWidth ||
            previewHeight != k.mParameters.previewHeight) {
        if (mState >= PREVIEW) {
            ALOGE("%s: Preview size cannot be updated when preview "
                    "is active! (Currently %d x %d, requested %d x %d",
                    __FUNCTION__,
                    k.mParameters.previewWidth, k.mParameters.previewHeight,
                    previewWidth, previewHeight);
            return BAD_VALUE;
        }
        camera_metadata_entry_t availablePreviewSizes =
            staticInfo(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES);
        for (i = 0; i < availablePreviewSizes.count; i += 2 ) {
            if (availablePreviewSizes.data.i32[i] == previewWidth &&
                    availablePreviewSizes.data.i32[i+1] == previewHeight) break;
        }
        if (i == availablePreviewSizes.count) {
            ALOGE("%s: Requested preview size %d x %d is not supported",
                    __FUNCTION__, previewWidth, previewHeight);
            return BAD_VALUE;
        }
    }

    // PREVIEW_FPS_RANGE
    int previewFpsRange[2];
    int previewFps = 0;
    bool fpsRangeChanged = false;
    newParams.getPreviewFpsRange(&previewFpsRange[0], &previewFpsRange[1]);
    if (previewFpsRange[0] != k.mParameters.previewFpsRange[0] ||
            previewFpsRange[1] != k.mParameters.previewFpsRange[1]) {
        fpsRangeChanged = true;
        camera_metadata_entry_t availablePreviewFpsRanges =
            staticInfo(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, 2);
        for (i = 0; i < availablePreviewFpsRanges.count; i += 2) {
            if ((availablePreviewFpsRanges.data.i32[i] ==
                    previewFpsRange[0]) &&
                (availablePreviewFpsRanges.data.i32[i+1] ==
                    previewFpsRange[1]) ) {
                break;
            }
        }
        if (i == availablePreviewFpsRanges.count) {
            ALOGE("%s: Requested preview FPS range %d - %d is not supported",
                __FUNCTION__, previewFpsRange[0], previewFpsRange[1]);
            return BAD_VALUE;
        }
        previewFps = previewFpsRange[0];
    }

    // PREVIEW_FORMAT
    int previewFormat = formatStringToEnum(newParams.getPreviewFormat());
    if (previewFormat != k.mParameters.previewFormat) {
        if (mState >= PREVIEW) {
            ALOGE("%s: Preview format cannot be updated when preview "
                    "is active!", __FUNCTION__);
            return BAD_VALUE;
        }
        camera_metadata_entry_t availableFormats =
            staticInfo(ANDROID_SCALER_AVAILABLE_FORMATS);
        for (i = 0; i < availableFormats.count; i++) {
            if (availableFormats.data.i32[i] == previewFormat) break;
        }
        if (i == availableFormats.count) {
            ALOGE("%s: Requested preview format %s (0x%x) is not supported",
                    __FUNCTION__, newParams.getPreviewFormat(), previewFormat);
            return BAD_VALUE;
        }
    }

    // PREVIEW_FRAME_RATE
    // Deprecated, only use if the preview fps range is unchanged this time.
    // The single-value FPS is the same as the minimum of the range.
    if (!fpsRangeChanged) {
        previewFps = newParams.getPreviewFrameRate();
        if (previewFps != k.mParameters.previewFps) {
            camera_metadata_entry_t availableFrameRates =
                staticInfo(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            for (i = 0; i < availableFrameRates.count; i+=2) {
                if (availableFrameRates.data.i32[i] == previewFps) break;
            }
            if (i == availableFrameRates.count) {
                ALOGE("%s: Requested preview frame rate %d is not supported",
                        __FUNCTION__, previewFps);
                return BAD_VALUE;
            }
            previewFpsRange[0] = availableFrameRates.data.i32[i];
            previewFpsRange[1] = availableFrameRates.data.i32[i+1];
        }
    }

    // PICTURE_SIZE
    int pictureWidth, pictureHeight;
    newParams.getPictureSize(&pictureWidth, &pictureHeight);
    if (pictureWidth == k.mParameters.pictureWidth ||
            pictureHeight == k.mParameters.pictureHeight) {
        camera_metadata_entry_t availablePictureSizes =
            staticInfo(ANDROID_SCALER_AVAILABLE_JPEG_SIZES);
        for (i = 0; i < availablePictureSizes.count; i+=2) {
            if (availablePictureSizes.data.i32[i] == pictureWidth &&
                    availablePictureSizes.data.i32[i+1] == pictureHeight) break;
        }
        if (i == availablePictureSizes.count) {
            ALOGE("%s: Requested picture size %d x %d is not supported",
                    __FUNCTION__, pictureWidth, pictureHeight);
            return BAD_VALUE;
        }
    }

    // JPEG_THUMBNAIL_WIDTH/HEIGHT
    int jpegThumbSize[2];
    jpegThumbSize[0] =
            newParams.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    jpegThumbSize[1] =
            newParams.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
    if (jpegThumbSize[0] != k.mParameters.jpegThumbSize[0] ||
            jpegThumbSize[1] != k.mParameters.jpegThumbSize[1]) {
        camera_metadata_entry_t availableJpegThumbSizes =
            staticInfo(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES);
        for (i = 0; i < availableJpegThumbSizes.count; i+=2) {
            if (availableJpegThumbSizes.data.i32[i] == jpegThumbSize[0] &&
                    availableJpegThumbSizes.data.i32[i+1] == jpegThumbSize[1]) {
                break;
            }
        }
        if (i == availableJpegThumbSizes.count) {
            ALOGE("%s: Requested JPEG thumbnail size %d x %d is not supported",
                    __FUNCTION__, jpegThumbSize[0], jpegThumbSize[1]);
            return BAD_VALUE;
        }
    }

    // JPEG_THUMBNAIL_QUALITY
    int jpegThumbQuality =
            newParams.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY);
    if (jpegThumbQuality < 0 || jpegThumbQuality > 100) {
        ALOGE("%s: Requested JPEG thumbnail quality %d is not supported",
                __FUNCTION__, jpegThumbQuality);
        return BAD_VALUE;
    }

    // JPEG_QUALITY
    int jpegQuality =
            newParams.getInt(CameraParameters::KEY_JPEG_QUALITY);
    if (jpegQuality < 0 || jpegQuality > 100) {
        ALOGE("%s: Requested JPEG quality %d is not supported",
                __FUNCTION__, jpegQuality);
        return BAD_VALUE;
    }

    // ROTATION
    int jpegRotation =
            newParams.getInt(CameraParameters::KEY_ROTATION);
    if (jpegRotation != 0 &&
            jpegRotation != 90 &&
            jpegRotation != 180 &&
            jpegRotation != 270) {
        ALOGE("%s: Requested picture rotation angle %d is not supported",
                __FUNCTION__, jpegRotation);
        return BAD_VALUE;
    }

    // GPS
    bool gpsEnabled = false;
    double gpsCoordinates[3] = {0,0,0};
    int64_t gpsTimestamp = 0;
    String8 gpsProcessingMethod;
    const char *gpsLatStr =
            newParams.get(CameraParameters::KEY_GPS_LATITUDE);
    if (gpsLatStr != NULL) {
        const char *gpsLongStr =
                newParams.get(CameraParameters::KEY_GPS_LONGITUDE);
        const char *gpsAltitudeStr =
                newParams.get(CameraParameters::KEY_GPS_ALTITUDE);
        const char *gpsTimeStr =
                newParams.get(CameraParameters::KEY_GPS_TIMESTAMP);
        const char *gpsProcMethodStr =
                newParams.get(CameraParameters::KEY_GPS_PROCESSING_METHOD);
        if (gpsLongStr == NULL ||
                gpsAltitudeStr == NULL ||
                gpsTimeStr == NULL ||
                gpsProcMethodStr == NULL) {
            ALOGE("%s: Incomplete set of GPS parameters provided",
                    __FUNCTION__);
            return BAD_VALUE;
        }
        char *endPtr;
        errno = 0;
        gpsCoordinates[0] = strtod(gpsLatStr, &endPtr);
        if (errno || endPtr == gpsLatStr) {
            ALOGE("%s: Malformed GPS latitude: %s", __FUNCTION__, gpsLatStr);
            return BAD_VALUE;
        }
        errno = 0;
        gpsCoordinates[1] = strtod(gpsLongStr, &endPtr);
        if (errno || endPtr == gpsLongStr) {
            ALOGE("%s: Malformed GPS longitude: %s", __FUNCTION__, gpsLongStr);
            return BAD_VALUE;
        }
        errno = 0;
        gpsCoordinates[2] = strtod(gpsAltitudeStr, &endPtr);
        if (errno || endPtr == gpsAltitudeStr) {
            ALOGE("%s: Malformed GPS altitude: %s", __FUNCTION__,
                    gpsAltitudeStr);
            return BAD_VALUE;
        }
        errno = 0;
        gpsTimestamp = strtoll(gpsTimeStr, &endPtr, 10);
        if (errno || endPtr == gpsTimeStr) {
            ALOGE("%s: Malformed GPS timestamp: %s", __FUNCTION__, gpsTimeStr);
            return BAD_VALUE;
        }
        gpsProcessingMethod = gpsProcMethodStr;

        gpsEnabled = true;
    }

    // WHITE_BALANCE
    int wbMode = wbModeStringToEnum(
        newParams.get(CameraParameters::KEY_WHITE_BALANCE) );
    if (wbMode != k.mParameters.wbMode) {
        camera_metadata_entry_t availableWbModes =
            staticInfo(ANDROID_CONTROL_AWB_AVAILABLE_MODES);
        for (i = 0; i < availableWbModes.count; i++) {
            if (wbMode == availableWbModes.data.u8[i]) break;
        }
        if (i == availableWbModes.count) {
            ALOGE("%s: Requested white balance mode %s is not supported",
                    __FUNCTION__,
                    newParams.get(CameraParameters::KEY_WHITE_BALANCE));
            return BAD_VALUE;
        }
    }

    // EFFECT
    int effectMode = effectModeStringToEnum(
        newParams.get(CameraParameters::KEY_EFFECT) );
    if (effectMode != k.mParameters.effectMode) {
        camera_metadata_entry_t availableEffectModes =
            staticInfo(ANDROID_CONTROL_AVAILABLE_EFFECTS);
        for (i = 0; i < availableEffectModes.count; i++) {
            if (effectMode == availableEffectModes.data.u8[i]) break;
        }
        if (i == availableEffectModes.count) {
            ALOGE("%s: Requested effect mode \"%s\" is not supported",
                    __FUNCTION__,
                    newParams.get(CameraParameters::KEY_EFFECT) );
            return BAD_VALUE;
        }
    }

    // ANTIBANDING
    int antibandingMode = abModeStringToEnum(
        newParams.get(CameraParameters::KEY_ANTIBANDING) );
    if (antibandingMode != k.mParameters.antibandingMode) {
        camera_metadata_entry_t availableAbModes =
            staticInfo(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES);
        for (i = 0; i < availableAbModes.count; i++) {
            if (antibandingMode == availableAbModes.data.u8[i]) break;
        }
        if (i == availableAbModes.count) {
            ALOGE("%s: Requested antibanding mode \"%s\" is not supported",
                    __FUNCTION__,
                    newParams.get(CameraParameters::KEY_ANTIBANDING));
            return BAD_VALUE;
        }
    }

    // SCENE_MODE
    int sceneMode = sceneModeStringToEnum(
        newParams.get(CameraParameters::KEY_SCENE_MODE) );
    if (sceneMode != k.mParameters.sceneMode) {
        camera_metadata_entry_t availableSceneModes =
            staticInfo(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
        for (i = 0; i < availableSceneModes.count; i++) {
            if (sceneMode == availableSceneModes.data.u8[i]) break;
        }
        if (i == availableSceneModes.count) {
            ALOGE("%s: Requested scene mode \"%s\" is not supported",
                    __FUNCTION__,
                    newParams.get(CameraParameters::KEY_SCENE_MODE));
            return BAD_VALUE;
        }
    }

    // FLASH_MODE
    Parameters::flashMode_t flashMode = flashModeStringToEnum(
        newParams.get(CameraParameters::KEY_FLASH_MODE) );
    if (flashMode != k.mParameters.flashMode) {
        camera_metadata_entry_t flashAvailable =
            staticInfo(ANDROID_FLASH_AVAILABLE, 1, 1);
        if (!flashAvailable.data.u8[0] &&
                flashMode != Parameters::FLASH_MODE_OFF) {
            ALOGE("%s: Requested flash mode \"%s\" is not supported: "
                    "No flash on device", __FUNCTION__,
                    newParams.get(CameraParameters::KEY_FLASH_MODE));
            return BAD_VALUE;
        } else if (flashMode == Parameters::FLASH_MODE_RED_EYE) {
            camera_metadata_entry_t availableAeModes =
                staticInfo(ANDROID_CONTROL_AE_AVAILABLE_MODES);
            for (i = 0; i < availableAeModes.count; i++) {
                if (flashMode == availableAeModes.data.u8[i]) break;
            }
            if (i == availableAeModes.count) {
                ALOGE("%s: Requested flash mode \"%s\" is not supported",
                        __FUNCTION__,
                        newParams.get(CameraParameters::KEY_FLASH_MODE));
                return BAD_VALUE;
            }
        } else if (flashMode == -1) {
            ALOGE("%s: Requested flash mode \"%s\" is unknown",
                    __FUNCTION__,
                    newParams.get(CameraParameters::KEY_FLASH_MODE));
            return BAD_VALUE;
        }
    }

    // FOCUS_MODE
    Parameters::focusMode_t focusMode = focusModeStringToEnum(
        newParams.get(CameraParameters::KEY_FOCUS_MODE));
    if (focusMode != k.mParameters.focusMode) {
        if (focusMode != Parameters::FOCUS_MODE_FIXED) {
            camera_metadata_entry_t minFocusDistance =
                staticInfo(ANDROID_LENS_MINIMUM_FOCUS_DISTANCE);
            if (minFocusDistance.data.f[0] == 0) {
                ALOGE("%s: Requested focus mode \"%s\" is not available: "
                        "fixed focus lens",
                        __FUNCTION__,
                        newParams.get(CameraParameters::KEY_FOCUS_MODE));
                return BAD_VALUE;
            } else if (focusMode != Parameters::FOCUS_MODE_INFINITY) {
                camera_metadata_entry_t availableFocusModes =
                    staticInfo(ANDROID_CONTROL_AF_AVAILABLE_MODES);
                for (i = 0; i < availableFocusModes.count; i++) {
                    if (focusMode == availableFocusModes.data.u8[i]) break;
                }
                if (i == availableFocusModes.count) {
                    ALOGE("%s: Requested focus mode \"%s\" is not supported",
                            __FUNCTION__,
                            newParams.get(CameraParameters::KEY_FOCUS_MODE));
                    return BAD_VALUE;
                }
            }
        }
    }

    // FOCUS_AREAS
    Vector<Parameters::Area> focusingAreas;
    res = parseAreas(newParams.get(CameraParameters::KEY_FOCUS_AREAS),
            &focusingAreas);
    size_t max3aRegions =
        (size_t)staticInfo(ANDROID_CONTROL_MAX_REGIONS, 1, 1).data.i32[0];
    if (res == OK) res = validateAreas(focusingAreas, max3aRegions);
    if (res != OK) {
        ALOGE("%s: Requested focus areas are malformed: %s",
                __FUNCTION__, newParams.get(CameraParameters::KEY_FOCUS_AREAS));
        return BAD_VALUE;
    }

    // EXPOSURE_COMPENSATION
    int exposureCompensation =
        newParams.getInt(CameraParameters::KEY_EXPOSURE_COMPENSATION);
    camera_metadata_entry_t exposureCompensationRange =
        staticInfo(ANDROID_CONTROL_AE_EXP_COMPENSATION_RANGE);
    if (exposureCompensation < exposureCompensationRange.data.i32[0] ||
            exposureCompensation > exposureCompensationRange.data.i32[1]) {
        ALOGE("%s: Requested exposure compensation index is out of bounds: %d",
                __FUNCTION__, exposureCompensation);
        return BAD_VALUE;
    }

    // AUTO_EXPOSURE_LOCK (always supported)
    bool autoExposureLock = boolFromString(
        newParams.get(CameraParameters::KEY_AUTO_EXPOSURE_LOCK));

    // AUTO_WHITEBALANCE_LOCK (always supported)
    bool autoWhiteBalanceLock = boolFromString(
        newParams.get(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK));

    // METERING_AREAS
    Vector<Parameters::Area> meteringAreas;
    res = parseAreas(newParams.get(CameraParameters::KEY_METERING_AREAS),
            &meteringAreas);
    if (res == OK) res = validateAreas(focusingAreas, max3aRegions);
    if (res != OK) {
        ALOGE("%s: Requested metering areas are malformed: %s",
                __FUNCTION__,
                newParams.get(CameraParameters::KEY_METERING_AREAS));
        return BAD_VALUE;
    }

    // ZOOM
    int zoom = newParams.getInt(CameraParameters::KEY_ZOOM);
    if (zoom < 0 || zoom > (int)NUM_ZOOM_STEPS) {
        ALOGE("%s: Requested zoom level %d is not supported",
                __FUNCTION__, zoom);
        return BAD_VALUE;
    }

    // VIDEO_SIZE
    int videoWidth, videoHeight;
    newParams.getVideoSize(&videoWidth, &videoHeight);
    if (videoWidth != k.mParameters.videoWidth ||
            videoHeight != k.mParameters.videoHeight) {
        if (mState == RECORD) {
            ALOGE("%s: Video size cannot be updated when recording is active!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
        camera_metadata_entry_t availableVideoSizes =
            staticInfo(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES);
        for (i = 0; i < availableVideoSizes.count; i += 2 ) {
            if (availableVideoSizes.data.i32[i] == videoWidth &&
                    availableVideoSizes.data.i32[i+1] == videoHeight)  break;
        }
        if (i == availableVideoSizes.count) {
            ALOGE("%s: Requested video size %d x %d is not supported",
                    __FUNCTION__, videoWidth, videoHeight);
            return BAD_VALUE;
        }
    }

    // RECORDING_HINT (always supported)
    bool recordingHint = boolFromString(
        newParams.get(CameraParameters::KEY_RECORDING_HINT) );

    // VIDEO_STABILIZATION
    bool videoStabilization = boolFromString(
        newParams.get(CameraParameters::KEY_VIDEO_STABILIZATION) );
    camera_metadata_entry_t availableVideoStabilizationModes =
        staticInfo(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
    if (videoStabilization && availableVideoStabilizationModes.count == 1) {
        ALOGE("%s: Video stabilization not supported", __FUNCTION__);
    }

    /** Update internal parameters */

    k.mParameters.previewWidth = previewWidth;
    k.mParameters.previewHeight = previewHeight;
    k.mParameters.previewFpsRange[0] = previewFpsRange[0];
    k.mParameters.previewFpsRange[1] = previewFpsRange[1];
    k.mParameters.previewFps = previewFps;
    k.mParameters.previewFormat = previewFormat;

    k.mParameters.pictureWidth = pictureWidth;
    k.mParameters.pictureHeight = pictureHeight;

    k.mParameters.jpegThumbSize[0] = jpegThumbSize[0];
    k.mParameters.jpegThumbSize[1] = jpegThumbSize[1];
    k.mParameters.jpegQuality = jpegQuality;
    k.mParameters.jpegThumbQuality = jpegThumbQuality;

    k.mParameters.gpsEnabled = gpsEnabled;
    k.mParameters.gpsCoordinates[0] = gpsCoordinates[0];
    k.mParameters.gpsCoordinates[1] = gpsCoordinates[1];
    k.mParameters.gpsCoordinates[2] = gpsCoordinates[2];
    k.mParameters.gpsTimestamp = gpsTimestamp;
    k.mParameters.gpsProcessingMethod = gpsProcessingMethod;

    k.mParameters.wbMode = wbMode;
    k.mParameters.effectMode = effectMode;
    k.mParameters.antibandingMode = antibandingMode;
    k.mParameters.sceneMode = sceneMode;

    k.mParameters.flashMode = flashMode;
    k.mParameters.focusMode = focusMode;

    k.mParameters.focusingAreas = focusingAreas;
    k.mParameters.exposureCompensation = exposureCompensation;
    k.mParameters.autoExposureLock = autoExposureLock;
    k.mParameters.autoWhiteBalanceLock = autoWhiteBalanceLock;
    k.mParameters.meteringAreas = meteringAreas;
    k.mParameters.zoom = zoom;

    k.mParameters.videoWidth = videoWidth;
    k.mParameters.videoHeight = videoHeight;

    k.mParameters.recordingHint = recordingHint;
    k.mParameters.videoStabilization = videoStabilization;

    res = updatePreviewRequest(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = updateCaptureRequest(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update capture request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    res = updateRecordingRequest(k.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update recording request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    if (mState == PREVIEW) {
        res = mDevice->setStreamingRequest(mPreviewRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error streaming new preview request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    } else if (mState == RECORD || mState == VIDEO_SNAPSHOT) {
        res = mDevice->setStreamingRequest(mRecordingRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error streaming new record request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    k.mParameters.paramsFlattened = params;

    return OK;
}

String8 Camera2Client::getParameters() const {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    LockedParameters::ReadKey k(mParameters);

    // TODO: Deal with focus distances
    return k.mParameters.paramsFlattened;
}

status_t Camera2Client::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    ALOGV("%s: Camera %d: Command %d (%d, %d)", __FUNCTION__, mCameraId,
            cmd, arg1, arg2);

    if (cmd == CAMERA_CMD_SET_DISPLAY_ORIENTATION) {
        LockedParameters::Key k(mParameters);
        int transform = degToTransform(arg1,
                mCameraFacing == CAMERA_FACING_FRONT);
        if (transform == -1) {
            ALOGE("%s: Camera %d: Error setting %d as display orientation value",
                    __FUNCTION__, mCameraId, arg1);
            return BAD_VALUE;
        }
        if (transform != k.mParameters.previewTransform &&
                mPreviewStreamId != NO_STREAM) {
            mDevice->setStreamTransform(mPreviewStreamId, transform);
        }
        k.mParameters.previewTransform = transform;
        return OK;
    }

    ALOGE("%s: Camera %d: Unimplemented command %d (%d, %d)", __FUNCTION__,
            mCameraId, cmd, arg1, arg2);

    return OK;
}

/** Device-related methods */

void Camera2Client::onCaptureAvailable() {
    ATRACE_CALL();
    status_t res;
    sp<ICameraClient> currentClient;
    ALOGV("%s: Camera %d: Still capture available", __FUNCTION__, mCameraId);

    CpuConsumer::LockedBuffer imgBuffer;
    {
        Mutex::Autolock icl(mICameraLock);

        // TODO: Signal errors here upstream
        if (mState != STILL_CAPTURE && mState != VIDEO_SNAPSHOT) {
            ALOGE("%s: Camera %d: Still image produced unexpectedly!",
                    __FUNCTION__, mCameraId);
            return;
        }

        res = mCaptureConsumer->lockNextBuffer(&imgBuffer);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error receiving still image buffer: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return;
        }

        if (imgBuffer.format != HAL_PIXEL_FORMAT_BLOB) {
            ALOGE("%s: Camera %d: Unexpected format for still image: "
                    "%x, expected %x", __FUNCTION__, mCameraId,
                    imgBuffer.format,
                    HAL_PIXEL_FORMAT_BLOB);
            mCaptureConsumer->unlockBuffer(imgBuffer);
            return;
        }

        // TODO: Optimize this to avoid memcopy
        void* captureMemory = mCaptureHeap->mHeap->getBase();
        size_t size = mCaptureHeap->mHeap->getSize();
        memcpy(captureMemory, imgBuffer.data, size);

        mCaptureConsumer->unlockBuffer(imgBuffer);

        currentClient = mCameraClient;
        switch (mState) {
            case STILL_CAPTURE:
                mState = STOPPED;
                break;
            case VIDEO_SNAPSHOT:
                mState = RECORD;
                break;
            default:
                ALOGE("%s: Camera %d: Unexpected state %d", __FUNCTION__,
                        mCameraId, mState);
                break;
        }
    }
    // Call outside mICameraLock to allow re-entrancy from notification
    if (currentClient != 0) {
        currentClient->dataCallback(CAMERA_MSG_COMPRESSED_IMAGE,
                mCaptureHeap->mBuffers[0], NULL);
    }
}

void Camera2Client::onRecordingFrameAvailable() {
    ATRACE_CALL();
    status_t res;
    sp<ICameraClient> currentClient;
    size_t heapIdx = 0;
    nsecs_t timestamp;
    {
        Mutex::Autolock icl(mICameraLock);
        // TODO: Signal errors here upstream
        bool discardData = false;
        if (mState != RECORD && mState != VIDEO_SNAPSHOT) {
            ALOGV("%s: Camera %d: Discarding recording image buffers received after "
                    "recording done",
                    __FUNCTION__, mCameraId);
            discardData = true;
        }

        buffer_handle_t imgBuffer;
        res = mRecordingConsumer->getNextBuffer(&imgBuffer, &timestamp);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error receiving recording buffer: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return;
        }

        if (discardData) {
            mRecordingConsumer->freeBuffer(imgBuffer);
            return;
        }

        if (mRecordingHeap == 0) {
            const size_t bufferSize = 4 + sizeof(buffer_handle_t);
            ALOGV("%s: Camera %d: Creating recording heap with %d buffers of "
                    "size %d bytes", __FUNCTION__, mCameraId,
                    kRecordingHeapCount, bufferSize);
            if (mRecordingHeap != 0) {
                ALOGV("%s: Camera %d: Previous heap has size %d "
                        "(new will be %d) bytes", __FUNCTION__, mCameraId,
                        mRecordingHeap->mHeap->getSize(),
                        bufferSize * kRecordingHeapCount);
            }
            // Need to allocate memory for heap
            mRecordingHeap.clear();

            mRecordingHeap = new Camera2Heap(bufferSize, kRecordingHeapCount,
                    "Camera2Client::RecordingHeap");
            if (mRecordingHeap->mHeap->getSize() == 0) {
                ALOGE("%s: Camera %d: Unable to allocate memory for recording",
                        __FUNCTION__, mCameraId);
                mRecordingConsumer->freeBuffer(imgBuffer);
                return;
            }
            mRecordingHeapHead = 0;
            mRecordingHeapFree = kRecordingHeapCount;
        }

        if ( mRecordingHeapFree == 0) {
            ALOGE("%s: Camera %d: No free recording buffers, dropping frame",
                    __FUNCTION__, mCameraId);
            mRecordingConsumer->freeBuffer(imgBuffer);
            return;
        }
        heapIdx = mRecordingHeapHead;
        mRecordingHeapHead = (mRecordingHeapHead + 1) % kRecordingHeapCount;
        mRecordingHeapFree--;

        ALOGV("%s: Camera %d: Timestamp %lld",
                __FUNCTION__, mCameraId, timestamp);

        ssize_t offset;
        size_t size;
        sp<IMemoryHeap> heap =
                mRecordingHeap->mBuffers[heapIdx]->getMemory(&offset,
                        &size);

        uint8_t *data = (uint8_t*)heap->getBase() + offset;
        uint32_t type = kMetadataBufferTypeGrallocSource;
        memcpy(data, &type, 4);
        memcpy(data + 4, &imgBuffer, sizeof(buffer_handle_t));
        ALOGV("%s: Camera %d: Sending out buffer_handle_t %p",
                __FUNCTION__, mCameraId, imgBuffer);
        currentClient = mCameraClient;
    }
    // Call outside mICameraLock to allow re-entrancy from notification
    if (currentClient != 0) {
        currentClient->dataCallbackTimestamp(timestamp,
                CAMERA_MSG_VIDEO_FRAME,
                mRecordingHeap->mBuffers[heapIdx]);
    }
}

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
    LockedParameters::Key k(mParameters);

    status_t res;
    CameraParameters params;

    camera_metadata_entry_t availableProcessedSizes =
        staticInfo(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, 2);
    if (!availableProcessedSizes.count) return NO_INIT;

    // TODO: Pick more intelligently
    k.mParameters.previewWidth = availableProcessedSizes.data.i32[0];
    k.mParameters.previewHeight = availableProcessedSizes.data.i32[1];
    k.mParameters.videoWidth = k.mParameters.previewWidth;
    k.mParameters.videoHeight = k.mParameters.previewHeight;

    params.setPreviewSize(k.mParameters.previewWidth, k.mParameters.previewHeight);
    params.setVideoSize(k.mParameters.videoWidth, k.mParameters.videoHeight);
    params.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO,
            String8::format("%dx%d",
                    k.mParameters.previewWidth, k.mParameters.previewHeight));
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

    k.mParameters.previewFpsRange[0] = availableFpsRanges.data.i32[0];
    k.mParameters.previewFpsRange[1] = availableFpsRanges.data.i32[1];

    params.set(CameraParameters::KEY_PREVIEW_FPS_RANGE,
            String8::format("%d,%d",
                    k.mParameters.previewFpsRange[0],
                    k.mParameters.previewFpsRange[1]));

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

    k.mParameters.previewFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
    params.set(CameraParameters::KEY_PREVIEW_FORMAT,
            formatEnumToString(k.mParameters.previewFormat)); // NV21

    k.mParameters.previewTransform = degToTransform(0,
            mCameraFacing == CAMERA_FACING_FRONT);

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
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_YUV422SP;
                break;
            case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_YUV420SP;
                break;
            case HAL_PIXEL_FORMAT_YCbCr_422_I:
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_YUV422I;
                break;
            case HAL_PIXEL_FORMAT_YV12:
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_YUV420P;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_RGB565;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                supportedPreviewFormats +=
                    CameraParameters::PIXEL_FORMAT_RGBA8888;
                break;
            // Not advertizing JPEG, RAW_SENSOR, etc, for preview formats
            case HAL_PIXEL_FORMAT_RAW_SENSOR:
            case HAL_PIXEL_FORMAT_BLOB:
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
            k.mParameters.previewFpsRange[0]);

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
    k.mParameters.pictureWidth = availableJpegSizes.data.i32[0];
    k.mParameters.pictureHeight = availableJpegSizes.data.i32[1];

    params.setPictureSize(k.mParameters.pictureWidth,
            k.mParameters.pictureHeight);

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
    k.mParameters.jpegThumbSize[0] = availableJpegThumbnailSizes.data.i32[0];
    k.mParameters.jpegThumbSize[1] = availableJpegThumbnailSizes.data.i32[1];

    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,
            k.mParameters.jpegThumbSize[0]);
    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,
            k.mParameters.jpegThumbSize[1]);

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

    k.mParameters.jpegThumbQuality = 90;
    params.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY,
            k.mParameters.jpegThumbQuality);
    k.mParameters.jpegQuality = 90;
    params.set(CameraParameters::KEY_JPEG_QUALITY,
            k.mParameters.jpegQuality);
    k.mParameters.jpegRotation = 0;
    params.set(CameraParameters::KEY_ROTATION,
            k.mParameters.jpegRotation);

    k.mParameters.gpsEnabled = false;
    k.mParameters.gpsProcessingMethod = "unknown";
    // GPS fields in CameraParameters are not set by implementation

    k.mParameters.wbMode = ANDROID_CONTROL_AWB_AUTO;
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
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_AUTO;
                break;
            case ANDROID_CONTROL_AWB_INCANDESCENT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_INCANDESCENT;
                break;
            case ANDROID_CONTROL_AWB_FLUORESCENT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_FLUORESCENT;
                break;
            case ANDROID_CONTROL_AWB_WARM_FLUORESCENT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT;
                break;
            case ANDROID_CONTROL_AWB_DAYLIGHT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_DAYLIGHT;
                break;
            case ANDROID_CONTROL_AWB_CLOUDY_DAYLIGHT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT;
                break;
            case ANDROID_CONTROL_AWB_TWILIGHT:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_TWILIGHT;
                break;
            case ANDROID_CONTROL_AWB_SHADE:
                supportedWhiteBalance +=
                    CameraParameters::WHITE_BALANCE_SHADE;
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

    k.mParameters.effectMode = ANDROID_CONTROL_EFFECT_OFF;
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
                    supportedEffects +=
                        CameraParameters::EFFECT_NONE;
                    break;
                case ANDROID_CONTROL_EFFECT_MONO:
                    supportedEffects +=
                        CameraParameters::EFFECT_MONO;
                    break;
                case ANDROID_CONTROL_EFFECT_NEGATIVE:
                    supportedEffects +=
                        CameraParameters::EFFECT_NEGATIVE;
                    break;
                case ANDROID_CONTROL_EFFECT_SOLARIZE:
                    supportedEffects +=
                        CameraParameters::EFFECT_SOLARIZE;
                    break;
                case ANDROID_CONTROL_EFFECT_SEPIA:
                    supportedEffects +=
                        CameraParameters::EFFECT_SEPIA;
                    break;
                case ANDROID_CONTROL_EFFECT_POSTERIZE:
                    supportedEffects +=
                        CameraParameters::EFFECT_POSTERIZE;
                    break;
                case ANDROID_CONTROL_EFFECT_WHITEBOARD:
                    supportedEffects +=
                        CameraParameters::EFFECT_WHITEBOARD;
                    break;
                case ANDROID_CONTROL_EFFECT_BLACKBOARD:
                    supportedEffects +=
                        CameraParameters::EFFECT_BLACKBOARD;
                    break;
                case ANDROID_CONTROL_EFFECT_AQUA:
                    supportedEffects +=
                        CameraParameters::EFFECT_AQUA;
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

    k.mParameters.antibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_AUTO;
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
                    supportedAntibanding +=
                        CameraParameters::ANTIBANDING_OFF;
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_50HZ:
                    supportedAntibanding +=
                        CameraParameters::ANTIBANDING_50HZ;
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_60HZ:
                    supportedAntibanding +=
                        CameraParameters::ANTIBANDING_60HZ;
                    break;
                case ANDROID_CONTROL_AE_ANTIBANDING_AUTO:
                    supportedAntibanding +=
                        CameraParameters::ANTIBANDING_AUTO;
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

    k.mParameters.sceneMode = ANDROID_CONTROL_OFF;
    params.set(CameraParameters::KEY_SCENE_MODE,
            CameraParameters::SCENE_MODE_AUTO);

    camera_metadata_entry_t availableSceneModes =
        staticInfo(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
    if (!availableSceneModes.count) return NO_INIT;
    {
        String8 supportedSceneModes(CameraParameters::SCENE_MODE_AUTO);
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
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_ACTION;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_PORTRAIT:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_PORTRAIT;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_LANDSCAPE:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_LANDSCAPE;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_NIGHT:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_NIGHT;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_NIGHT_PORTRAIT:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_NIGHT_PORTRAIT;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_THEATRE:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_THEATRE;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_BEACH:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_BEACH;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SNOW:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_SNOW;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SUNSET:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_SUNSET;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_STEADYPHOTO:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_STEADYPHOTO;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_FIREWORKS:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_FIREWORKS;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_SPORTS:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_SPORTS;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_PARTY:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_PARTY;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_CANDLELIGHT;
                    break;
                case ANDROID_CONTROL_SCENE_MODE_BARCODE:
                    supportedSceneModes +=
                        CameraParameters::SCENE_MODE_BARCODE;
                    break;
                default:
                    ALOGW("%s: Camera %d: Unknown scene mode value: %d",
                        __FUNCTION__, mCameraId,
                            availableSceneModes.data.u8[i]);
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
        k.mParameters.flashMode = Parameters::FLASH_MODE_AUTO;
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
        k.mParameters.flashMode = Parameters::FLASH_MODE_OFF;
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
        k.mParameters.focusMode = Parameters::FOCUS_MODE_FIXED;
        params.set(CameraParameters::KEY_FOCUS_MODE,
                CameraParameters::FOCUS_MODE_FIXED);
        params.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                CameraParameters::FOCUS_MODE_FIXED);
    } else {
        k.mParameters.focusMode = Parameters::FOCUS_MODE_AUTO;
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
                    supportedFocusModes +=
                        CameraParameters::FOCUS_MODE_AUTO;
                    break;
                case ANDROID_CONTROL_AF_MACRO:
                    supportedFocusModes +=
                        CameraParameters::FOCUS_MODE_MACRO;
                    break;
                case ANDROID_CONTROL_AF_CONTINUOUS_VIDEO:
                    supportedFocusModes +=
                        CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO;
                    break;
                case ANDROID_CONTROL_AF_CONTINUOUS_PICTURE:
                    supportedFocusModes +=
                        CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE;
                    break;
                case ANDROID_CONTROL_AF_EDOF:
                    supportedFocusModes +=
                        CameraParameters::FOCUS_MODE_EDOF;
                    break;
                // Not supported in old API
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
    k.mParameters.focusingAreas.clear();
    k.mParameters.focusingAreas.add(Parameters::Area(0,0,0,0,0));

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

    k.mParameters.exposureCompensation = 0;
    params.set(CameraParameters::KEY_EXPOSURE_COMPENSATION,
                k.mParameters.exposureCompensation);

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

    k.mParameters.autoExposureLock = false;
    params.set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK,
            CameraParameters::FALSE);
    params.set(CameraParameters::KEY_AUTO_EXPOSURE_LOCK_SUPPORTED,
            CameraParameters::TRUE);

    k.mParameters.autoWhiteBalanceLock = false;
    params.set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK,
            CameraParameters::FALSE);
    params.set(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK_SUPPORTED,
            CameraParameters::TRUE);

    k.mParameters.meteringAreas.add(Parameters::Area(0, 0, 0, 0, 0));
    params.set(CameraParameters::KEY_MAX_NUM_METERING_AREAS,
            max3aRegions.data.i32[0]);
    params.set(CameraParameters::KEY_METERING_AREAS,
            "(0,0,0,0,0)");

    k.mParameters.zoom = 0;
    params.set(CameraParameters::KEY_ZOOM, k.mParameters.zoom);
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
            CameraParameters::PIXEL_FORMAT_ANDROID_OPAQUE);

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

    // Always use metadata mode for recording
    k.mParameters.storeMetadataInBuffers = true;

    k.mParameters.paramsFlattened = params.flatten();

    return OK;
}

status_t Camera2Client::updatePreviewStream(const Parameters &params) {
    ATRACE_CALL();
    status_t res;

    if (mPreviewStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = mDevice->getStreamInfo(mPreviewStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying preview stream info: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.previewWidth ||
                currentHeight != (uint32_t)params.previewHeight) {
            ALOGV("%s: Camera %d: Preview size switch: %d x %d -> %d x %d",
                    __FUNCTION__, mCameraId, currentWidth, currentHeight,
                    params.previewWidth, params.previewHeight);
            res = mDevice->waitUntilDrained();
            if (res != OK) {
                ALOGE("%s: Camera %d: Error waiting for preview to drain: "
                        "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
                return res;
            }
            res = mDevice->deleteStream(mPreviewStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for preview: %s (%d)", __FUNCTION__, mCameraId,
                        strerror(-res), res);
                return res;
            }
            mPreviewStreamId = NO_STREAM;
        }
    }

    if (mPreviewStreamId == NO_STREAM) {
        res = mDevice->createStream(mPreviewWindow,
                params.previewWidth, params.previewHeight,
                CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, 0,
                &mPreviewStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create preview stream: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = mDevice->setStreamTransform(mPreviewStreamId,
            params.previewTransform);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview stream transform: "
                "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    return OK;
}

status_t Camera2Client::updatePreviewRequest(const Parameters &params) {
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

    res = updateRequestCommon(mPreviewRequest, params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of preview "
                "request: %s (%d)", __FUNCTION__, mCameraId,
                strerror(-res), res);
        return res;
    }

    return OK;
}

status_t Camera2Client::updateCaptureStream(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    // Find out buffer size for JPEG
    camera_metadata_entry_t maxJpegSize =
            staticInfo(ANDROID_JPEG_MAX_SIZE);
    if (maxJpegSize.count == 0) {
        ALOGE("%s: Camera %d: Can't find ANDROID_JPEG_MAX_SIZE!",
                __FUNCTION__, mCameraId);
        return INVALID_OPERATION;
    }

    if (mCaptureConsumer == 0) {
        // Create CPU buffer queue endpoint
        mCaptureConsumer = new CpuConsumer(1);
        mCaptureConsumer->setFrameAvailableListener(new CaptureWaiter(this));
        mCaptureConsumer->setName(String8("Camera2Client::CaptureConsumer"));
        mCaptureWindow = new SurfaceTextureClient(
            mCaptureConsumer->getProducerInterface());
        // Create memory for API consumption
        mCaptureHeap = new Camera2Heap(maxJpegSize.data.i32[0], 1,
                                       "Camera2Client::CaptureHeap");
        if (mCaptureHeap->mHeap->getSize() == 0) {
            ALOGE("%s: Camera %d: Unable to allocate memory for capture",
                    __FUNCTION__, mCameraId);
            return NO_MEMORY;
        }
    }

    if (mCaptureStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = mDevice->getStreamInfo(mCaptureStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying capture output stream info: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.pictureWidth ||
                currentHeight != (uint32_t)params.pictureHeight) {
            res = mDevice->deleteStream(mCaptureStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for capture: %s (%d)", __FUNCTION__, mCameraId,
                        strerror(-res), res);
                return res;
            }
            mCaptureStreamId = NO_STREAM;
        }
    }

    if (mCaptureStreamId == NO_STREAM) {
        // Create stream for HAL production
        res = mDevice->createStream(mCaptureWindow,
                params.pictureWidth, params.pictureHeight,
                HAL_PIXEL_FORMAT_BLOB, maxJpegSize.data.i32[0],
                &mCaptureStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for capture: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }

    }
    return OK;
}

status_t Camera2Client::updateCaptureRequest(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    if (mCaptureRequest == NULL) {
        res = mDevice->createDefaultRequest(CAMERA2_TEMPLATE_STILL_CAPTURE,
                &mCaptureRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default still image request:"
                    " %s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = updateRequestCommon(mCaptureRequest, params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of capture "
                "request: %s (%d)", __FUNCTION__, mCameraId,
                strerror(-res), res);
        return res;
    }

    res = updateEntry(mCaptureRequest,
            ANDROID_JPEG_THUMBNAIL_SIZE,
            params.jpegThumbSize, 2);
    if (res != OK) return res;
    res = updateEntry(mCaptureRequest,
            ANDROID_JPEG_THUMBNAIL_QUALITY,
            &params.jpegThumbQuality, 1);
    if (res != OK) return res;
    res = updateEntry(mCaptureRequest,
            ANDROID_JPEG_QUALITY,
            &params.jpegQuality, 1);
    if (res != OK) return res;
    res = updateEntry(mCaptureRequest,
            ANDROID_JPEG_ORIENTATION,
            &params.jpegRotation, 1);
    if (res != OK) return res;

    if (params.gpsEnabled) {
        res = updateEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_COORDINATES,
                params.gpsCoordinates, 3);
        if (res != OK) return res;
        res = updateEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_TIMESTAMP,
                &params.gpsTimestamp, 1);
        if (res != OK) return res;
        res = updateEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_PROCESSING_METHOD,
                params.gpsProcessingMethod.string(),
                params.gpsProcessingMethod.size());
        if (res != OK) return res;
    } else {
        res = deleteEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_COORDINATES);
        if (res != OK) return res;
        res = deleteEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_TIMESTAMP);
        if (res != OK) return res;
        res = deleteEntry(mCaptureRequest,
                ANDROID_JPEG_GPS_PROCESSING_METHOD);
        if (res != OK) return res;
    }

    return OK;
}

status_t Camera2Client::updateRecordingRequest(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    if (mRecordingRequest == NULL) {
        res = mDevice->createDefaultRequest(CAMERA2_TEMPLATE_VIDEO_RECORD,
                &mRecordingRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default recording request:"
                    " %s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = updateRequestCommon(mRecordingRequest, params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of recording "
                "request: %s (%d)", __FUNCTION__, mCameraId,
                strerror(-res), res);
        return res;
    }

    return OK;
}

status_t Camera2Client::updateRecordingStream(const Parameters &params) {
    status_t res;

    if (mRecordingConsumer == 0) {
        // Create CPU buffer queue endpoint
        mRecordingConsumer = new MediaConsumer(kRecordingHeapCount);
        mRecordingConsumer->setFrameAvailableListener(new RecordingWaiter(this));
        mRecordingConsumer->setName(String8("Camera2Client::RecordingConsumer"));
        mRecordingWindow = new SurfaceTextureClient(
            mRecordingConsumer->getProducerInterface());
        // Allocate memory later, since we don't know buffer size until receipt
    }

    if (mRecordingStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = mDevice->getStreamInfo(mRecordingStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying recording output stream info: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.videoWidth ||
                currentHeight != (uint32_t)params.videoHeight) {
            // TODO: Should wait to be sure previous recording has finished
            res = mDevice->deleteStream(mRecordingStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for recording: %s (%d)", __FUNCTION__, mCameraId,
                        strerror(-res), res);
                return res;
            }
            mRecordingStreamId = NO_STREAM;
        }
    }

    if (mRecordingStreamId == NO_STREAM) {
        res = mDevice->createStream(mRecordingWindow,
                params.videoWidth, params.videoHeight,
                CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, 0, &mRecordingStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for recording: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    return OK;
}

status_t Camera2Client::updateRequestCommon(camera_metadata_t *request,
        const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    res = updateEntry(request,
            ANDROID_CONTROL_AE_TARGET_FPS_RANGE, params.previewFpsRange, 2);
    if (res != OK) return res;

    uint8_t wbMode = params.autoWhiteBalanceLock ?
            ANDROID_CONTROL_AWB_LOCKED : params.wbMode;
    res = updateEntry(request,
            ANDROID_CONTROL_AWB_MODE, &wbMode, 1);
    if (res != OK) return res;
    res = updateEntry(request,
            ANDROID_CONTROL_EFFECT_MODE, &params.effectMode, 1);
    if (res != OK) return res;
    res = updateEntry(request,
            ANDROID_CONTROL_AE_ANTIBANDING_MODE,
            &params.antibandingMode, 1);
    if (res != OK) return res;

    uint8_t controlMode =
            (params.sceneMode == ANDROID_CONTROL_SCENE_MODE_UNSUPPORTED) ?
            ANDROID_CONTROL_AUTO : ANDROID_CONTROL_USE_SCENE_MODE;
    res = updateEntry(request,
            ANDROID_CONTROL_MODE, &controlMode, 1);
    if (res != OK) return res;
    if (controlMode == ANDROID_CONTROL_USE_SCENE_MODE) {
        res = updateEntry(request,
                ANDROID_CONTROL_SCENE_MODE,
                &params.sceneMode, 1);
        if (res != OK) return res;
    }

    uint8_t flashMode = ANDROID_FLASH_OFF;
    uint8_t aeMode;
    switch (params.flashMode) {
        case Parameters::FLASH_MODE_OFF:
            aeMode = ANDROID_CONTROL_AE_ON; break;
        case Parameters::FLASH_MODE_AUTO:
            aeMode = ANDROID_CONTROL_AE_ON_AUTO_FLASH; break;
        case Parameters::FLASH_MODE_ON:
            aeMode = ANDROID_CONTROL_AE_ON_ALWAYS_FLASH; break;
        case Parameters::FLASH_MODE_TORCH:
            aeMode = ANDROID_CONTROL_AE_ON;
            flashMode = ANDROID_FLASH_TORCH;
            break;
        case Parameters::FLASH_MODE_RED_EYE:
            aeMode = ANDROID_CONTROL_AE_ON_AUTO_FLASH_REDEYE; break;
        default:
            ALOGE("%s: Camera %d: Unknown flash mode %d", __FUNCTION__,
                    mCameraId, params.flashMode);
            return BAD_VALUE;
    }
    if (params.autoExposureLock) aeMode = ANDROID_CONTROL_AE_LOCKED;

    res = updateEntry(request,
            ANDROID_FLASH_MODE, &flashMode, 1);
    if (res != OK) return res;
    res = updateEntry(request,
            ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    if (res != OK) return res;

    float focusDistance = 0; // infinity focus in diopters
    uint8_t focusMode;
    switch (params.focusMode) {
        case Parameters::FOCUS_MODE_AUTO:
        case Parameters::FOCUS_MODE_MACRO:
        case Parameters::FOCUS_MODE_CONTINUOUS_VIDEO:
        case Parameters::FOCUS_MODE_CONTINUOUS_PICTURE:
        case Parameters::FOCUS_MODE_EDOF:
            focusMode = params.focusMode;
            break;
        case Parameters::FOCUS_MODE_INFINITY:
        case Parameters::FOCUS_MODE_FIXED:
            focusMode = ANDROID_CONTROL_AF_OFF;
            break;
        default:
            ALOGE("%s: Camera %d: Unknown focus mode %d", __FUNCTION__,
                    mCameraId, params.focusMode);
            return BAD_VALUE;
    }
    res = updateEntry(request,
            ANDROID_LENS_FOCUS_DISTANCE, &focusDistance, 1);
    if (res != OK) return res;
    res = updateEntry(request,
            ANDROID_CONTROL_AF_MODE, &focusMode, 1);
    if (res != OK) return res;

    size_t focusingAreasSize = params.focusingAreas.size() * 5;
    int32_t *focusingAreas = new int32_t[focusingAreasSize];
    for (size_t i = 0; i < focusingAreasSize; i += 5) {
        focusingAreas[i + 0] = params.focusingAreas[i].left;
        focusingAreas[i + 1] = params.focusingAreas[i].top;
        focusingAreas[i + 2] = params.focusingAreas[i].right;
        focusingAreas[i + 3] = params.focusingAreas[i].bottom;
        focusingAreas[i + 4] = params.focusingAreas[i].weight;
    }
    res = updateEntry(request,
            ANDROID_CONTROL_AF_REGIONS, focusingAreas,focusingAreasSize);
    if (res != OK) return res;
    delete[] focusingAreas;

    res = updateEntry(request,
            ANDROID_CONTROL_AE_EXP_COMPENSATION,
            &params.exposureCompensation, 1);
    if (res != OK) return res;

    size_t meteringAreasSize = params.meteringAreas.size() * 5;
    int32_t *meteringAreas = new int32_t[meteringAreasSize];
    for (size_t i = 0; i < meteringAreasSize; i += 5) {
        meteringAreas[i + 0] = params.meteringAreas[i].left;
        meteringAreas[i + 1] = params.meteringAreas[i].top;
        meteringAreas[i + 2] = params.meteringAreas[i].right;
        meteringAreas[i + 3] = params.meteringAreas[i].bottom;
        meteringAreas[i + 4] = params.meteringAreas[i].weight;
    }
    res = updateEntry(request,
            ANDROID_CONTROL_AE_REGIONS, meteringAreas, meteringAreasSize);
    if (res != OK) return res;

    res = updateEntry(request,
            ANDROID_CONTROL_AWB_REGIONS, meteringAreas, meteringAreasSize);
    if (res != OK) return res;
    delete[] meteringAreas;

    // Need to convert zoom index into a crop rectangle. The rectangle is
    // chosen to maximize its area on the sensor

    camera_metadata_entry_t maxDigitalZoom =
            staticInfo(ANDROID_SCALER_AVAILABLE_MAX_ZOOM);
    float zoomIncrement = (maxDigitalZoom.data.f[0] - 1) /
            (NUM_ZOOM_STEPS-1);
    float zoomRatio = 1 + zoomIncrement * params.zoom;

    camera_metadata_entry_t activePixelArraySize =
            staticInfo(ANDROID_SENSOR_ACTIVE_ARRAY_SIZE, 2, 2);
    int32_t arrayWidth = activePixelArraySize.data.i32[0];
    int32_t arrayHeight = activePixelArraySize.data.i32[1];
    float zoomLeft, zoomTop, zoomWidth, zoomHeight;
    if (params.previewWidth >= params.previewHeight) {
        zoomWidth =  arrayWidth / zoomRatio;
        zoomHeight = zoomWidth *
                params.previewHeight / params.previewWidth;
    } else {
        zoomHeight = arrayHeight / zoomRatio;
        zoomWidth = zoomHeight *
                params.previewWidth / params.previewHeight;
    }
    zoomLeft = (arrayWidth - zoomWidth) / 2;
    zoomTop = (arrayHeight - zoomHeight) / 2;

    int32_t cropRegion[3] = { zoomLeft, zoomTop, zoomWidth };
    res = updateEntry(request,
            ANDROID_SCALER_CROP_REGION, cropRegion, 3);
    if (res != OK) return res;

    // TODO: Decide how to map recordingHint, or whether just to ignore it

    uint8_t vstabMode = params.videoStabilization ?
            ANDROID_CONTROL_VIDEO_STABILIZATION_ON :
            ANDROID_CONTROL_VIDEO_STABILIZATION_OFF;
    res = updateEntry(request,
            ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
            &vstabMode, 1);
    if (res != OK) return res;

    return OK;
}

status_t Camera2Client::updateEntry(camera_metadata_t *buffer,
        uint32_t tag, const void *data, size_t data_count) {
    camera_metadata_entry_t entry;
    status_t res;
    res = find_camera_metadata_entry(buffer, tag, &entry);
    if (res == NAME_NOT_FOUND) {
        res = add_camera_metadata_entry(buffer,
                tag, data, data_count);
    } else if (res == OK) {
        res = update_camera_metadata_entry(buffer,
                entry.index, data, data_count, NULL);
    }

    if (res != OK) {
        ALOGE("%s: Unable to update metadata entry %s.%s (%x): %s (%d)",
                __FUNCTION__, get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
    }
    return res;
}

status_t Camera2Client::deleteEntry(camera_metadata_t *buffer, uint32_t tag) {
    camera_metadata_entry_t entry;
    status_t res;
    res = find_camera_metadata_entry(buffer, tag, &entry);
    if (res == NAME_NOT_FOUND) {
        return OK;
    } else if (res != OK) {
        ALOGE("%s: Error looking for entry %s.%s (%x): %s %d",
                __FUNCTION__,
                get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
        return res;
    }
    res = delete_camera_metadata_entry(buffer, entry.index);
    if (res != OK) {
        ALOGE("%s: Error deleting entry %s.%s (%x): %s %d",
                __FUNCTION__,
                get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
    }
    return res;
}

int Camera2Client::formatStringToEnum(const char *format) {
    return
        !strcmp(format, CameraParameters::PIXEL_FORMAT_YUV422SP) ?
            HAL_PIXEL_FORMAT_YCbCr_422_SP : // NV16
        !strcmp(format, CameraParameters::PIXEL_FORMAT_YUV420SP) ?
            HAL_PIXEL_FORMAT_YCrCb_420_SP : // NV21
        !strcmp(format, CameraParameters::PIXEL_FORMAT_YUV422I) ?
            HAL_PIXEL_FORMAT_YCbCr_422_I :  // YUY2
        !strcmp(format, CameraParameters::PIXEL_FORMAT_YUV420P) ?
            HAL_PIXEL_FORMAT_YV12 :         // YV12
        !strcmp(format, CameraParameters::PIXEL_FORMAT_RGB565) ?
            HAL_PIXEL_FORMAT_RGB_565 :      // RGB565
        !strcmp(format, CameraParameters::PIXEL_FORMAT_RGBA8888) ?
            HAL_PIXEL_FORMAT_RGBA_8888 :    // RGB8888
        !strcmp(format, CameraParameters::PIXEL_FORMAT_BAYER_RGGB) ?
            HAL_PIXEL_FORMAT_RAW_SENSOR :   // Raw sensor data
        -1;
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

int Camera2Client::wbModeStringToEnum(const char *wbMode) {
    return
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_AUTO) ?
            ANDROID_CONTROL_AWB_AUTO :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_INCANDESCENT) ?
            ANDROID_CONTROL_AWB_INCANDESCENT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_FLUORESCENT) ?
            ANDROID_CONTROL_AWB_FLUORESCENT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT) ?
            ANDROID_CONTROL_AWB_WARM_FLUORESCENT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_DAYLIGHT) ?
            ANDROID_CONTROL_AWB_DAYLIGHT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT) ?
            ANDROID_CONTROL_AWB_CLOUDY_DAYLIGHT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_TWILIGHT) ?
            ANDROID_CONTROL_AWB_TWILIGHT :
        !strcmp(wbMode, CameraParameters::WHITE_BALANCE_SHADE) ?
            ANDROID_CONTROL_AWB_SHADE :
        -1;
}

int Camera2Client::effectModeStringToEnum(const char *effectMode) {
    return
        !strcmp(effectMode, CameraParameters::EFFECT_NONE) ?
            ANDROID_CONTROL_EFFECT_OFF :
        !strcmp(effectMode, CameraParameters::EFFECT_MONO) ?
            ANDROID_CONTROL_EFFECT_MONO :
        !strcmp(effectMode, CameraParameters::EFFECT_NEGATIVE) ?
            ANDROID_CONTROL_EFFECT_NEGATIVE :
        !strcmp(effectMode, CameraParameters::EFFECT_SOLARIZE) ?
            ANDROID_CONTROL_EFFECT_SOLARIZE :
        !strcmp(effectMode, CameraParameters::EFFECT_SEPIA) ?
            ANDROID_CONTROL_EFFECT_SEPIA :
        !strcmp(effectMode, CameraParameters::EFFECT_POSTERIZE) ?
            ANDROID_CONTROL_EFFECT_POSTERIZE :
        !strcmp(effectMode, CameraParameters::EFFECT_WHITEBOARD) ?
            ANDROID_CONTROL_EFFECT_WHITEBOARD :
        !strcmp(effectMode, CameraParameters::EFFECT_BLACKBOARD) ?
            ANDROID_CONTROL_EFFECT_BLACKBOARD :
        !strcmp(effectMode, CameraParameters::EFFECT_AQUA) ?
            ANDROID_CONTROL_EFFECT_AQUA :
        -1;
}

int Camera2Client::abModeStringToEnum(const char *abMode) {
    return
        !strcmp(abMode, CameraParameters::ANTIBANDING_AUTO) ?
            ANDROID_CONTROL_AE_ANTIBANDING_AUTO :
        !strcmp(abMode, CameraParameters::ANTIBANDING_OFF) ?
            ANDROID_CONTROL_AE_ANTIBANDING_OFF :
        !strcmp(abMode, CameraParameters::ANTIBANDING_50HZ) ?
            ANDROID_CONTROL_AE_ANTIBANDING_50HZ :
        !strcmp(abMode, CameraParameters::ANTIBANDING_60HZ) ?
            ANDROID_CONTROL_AE_ANTIBANDING_60HZ :
        -1;
}

int Camera2Client::sceneModeStringToEnum(const char *sceneMode) {
    return
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_AUTO) ?
            ANDROID_CONTROL_SCENE_MODE_UNSUPPORTED :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_ACTION) ?
            ANDROID_CONTROL_SCENE_MODE_ACTION :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_PORTRAIT) ?
            ANDROID_CONTROL_SCENE_MODE_PORTRAIT :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_LANDSCAPE) ?
            ANDROID_CONTROL_SCENE_MODE_LANDSCAPE :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_NIGHT) ?
            ANDROID_CONTROL_SCENE_MODE_NIGHT :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_NIGHT_PORTRAIT) ?
            ANDROID_CONTROL_SCENE_MODE_NIGHT_PORTRAIT :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_THEATRE) ?
            ANDROID_CONTROL_SCENE_MODE_THEATRE :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_BEACH) ?
            ANDROID_CONTROL_SCENE_MODE_BEACH :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_SNOW) ?
            ANDROID_CONTROL_SCENE_MODE_SNOW :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_SUNSET) ?
            ANDROID_CONTROL_SCENE_MODE_SUNSET :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_STEADYPHOTO) ?
            ANDROID_CONTROL_SCENE_MODE_STEADYPHOTO :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_FIREWORKS) ?
            ANDROID_CONTROL_SCENE_MODE_FIREWORKS :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_SPORTS) ?
            ANDROID_CONTROL_SCENE_MODE_SPORTS :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_PARTY) ?
            ANDROID_CONTROL_SCENE_MODE_PARTY :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_CANDLELIGHT) ?
            ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT :
        !strcmp(sceneMode, CameraParameters::SCENE_MODE_BARCODE) ?
            ANDROID_CONTROL_SCENE_MODE_BARCODE:
        -1;
}

Camera2Client::Parameters::flashMode_t Camera2Client::flashModeStringToEnum(
        const char *flashMode) {
    return
        !strcmp(flashMode, CameraParameters::FLASH_MODE_OFF) ?
            Parameters::FLASH_MODE_OFF :
        !strcmp(flashMode, CameraParameters::FLASH_MODE_AUTO) ?
            Parameters::FLASH_MODE_AUTO :
        !strcmp(flashMode, CameraParameters::FLASH_MODE_ON) ?
            Parameters::FLASH_MODE_ON :
        !strcmp(flashMode, CameraParameters::FLASH_MODE_RED_EYE) ?
            Parameters::FLASH_MODE_RED_EYE :
        !strcmp(flashMode, CameraParameters::FLASH_MODE_TORCH) ?
            Parameters::FLASH_MODE_TORCH :
        Parameters::FLASH_MODE_INVALID;
}

Camera2Client::Parameters::focusMode_t Camera2Client::focusModeStringToEnum(
        const char *focusMode) {
    return
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_AUTO) ?
            Parameters::FOCUS_MODE_AUTO :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_INFINITY) ?
            Parameters::FOCUS_MODE_INFINITY :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_MACRO) ?
            Parameters::FOCUS_MODE_MACRO :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_FIXED) ?
            Parameters::FOCUS_MODE_FIXED :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_EDOF) ?
            Parameters::FOCUS_MODE_EDOF :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO) ?
            Parameters::FOCUS_MODE_CONTINUOUS_VIDEO :
        !strcmp(focusMode, CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE) ?
            Parameters::FOCUS_MODE_CONTINUOUS_PICTURE :
        Parameters::FOCUS_MODE_INVALID;
}

status_t Camera2Client::parseAreas(const char *areasCStr,
        Vector<Parameters::Area> *areas) {
    static const size_t NUM_FIELDS = 5;
    areas->clear();
    if (areasCStr == NULL) {
        // If no key exists, use default (0,0,0,0,0)
        areas->push();
        return OK;
    }
    String8 areasStr(areasCStr);
    ssize_t areaStart = areasStr.find("(", 0) + 1;
    while (areaStart != 0) {
        const char* area = areasStr.string() + areaStart;
        char *numEnd;
        int vals[NUM_FIELDS];
        for (size_t i = 0; i < NUM_FIELDS; i++) {
            errno = 0;
            vals[i] = strtol(area, &numEnd, 10);
            if (errno || numEnd == area) return BAD_VALUE;
            area = numEnd + 1;
        }
        areas->push(Parameters::Area(
            vals[0], vals[1], vals[2], vals[3], vals[4]) );
        areaStart = areasStr.find("(", areaStart) + 1;
    }
    return OK;
}

status_t Camera2Client::validateAreas(const Vector<Parameters::Area> &areas,
                                      size_t maxRegions) {
    // Definition of valid area can be found in
    // include/camera/CameraParameters.h
    if (areas.size() == 0) return BAD_VALUE;
    if (areas.size() == 1) {
        if (areas[0].left == 0 &&
                areas[0].top == 0 &&
                areas[0].right == 0 &&
                areas[0].bottom == 0 &&
                areas[0].weight == 0) {
            // Single (0,0,0,0,0) entry is always valid (== driver decides)
            return OK;
        }
    }
    if (areas.size() > maxRegions) {
        ALOGE("%s: Too many areas requested: %d",
                __FUNCTION__, areas.size());
        return BAD_VALUE;
    }

    for (Vector<Parameters::Area>::const_iterator a = areas.begin();
         a != areas.end(); a++) {
        if (a->weight < 1 || a->weight > 1000) return BAD_VALUE;
        if (a->left < -1000 || a->left > 1000) return BAD_VALUE;
        if (a->top < -1000 || a->top > 1000) return BAD_VALUE;
        if (a->right < -1000 || a->right > 1000) return BAD_VALUE;
        if (a->bottom < -1000 || a->bottom > 1000) return BAD_VALUE;
        if (a->left >= a->right) return BAD_VALUE;
        if (a->top >= a->bottom) return BAD_VALUE;
    }
    return OK;
}

bool Camera2Client::boolFromString(const char *boolStr) {
    return !boolStr ? false :
        !strcmp(boolStr, CameraParameters::TRUE) ? true :
        false;
}

int Camera2Client::degToTransform(int degrees, bool mirror) {
    if (!mirror) {
        if (degrees == 0) return 0;
        else if (degrees == 90) return HAL_TRANSFORM_ROT_90;
        else if (degrees == 180) return HAL_TRANSFORM_ROT_180;
        else if (degrees == 270) return HAL_TRANSFORM_ROT_270;
    } else {  // Do mirror (horizontal flip)
        if (degrees == 0) {           // FLIP_H and ROT_0
            return HAL_TRANSFORM_FLIP_H;
        } else if (degrees == 90) {   // FLIP_H and ROT_90
            return HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_ROT_90;
        } else if (degrees == 180) {  // FLIP_H and ROT_180
            return HAL_TRANSFORM_FLIP_V;
        } else if (degrees == 270) {  // FLIP_H and ROT_270
            return HAL_TRANSFORM_FLIP_V | HAL_TRANSFORM_ROT_90;
        }
    }
    ALOGE("%s: Bad input: %d", __FUNCTION__, degrees);
    return -1;
}

} // namespace android
