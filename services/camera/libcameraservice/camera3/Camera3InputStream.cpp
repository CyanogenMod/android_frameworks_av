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

#define LOG_TAG "Camera3-InputStream"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include "Camera3InputStream.h"

namespace android {

namespace camera3 {

Camera3InputStream::Camera3InputStream(int id,
        uint32_t width, uint32_t height, int format) :
        Camera3Stream(id, CAMERA3_STREAM_INPUT, width, height, 0, format) {
}

status_t Camera3InputStream::getBufferLocked(camera3_stream_buffer *buffer) {
    (void) buffer;
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3InputStream::returnBufferLocked(
        const camera3_stream_buffer &buffer,
        nsecs_t timestamp) {
    (void) timestamp;
    (void) buffer;
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

bool Camera3InputStream::hasOutstandingBuffersLocked() const {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return false;
}

status_t Camera3InputStream::waitUntilIdle(nsecs_t timeout) {
    (void) timeout;
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3InputStream::disconnectLocked() {
    ALOGE("%s: Not implemented", __FUNCTION__);
    return INVALID_OPERATION;
}

sp<IGraphicBufferProducer> Camera3InputStream::getProducerInterface() const {
    return mConsumer->getProducerInterface();
}

void Camera3InputStream::dump(int fd, const Vector<String16> &args) const {
    (void) fd;
    (void) args;
    ALOGE("%s: Not implemented", __FUNCTION__);
}

}; // namespace camera3

}; // namespace android
