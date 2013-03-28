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

#ifndef ANDROID_SERVERS_CAMERA3_ZSL_STREAM_H
#define ANDROID_SERVERS_CAMERA3_ZSL_STREAM_H

#include <utils/RefBase.h>
#include <gui/Surface.h>

#include "Camera3Stream.h"

namespace android {

namespace camera3 {

/**
 * A class for managing a single opaque ZSL stream to/from the camera device.
 * This acts as a bidirectional stream at the HAL layer, caching and discarding
 * most output buffers, and when directed, pushes a buffer back to the HAL for
 * processing.
 */
class Camera3ZslStream: public Camera3Stream {
  public:
    /**
     * Set up a ZSL stream of a given resolution. Depth is the number of buffers
     * cached within the stream that can be retrieved for input.
     */
    Camera3ZslStream(int id, uint32_t width, uint32_t height, int depth);

    virtual status_t waitUntilIdle(nsecs_t timeout);
    virtual void     dump(int fd, const Vector<String16> &args) const;

    /**
     * Get an input buffer matching a specific timestamp. If no buffer matching
     * the timestamp is available, NO_MEMORY is returned.
     */
    status_t getInputBuffer(camera3_stream_buffer *buffer, nsecs_t timestamp);

    /**
     * Return input buffer from HAL. The buffer is then marked as unfilled, and
     * returned to the output-side stream for refilling.
     */
    status_t returnInputBuffer(const camera3_stream_buffer &buffer);

  private:

    int mDepth;

    /**
     * Camera3Stream interface
     */

    // getBuffer/returnBuffer operate the output stream side of the ZslStream.
    virtual status_t getBufferLocked(camera3_stream_buffer *buffer);
    virtual status_t returnBufferLocked(const camera3_stream_buffer &buffer,
            nsecs_t timestamp);
    virtual bool     hasOutstandingBuffersLocked() const;
    virtual status_t disconnectLocked();

}; // class Camera3ZslStream

}; // namespace camera3

}; // namespace android

#endif
