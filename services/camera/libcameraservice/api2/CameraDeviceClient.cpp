/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "CameraDeviceClient"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <gui/Surface.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/CameraUtils.h>

#include "common/CameraDeviceBase.h"
#include "api2/CameraDeviceClient.h"



namespace android {
using namespace camera2;

CameraDeviceClientBase::CameraDeviceClientBase(
        const sp<CameraService>& cameraService,
        const sp<ICameraDeviceCallbacks>& remoteCallback,
        const String16& clientPackageName,
        int cameraId,
        int cameraFacing,
        int clientPid,
        uid_t clientUid,
        int servicePid) :
    BasicClient(cameraService,
            IInterface::asBinder(remoteCallback),
            clientPackageName,
            cameraId,
            cameraFacing,
            clientPid,
            clientUid,
            servicePid),
    mRemoteCallback(remoteCallback) {
}

// Interface used by CameraService

CameraDeviceClient::CameraDeviceClient(const sp<CameraService>& cameraService,
                                   const sp<ICameraDeviceCallbacks>& remoteCallback,
                                   const String16& clientPackageName,
                                   int cameraId,
                                   int cameraFacing,
                                   int clientPid,
                                   uid_t clientUid,
                                   int servicePid) :
    Camera2ClientBase(cameraService, remoteCallback, clientPackageName,
                cameraId, cameraFacing, clientPid, clientUid, servicePid),
    mInputStream(),
    mRequestIdCounter(0) {

    ATRACE_CALL();
    ALOGI("CameraDeviceClient %d: Opened", cameraId);
}

status_t CameraDeviceClient::initialize(CameraModule *module)
{
    ATRACE_CALL();
    status_t res;

    res = Camera2ClientBase::initialize(module);
    if (res != OK) {
        return res;
    }

    String8 threadName;
    mFrameProcessor = new FrameProcessorBase(mDevice);
    threadName = String8::format("CDU-%d-FrameProc", mCameraId);
    mFrameProcessor->run(threadName.string());

    mFrameProcessor->registerListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                      FRAME_PROCESSOR_LISTENER_MAX_ID,
                                      /*listener*/this,
                                      /*sendPartials*/true);

    return OK;
}

CameraDeviceClient::~CameraDeviceClient() {
}

status_t CameraDeviceClient::submitRequest(sp<CaptureRequest> request,
                                         bool streaming,
                                         /*out*/
                                         int64_t* lastFrameNumber) {
    List<sp<CaptureRequest> > requestList;
    requestList.push_back(request);
    return submitRequestList(requestList, streaming, lastFrameNumber);
}

status_t CameraDeviceClient::submitRequestList(List<sp<CaptureRequest> > requests,
                                               bool streaming, int64_t* lastFrameNumber) {
    ATRACE_CALL();
    ALOGV("%s-start of function. Request list size %zu", __FUNCTION__, requests.size());

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    if (requests.empty()) {
        ALOGE("%s: Camera %d: Sent null request. Rejecting request.",
              __FUNCTION__, mCameraId);
        return BAD_VALUE;
    }

    List<const CameraMetadata> metadataRequestList;
    int32_t requestId = mRequestIdCounter;
    uint32_t loopCounter = 0;

    for (List<sp<CaptureRequest> >::iterator it = requests.begin(); it != requests.end(); ++it) {
        sp<CaptureRequest> request = *it;
        if (request == 0) {
            ALOGE("%s: Camera %d: Sent null request.",
                    __FUNCTION__, mCameraId);
            return BAD_VALUE;
        } else if (request->mIsReprocess) {
            if (!mInputStream.configured) {
                ALOGE("%s: Camera %d: no input stream is configured.", __FUNCTION__, mCameraId);
                return BAD_VALUE;
            } else if (streaming) {
                ALOGE("%s: Camera %d: streaming reprocess requests not supported.", __FUNCTION__,
                        mCameraId);
                return BAD_VALUE;
            }
        }

        CameraMetadata metadata(request->mMetadata);
        if (metadata.isEmpty()) {
            ALOGE("%s: Camera %d: Sent empty metadata packet. Rejecting request.",
                   __FUNCTION__, mCameraId);
            return BAD_VALUE;
        } else if (request->mSurfaceList.isEmpty()) {
            ALOGE("%s: Camera %d: Requests must have at least one surface target. "
                  "Rejecting request.", __FUNCTION__, mCameraId);
            return BAD_VALUE;
        }

        if (!enforceRequestPermissions(metadata)) {
            // Callee logs
            return PERMISSION_DENIED;
        }

        /**
         * Write in the output stream IDs which we calculate from
         * the capture request's list of surface targets
         */
        Vector<int32_t> outputStreamIds;
        outputStreamIds.setCapacity(request->mSurfaceList.size());
        for (size_t i = 0; i < request->mSurfaceList.size(); ++i) {
            sp<Surface> surface = request->mSurfaceList[i];
            if (surface == 0) continue;

            sp<IGraphicBufferProducer> gbp = surface->getIGraphicBufferProducer();
            int idx = mStreamMap.indexOfKey(IInterface::asBinder(gbp));

            // Trying to submit request with surface that wasn't created
            if (idx == NAME_NOT_FOUND) {
                ALOGE("%s: Camera %d: Tried to submit a request with a surface that"
                      " we have not called createStream on",
                      __FUNCTION__, mCameraId);
                return BAD_VALUE;
            }

            int streamId = mStreamMap.valueAt(idx);
            outputStreamIds.push_back(streamId);
            ALOGV("%s: Camera %d: Appending output stream %d to request",
                  __FUNCTION__, mCameraId, streamId);
        }

        metadata.update(ANDROID_REQUEST_OUTPUT_STREAMS, &outputStreamIds[0],
                        outputStreamIds.size());

        if (request->mIsReprocess) {
            metadata.update(ANDROID_REQUEST_INPUT_STREAMS, &mInputStream.id, 1);
        }

        metadata.update(ANDROID_REQUEST_ID, &requestId, /*size*/1);
        loopCounter++; // loopCounter starts from 1
        ALOGV("%s: Camera %d: Creating request with ID %d (%d of %zu)",
              __FUNCTION__, mCameraId, requestId, loopCounter, requests.size());

        metadataRequestList.push_back(metadata);
    }
    mRequestIdCounter++;

    if (streaming) {
        res = mDevice->setStreamingRequestList(metadataRequestList, lastFrameNumber);
        if (res != OK) {
            ALOGE("%s: Camera %d:  Got error %d after trying to set streaming "
                  "request", __FUNCTION__, mCameraId, res);
        } else {
            mStreamingRequestList.push_back(requestId);
        }
    } else {
        res = mDevice->captureList(metadataRequestList, lastFrameNumber);
        if (res != OK) {
            ALOGE("%s: Camera %d: Got error %d after trying to set capture",
                __FUNCTION__, mCameraId, res);
        }
        ALOGV("%s: requestId = %d ", __FUNCTION__, requestId);
    }

    ALOGV("%s: Camera %d: End of function", __FUNCTION__, mCameraId);
    if (res == OK) {
        return requestId;
    }

    return res;
}

status_t CameraDeviceClient::cancelRequest(int requestId, int64_t* lastFrameNumber) {
    ATRACE_CALL();
    ALOGV("%s, requestId = %d", __FUNCTION__, requestId);

    status_t res;

    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    Vector<int>::iterator it, end;
    for (it = mStreamingRequestList.begin(), end = mStreamingRequestList.end();
         it != end; ++it) {
        if (*it == requestId) {
            break;
        }
    }

    if (it == end) {
        ALOGE("%s: Camera%d: Did not find request id %d in list of streaming "
              "requests", __FUNCTION__, mCameraId, requestId);
        return BAD_VALUE;
    }

    res = mDevice->clearStreamingRequest(lastFrameNumber);

    if (res == OK) {
        ALOGV("%s: Camera %d: Successfully cleared streaming request",
              __FUNCTION__, mCameraId);
        mStreamingRequestList.erase(it);
    }

    return res;
}

status_t CameraDeviceClient::beginConfigure() {
    // TODO: Implement this.
    ALOGV("%s: Not implemented yet.", __FUNCTION__);
    return OK;
}

status_t CameraDeviceClient::endConfigure(bool isConstrainedHighSpeed) {
    ALOGV("%s: ending configure (%d input stream, %zu output streams)",
            __FUNCTION__, mInputStream.configured ? 1 : 0, mStreamMap.size());

    // Sanitize the high speed session against necessary capability bit.
    if (isConstrainedHighSpeed) {
        CameraMetadata staticInfo = mDevice->info();
        camera_metadata_entry_t entry = staticInfo.find(ANDROID_REQUEST_AVAILABLE_CAPABILITIES);
        bool isConstrainedHighSpeedSupported = false;
        for(size_t i = 0; i < entry.count; ++i) {
            uint8_t capability = entry.data.u8[i];
            if (capability == ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) {
                isConstrainedHighSpeedSupported = true;
                break;
            }
        }
        if (!isConstrainedHighSpeedSupported) {
            ALOGE("%s: Camera %d: Try to create a constrained high speed configuration on a device"
                    " that doesn't support it.",
                          __FUNCTION__, mCameraId);
            return INVALID_OPERATION;
        }
    }

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    return mDevice->configureStreams(isConstrainedHighSpeed);
}

status_t CameraDeviceClient::deleteStream(int streamId) {
    ATRACE_CALL();
    ALOGV("%s (streamId = 0x%x)", __FUNCTION__, streamId);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    bool isInput = false;
    ssize_t index = NAME_NOT_FOUND;

    if (mInputStream.configured && mInputStream.id == streamId) {
        isInput = true;
    } else {
        // Guard against trying to delete non-created streams
        for (size_t i = 0; i < mStreamMap.size(); ++i) {
            if (streamId == mStreamMap.valueAt(i)) {
                index = i;
                break;
            }
        }

        if (index == NAME_NOT_FOUND) {
            ALOGW("%s: Camera %d: Invalid stream ID (%d) specified, no stream "
                  "created yet", __FUNCTION__, mCameraId, streamId);
            return BAD_VALUE;
        }
    }

    // Also returns BAD_VALUE if stream ID was not valid
    res = mDevice->deleteStream(streamId);

    if (res == BAD_VALUE) {
        ALOGE("%s: Camera %d: Unexpected BAD_VALUE when deleting stream, but we"
              " already checked and the stream ID (%d) should be valid.",
              __FUNCTION__, mCameraId, streamId);
    } else if (res == OK) {
        if (isInput) {
            mInputStream.configured = false;
        } else {
            mStreamMap.removeItemsAt(index);
        }
    }

    return res;
}

status_t CameraDeviceClient::createStream(const OutputConfiguration &outputConfiguration)
{
    ATRACE_CALL();

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);


    sp<IGraphicBufferProducer> bufferProducer = outputConfiguration.getGraphicBufferProducer();
    if (bufferProducer == NULL) {
        ALOGE("%s: bufferProducer must not be null", __FUNCTION__);
        return BAD_VALUE;
    }
    if (!mDevice.get()) return DEAD_OBJECT;

    // Don't create multiple streams for the same target surface
    {
        ssize_t index = mStreamMap.indexOfKey(IInterface::asBinder(bufferProducer));
        if (index != NAME_NOT_FOUND) {
            ALOGW("%s: Camera %d: Buffer producer already has a stream for it "
                  "(ID %zd)",
                  __FUNCTION__, mCameraId, index);
            return ALREADY_EXISTS;
        }
    }

    // HACK b/10949105
    // Query consumer usage bits to set async operation mode for
    // GLConsumer using controlledByApp parameter.
    bool useAsync = false;
    int32_t consumerUsage;
    if ((res = bufferProducer->query(NATIVE_WINDOW_CONSUMER_USAGE_BITS,
            &consumerUsage)) != OK) {
        ALOGE("%s: Camera %d: Failed to query consumer usage", __FUNCTION__,
              mCameraId);
        return res;
    }
    if (consumerUsage & GraphicBuffer::USAGE_HW_TEXTURE) {
        ALOGW("%s: Camera %d: Forcing asynchronous mode for stream",
                __FUNCTION__, mCameraId);
        useAsync = true;
    }

    int32_t disallowedFlags = GraphicBuffer::USAGE_HW_VIDEO_ENCODER |
                              GRALLOC_USAGE_RENDERSCRIPT;
    int32_t allowedFlags = GraphicBuffer::USAGE_SW_READ_MASK |
                           GraphicBuffer::USAGE_HW_TEXTURE |
                           GraphicBuffer::USAGE_HW_COMPOSER;
    bool flexibleConsumer = (consumerUsage & disallowedFlags) == 0 &&
            (consumerUsage & allowedFlags) != 0;

    sp<IBinder> binder = IInterface::asBinder(bufferProducer);
    sp<Surface> surface = new Surface(bufferProducer, useAsync);
    ANativeWindow *anw = surface.get();

    int width, height, format;
    android_dataspace dataSpace;

    if ((res = anw->query(anw, NATIVE_WINDOW_WIDTH, &width)) != OK) {
        ALOGE("%s: Camera %d: Failed to query Surface width", __FUNCTION__,
              mCameraId);
        return res;
    }
    if ((res = anw->query(anw, NATIVE_WINDOW_HEIGHT, &height)) != OK) {
        ALOGE("%s: Camera %d: Failed to query Surface height", __FUNCTION__,
              mCameraId);
        return res;
    }
    if ((res = anw->query(anw, NATIVE_WINDOW_FORMAT, &format)) != OK) {
        ALOGE("%s: Camera %d: Failed to query Surface format", __FUNCTION__,
              mCameraId);
        return res;
    }
    if ((res = anw->query(anw, NATIVE_WINDOW_DEFAULT_DATASPACE,
                            reinterpret_cast<int*>(&dataSpace))) != OK) {
        ALOGE("%s: Camera %d: Failed to query Surface dataSpace", __FUNCTION__,
              mCameraId);
        return res;
    }

    // FIXME: remove this override since the default format should be
    //       IMPLEMENTATION_DEFINED. b/9487482
    if (format >= HAL_PIXEL_FORMAT_RGBA_8888 &&
        format <= HAL_PIXEL_FORMAT_BGRA_8888) {
        ALOGW("%s: Camera %d: Overriding format %#x to IMPLEMENTATION_DEFINED",
              __FUNCTION__, mCameraId, format);
        format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    }

    // Round dimensions to the nearest dimensions available for this format
    if (flexibleConsumer && !CameraDeviceClient::roundBufferDimensionNearest(width, height,
            format, dataSpace, mDevice->info(), /*out*/&width, /*out*/&height)) {
        ALOGE("%s: No stream configurations with the format %#x defined, failed to create stream.",
                __FUNCTION__, format);
        return BAD_VALUE;
    }

    int streamId = -1;
    res = mDevice->createStream(surface, width, height, format, dataSpace,
                                static_cast<camera3_stream_rotation_t>
                                        (outputConfiguration.getRotation()),
                                &streamId);

    if (res == OK) {
        mStreamMap.add(binder, streamId);

        ALOGV("%s: Camera %d: Successfully created a new stream ID %d",
              __FUNCTION__, mCameraId, streamId);

        /**
         * Set the stream transform flags to automatically
         * rotate the camera stream for preview use cases.
         */
        int32_t transform = 0;
        res = getRotationTransformLocked(&transform);

        if (res != OK) {
            // Error logged by getRotationTransformLocked.
            return res;
        }

        res = mDevice->setStreamTransform(streamId, transform);
        if (res != OK) {
            ALOGE("%s: Failed to set stream transform (stream id %d)",
                  __FUNCTION__, streamId);
            return res;
        }

        return streamId;
    }

    return res;
}


status_t CameraDeviceClient::createInputStream(int width, int height,
        int format) {

    ATRACE_CALL();
    ALOGV("%s (w = %d, h = %d, f = 0x%x)", __FUNCTION__, width, height, format);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);
    if (!mDevice.get()) return DEAD_OBJECT;

    if (mInputStream.configured) {
        ALOGE("%s: Camera %d: Already has an input stream "
                " configuration. (ID %zd)", __FUNCTION__, mCameraId,
                mInputStream.id);
        return ALREADY_EXISTS;
    }

    int streamId = -1;
    res = mDevice->createInputStream(width, height, format, &streamId);
    if (res == OK) {
        mInputStream.configured = true;
        mInputStream.width = width;
        mInputStream.height = height;
        mInputStream.format = format;
        mInputStream.id = streamId;

        ALOGV("%s: Camera %d: Successfully created a new input stream ID %d",
              __FUNCTION__, mCameraId, streamId);

        return streamId;
    }

    return res;
}

status_t CameraDeviceClient::getInputBufferProducer(
        /*out*/sp<IGraphicBufferProducer> *producer) {
    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    if (producer == NULL) {
        return BAD_VALUE;
    }

    Mutex::Autolock icl(mBinderSerializationLock);
    if (!mDevice.get()) return DEAD_OBJECT;

    return mDevice->getInputBufferProducer(producer);
}

bool CameraDeviceClient::roundBufferDimensionNearest(int32_t width, int32_t height,
        int32_t format, android_dataspace dataSpace, const CameraMetadata& info,
        /*out*/int32_t* outWidth, /*out*/int32_t* outHeight) {

    camera_metadata_ro_entry streamConfigs =
            (dataSpace == HAL_DATASPACE_DEPTH) ?
            info.find(ANDROID_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS) :
            info.find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);

    int32_t bestWidth = -1;
    int32_t bestHeight = -1;

    // Iterate through listed stream configurations and find the one with the smallest euclidean
    // distance from the given dimensions for the given format.
    for (size_t i = 0; i < streamConfigs.count; i += 4) {
        int32_t fmt = streamConfigs.data.i32[i];
        int32_t w = streamConfigs.data.i32[i + 1];
        int32_t h = streamConfigs.data.i32[i + 2];

        // Ignore input/output type for now
        if (fmt == format) {
            if (w == width && h == height) {
                bestWidth = width;
                bestHeight = height;
                break;
            } else if (w <= ROUNDING_WIDTH_CAP && (bestWidth == -1 ||
                    CameraDeviceClient::euclidDistSquare(w, h, width, height) <
                    CameraDeviceClient::euclidDistSquare(bestWidth, bestHeight, width, height))) {
                bestWidth = w;
                bestHeight = h;
            }
        }
    }

    if (bestWidth == -1) {
        // Return false if no configurations for this format were listed
        return false;
    }

    // Set the outputs to the closet width/height
    if (outWidth != NULL) {
        *outWidth = bestWidth;
    }
    if (outHeight != NULL) {
        *outHeight = bestHeight;
    }

    // Return true if at least one configuration for this format was listed
    return true;
}

int64_t CameraDeviceClient::euclidDistSquare(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int64_t d0 = x0 - x1;
    int64_t d1 = y0 - y1;
    return d0 * d0 + d1 * d1;
}

// Create a request object from a template.
status_t CameraDeviceClient::createDefaultRequest(int templateId,
                                                  /*out*/
                                                  CameraMetadata* request)
{
    ATRACE_CALL();
    ALOGV("%s (templateId = 0x%x)", __FUNCTION__, templateId);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    CameraMetadata metadata;
    if ( (res = mDevice->createDefaultRequest(templateId, &metadata) ) == OK &&
        request != NULL) {

        request->swap(metadata);
    }

    return res;
}

status_t CameraDeviceClient::getCameraInfo(/*out*/CameraMetadata* info)
{
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;

    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    if (info != NULL) {
        *info = mDevice->info(); // static camera metadata
        // TODO: merge with device-specific camera metadata
    }

    return res;
}

status_t CameraDeviceClient::waitUntilIdle()
{
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    // FIXME: Also need check repeating burst.
    if (!mStreamingRequestList.isEmpty()) {
        ALOGE("%s: Camera %d: Try to waitUntilIdle when there are active streaming requests",
              __FUNCTION__, mCameraId);
        return INVALID_OPERATION;
    }
    res = mDevice->waitUntilDrained();
    ALOGV("%s Done", __FUNCTION__);

    return res;
}

status_t CameraDeviceClient::flush(int64_t* lastFrameNumber) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    if (!mDevice.get()) return DEAD_OBJECT;

    mStreamingRequestList.clear();
    return mDevice->flush(lastFrameNumber);
}

status_t CameraDeviceClient::prepare(int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        ALOGW("%s: Camera %d: Invalid stream ID (%d) specified, no stream "
              "created yet", __FUNCTION__, mCameraId, streamId);
        return BAD_VALUE;
    }

    // Also returns BAD_VALUE if stream ID was not valid, or stream already
    // has been used
    res = mDevice->prepare(streamId);

    return res;
}

status_t CameraDeviceClient::prepare2(int maxCount, int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        ALOGW("%s: Camera %d: Invalid stream ID (%d) specified, no stream created yet",
                __FUNCTION__, mCameraId, streamId);
        return BAD_VALUE;
    }

    if (maxCount <= 0) {
        ALOGE("%s: Camera %d: Invalid maxCount (%d) specified, must be greater than 0.",
                __FUNCTION__, mCameraId, maxCount);
        return BAD_VALUE;
    }

    // Also returns BAD_VALUE if stream ID was not valid, or stream already
    // has been used
    res = mDevice->prepare(maxCount, streamId);

    return res;
}

status_t CameraDeviceClient::tearDown(int streamId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    status_t res = OK;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mBinderSerializationLock);

    // Guard against trying to prepare non-created streams
    ssize_t index = NAME_NOT_FOUND;
    for (size_t i = 0; i < mStreamMap.size(); ++i) {
        if (streamId == mStreamMap.valueAt(i)) {
            index = i;
            break;
        }
    }

    if (index == NAME_NOT_FOUND) {
        ALOGW("%s: Camera %d: Invalid stream ID (%d) specified, no stream "
              "created yet", __FUNCTION__, mCameraId, streamId);
        return BAD_VALUE;
    }

    // Also returns BAD_VALUE if stream ID was not valid or if the stream is in
    // use
    res = mDevice->tearDown(streamId);

    return res;
}

status_t CameraDeviceClient::dump(int fd, const Vector<String16>& args) {
    return BasicClient::dump(fd, args);
}

status_t CameraDeviceClient::dumpClient(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("CameraDeviceClient[%d] (%p) dump:\n",
            mCameraId,
            (getRemoteCallback() != NULL ?
                    IInterface::asBinder(getRemoteCallback()).get() : NULL) );
    result.appendFormat("  Current client UID %u\n", mClientUid);

    result.append("  State:\n");
    result.appendFormat("    Request ID counter: %d\n", mRequestIdCounter);
    if (mInputStream.configured) {
        result.appendFormat("    Current input stream ID: %d\n",
                    mInputStream.id);
    } else {
        result.append("    No input stream configured.\n");
    }
    if (!mStreamMap.isEmpty()) {
        result.append("    Current output stream IDs:\n");
        for (size_t i = 0; i < mStreamMap.size(); i++) {
            result.appendFormat("      Stream %d\n", mStreamMap.valueAt(i));
        }
    } else {
        result.append("    No output streams configured.\n");
    }
    write(fd, result.string(), result.size());
    // TODO: print dynamic/request section from most recent requests
    mFrameProcessor->dump(fd, args);

    return dumpDevice(fd, args);
}

void CameraDeviceClient::notifyError(ICameraDeviceCallbacks::CameraErrorCode errorCode,
                                     const CaptureResultExtras& resultExtras) {
    // Thread safe. Don't bother locking.
    sp<ICameraDeviceCallbacks> remoteCb = getRemoteCallback();

    if (remoteCb != 0) {
        remoteCb->onDeviceError(errorCode, resultExtras);
    }
}

void CameraDeviceClient::notifyIdle() {
    // Thread safe. Don't bother locking.
    sp<ICameraDeviceCallbacks> remoteCb = getRemoteCallback();

    if (remoteCb != 0) {
        remoteCb->onDeviceIdle();
    }
    Camera2ClientBase::notifyIdle();
}

void CameraDeviceClient::notifyShutter(const CaptureResultExtras& resultExtras,
        nsecs_t timestamp) {
    // Thread safe. Don't bother locking.
    sp<ICameraDeviceCallbacks> remoteCb = getRemoteCallback();
    if (remoteCb != 0) {
        remoteCb->onCaptureStarted(resultExtras, timestamp);
    }
    Camera2ClientBase::notifyShutter(resultExtras, timestamp);
}

void CameraDeviceClient::notifyPrepared(int streamId) {
    // Thread safe. Don't bother locking.
    sp<ICameraDeviceCallbacks> remoteCb = getRemoteCallback();
    if (remoteCb != 0) {
        remoteCb->onPrepared(streamId);
    }
}

void CameraDeviceClient::detachDevice() {
    if (mDevice == 0) return;

    ALOGV("Camera %d: Stopping processors", mCameraId);

    mFrameProcessor->removeListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                    FRAME_PROCESSOR_LISTENER_MAX_ID,
                                    /*listener*/this);
    mFrameProcessor->requestExit();
    ALOGV("Camera %d: Waiting for threads", mCameraId);
    mFrameProcessor->join();
    ALOGV("Camera %d: Disconnecting device", mCameraId);

    // WORKAROUND: HAL refuses to disconnect while there's streams in flight
    {
        mDevice->clearStreamingRequest();

        status_t code;
        if ((code = mDevice->waitUntilDrained()) != OK) {
            ALOGE("%s: waitUntilDrained failed with code 0x%x", __FUNCTION__,
                  code);
        }
    }

    Camera2ClientBase::detachDevice();
}

/** Device-related methods */
void CameraDeviceClient::onResultAvailable(const CaptureResult& result) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    // Thread-safe. No lock necessary.
    sp<ICameraDeviceCallbacks> remoteCb = mRemoteCallback;
    if (remoteCb != NULL) {
        remoteCb->onResultReceived(result.mMetadata, result.mResultExtras);
    }
}

// TODO: move to Camera2ClientBase
bool CameraDeviceClient::enforceRequestPermissions(CameraMetadata& metadata) {

    const int pid = IPCThreadState::self()->getCallingPid();
    const int selfPid = getpid();
    camera_metadata_entry_t entry;

    /**
     * Mixin default important security values
     * - android.led.transmit = defaulted ON
     */
    CameraMetadata staticInfo = mDevice->info();
    entry = staticInfo.find(ANDROID_LED_AVAILABLE_LEDS);
    for(size_t i = 0; i < entry.count; ++i) {
        uint8_t led = entry.data.u8[i];

        switch(led) {
            case ANDROID_LED_AVAILABLE_LEDS_TRANSMIT: {
                uint8_t transmitDefault = ANDROID_LED_TRANSMIT_ON;
                if (!metadata.exists(ANDROID_LED_TRANSMIT)) {
                    metadata.update(ANDROID_LED_TRANSMIT,
                                    &transmitDefault, 1);
                }
                break;
            }
        }
    }

    // We can do anything!
    if (pid == selfPid) {
        return true;
    }

    /**
     * Permission check special fields in the request
     * - android.led.transmit = android.permission.CAMERA_DISABLE_TRANSMIT
     */
    entry = metadata.find(ANDROID_LED_TRANSMIT);
    if (entry.count > 0 && entry.data.u8[0] != ANDROID_LED_TRANSMIT_ON) {
        String16 permissionString =
            String16("android.permission.CAMERA_DISABLE_TRANSMIT_LED");
        if (!checkCallingPermission(permissionString)) {
            const int uid = IPCThreadState::self()->getCallingUid();
            ALOGE("Permission Denial: "
                  "can't disable transmit LED pid=%d, uid=%d", pid, uid);
            return false;
        }
    }

    return true;
}

status_t CameraDeviceClient::getRotationTransformLocked(int32_t* transform) {
    ALOGV("%s: begin", __FUNCTION__);

    const CameraMetadata& staticInfo = mDevice->info();
    return CameraUtils::getRotationTransform(staticInfo, transform);
}

} // namespace android
