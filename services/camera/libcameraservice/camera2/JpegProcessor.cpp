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

#define LOG_TAG "Camera2Client::JpegProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include "JpegProcessor.h"
#include <gui/SurfaceTextureClient.h>
#include "../Camera2Device.h"
#include "../Camera2Client.h"


namespace android {
namespace camera2 {

JpegProcessor::JpegProcessor(
    wp<Camera2Client> client,
    wp<CaptureSequencer> sequencer):
        Thread(false),
        mClient(client),
        mSequencer(sequencer),
        mCaptureAvailable(false),
        mCaptureStreamId(NO_STREAM) {
}

JpegProcessor::~JpegProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
}

void JpegProcessor::onFrameAvailable() {
    Mutex::Autolock l(mInputMutex);
    if (!mCaptureAvailable) {
        mCaptureAvailable = true;
        mCaptureAvailableSignal.signal();
    }
}

status_t JpegProcessor::updateStream(const Parameters &params) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return OK;
    sp<Camera2Device> device = client->getCameraDevice();

    // Find out buffer size for JPEG
    camera_metadata_ro_entry_t maxJpegSize =
            params.staticInfo(ANDROID_JPEG_MAX_SIZE);
    if (maxJpegSize.count == 0) {
        ALOGE("%s: Camera %d: Can't find ANDROID_JPEG_MAX_SIZE!",
                __FUNCTION__, client->getCameraId());
        return INVALID_OPERATION;
    }

    if (mCaptureConsumer == 0) {
        // Create CPU buffer queue endpoint
        mCaptureConsumer = new CpuConsumer(1);
        mCaptureConsumer->setFrameAvailableListener(this);
        mCaptureConsumer->setName(String8("Camera2Client::CaptureConsumer"));
        mCaptureWindow = new SurfaceTextureClient(
            mCaptureConsumer->getProducerInterface());
        // Create memory for API consumption
        mCaptureHeap = new Camera2Heap(maxJpegSize.data.i32[0], 1,
                                       "Camera2Client::CaptureHeap");
        if (mCaptureHeap->mHeap->getSize() == 0) {
            ALOGE("%s: Camera %d: Unable to allocate memory for capture",
                    __FUNCTION__, client->getCameraId());
            return NO_MEMORY;
        }
    }

    if (mCaptureStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = device->getStreamInfo(mCaptureStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying capture output stream info: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.pictureWidth ||
                currentHeight != (uint32_t)params.pictureHeight) {
            res = device->deleteStream(mCaptureStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for capture: %s (%d)", __FUNCTION__,
                        client->getCameraId(), strerror(-res), res);
                return res;
            }
            mCaptureStreamId = NO_STREAM;
        }
    }

    if (mCaptureStreamId == NO_STREAM) {
        // Create stream for HAL production
        res = device->createStream(mCaptureWindow,
                params.pictureWidth, params.pictureHeight,
                HAL_PIXEL_FORMAT_BLOB, maxJpegSize.data.i32[0],
                &mCaptureStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for capture: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }

    }
    return OK;
}

status_t JpegProcessor::deleteStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    if (mCaptureStreamId != NO_STREAM) {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return OK;
        sp<Camera2Device> device = client->getCameraDevice();

        device->deleteStream(mCaptureStreamId);
        mCaptureStreamId = NO_STREAM;
    }
    return OK;
}

int JpegProcessor::getStreamId() const {
    Mutex::Autolock l(mInputMutex);
    return mCaptureStreamId;
}

void JpegProcessor::dump(int fd, const Vector<String16>& args) const {
}

bool JpegProcessor::threadLoop() {
    status_t res;

    {
        Mutex::Autolock l(mInputMutex);
        while (!mCaptureAvailable) {
            res = mCaptureAvailableSignal.waitRelative(mInputMutex,
                    kWaitDuration);
            if (res == TIMED_OUT) return true;
        }
        mCaptureAvailable = false;
    }

    do {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        res = processNewCapture(client);
    } while (res == OK);

    return true;
}

status_t JpegProcessor::processNewCapture(sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;
    sp<Camera2Heap> captureHeap;

    CpuConsumer::LockedBuffer imgBuffer;

    res = mCaptureConsumer->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        if (res != BAD_VALUE) {
            ALOGE("%s: Camera %d: Error receiving still image buffer: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
        }
        return res;
    }

    ALOGV("%s: Camera %d: Still capture available", __FUNCTION__,
            client->getCameraId());

    // TODO: Signal errors here upstream
    {
        SharedParameters::Lock l(client->getParameters());

        switch (l.mParameters.state) {
            case Parameters::STILL_CAPTURE:
            case Parameters::VIDEO_SNAPSHOT:
                break;
            default:
                ALOGE("%s: Camera %d: Still image produced unexpectedly "
                        "in state %s!",
                        __FUNCTION__, client->getCameraId(),
                        Parameters::getStateName(l.mParameters.state));
                mCaptureConsumer->unlockBuffer(imgBuffer);
                return BAD_VALUE;
        }
    }

    if (imgBuffer.format != HAL_PIXEL_FORMAT_BLOB) {
        ALOGE("%s: Camera %d: Unexpected format for still image: "
                "%x, expected %x", __FUNCTION__, client->getCameraId(),
                imgBuffer.format,
                HAL_PIXEL_FORMAT_BLOB);
        mCaptureConsumer->unlockBuffer(imgBuffer);
        return OK;
    }

    sp<CaptureSequencer> sequencer = mSequencer.promote();
    if (sequencer != 0) {
        sequencer->onCaptureAvailable(imgBuffer.timestamp);
    }

    // TODO: Optimize this to avoid memcopy
    void* captureMemory = mCaptureHeap->mHeap->getBase();
    size_t size = mCaptureHeap->mHeap->getSize();
    memcpy(captureMemory, imgBuffer.data, size);

    mCaptureConsumer->unlockBuffer(imgBuffer);

    captureHeap = mCaptureHeap;

    Camera2Client::SharedCameraClient::Lock l(client->mSharedCameraClient);
    ALOGV("%s: Sending still image to client", __FUNCTION__);
    if (l.mCameraClient != 0) {
        l.mCameraClient->dataCallback(CAMERA_MSG_COMPRESSED_IMAGE,
                captureHeap->mBuffers[0], NULL);
    } else {
        ALOGV("%s: No client!", __FUNCTION__);
    }
    return OK;
}

}; // namespace camera2
}; // namespace android
