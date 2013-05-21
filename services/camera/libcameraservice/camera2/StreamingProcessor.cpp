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

#define LOG_TAG "Camera2-StreamingProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include <gui/Surface.h>
#include <media/hardware/MetadataBufferType.h>

#include "StreamingProcessor.h"
#include "Camera2Heap.h"
#include "../Camera2Client.h"
#include "../CameraDeviceBase.h"

namespace android {
namespace camera2 {

StreamingProcessor::StreamingProcessor(sp<Camera2Client> client):
        mClient(client),
        mDevice(client->getCameraDevice()),
        mId(client->getCameraId()),
        mActiveRequest(NONE),
        mPaused(false),
        mPreviewRequestId(Camera2Client::kPreviewRequestIdStart),
        mPreviewStreamId(NO_STREAM),
        mRecordingRequestId(Camera2Client::kRecordingRequestIdStart),
        mRecordingStreamId(NO_STREAM),
        mRecordingHeapCount(kDefaultRecordingHeapCount)
{
}

StreamingProcessor::~StreamingProcessor() {
    deletePreviewStream();
    deleteRecordingStream();
}

status_t StreamingProcessor::setPreviewWindow(sp<ANativeWindow> window) {
    ATRACE_CALL();
    status_t res;

    res = deletePreviewStream();
    if (res != OK) return res;

    Mutex::Autolock m(mMutex);

    mPreviewWindow = window;

    return OK;
}

bool StreamingProcessor::haveValidPreviewWindow() const {
    Mutex::Autolock m(mMutex);
    return mPreviewWindow != 0;
}

status_t StreamingProcessor::updatePreviewRequest(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    Mutex::Autolock m(mMutex);
    if (mPreviewRequest.entryCount() == 0) {
        res = device->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
                &mPreviewRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default preview request: "
                    "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }

    res = params.updateRequest(&mPreviewRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of preview "
                "request: %s (%d)", __FUNCTION__, mId,
                strerror(-res), res);
        return res;
    }

    res = mPreviewRequest.update(ANDROID_REQUEST_ID,
            &mPreviewRequestId, 1);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update request id for preview: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    return OK;
}

status_t StreamingProcessor::updatePreviewStream(const Parameters &params) {
    ATRACE_CALL();
    Mutex::Autolock m(mMutex);

    status_t res;
    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (mPreviewStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = device->getStreamInfo(mPreviewStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying preview stream info: "
                    "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.previewWidth ||
                currentHeight != (uint32_t)params.previewHeight) {
            ALOGV("%s: Camera %d: Preview size switch: %d x %d -> %d x %d",
                    __FUNCTION__, mId, currentWidth, currentHeight,
                    params.previewWidth, params.previewHeight);
            res = device->waitUntilDrained();
            if (res != OK) {
                ALOGE("%s: Camera %d: Error waiting for preview to drain: "
                        "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
                return res;
            }
            res = device->deleteStream(mPreviewStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for preview: %s (%d)", __FUNCTION__, mId,
                        strerror(-res), res);
                return res;
            }
            mPreviewStreamId = NO_STREAM;
        }
    }

    if (mPreviewStreamId == NO_STREAM) {
        res = device->createStream(mPreviewWindow,
                params.previewWidth, params.previewHeight,
                CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, 0,
                &mPreviewStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create preview stream: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }

    res = device->setStreamTransform(mPreviewStreamId,
            params.previewTransform);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview stream transform: "
                "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    return OK;
}

status_t StreamingProcessor::deletePreviewStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock m(mMutex);

    if (mPreviewStreamId != NO_STREAM) {
        sp<CameraDeviceBase> device = mDevice.promote();
        if (device == 0) {
            ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
            return INVALID_OPERATION;
        }

        ALOGV("%s: for cameraId %d on streamId %d",
            __FUNCTION__, mId, mPreviewStreamId);

        res = device->waitUntilDrained();
        if (res != OK) {
            ALOGE("%s: Error waiting for preview to drain: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        res = device->deleteStream(mPreviewStreamId);
        if (res != OK) {
            ALOGE("%s: Unable to delete old preview stream: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        mPreviewStreamId = NO_STREAM;
    }
    return OK;
}

int StreamingProcessor::getPreviewStreamId() const {
    Mutex::Autolock m(mMutex);
    return mPreviewStreamId;
}

status_t StreamingProcessor::setRecordingBufferCount(size_t count) {
    ATRACE_CALL();
    // 32 is the current upper limit on the video buffer count for BufferQueue
    if (count > 32) {
        ALOGE("%s: Camera %d: Error setting %d as video buffer count value",
                __FUNCTION__, mId, count);
        return BAD_VALUE;
    }

    Mutex::Autolock m(mMutex);

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

status_t StreamingProcessor::updateRecordingRequest(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    Mutex::Autolock m(mMutex);

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (mRecordingRequest.entryCount() == 0) {
        res = device->createDefaultRequest(CAMERA2_TEMPLATE_VIDEO_RECORD,
                &mRecordingRequest);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to create default recording request:"
                    " %s (%d)", __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }

    res = params.updateRequest(&mRecordingRequest);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update common entries of recording "
                "request: %s (%d)", __FUNCTION__, mId,
                strerror(-res), res);
        return res;
    }

    res = mRecordingRequest.update(ANDROID_REQUEST_ID,
            &mRecordingRequestId, 1);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to update request id for request: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    return OK;
}

status_t StreamingProcessor::updateRecordingStream(const Parameters &params) {
    ATRACE_CALL();
    status_t res;
    Mutex::Autolock m(mMutex);

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (mRecordingConsumer == 0) {
        // Create CPU buffer queue endpoint. We need one more buffer here so that we can
        // always acquire and free a buffer when the heap is full; otherwise the consumer
        // will have buffers in flight we'll never clear out.
        mRecordingConsumer = new BufferItemConsumer(
                GRALLOC_USAGE_HW_VIDEO_ENCODER,
                mRecordingHeapCount + 1,
                true);
        mRecordingConsumer->setFrameAvailableListener(this);
        mRecordingConsumer->setName(String8("Camera2-RecordingConsumer"));
        mRecordingWindow = new Surface(
            mRecordingConsumer->getProducerInterface());
        // Allocate memory later, since we don't know buffer size until receipt
    }

    if (mRecordingStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = device->getStreamInfo(mRecordingStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying recording output stream info: "
                    "%s (%d)", __FUNCTION__, mId,
                    strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.videoWidth ||
                currentHeight != (uint32_t)params.videoHeight) {
            // TODO: Should wait to be sure previous recording has finished
            res = device->deleteStream(mRecordingStreamId);

            if (res == -EBUSY) {
                ALOGV("%s: Camera %d: Device is busy, call "
                      "updateRecordingStream after it becomes idle",
                      __FUNCTION__, mId);
                return res;
            } else if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for recording: %s (%d)", __FUNCTION__,
                        mId, strerror(-res), res);
                return res;
            }
            mRecordingStreamId = NO_STREAM;
        }
    }

    if (mRecordingStreamId == NO_STREAM) {
        mRecordingFrameCount = 0;
        res = device->createStream(mRecordingWindow,
                params.videoWidth, params.videoHeight,
                CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, 0, &mRecordingStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for recording: "
                    "%s (%d)", __FUNCTION__, mId,
                    strerror(-res), res);
            return res;
        }
    }

    return OK;
}

status_t StreamingProcessor::deleteRecordingStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock m(mMutex);

    if (mRecordingStreamId != NO_STREAM) {
        sp<CameraDeviceBase> device = mDevice.promote();
        if (device == 0) {
            ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
            return INVALID_OPERATION;
        }

        res = device->waitUntilDrained();
        if (res != OK) {
            ALOGE("%s: Error waiting for HAL to drain: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        res = device->deleteStream(mRecordingStreamId);
        if (res != OK) {
            ALOGE("%s: Unable to delete recording stream: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
        mRecordingStreamId = NO_STREAM;
    }
    return OK;
}

int StreamingProcessor::getRecordingStreamId() const {
    return mRecordingStreamId;
}

status_t StreamingProcessor::startStream(StreamType type,
        const Vector<uint8_t> &outputStreams) {
    ATRACE_CALL();
    status_t res;

    if (type == NONE) return INVALID_OPERATION;

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    ALOGV("%s: Camera %d: type = %d", __FUNCTION__, mId, type);

    Mutex::Autolock m(mMutex);

    CameraMetadata &request = (type == PREVIEW) ?
            mPreviewRequest : mRecordingRequest;

    res = request.update(
        ANDROID_REQUEST_OUTPUT_STREAMS,
        outputStreams);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set up preview request: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    res = request.sort();
    if (res != OK) {
        ALOGE("%s: Camera %d: Error sorting preview request: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    res = device->setStreamingRequest(request);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to set preview request to start preview: "
                "%s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }
    mActiveRequest = type;
    mPaused = false;

    return OK;
}

status_t StreamingProcessor::togglePauseStream(bool pause) {
    ATRACE_CALL();
    status_t res;

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    ALOGV("%s: Camera %d: toggling pause to %d", __FUNCTION__, mId, pause);

    Mutex::Autolock m(mMutex);

    if (mActiveRequest == NONE) {
        ALOGE("%s: Camera %d: Can't toggle pause, streaming was not started",
              __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (mPaused == pause) {
        return OK;
    }

    if (pause) {
        res = device->clearStreamingRequest();
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't clear stream request: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    } else {
        CameraMetadata &request =
                (mActiveRequest == PREVIEW) ? mPreviewRequest
                                            : mRecordingRequest;
        res = device->setStreamingRequest(request);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to set preview request to resume: "
                    "%s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }

    mPaused = pause;
    return OK;
}

status_t StreamingProcessor::stopStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock m(mMutex);

    sp<CameraDeviceBase> device = mDevice.promote();
    if (device == 0) {
        ALOGE("%s: Camera %d: Device does not exist", __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    res = device->clearStreamingRequest();
    if (res != OK) {
        ALOGE("%s: Camera %d: Can't clear stream request: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    mActiveRequest = NONE;
    mPaused = false;

    return OK;
}

int32_t StreamingProcessor::getActiveRequestId() const {
    Mutex::Autolock m(mMutex);
    switch (mActiveRequest) {
        case NONE:
            return 0;
        case PREVIEW:
            return mPreviewRequestId;
        case RECORD:
            return mRecordingRequestId;
        default:
            ALOGE("%s: Unexpected mode %d", __FUNCTION__, mActiveRequest);
            return 0;
    }
}

status_t StreamingProcessor::incrementStreamingIds() {
    ATRACE_CALL();
    Mutex::Autolock m(mMutex);

    mPreviewRequestId++;
    if (mPreviewRequestId >= Camera2Client::kPreviewRequestIdEnd) {
        mPreviewRequestId = Camera2Client::kPreviewRequestIdStart;
    }
    mRecordingRequestId++;
    if (mRecordingRequestId >= Camera2Client::kRecordingRequestIdEnd) {
        mRecordingRequestId = Camera2Client::kRecordingRequestIdStart;
    }
    return OK;
}

void StreamingProcessor::onFrameAvailable() {
    ATRACE_CALL();
    status_t res;
    sp<Camera2Heap> recordingHeap;
    size_t heapIdx = 0;
    nsecs_t timestamp;

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) {
        // Discard frames during shutdown
        BufferItemConsumer::BufferItem imgBuffer;
        res = mRecordingConsumer->acquireBuffer(&imgBuffer);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error receiving recording buffer: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return;
        }
        mRecordingConsumer->releaseBuffer(imgBuffer);
        return;
    }

    {
        /* acquire SharedParameters before mMutex so we don't dead lock
            with Camera2Client code calling into StreamingProcessor */
        SharedParameters::Lock l(client->getParameters());
        Mutex::Autolock m(mMutex);
        BufferItemConsumer::BufferItem imgBuffer;
        res = mRecordingConsumer->acquireBuffer(&imgBuffer);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error receiving recording buffer: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return;
        }
        timestamp = imgBuffer.mTimestamp;

        mRecordingFrameCount++;
        ALOGV("OnRecordingFrame: Frame %d", mRecordingFrameCount);

        // TODO: Signal errors here upstream
        if (l.mParameters.state != Parameters::RECORD &&
                l.mParameters.state != Parameters::VIDEO_SNAPSHOT) {
            ALOGV("%s: Camera %d: Discarding recording image buffers "
                    "received after recording done", __FUNCTION__,
                    mId);
            mRecordingConsumer->releaseBuffer(imgBuffer);
            return;
        }

        if (mRecordingHeap == 0) {
            const size_t bufferSize = 4 + sizeof(buffer_handle_t);
            ALOGV("%s: Camera %d: Creating recording heap with %d buffers of "
                    "size %d bytes", __FUNCTION__, mId,
                    mRecordingHeapCount, bufferSize);

            mRecordingHeap = new Camera2Heap(bufferSize, mRecordingHeapCount,
                    "Camera2Client::RecordingHeap");
            if (mRecordingHeap->mHeap->getSize() == 0) {
                ALOGE("%s: Camera %d: Unable to allocate memory for recording",
                        __FUNCTION__, mId);
                mRecordingConsumer->releaseBuffer(imgBuffer);
                return;
            }
            for (size_t i = 0; i < mRecordingBuffers.size(); i++) {
                if (mRecordingBuffers[i].mBuf !=
                        BufferItemConsumer::INVALID_BUFFER_SLOT) {
                    ALOGE("%s: Camera %d: Non-empty recording buffers list!",
                            __FUNCTION__, mId);
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
                    __FUNCTION__, mId);
            mRecordingConsumer->releaseBuffer(imgBuffer);
            return;
        }

        heapIdx = mRecordingHeapHead;
        mRecordingHeapHead = (mRecordingHeapHead + 1) % mRecordingHeapCount;
        mRecordingHeapFree--;

        ALOGV("%s: Camera %d: Timestamp %lld",
                __FUNCTION__, mId, timestamp);

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
                __FUNCTION__, mId,
                imgBuffer.mGraphicBuffer->handle);
        mRecordingBuffers.replaceAt(imgBuffer, heapIdx);
        recordingHeap = mRecordingHeap;
    }

    // Call outside locked parameters to allow re-entrancy from notification
    Camera2Client::SharedCameraCallbacks::Lock l(client->mSharedCameraCallbacks);
    if (l.mRemoteCallback != 0) {
        l.mRemoteCallback->dataCallbackTimestamp(timestamp,
                CAMERA_MSG_VIDEO_FRAME,
                recordingHeap->mBuffers[heapIdx]);
    }
}

void StreamingProcessor::releaseRecordingFrame(const sp<IMemory>& mem) {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock m(mMutex);
    // Make sure this is for the current heap
    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap = mem->getMemory(&offset, &size);
    if (heap->getHeapID() != mRecordingHeap->mHeap->getHeapID()) {
        ALOGW("%s: Camera %d: Mismatched heap ID, ignoring release "
                "(got %x, expected %x)", __FUNCTION__, mId,
                heap->getHeapID(), mRecordingHeap->mHeap->getHeapID());
        return;
    }
    uint8_t *data = (uint8_t*)heap->getBase() + offset;
    uint32_t type = *(uint32_t*)data;
    if (type != kMetadataBufferTypeGrallocSource) {
        ALOGE("%s: Camera %d: Recording frame type invalid (got %x, expected %x)",
                __FUNCTION__, mId, type,
                kMetadataBufferTypeGrallocSource);
        return;
    }

    // Release the buffer back to the recording queue

    buffer_handle_t imgHandle = *(buffer_handle_t*)(data + 4);

    size_t itemIndex;
    for (itemIndex = 0; itemIndex < mRecordingBuffers.size(); itemIndex++) {
        const BufferItemConsumer::BufferItem item =
                mRecordingBuffers[itemIndex];
        if (item.mBuf != BufferItemConsumer::INVALID_BUFFER_SLOT &&
                item.mGraphicBuffer->handle == imgHandle) {
            break;
        }
    }
    if (itemIndex == mRecordingBuffers.size()) {
        ALOGE("%s: Camera %d: Can't find buffer_handle_t %p in list of "
                "outstanding buffers", __FUNCTION__, mId,
                imgHandle);
        return;
    }

    ALOGV("%s: Camera %d: Freeing buffer_handle_t %p", __FUNCTION__,
            mId, imgHandle);

    res = mRecordingConsumer->releaseBuffer(mRecordingBuffers[itemIndex]);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to free recording frame "
                "(buffer_handle_t: %p): %s (%d)", __FUNCTION__,
                mId, imgHandle, strerror(-res), res);
        return;
    }
    mRecordingBuffers.replaceAt(itemIndex);

    mRecordingHeapFree++;
}


status_t StreamingProcessor::dump(int fd, const Vector<String16>& /*args*/) {
    String8 result;

    result.append("  Current requests:\n");
    if (mPreviewRequest.entryCount() != 0) {
        result.append("    Preview request:\n");
        write(fd, result.string(), result.size());
        mPreviewRequest.dump(fd, 2, 6);
        result.clear();
    } else {
        result.append("    Preview request: undefined\n");
    }

    if (mRecordingRequest.entryCount() != 0) {
        result = "    Recording request:\n";
        write(fd, result.string(), result.size());
        mRecordingRequest.dump(fd, 2, 6);
        result.clear();
    } else {
        result = "    Recording request: undefined\n";
    }

    const char* streamTypeString[] = {
        "none", "preview", "record"
    };
    result.append(String8::format("   Active request: %s (paused: %s)\n",
                                  streamTypeString[mActiveRequest],
                                  mPaused ? "yes" : "no"));

    write(fd, result.string(), result.size());

    return OK;
}

}; // namespace camera2
}; // namespace android
