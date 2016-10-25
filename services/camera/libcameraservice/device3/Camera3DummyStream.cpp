/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define LOG_TAG "Camera3-DummyStream"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include "Camera3DummyStream.h"

namespace android {

namespace camera3 {

Camera3DummyStream::Camera3DummyStream(int id) :
        Camera3IOStreamBase(id, CAMERA3_STREAM_OUTPUT, DUMMY_WIDTH, DUMMY_HEIGHT,
                /*maxSize*/0, DUMMY_FORMAT, DUMMY_DATASPACE, DUMMY_ROTATION) {

}

Camera3DummyStream::~Camera3DummyStream() {

}

status_t Camera3DummyStream::getBufferLocked(camera3_stream_buffer *) {
    ATRACE_CALL();
    ALOGE("%s: Stream %d: Dummy stream cannot produce buffers!", __FUNCTION__, mId);
    return INVALID_OPERATION;
}

status_t Camera3DummyStream::returnBufferLocked(
        const camera3_stream_buffer &,
        nsecs_t) {
    ATRACE_CALL();
    ALOGE("%s: Stream %d: Dummy stream cannot return buffers!", __FUNCTION__, mId);
    return INVALID_OPERATION;
}

status_t Camera3DummyStream::returnBufferCheckedLocked(
            const camera3_stream_buffer &,
            nsecs_t,
            bool,
            /*out*/
            sp<Fence>*) {
    ATRACE_CALL();
    ALOGE("%s: Stream %d: Dummy stream cannot return buffers!", __FUNCTION__, mId);
    return INVALID_OPERATION;
}

void Camera3DummyStream::dump(int fd, const Vector<String16> &args) const {
    (void) args;
    String8 lines;
    lines.appendFormat("    Stream[%d]: Dummy\n", mId);
    write(fd, lines.string(), lines.size());

    Camera3IOStreamBase::dump(fd, args);
}

status_t Camera3DummyStream::setTransform(int) {
    ATRACE_CALL();
    // Do nothing
    return OK;
}

status_t Camera3DummyStream::detachBuffer(sp<GraphicBuffer>* buffer, int* fenceFd) {
    (void) buffer;
    (void) fenceFd;
    // Do nothing
    return OK;
}

status_t Camera3DummyStream::configureQueueLocked() {
    // Do nothing
    return OK;
}

status_t Camera3DummyStream::disconnectLocked() {
    mState = (mState == STATE_IN_RECONFIG) ? STATE_IN_CONFIG
                                           : STATE_CONSTRUCTED;
    return OK;
}

status_t Camera3DummyStream::getEndpointUsage(uint32_t *usage) const {
    *usage = DUMMY_USAGE;
    return OK;
}

bool Camera3DummyStream::isVideoStream() const {
    return false;
}

bool Camera3DummyStream::isConsumerConfigurationDeferred() const {
    return false;
}

status_t Camera3DummyStream::setConsumer(sp<Surface> consumer) {
    ALOGE("%s: Stream %d: Dummy stream doesn't support set consumer surface %p!",
            __FUNCTION__, mId, consumer.get());
    return INVALID_OPERATION;
}
}; // namespace camera3

}; // namespace android
