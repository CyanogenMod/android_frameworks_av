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

#define LOG_TAG "Camera2-CaptureSequencer"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include <utils/Vector.h>

#include "CaptureSequencer.h"
#include "BurstCapture.h"
#include "../Camera2Device.h"
#include "../Camera2Client.h"
#include "Parameters.h"

namespace android {
namespace camera2 {

/** Public members */

CaptureSequencer::CaptureSequencer(wp<Camera2Client> client):
        Thread(false),
        mStartCapture(false),
        mBusy(false),
        mNewAEState(false),
        mNewFrameReceived(false),
        mNewCaptureReceived(false),
        mClient(client),
        mCaptureState(IDLE),
        mTriggerId(0),
        mTimeoutCount(0),
        mCaptureId(Camera2Client::kFirstCaptureRequestId) {
    ALOGV("%s", __FUNCTION__);
}

CaptureSequencer::~CaptureSequencer() {
    ALOGV("%s: Exit", __FUNCTION__);
}

void CaptureSequencer::setZslProcessor(wp<ZslProcessor> processor) {
    Mutex::Autolock l(mInputMutex);
    mZslProcessor = processor;
}

status_t CaptureSequencer::startCapture() {
    ALOGV("%s", __FUNCTION__);
    ATRACE_CALL();
    Mutex::Autolock l(mInputMutex);
    if (mBusy) {
        ALOGE("%s: Already busy capturing!", __FUNCTION__);
        return INVALID_OPERATION;
    }
    if (!mStartCapture) {
        mStartCapture = true;
        mStartCaptureSignal.signal();
    }
    return OK;
}

void CaptureSequencer::notifyAutoExposure(uint8_t newState, int triggerId) {
    ATRACE_CALL();
    Mutex::Autolock l(mInputMutex);
    mAEState = newState;
    mAETriggerId = triggerId;
    if (!mNewAEState) {
        mNewAEState = true;
        mNewNotifySignal.signal();
    }
}

void CaptureSequencer::onFrameAvailable(int32_t frameId,
        CameraMetadata &frame) {
    ALOGV("%s: Listener found new frame", __FUNCTION__);
    ATRACE_CALL();
    Mutex::Autolock l(mInputMutex);
    mNewFrameId = frameId;
    mNewFrame.acquire(frame);
    if (!mNewFrameReceived) {
        mNewFrameReceived = true;
        mNewFrameSignal.signal();
    }
}

void CaptureSequencer::onCaptureAvailable(nsecs_t timestamp,
        sp<MemoryBase> captureBuffer) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);
    Mutex::Autolock l(mInputMutex);
    mCaptureTimestamp = timestamp;
    mCaptureBuffer = captureBuffer;
    if (!mNewCaptureReceived) {
        mNewCaptureReceived = true;
        mNewCaptureSignal.signal();
    }
}


void CaptureSequencer::dump(int fd, const Vector<String16>& args) {
    String8 result;
    if (mCaptureRequest.entryCount() != 0) {
        result = "    Capture request:\n";
        write(fd, result.string(), result.size());
        mCaptureRequest.dump(fd, 2, 6);
    } else {
        result = "    Capture request: undefined\n";
        write(fd, result.string(), result.size());
    }
    result = String8::format("    Current capture state: %s\n",
            kStateNames[mCaptureState]);
    result.append("    Latest captured frame:\n");
    write(fd, result.string(), result.size());
    mNewFrame.dump(fd, 2, 6);
}

/** Private members */

const char* CaptureSequencer::kStateNames[CaptureSequencer::NUM_CAPTURE_STATES+1] =
{
    "IDLE",
    "START",
    "ZSL_START",
    "ZSL_WAITING",
    "ZSL_REPROCESSING",
    "STANDARD_START",
    "STANDARD_PRECAPTURE",
    "STANDARD_CAPTURING",
    "BURST_CAPTURE_START",
    "BURST_CAPTURE_WAIT",
    "DONE",
    "ERROR",
    "UNKNOWN"
};

const CaptureSequencer::StateManager
        CaptureSequencer::kStateManagers[CaptureSequencer::NUM_CAPTURE_STATES-1] = {
    &CaptureSequencer::manageIdle,
    &CaptureSequencer::manageStart,
    &CaptureSequencer::manageZslStart,
    &CaptureSequencer::manageZslWaiting,
    &CaptureSequencer::manageZslReprocessing,
    &CaptureSequencer::manageStandardStart,
    &CaptureSequencer::manageStandardPrecaptureWait,
    &CaptureSequencer::manageStandardCapture,
    &CaptureSequencer::manageStandardCaptureWait,
    &CaptureSequencer::manageBurstCaptureStart,
    &CaptureSequencer::manageBurstCaptureWait,
    &CaptureSequencer::manageDone,
};

bool CaptureSequencer::threadLoop() {
    status_t res;

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return false;

    if (mCaptureState < ERROR) {
        CaptureState oldState = mCaptureState;
        mCaptureState = (this->*kStateManagers[mCaptureState])(client);
        if (ATRACE_ENABLED() && oldState != mCaptureState) {
            ATRACE_INT("cam2_capt_state", mCaptureState);
        }
    } else {
        ALOGE("%s: Bad capture state: %s",
                __FUNCTION__, kStateNames[mCaptureState]);
        return false;
    }

    return true;
}

CaptureSequencer::CaptureState CaptureSequencer::manageIdle(sp<Camera2Client> &client) {
    status_t res;
    Mutex::Autolock l(mInputMutex);
    while (!mStartCapture) {
        res = mStartCaptureSignal.waitRelative(mInputMutex,
                kWaitDuration);
        if (res == TIMED_OUT) break;
    }
    if (mStartCapture) {
        mStartCapture = false;
        mBusy = true;
        return START;
    }
    return IDLE;
}

CaptureSequencer::CaptureState CaptureSequencer::manageDone(sp<Camera2Client> &client) {
    status_t res = OK;
    ATRACE_CALL();
    mCaptureId++;

    {
        Mutex::Autolock l(mInputMutex);
        mBusy = false;
    }

    {
        SharedParameters::Lock l(client->getParameters());
        switch (l.mParameters.state) {
            case Parameters::STILL_CAPTURE:
                l.mParameters.state = Parameters::STOPPED;
                break;
            case Parameters::VIDEO_SNAPSHOT:
                l.mParameters.state = Parameters::RECORD;
                break;
            default:
                ALOGE("%s: Camera %d: Still image produced unexpectedly "
                        "in state %s!",
                        __FUNCTION__, client->getCameraId(),
                        Parameters::getStateName(l.mParameters.state));
                res = INVALID_OPERATION;
        }
    }
    sp<ZslProcessor> processor = mZslProcessor.promote();
    if (processor != 0) {
        processor->clearZslQueue();
    }

    if (mCaptureBuffer != 0 && res == OK) {
        Camera2Client::SharedCameraClient::Lock l(client->mSharedCameraClient);
        ALOGV("%s: Sending still image to client", __FUNCTION__);
        if (l.mCameraClient != 0) {
            l.mCameraClient->dataCallback(CAMERA_MSG_COMPRESSED_IMAGE,
                    mCaptureBuffer, NULL);
        } else {
            ALOGV("%s: No client!", __FUNCTION__);
        }
    }
    mCaptureBuffer.clear();

    return IDLE;
}

CaptureSequencer::CaptureState CaptureSequencer::manageStart(
        sp<Camera2Client> &client) {
    ALOGV("%s", __FUNCTION__);
    status_t res;
    ATRACE_CALL();
    SharedParameters::Lock l(client->getParameters());
    CaptureState nextState = DONE;

    res = updateCaptureRequest(l.mParameters, client);
    if (res != OK ) {
        ALOGE("%s: Camera %d: Can't update still image capture request: %s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return DONE;
    }

    if(l.mParameters.lightFx != Parameters::LIGHTFX_NONE &&
            l.mParameters.state == Parameters::STILL_CAPTURE) {
        nextState = BURST_CAPTURE_START;
    }
    else if (l.mParameters.zslMode &&
            l.mParameters.state == Parameters::STILL_CAPTURE &&
            l.mParameters.flashMode != Parameters::FLASH_MODE_ON) {
        nextState = ZSL_START;
    } else {
        nextState = STANDARD_START;
    }

    return nextState;
}

CaptureSequencer::CaptureState CaptureSequencer::manageZslStart(
        sp<Camera2Client> &client) {
    ALOGV("%s", __FUNCTION__);
    status_t res;
    sp<ZslProcessor> processor = mZslProcessor.promote();
    if (processor == 0) {
        ALOGE("%s: No ZSL queue to use!", __FUNCTION__);
        return DONE;
    }

    client->registerFrameListener(mCaptureId,
            this);

    res = client->getCameraDevice()->clearStreamingRequest();
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to stop preview for ZSL capture: "
                "%s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return DONE;
    }
    // TODO: Actually select the right thing here.
    res = processor->pushToReprocess(mCaptureId);
    if (res != OK) {
        if (res == NOT_ENOUGH_DATA) {
            ALOGV("%s: Camera %d: ZSL queue doesn't have good frame, "
                    "falling back to normal capture", __FUNCTION__,
                    client->getCameraId());
        } else {
            ALOGE("%s: Camera %d: Error in ZSL queue: %s (%d)",
                    __FUNCTION__, client->getCameraId(), strerror(-res), res);
        }
        return STANDARD_START;
    }

    SharedParameters::Lock l(client->getParameters());
    /* warning: this also locks a SharedCameraClient */
    shutterNotifyLocked(l.mParameters, client);

    mTimeoutCount = kMaxTimeoutsForCaptureEnd;
    return STANDARD_CAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageZslWaiting(
        sp<Camera2Client> &client) {
    ALOGV("%s", __FUNCTION__);
    return DONE;
}

CaptureSequencer::CaptureState CaptureSequencer::manageZslReprocessing(
        sp<Camera2Client> &client) {
    ALOGV("%s", __FUNCTION__);
    return START;
}

CaptureSequencer::CaptureState CaptureSequencer::manageStandardStart(
        sp<Camera2Client> &client) {
    ATRACE_CALL();
    client->registerFrameListener(mCaptureId,
            this);
    {
        SharedParameters::Lock l(client->getParameters());
        mTriggerId = l.mParameters.precaptureTriggerCounter++;
    }
    client->getCameraDevice()->triggerPrecaptureMetering(mTriggerId);

    mAeInPrecapture = false;
    mTimeoutCount = kMaxTimeoutsForPrecaptureStart;
    return STANDARD_PRECAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageStandardPrecaptureWait(
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    Mutex::Autolock l(mInputMutex);
    while (!mNewAEState) {
        res = mNewNotifySignal.waitRelative(mInputMutex, kWaitDuration);
        if (res == TIMED_OUT) {
            mTimeoutCount--;
            break;
        }
    }
    if (mTimeoutCount <= 0) {
        ALOGW("Timed out waiting for precapture %s",
                mAeInPrecapture ? "end" : "start");
        return STANDARD_CAPTURE;
    }
    if (mNewAEState) {
        if (!mAeInPrecapture) {
            // Waiting to see PRECAPTURE state
            if (mAETriggerId == mTriggerId &&
                    mAEState == ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
                ALOGV("%s: Got precapture start", __FUNCTION__);
                mAeInPrecapture = true;
                mTimeoutCount = kMaxTimeoutsForPrecaptureEnd;
            }
        } else {
            // Waiting to see PRECAPTURE state end
            if (mAETriggerId == mTriggerId &&
                    mAEState != ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
                ALOGV("%s: Got precapture end", __FUNCTION__);
                return STANDARD_CAPTURE;
            }
        }
        mNewAEState = false;
    }
    return STANDARD_PRECAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageStandardCapture(
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    SharedParameters::Lock l(client->getParameters());
    Vector<uint8_t> outputStreams;

    outputStreams.push(client->getPreviewStreamId());
    outputStreams.push(client->getCaptureStreamId());

    if (l.mParameters.previewCallbackFlags &
            CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK) {
        outputStreams.push(client->getCallbackStreamId());
    }

    if (l.mParameters.state == Parameters::VIDEO_SNAPSHOT) {
        outputStreams.push(client->getRecordingStreamId());
    }

    res = mCaptureRequest.update(ANDROID_REQUEST_OUTPUT_STREAMS,
            outputStreams);
    if (res == OK) {
        res = mCaptureRequest.update(ANDROID_REQUEST_ID,
                &mCaptureId, 1);
    }
    if (res == OK) {
        res = mCaptureRequest.sort();
    }

    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up still capture request: %s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return DONE;
    }

    CameraMetadata captureCopy = mCaptureRequest;
    if (captureCopy.entryCount() == 0) {
        ALOGE("%s: Camera %d: Unable to copy capture request for HAL device",
                __FUNCTION__, client->getCameraId());
        return DONE;
    }

    if (l.mParameters.state == Parameters::STILL_CAPTURE) {
        res = client->getCameraDevice()->clearStreamingRequest();
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to stop preview for still capture: "
                    "%s (%d)",
                    __FUNCTION__, client->getCameraId(), strerror(-res), res);
            return DONE;
        }
    }
    // TODO: Capture should be atomic with setStreamingRequest here
    res = client->getCameraDevice()->capture(captureCopy);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to submit still image capture request: "
                "%s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return DONE;
    }

    /* warning: this also locks a SharedCameraClient */
    shutterNotifyLocked(l.mParameters, client);

    mTimeoutCount = kMaxTimeoutsForCaptureEnd;
    return STANDARD_CAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageStandardCaptureWait(
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    Mutex::Autolock l(mInputMutex);
    while (!mNewFrameReceived) {
        res = mNewFrameSignal.waitRelative(mInputMutex, kWaitDuration);
        if (res == TIMED_OUT) {
            mTimeoutCount--;
            break;
        }
    }
    while (!mNewCaptureReceived) {
        res = mNewCaptureSignal.waitRelative(mInputMutex, kWaitDuration);
        if (res == TIMED_OUT) {
            mTimeoutCount--;
            break;
        }
    }
    if (mTimeoutCount <= 0) {
        ALOGW("Timed out waiting for capture to complete");
        return DONE;
    }
    if (mNewFrameReceived && mNewCaptureReceived) {
        if (mNewFrameId != mCaptureId) {
            ALOGW("Mismatched capture frame IDs: Expected %d, got %d",
                    mCaptureId, mNewFrameId);
        }
        camera_metadata_entry_t entry;
        entry = mNewFrame.find(ANDROID_SENSOR_TIMESTAMP);
        if (entry.count == 0) {
            ALOGE("No timestamp field in capture frame!");
        }
        if (entry.data.i64[0] != mCaptureTimestamp) {
            ALOGW("Mismatched capture timestamps: Metadata frame %lld,"
                    " captured buffer %lld", entry.data.i64[0], mCaptureTimestamp);
        }
        client->removeFrameListener(mCaptureId);

        mNewFrameReceived = false;
        mNewCaptureReceived = false;
        return DONE;
    }
    return STANDARD_CAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageBurstCaptureStart(
        sp<Camera2Client> &client) {
    ALOGV("%s", __FUNCTION__);
    status_t res;
    ATRACE_CALL();

    // check which burst mode is set, create respective burst object
    {
        SharedParameters::Lock l(client->getParameters());

        res = updateCaptureRequest(l.mParameters, client);
        if(res != OK) {
            return DONE;
        }

        //
        // check for burst mode type in mParameters here
        //
        mBurstCapture = new BurstCapture(client, this);
    }

    res = mCaptureRequest.update(ANDROID_REQUEST_ID, &mCaptureId, 1);
    if (res == OK) {
        res = mCaptureRequest.sort();
    }
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up still capture request: %s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return DONE;
    }

    CameraMetadata captureCopy = mCaptureRequest;
    if (captureCopy.entryCount() == 0) {
        ALOGE("%s: Camera %d: Unable to copy capture request for HAL device",
                __FUNCTION__, client->getCameraId());
        return DONE;
    }

    Vector<CameraMetadata> requests;
    requests.push(mCaptureRequest);
    res = mBurstCapture->start(requests, mCaptureId);
    mTimeoutCount = kMaxTimeoutsForCaptureEnd * 10;
    return BURST_CAPTURE_WAIT;
}

CaptureSequencer::CaptureState CaptureSequencer::manageBurstCaptureWait(
        sp<Camera2Client> &client) {
    status_t res;
    ATRACE_CALL();

    while (!mNewCaptureReceived) {
        res = mNewCaptureSignal.waitRelative(mInputMutex, kWaitDuration);
        if (res == TIMED_OUT) {
            mTimeoutCount--;
            break;
        }
    }

    if (mTimeoutCount <= 0) {
        ALOGW("Timed out waiting for burst capture to complete");
        return DONE;
    }
    if (mNewCaptureReceived) {
        mNewCaptureReceived = false;
        // TODO: update mCaptureId to last burst's capture ID + 1?
        return DONE;
    }

    return BURST_CAPTURE_WAIT;
}

status_t CaptureSequencer::updateCaptureRequest(const Parameters &params,
        sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;
    if (mCaptureRequest.entryCount() == 0) {
        res = client->getCameraDevice()->createDefaultRequest(
                CAMERA2_TEMPLATE_STILL_CAPTURE,
                &mCaptureRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default still image request:"
                    " %s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }
    }

    res = params.updateRequest(&mCaptureRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of capture "
                "request: %s (%d)", __FUNCTION__, client->getCameraId(),
                strerror(-res), res);
        return res;
    }

    res = mCaptureRequest.update(ANDROID_JPEG_THUMBNAIL_SIZE,
            params.jpegThumbSize, 2);
    if (res != OK) return res;
    res = mCaptureRequest.update(ANDROID_JPEG_THUMBNAIL_QUALITY,
            &params.jpegThumbQuality, 1);
    if (res != OK) return res;
    res = mCaptureRequest.update(ANDROID_JPEG_QUALITY,
            &params.jpegQuality, 1);
    if (res != OK) return res;
    res = mCaptureRequest.update(
            ANDROID_JPEG_ORIENTATION,
            &params.jpegRotation, 1);
    if (res != OK) return res;

    if (params.gpsEnabled) {
        res = mCaptureRequest.update(
                ANDROID_JPEG_GPS_COORDINATES,
                params.gpsCoordinates, 3);
        if (res != OK) return res;
        res = mCaptureRequest.update(
                ANDROID_JPEG_GPS_TIMESTAMP,
                &params.gpsTimestamp, 1);
        if (res != OK) return res;
        res = mCaptureRequest.update(
                ANDROID_JPEG_GPS_PROCESSING_METHOD,
                params.gpsProcessingMethod);
        if (res != OK) return res;
    } else {
        res = mCaptureRequest.erase(ANDROID_JPEG_GPS_COORDINATES);
        if (res != OK) return res;
        res = mCaptureRequest.erase(ANDROID_JPEG_GPS_TIMESTAMP);
        if (res != OK) return res;
        res = mCaptureRequest.erase(ANDROID_JPEG_GPS_PROCESSING_METHOD);
        if (res != OK) return res;
    }

    return OK;
}

/*static*/ void CaptureSequencer::shutterNotifyLocked(const Parameters &params,
            sp<Camera2Client> client) {
    ATRACE_CALL();

    if (params.state == Parameters::STILL_CAPTURE && params.playShutterSound) {
        client->getCameraService()->playSound(CameraService::SOUND_SHUTTER);
    }

    {
        Camera2Client::SharedCameraClient::Lock l(client->mSharedCameraClient);

        ALOGV("%s: Notifying of shutter close to client", __FUNCTION__);
        if (l.mCameraClient != 0) {
            // ShutterCallback
            l.mCameraClient->notifyCallback(CAMERA_MSG_SHUTTER,
                                            /*ext1*/0, /*ext2*/0);

            // RawCallback with null buffer
            l.mCameraClient->notifyCallback(CAMERA_MSG_RAW_IMAGE_NOTIFY,
                                            /*ext1*/0, /*ext2*/0);
        } else {
            ALOGV("%s: No client!", __FUNCTION__);
        }
    }
}


}; // namespace camera2
}; // namespace android
