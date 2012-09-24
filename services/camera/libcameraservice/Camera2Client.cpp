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

#define LOG_TAG "Camera2"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <gui/Surface.h>
#include <media/hardware/MetadataBufferType.h>
#include "camera2/Parameters.h"
#include "Camera2Client.h"

#define ALOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define ALOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

namespace android {
using namespace camera2;

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
        mSharedCameraClient(cameraClient),
        mParameters(cameraId, cameraFacing),
        mPreviewStreamId(NO_STREAM),
        mRecordingStreamId(NO_STREAM),
        mRecordingHeapCount(kDefaultRecordingHeapCount)
{
    ATRACE_CALL();
    ALOGI("Camera %d: Opened", cameraId);

    mDevice = new Camera2Device(cameraId);

    SharedParameters::Lock l(mParameters);
    l.mParameters.state = Parameters::DISCONNECTED;
}

status_t Camera2Client::checkPid(const char* checkLocation) const {
    int callingPid = getCallingPid();
    if (callingPid == mClientPid) return NO_ERROR;

    ALOGE("%s: attempt to use a locked camera from a different process"
            " (old pid %d, new pid %d)", checkLocation, mClientPid, callingPid);
    return PERMISSION_DENIED;
}

status_t Camera2Client::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    ALOGV("%s: Initializing client for camera %d", __FUNCTION__, mCameraId);
    status_t res;

    res = mDevice->initialize(module);
    if (res != OK) {
        ALOGE("%s: Camera %d: unable to initialize device: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return NO_INIT;
    }

    res = mDevice->setNotifyCallback(this);

    SharedParameters::Lock l(mParameters);

    res = l.mParameters.initialize(&(mDevice->info()));
    if (res != OK) {
        ALOGE("%s: Camera %d: unable to build defaults: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return NO_INIT;
    }

    String8 threadName;

    mFrameProcessor = new FrameProcessor(this);
    threadName = String8::format("C2-%d-FrameProc",
            mCameraId);
    mFrameProcessor->run(threadName.string());

    mCaptureSequencer = new CaptureSequencer(this);
    threadName = String8::format("C2-%d-CaptureSeq",
            mCameraId);
    mCaptureSequencer->run(threadName.string());

    mJpegProcessor = new JpegProcessor(this, mCaptureSequencer);
    threadName = String8::format("C2-%d-JpegProc",
            mCameraId);
    mJpegProcessor->run(threadName.string());

    mZslProcessor = new ZslProcessor(this, mCaptureSequencer);
    threadName = String8::format("C2-%d-ZslProc",
            mCameraId);
    mZslProcessor->run(threadName.string());

    mCallbackProcessor = new CallbackProcessor(this);
    threadName = String8::format("C2-%d-CallbkProc",
            mCameraId);
    mCallbackProcessor->run(threadName.string());

    if (gLogLevel >= 1) {
        ALOGD("%s: Default parameters converted from camera %d:", __FUNCTION__,
              mCameraId);
        ALOGD("%s", l.mParameters.paramsFlattened.string());
    }

    return OK;
}

Camera2Client::~Camera2Client() {
    ATRACE_CALL();

    mDestructionStarted = true;

    // Rewrite mClientPid to allow shutdown by CameraService
    mClientPid = getCallingPid();
    disconnect();

    ALOGI("Camera %d: Closed", mCameraId);
}

status_t Camera2Client::dump(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("Client2[%d] (%p) PID: %d, dump:\n",
            mCameraId,
            getCameraClient()->asBinder().get(),
            mClientPid);
    result.append("  State: ");
#define CASE_APPEND_ENUM(x) case x: result.append(#x "\n"); break;

    const Parameters& p = mParameters.unsafeAccess();

    result.append(Parameters::getStateName(p.state));

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
    result.appendFormat("    Preview stream ID: %d\n",
            getPreviewStreamId());
    result.appendFormat("    Capture stream ID: %d\n",
            getCaptureStreamId());
    result.appendFormat("    Recording stream ID: %d\n",
            getRecordingStreamId());

    result.append("  Current requests:\n");
    if (mPreviewRequest.entryCount() != 0) {
        result.append("    Preview request:\n");
        write(fd, result.string(), result.size());
        mPreviewRequest.dump(fd, 2, 6);
    } else {
        result.append("    Preview request: undefined\n");
        write(fd, result.string(), result.size());
    }

    if (mRecordingRequest.entryCount() != 0) {
        result = "    Recording request:\n";
        write(fd, result.string(), result.size());
        mRecordingRequest.dump(fd, 2, 6);
    } else {
        result = "    Recording request: undefined\n";
        write(fd, result.string(), result.size());
    }

    mCaptureSequencer->dump(fd, args);

    mFrameProcessor->dump(fd, args);

    mZslProcessor->dump(fd, args);

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

// ICamera interface

void Camera2Client::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return;

    if (mDevice == 0) return;

    ALOGV("Camera %d: Shutting down", mCameraId);

    stopPreviewL();

    {
        SharedParameters::Lock l(mParameters);
        l.mParameters.state = Parameters::DISCONNECTED;
    }

    if (mPreviewStreamId != NO_STREAM) {
        mDevice->deleteStream(mPreviewStreamId);
        mPreviewStreamId = NO_STREAM;
    }

    mJpegProcessor->deleteStream();

    if (mRecordingStreamId != NO_STREAM) {
        mDevice->deleteStream(mRecordingStreamId);
        mRecordingStreamId = NO_STREAM;
    }

    mCallbackProcessor->deleteStream();

    mZslProcessor->deleteStream();

    mFrameProcessor->requestExit();
    mCaptureSequencer->requestExit();
    mJpegProcessor->requestExit();
    mZslProcessor->requestExit();
    mCallbackProcessor->requestExit();

    ALOGV("Camera %d: Waiting for threads", mCameraId);

    mFrameProcessor->join();
    mCaptureSequencer->join();
    mJpegProcessor->join();
    mZslProcessor->join();
    mCallbackProcessor->join();

    ALOGV("Camera %d: Disconnecting device", mCameraId);

    mDevice->disconnect();

    mDevice.clear();

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
    mSharedCameraClient = client;

    SharedParameters::Lock l(mParameters);
    l.mParameters.state = Parameters::STOPPED;

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
        mSharedCameraClient.clear();
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
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surface != 0) {
        binder = surface->asBinder();
        window = surface;
    }

    return setPreviewWindowL(binder,window);
}

status_t Camera2Client::setPreviewTexture(
        const sp<ISurfaceTexture>& surfaceTexture) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (surfaceTexture != 0) {
        binder = surfaceTexture->asBinder();
        window = new SurfaceTextureClient(surfaceTexture);
    }
    return setPreviewWindowL(binder, window);
}

status_t Camera2Client::setPreviewWindowL(const sp<IBinder>& binder,
        sp<ANativeWindow> window) {
    ATRACE_CALL();
    status_t res;

    if (binder == mPreviewSurface) {
        ALOGV("%s: Camera %d: New window is same as old window",
                __FUNCTION__, mCameraId);
        return NO_ERROR;
    }

    SharedParameters::Lock l(mParameters);
    switch (l.mParameters.state) {
        case Parameters::DISCONNECTED:
        case Parameters::RECORD:
        case Parameters::STILL_CAPTURE:
        case Parameters::VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Cannot set preview display while in state %s",
                    __FUNCTION__, mCameraId,
                    Parameters::getStateName(l.mParameters.state));
            return INVALID_OPERATION;
        case Parameters::STOPPED:
        case Parameters::WAITING_FOR_PREVIEW_WINDOW:
            // OK
            break;
        case Parameters::PREVIEW:
            // Already running preview - need to stop and create a new stream
            // TODO: Optimize this so that we don't wait for old stream to drain
            // before spinning up new stream
            mDevice->clearStreamingRequest();
            l.mParameters.state = Parameters::WAITING_FOR_PREVIEW_WINDOW;
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

    if (l.mParameters.state == Parameters::WAITING_FOR_PREVIEW_WINDOW) {
        return startPreviewL(l.mParameters, false);
    }

    return OK;
}

void Camera2Client::setPreviewCallbackFlag(int flag) {
    ATRACE_CALL();
    ALOGV("%s: Camera %d: Flag 0x%x", __FUNCTION__, mCameraId, flag);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( checkPid(__FUNCTION__) != OK) return;

    SharedParameters::Lock l(mParameters);
    setPreviewCallbackFlagL(l.mParameters, flag);
}

void Camera2Client::setPreviewCallbackFlagL(Parameters &params, int flag) {
    status_t res = OK;
    if (flag & CAMERA_FRAME_CALLBACK_FLAG_ONE_SHOT_MASK) {
        ALOGV("%s: setting oneshot", __FUNCTION__);
        params.previewCallbackOneShot = true;
    }
    if (params.previewCallbackFlags != (uint32_t)flag) {
        params.previewCallbackFlags = flag;
        switch(params.state) {
        case Parameters::PREVIEW:
            res = startPreviewL(params, true);
            break;
        case Parameters::RECORD:
        case Parameters::VIDEO_SNAPSHOT:
            res = startRecordingL(params, true);
            break;
        default:
            break;
        }
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to refresh request in state %s",
                    __FUNCTION__, mCameraId,
                    Parameters::getStateName(params.state));
        }
    }

}

status_t Camera2Client::startPreview() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;
    SharedParameters::Lock l(mParameters);
    return startPreviewL(l.mParameters, false);
}

status_t Camera2Client::startPreviewL(Parameters &params, bool restart) {
    ATRACE_CALL();
    status_t res;
    if (params.state >= Parameters::PREVIEW && !restart) {
        ALOGE("%s: Can't start preview in state %s",
                __FUNCTION__,
                Parameters::getStateName(params.state));
        return INVALID_OPERATION;
    }

    if (mPreviewWindow == 0) {
        params.state = Parameters::WAITING_FOR_PREVIEW_WINDOW;
        return OK;
    }
    params.state = Parameters::STOPPED;

    res = updatePreviewStream(params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update preview stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    bool callbacksEnabled = params.previewCallbackFlags &
        CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK;
    if (callbacksEnabled) {
        res = mCallbackProcessor->updateStream(params);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to update callback stream: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }
    if (params.zslMode && !params.recordingHint) {
        res = mZslProcessor->updateStream(params);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to update ZSL stream: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    CameraMetadata *request;
    if (!params.recordingHint) {
        if (mPreviewRequest.entryCount() == 0) {
            res = updatePreviewRequest(params);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to create preview request: %s (%d)",
                        __FUNCTION__, mCameraId, strerror(-res), res);
                return res;
            }
        }
        request = &mPreviewRequest;
    } else {
        // With recording hint set, we're going to be operating under the
        // assumption that the user will record video. To optimize recording
        // startup time, create the necessary output streams for recording and
        // video snapshot now if they don't already exist.
        if (mRecordingRequest.entryCount() == 0) {
            res = updateRecordingRequest(params);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to create recording preview "
                        "request: %s (%d)",
                        __FUNCTION__, mCameraId, strerror(-res), res);
                return res;
            }
        }
        request = &mRecordingRequest;

        // TODO: Re-enable recording stream creation/update here once issues are
        // resolved

        res = mJpegProcessor->updateStream(params);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't pre-configure still image "
                    "stream: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    Vector<uint8_t> outputStreams;
    outputStreams.push(getPreviewStreamId());

    if (callbacksEnabled) {
        outputStreams.push(getCallbackStreamId());
    }
    if (params.zslMode && !params.recordingHint) {
        outputStreams.push(getZslStreamId());
    }

    res = request->update(
        ANDROID_REQUEST_OUTPUT_STREAMS,
        outputStreams);

    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = request->sort();
    if (res != OK) {
        ALOGE("%s: Camera %d: Error sorting preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    res = mDevice->setStreamingRequest(*request);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview request to start preview: "
                "%s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    params.state = Parameters::PREVIEW;

    return OK;
}

void Camera2Client::stopPreview() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return;
    stopPreviewL();
}

void Camera2Client::stopPreviewL() {
    ATRACE_CALL();
    Parameters::State state;
    {
        SharedParameters::Lock l(mParameters);
        state = l.mParameters.state;
    }

    switch (state) {
        case Parameters::DISCONNECTED:
            ALOGE("%s: Camera %d: Call before initialized",
                    __FUNCTION__, mCameraId);
            break;
        case Parameters::STOPPED:
            break;
        case Parameters::STILL_CAPTURE:
            ALOGE("%s: Camera %d: Cannot stop preview during still capture.",
                    __FUNCTION__, mCameraId);
            break;
        case Parameters::RECORD:
            // no break - identical to preview
        case Parameters::PREVIEW:
            mDevice->clearStreamingRequest();
            mDevice->waitUntilDrained();
            // no break
        case Parameters::WAITING_FOR_PREVIEW_WINDOW: {
            SharedParameters::Lock l(mParameters);
            l.mParameters.state = Parameters::STOPPED;
            commandStopFaceDetectionL(l.mParameters);
            break;
        }
        default:
            ALOGE("%s: Camera %d: Unknown state %d", __FUNCTION__, mCameraId,
                    state);
    }
}

bool Camera2Client::previewEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return false;

    SharedParameters::Lock l(mParameters);
    return l.mParameters.state == Parameters::PREVIEW;
}

status_t Camera2Client::storeMetaDataInBuffers(bool enabled) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    SharedParameters::Lock l(mParameters);
    switch (l.mParameters.state) {
        case Parameters::RECORD:
        case Parameters::VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Can't be called in state %s",
                    __FUNCTION__, mCameraId,
                    Parameters::getStateName(l.mParameters.state));
            return INVALID_OPERATION;
        default:
            // OK
            break;
    }

    l.mParameters.storeMetadataInBuffers = enabled;

    return OK;
}

status_t Camera2Client::startRecording() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;
    SharedParameters::Lock l(mParameters);

    return startRecordingL(l.mParameters, false);
}

status_t Camera2Client::startRecordingL(Parameters &params, bool restart) {
    status_t res;
    switch (params.state) {
        case Parameters::STOPPED:
            res = startPreviewL(params, false);
            if (res != OK) return res;
            break;
        case Parameters::PREVIEW:
            // Ready to go
            break;
        case Parameters::RECORD:
        case Parameters::VIDEO_SNAPSHOT:
            // OK to call this when recording is already on, just skip unless
            // we're looking to restart
            if (!restart) return OK;
            break;
        default:
            ALOGE("%s: Camera %d: Can't start recording in state %s",
                    __FUNCTION__, mCameraId,
                    Parameters::getStateName(params.state));
            return INVALID_OPERATION;
    };

    if (!params.storeMetadataInBuffers) {
        ALOGE("%s: Camera %d: Recording only supported in metadata mode, but "
                "non-metadata recording mode requested!", __FUNCTION__,
                mCameraId);
        return INVALID_OPERATION;
    }

    mCameraService->playSound(CameraService::SOUND_RECORDING);

    res = updateRecordingStream(params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update recording stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    bool callbacksEnabled = params.previewCallbackFlags &
        CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK;
    if (callbacksEnabled) {
        res = mCallbackProcessor->updateStream(params);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to update callback stream: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    if (mRecordingRequest.entryCount() == 0) {
        res = updateRecordingRequest(params);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create recording request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    if (callbacksEnabled) {
        uint8_t outputStreams[3] ={
            getPreviewStreamId(),
            getRecordingStreamId(),
            getCallbackStreamId()
        };
        res = mRecordingRequest.update(
                ANDROID_REQUEST_OUTPUT_STREAMS,
                outputStreams, 3);
    } else {
        uint8_t outputStreams[2] = {
            getPreviewStreamId(),
            getRecordingStreamId()
        };
        res = mRecordingRequest.update(
                ANDROID_REQUEST_OUTPUT_STREAMS,
                outputStreams, 2);
    }
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up recording request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = mRecordingRequest.sort();
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
    if (params.state < Parameters::RECORD) {
        params.state = Parameters::RECORD;
    }

    return OK;
}

void Camera2Client::stopRecording() {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    SharedParameters::Lock l(mParameters);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return;

    switch (l.mParameters.state) {
        case Parameters::RECORD:
            // OK to stop
            break;
        case Parameters::STOPPED:
        case Parameters::PREVIEW:
        case Parameters::STILL_CAPTURE:
        case Parameters::VIDEO_SNAPSHOT:
        default:
            ALOGE("%s: Camera %d: Can't stop recording in state %s",
                    __FUNCTION__, mCameraId,
                    Parameters::getStateName(l.mParameters.state));
            return;
    };

    mCameraService->playSound(CameraService::SOUND_RECORDING);

    res = startPreviewL(l.mParameters, true);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to return to preview",
                __FUNCTION__, mCameraId);
    }
}

bool Camera2Client::recordingEnabled() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);

    if ( checkPid(__FUNCTION__) != OK) return false;

    return recordingEnabledL();
}

bool Camera2Client::recordingEnabledL() {
    ATRACE_CALL();
    SharedParameters::Lock l(mParameters);

    return (l.mParameters.state == Parameters::RECORD
            || l.mParameters.state == Parameters::VIDEO_SNAPSHOT);
}

void Camera2Client::releaseRecordingFrame(const sp<IMemory>& mem) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( checkPid(__FUNCTION__) != OK) return;

    SharedParameters::Lock l(mParameters);

    // Make sure this is for the current heap
    ssize_t offset;
    size_t size;
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

    // Release the buffer back to the recording queue

    buffer_handle_t imgHandle = *(buffer_handle_t*)(data + 4);

    size_t itemIndex;
    for (itemIndex = 0; itemIndex < mRecordingBuffers.size(); itemIndex++) {
        const BufferItemConsumer::BufferItem item = mRecordingBuffers[itemIndex];
        if (item.mBuf != BufferItemConsumer::INVALID_BUFFER_SLOT &&
                item.mGraphicBuffer->handle == imgHandle) {
            break;
        }
    }
    if (itemIndex == mRecordingBuffers.size()) {
        ALOGE("%s: Camera %d: Can't find buffer_handle_t %p in list of "
                "outstanding buffers", __FUNCTION__, mCameraId, imgHandle);
        return;
    }

    ALOGV("%s: Camera %d: Freeing buffer_handle_t %p", __FUNCTION__, mCameraId,
            imgHandle);

    res = mRecordingConsumer->releaseBuffer(mRecordingBuffers[itemIndex]);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to free recording frame (buffer_handle_t: %p):"
                "%s (%d)",
                __FUNCTION__, mCameraId, imgHandle, strerror(-res), res);
        return;
    }
    mRecordingBuffers.replaceAt(itemIndex);

    mRecordingHeapFree++;
}

status_t Camera2Client::autoFocus() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    int triggerId;
    {
        SharedParameters::Lock l(mParameters);
        l.mParameters.currentAfTriggerId = ++l.mParameters.afTriggerCounter;
        triggerId = l.mParameters.currentAfTriggerId;
    }

    mDevice->triggerAutofocus(triggerId);

    return OK;
}

status_t Camera2Client::cancelAutoFocus() {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    int triggerId;
    {
        SharedParameters::Lock l(mParameters);
        triggerId = ++l.mParameters.afTriggerCounter;
    }

    mDevice->triggerCancelAutofocus(triggerId);

    return OK;
}

status_t Camera2Client::takePicture(int msgType) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    SharedParameters::Lock l(mParameters);
    switch (l.mParameters.state) {
        case Parameters::DISCONNECTED:
        case Parameters::STOPPED:
        case Parameters::WAITING_FOR_PREVIEW_WINDOW:
            ALOGE("%s: Camera %d: Cannot take picture without preview enabled",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
        case Parameters::PREVIEW:
            // Good to go for takePicture
            res = commandStopFaceDetectionL(l.mParameters);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to stop face detection for still capture",
                        __FUNCTION__, mCameraId);
                return res;
            }
            l.mParameters.state = Parameters::STILL_CAPTURE;
            break;
        case Parameters::RECORD:
            // Good to go for video snapshot
            l.mParameters.state = Parameters::VIDEO_SNAPSHOT;
            break;
        case Parameters::STILL_CAPTURE:
        case Parameters::VIDEO_SNAPSHOT:
            ALOGE("%s: Camera %d: Already taking a picture",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
    }

    ALOGV("%s: Camera %d: Starting picture capture", __FUNCTION__, mCameraId);

    res = mJpegProcessor->updateStream(l.mParameters);
    if (res != OK) {
        ALOGE("%s: Camera %d: Can't set up still image stream: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    res = mCaptureSequencer->startCapture();
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to start capture: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
    }

    return res;
}

status_t Camera2Client::setParameters(const String8& params) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    SharedParameters::Lock l(mParameters);

    res = l.mParameters.set(params);
    if (res != OK) return res;

    res = updateRequests(l.mParameters);

    return res;
}

String8 Camera2Client::getParameters() const {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    if ( checkPid(__FUNCTION__) != OK) return String8();

    SharedParameters::ReadLock l(mParameters);

    // TODO: Deal with focus distances
    return l.mParameters.paramsFlattened;
}

status_t Camera2Client::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) {
    ATRACE_CALL();
    Mutex::Autolock icl(mICameraLock);
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    ALOGV("%s: Camera %d: Command %d (%d, %d)", __FUNCTION__, mCameraId,
            cmd, arg1, arg2);

    switch (cmd) {
        case CAMERA_CMD_START_SMOOTH_ZOOM:
            return commandStartSmoothZoomL();
        case CAMERA_CMD_STOP_SMOOTH_ZOOM:
            return commandStopSmoothZoomL();
        case CAMERA_CMD_SET_DISPLAY_ORIENTATION:
            return commandSetDisplayOrientationL(arg1);
        case CAMERA_CMD_ENABLE_SHUTTER_SOUND:
            return commandEnableShutterSoundL(arg1 == 1);
        case CAMERA_CMD_PLAY_RECORDING_SOUND:
            return commandPlayRecordingSoundL();
        case CAMERA_CMD_START_FACE_DETECTION:
            return commandStartFaceDetectionL(arg1);
        case CAMERA_CMD_STOP_FACE_DETECTION: {
            SharedParameters::Lock l(mParameters);
            return commandStopFaceDetectionL(l.mParameters);
        }
        case CAMERA_CMD_ENABLE_FOCUS_MOVE_MSG:
            return commandEnableFocusMoveMsgL(arg1 == 1);
        case CAMERA_CMD_PING:
            return commandPingL();
        case CAMERA_CMD_SET_VIDEO_BUFFER_COUNT:
            return commandSetVideoBufferCountL(arg1);
        default:
            ALOGE("%s: Unknown command %d (arguments %d, %d)",
                    __FUNCTION__, cmd, arg1, arg2);
            return BAD_VALUE;
    }
}

status_t Camera2Client::commandStartSmoothZoomL() {
    ALOGE("%s: Unimplemented!", __FUNCTION__);
    return OK;
}

status_t Camera2Client::commandStopSmoothZoomL() {
    ALOGE("%s: Unimplemented!", __FUNCTION__);
    return OK;
}

status_t Camera2Client::commandSetDisplayOrientationL(int degrees) {
    int transform = Parameters::degToTransform(degrees,
            mCameraFacing == CAMERA_FACING_FRONT);
    if (transform == -1) {
        ALOGE("%s: Camera %d: Error setting %d as display orientation value",
                __FUNCTION__, mCameraId, degrees);
        return BAD_VALUE;
    }
    SharedParameters::Lock l(mParameters);
    if (transform != l.mParameters.previewTransform &&
            mPreviewStreamId != NO_STREAM) {
        mDevice->setStreamTransform(mPreviewStreamId, transform);
    }
    l.mParameters.previewTransform = transform;
    return OK;
}

status_t Camera2Client::commandEnableShutterSoundL(bool enable) {
    SharedParameters::Lock l(mParameters);
    if (enable) {
        l.mParameters.playShutterSound = true;
        return OK;
    }

    // Disabling shutter sound may not be allowed. In that case only
    // allow the mediaserver process to disable the sound.
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.camera.sound.forced", value, "0");
    if (strncmp(value, "0", 2) != 0) {
        // Disabling shutter sound is not allowed. Deny if the current
        // process is not mediaserver.
        if (getCallingPid() != getpid()) {
            ALOGE("Failed to disable shutter sound. Permission denied (pid %d)",
                    getCallingPid());
            return PERMISSION_DENIED;
        }
    }

    l.mParameters.playShutterSound = false;
    return OK;
}

status_t Camera2Client::commandPlayRecordingSoundL() {
    mCameraService->playSound(CameraService::SOUND_RECORDING);
    return OK;
}

status_t Camera2Client::commandStartFaceDetectionL(int type) {
    ALOGV("%s: Camera %d: Starting face detection",
          __FUNCTION__, mCameraId);
    status_t res;
    SharedParameters::Lock l(mParameters);
    switch (l.mParameters.state) {
        case Parameters::DISCONNECTED:
        case Parameters::STOPPED:
        case Parameters::WAITING_FOR_PREVIEW_WINDOW:
        case Parameters::STILL_CAPTURE:
            ALOGE("%s: Camera %d: Cannot start face detection without preview active",
                    __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
        case Parameters::PREVIEW:
        case Parameters::RECORD:
        case Parameters::VIDEO_SNAPSHOT:
            // Good to go for starting face detect
            break;
    }
    // Ignoring type
    if (l.mParameters.fastInfo.bestFaceDetectMode ==
            ANDROID_STATS_FACE_DETECTION_OFF) {
        ALOGE("%s: Camera %d: Face detection not supported",
                __FUNCTION__, mCameraId);
        return INVALID_OPERATION;
    }
    if (l.mParameters.enableFaceDetect) return OK;

    l.mParameters.enableFaceDetect = true;

    res = updateRequests(l.mParameters);

    return res;
}

status_t Camera2Client::commandStopFaceDetectionL(Parameters &params) {
    status_t res = OK;
    ALOGV("%s: Camera %d: Stopping face detection",
          __FUNCTION__, mCameraId);

    if (!params.enableFaceDetect) return OK;

    params.enableFaceDetect = false;

    if (params.state == Parameters::PREVIEW
            || params.state == Parameters::RECORD
            || params.state == Parameters::VIDEO_SNAPSHOT) {
        res = updateRequests(params);
    }

    return res;
}

status_t Camera2Client::commandEnableFocusMoveMsgL(bool enable) {
    SharedParameters::Lock l(mParameters);
    l.mParameters.enableFocusMoveMessages = enable;

    return OK;
}

status_t Camera2Client::commandPingL() {
    // Always ping back if access is proper and device is alive
    SharedParameters::Lock l(mParameters);
    if (l.mParameters.state != Parameters::DISCONNECTED) {
        return OK;
    } else {
        return NO_INIT;
    }
}

status_t Camera2Client::commandSetVideoBufferCountL(size_t count) {
    if (recordingEnabledL()) {
        ALOGE("%s: Camera %d: Error setting video buffer count after "
                "recording was started", __FUNCTION__, mCameraId);
        return INVALID_OPERATION;
    }

    // 32 is the current upper limit on the video buffer count for BufferQueue
    if (count > 32) {
        ALOGE("%s: Camera %d: Error setting %d as video buffer count value",
                __FUNCTION__, mCameraId, count);
        return BAD_VALUE;
    }

    // Need to reallocate memory for heap
    if (mRecordingHeapCount != count) {
        if  (mRecordingHeap != 0) {
            mRecordingHeap.clear();
            mRecordingHeap = NULL;
        }
        mRecordingHeapCount = count;
    }

    return OK;
}

/** Device-related methods */

void Camera2Client::notifyError(int errorCode, int arg1, int arg2) {
    ALOGE("Error condition %d reported by HAL, arguments %d, %d", errorCode, arg1, arg2);
}

void Camera2Client::notifyShutter(int frameNumber, nsecs_t timestamp) {
    ALOGV("%s: Shutter notification for frame %d at time %lld", __FUNCTION__,
            frameNumber, timestamp);
}

void Camera2Client::notifyAutoFocus(uint8_t newState, int triggerId) {
    ALOGV("%s: Autofocus state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
    bool sendCompletedMessage = false;
    bool sendMovingMessage = false;

    bool success = false;
    bool afInMotion = false;
    {
        SharedParameters::Lock l(mParameters);
        switch (l.mParameters.focusMode) {
            case Parameters::FOCUS_MODE_AUTO:
            case Parameters::FOCUS_MODE_MACRO:
                // Don't send notifications upstream if they're not for the current AF
                // trigger. For example, if cancel was called in between, or if we
                // already sent a notification about this AF call.
                if (triggerId != l.mParameters.currentAfTriggerId) break;
                switch (newState) {
                    case ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED:
                        success = true;
                        // no break
                    case ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
                        sendCompletedMessage = true;
                        l.mParameters.currentAfTriggerId = -1;
                        break;
                    case ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN:
                        // Just starting focusing, ignore
                        break;
                    case ANDROID_CONTROL_AF_STATE_INACTIVE:
                    case ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN:
                    case ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED:
                    default:
                        // Unexpected in AUTO/MACRO mode
                        ALOGE("%s: Unexpected AF state transition in AUTO/MACRO mode: %d",
                                __FUNCTION__, newState);
                        break;
                }
                break;
            case Parameters::FOCUS_MODE_CONTINUOUS_VIDEO:
            case Parameters::FOCUS_MODE_CONTINUOUS_PICTURE:
                switch (newState) {
                    case ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED:
                        success = true;
                        // no break
                    case ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
                        // Don't send notifications upstream if they're not for
                        // the current AF trigger. For example, if cancel was
                        // called in between, or if we already sent a
                        // notification about this AF call.
                        // Send both a 'AF done' callback and a 'AF move' callback
                        if (triggerId != l.mParameters.currentAfTriggerId) break;
                        sendCompletedMessage = true;
                        afInMotion = false;
                        if (l.mParameters.enableFocusMoveMessages &&
                                l.mParameters.afInMotion) {
                            sendMovingMessage = true;
                        }
                        l.mParameters.currentAfTriggerId = -1;
                        break;
                    case ANDROID_CONTROL_AF_STATE_INACTIVE:
                        // Cancel was called, or we switched state; care if
                        // currently moving
                        afInMotion = false;
                        if (l.mParameters.enableFocusMoveMessages &&
                                l.mParameters.afInMotion) {
                            sendMovingMessage = true;
                        }
                        break;
                    case ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN:
                        // Start passive scan, inform upstream
                        afInMotion = true;
                        // no break
                    case ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED:
                        // Stop passive scan, inform upstream
                        if (l.mParameters.enableFocusMoveMessages) {
                            sendMovingMessage = true;
                        }
                        break;
                }
                l.mParameters.afInMotion = afInMotion;
                break;
            case Parameters::FOCUS_MODE_EDOF:
            case Parameters::FOCUS_MODE_INFINITY:
            case Parameters::FOCUS_MODE_FIXED:
            default:
                if (newState != ANDROID_CONTROL_AF_STATE_INACTIVE) {
                    ALOGE("%s: Unexpected AF state change %d "
                            "(ID %d) in focus mode %d",
                          __FUNCTION__, newState, triggerId,
                            l.mParameters.focusMode);
                }
        }
    }
    if (sendMovingMessage) {
        SharedCameraClient::Lock l(mSharedCameraClient);
        if (l.mCameraClient != 0) {
            l.mCameraClient->notifyCallback(CAMERA_MSG_FOCUS_MOVE,
                    afInMotion ? 1 : 0, 0);
        }
    }
    if (sendCompletedMessage) {
        SharedCameraClient::Lock l(mSharedCameraClient);
        if (l.mCameraClient != 0) {
            l.mCameraClient->notifyCallback(CAMERA_MSG_FOCUS,
                    success ? 1 : 0, 0);
        }
    }
}

void Camera2Client::notifyAutoExposure(uint8_t newState, int triggerId) {
    ALOGV("%s: Autoexposure state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
    mCaptureSequencer->notifyAutoExposure(newState, triggerId);
}

void Camera2Client::notifyAutoWhitebalance(uint8_t newState, int triggerId) {
    ALOGV("%s: Auto-whitebalance state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
}

int Camera2Client::getCameraId() const {
    return mCameraId;
}

const sp<Camera2Device>& Camera2Client::getCameraDevice() {
    return mDevice;
}

const sp<CameraService>& Camera2Client::getCameraService() {
    return mCameraService;
}

camera2::SharedParameters& Camera2Client::getParameters() {
    return mParameters;
}

int Camera2Client::getPreviewStreamId() const {
    return mPreviewStreamId;
}

int Camera2Client::getCaptureStreamId() const {
    return mJpegProcessor->getStreamId();
}

int Camera2Client::getCallbackStreamId() const {
    return mCallbackProcessor->getStreamId();
}

int Camera2Client::getRecordingStreamId() const {
    return mRecordingStreamId;
}

int Camera2Client::getZslStreamId() const {
    return mZslProcessor->getStreamId();
}

status_t Camera2Client::registerFrameListener(int32_t id,
        wp<camera2::FrameProcessor::FilteredListener> listener) {
    return mFrameProcessor->registerListener(id, listener);
}

status_t Camera2Client::removeFrameListener(int32_t id) {
    return mFrameProcessor->removeListener(id);
}

Camera2Client::SharedCameraClient::Lock::Lock(SharedCameraClient &client):
        mCameraClient(client.mCameraClient),
        mSharedClient(client) {
    mSharedClient.mCameraClientLock.lock();
}

Camera2Client::SharedCameraClient::Lock::~Lock() {
    mSharedClient.mCameraClientLock.unlock();
}

Camera2Client::SharedCameraClient::SharedCameraClient(const sp<ICameraClient>&client):
        mCameraClient(client) {
}

Camera2Client::SharedCameraClient& Camera2Client::SharedCameraClient::operator=(
        const sp<ICameraClient>&client) {
    Mutex::Autolock l(mCameraClientLock);
    mCameraClient = client;
    return *this;
}

void Camera2Client::SharedCameraClient::clear() {
    Mutex::Autolock l(mCameraClientLock);
    mCameraClient.clear();
}

const int32_t Camera2Client::kPreviewRequestId;
const int32_t Camera2Client::kRecordRequestId;
const int32_t Camera2Client::kFirstCaptureRequestId;

void Camera2Client::onRecordingFrameAvailable() {
    ATRACE_CALL();
    status_t res;
    sp<Camera2Heap> recordingHeap;
    size_t heapIdx = 0;
    nsecs_t timestamp;
    {
        SharedParameters::Lock l(mParameters);

        BufferItemConsumer::BufferItem imgBuffer;
        res = mRecordingConsumer->acquireBuffer(&imgBuffer);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error receiving recording buffer: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return;
        }
        timestamp = imgBuffer.mTimestamp;

        mRecordingFrameCount++;
        ALOGV("OnRecordingFrame: Frame %d", mRecordingFrameCount);

        // TODO: Signal errors here upstream
        if (l.mParameters.state != Parameters::RECORD &&
                l.mParameters.state != Parameters::VIDEO_SNAPSHOT) {
            ALOGV("%s: Camera %d: Discarding recording image buffers received after "
                    "recording done",
                    __FUNCTION__, mCameraId);
            mRecordingConsumer->releaseBuffer(imgBuffer);
            return;
        }

        if (mRecordingHeap == 0) {
            const size_t bufferSize = 4 + sizeof(buffer_handle_t);
            ALOGV("%s: Camera %d: Creating recording heap with %d buffers of "
                    "size %d bytes", __FUNCTION__, mCameraId,
                    mRecordingHeapCount, bufferSize);

            mRecordingHeap = new Camera2Heap(bufferSize, mRecordingHeapCount,
                    "Camera2Client::RecordingHeap");
            if (mRecordingHeap->mHeap->getSize() == 0) {
                ALOGE("%s: Camera %d: Unable to allocate memory for recording",
                        __FUNCTION__, mCameraId);
                mRecordingConsumer->releaseBuffer(imgBuffer);
                return;
            }
            for (size_t i = 0; i < mRecordingBuffers.size(); i++) {
                if (mRecordingBuffers[i].mBuf !=
                        BufferItemConsumer::INVALID_BUFFER_SLOT) {
                    ALOGE("%s: Camera %d: Non-empty recording buffers list!",
                            __FUNCTION__, mCameraId);
                }
            }
            mRecordingBuffers.clear();
            mRecordingBuffers.setCapacity(mRecordingHeapCount);
            mRecordingBuffers.insertAt(0, mRecordingHeapCount);

            mRecordingHeapHead = 0;
            mRecordingHeapFree = mRecordingHeapCount;
        }

        if ( mRecordingHeapFree == 0) {
            ALOGE("%s: Camera %d: No free recording buffers, dropping frame",
                    __FUNCTION__, mCameraId);
            mRecordingConsumer->releaseBuffer(imgBuffer);
            return;
        }

        heapIdx = mRecordingHeapHead;
        mRecordingHeapHead = (mRecordingHeapHead + 1) % mRecordingHeapCount;
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
        *((uint32_t*)data) = type;
        *((buffer_handle_t*)(data + 4)) = imgBuffer.mGraphicBuffer->handle;
        ALOGV("%s: Camera %d: Sending out buffer_handle_t %p",
                __FUNCTION__, mCameraId, imgBuffer.mGraphicBuffer->handle);
        mRecordingBuffers.replaceAt(imgBuffer, heapIdx);
        recordingHeap = mRecordingHeap;
    }

    // Call outside locked parameters to allow re-entrancy from notification
    SharedCameraClient::Lock l(mSharedCameraClient);
    if (l.mCameraClient != 0) {
        l.mCameraClient->dataCallbackTimestamp(timestamp,
                CAMERA_MSG_VIDEO_FRAME,
                recordingHeap->mBuffers[heapIdx]);
    }
}

/** Utility methods */

status_t Camera2Client::updateRequests(Parameters &params) {
    status_t res;

    res = updatePreviewRequest(params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update preview request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }
    res = updateRecordingRequest(params);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update recording request: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return res;
    }

    if (params.state == Parameters::PREVIEW) {
        res = startPreviewL(params, true);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error streaming new preview request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    } else if (params.state == Parameters::RECORD ||
            params.state == Parameters::VIDEO_SNAPSHOT) {
        res = mDevice->setStreamingRequest(mRecordingRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error streaming new record request: %s (%d)",
                    __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }
    return res;
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
    if (mPreviewRequest.entryCount() == 0) {
        res = mDevice->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
                &mPreviewRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default preview request: "
                    "%s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = params.updateRequest(&mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of preview "
                "request: %s (%d)", __FUNCTION__, mCameraId,
                strerror(-res), res);
        return res;
    }

    res = mPreviewRequest.update(ANDROID_REQUEST_ID,
            &kPreviewRequestId, 1);

    return OK;
}

status_t Camera2Client::updateRecordingRequest(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    if (mRecordingRequest.entryCount() == 0) {
        res = mDevice->createDefaultRequest(CAMERA2_TEMPLATE_VIDEO_RECORD,
                &mRecordingRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default recording request:"
                    " %s (%d)", __FUNCTION__, mCameraId, strerror(-res), res);
            return res;
        }
    }

    res = params.updateRequest(&mRecordingRequest);
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
        // Create CPU buffer queue endpoint. We need one more buffer here so that we can
        // always acquire and free a buffer when the heap is full; otherwise the consumer
        // will have buffers in flight we'll never clear out.
        mRecordingConsumer = new BufferItemConsumer(
                GRALLOC_USAGE_HW_VIDEO_ENCODER,
                mRecordingHeapCount + 1,
                true);
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
        mRecordingFrameCount = 0;
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

size_t Camera2Client::calculateBufferSize(int width, int height,
        int format, int stride) {
    switch (format) {
        case HAL_PIXEL_FORMAT_YCbCr_422_SP: // NV16
            return width * height * 2;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP: // NV21
            return width * height * 3 / 2;
        case HAL_PIXEL_FORMAT_YCbCr_422_I: // YUY2
            return width * height * 2;
        case HAL_PIXEL_FORMAT_YV12: {      // YV12
            size_t ySize = stride * height;
            size_t uvStride = (stride / 2 + 0xF) & ~0xF;
            size_t uvSize = uvStride * height / 2;
            return ySize + uvSize * 2;
        }
        case HAL_PIXEL_FORMAT_RGB_565:
            return width * height * 2;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            return width * height * 4;
        case HAL_PIXEL_FORMAT_RAW_SENSOR:
            return width * height * 2;
        default:
            ALOGE("%s: Unknown preview format: %x",
                    __FUNCTION__,  format);
            return 0;
    }
}

} // namespace android
