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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2_FRAMEPROCESSOR_H
#define ANDROID_SERVERS_CAMERA_CAMERA2_FRAMEPROCESSOR_H

#include <utils/Thread.h>
#include <utils/String16.h>
#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include "CameraMetadata.h"

struct camera_frame_metadata;

namespace android {

class Camera2Client;

namespace camera2 {

/* Output frame metadata processing thread.  This thread waits for new
 * frames from the device, and analyzes them as necessary.
 */
class FrameProcessor: public Thread {
  public:
    FrameProcessor(wp<Camera2Client> client);
    ~FrameProcessor();

    struct FilteredListener: virtual public RefBase {
        // Listener may take ownership of frame
        virtual void onFrameAvailable(int32_t frameId, CameraMetadata &frame) = 0;
    };

    // Register a listener for a specific frame ID (android.request.id).
    // De-registers any existing listeners for that ID
    status_t registerListener(int32_t id, wp<FilteredListener> listener);

    status_t removeListener(int32_t id);

    void dump(int fd, const Vector<String16>& args);
  private:
    static const nsecs_t kWaitDuration = 10000000; // 10 ms
    wp<Camera2Client> mClient;

    virtual bool threadLoop();

    Mutex mInputMutex;
    KeyedVector<int32_t, wp<FilteredListener> > mListeners;

    void processNewFrames(sp<Camera2Client> &client);

    status_t processFaceDetect(const CameraMetadata &frame,
            sp<Camera2Client> &client);

    status_t processListener(CameraMetadata &frame,
            sp<Camera2Client> &client);

    CameraMetadata mLastFrame;
    int mLastFrameNumberOfFaces;

    // Emit FaceDetection event to java if faces changed
    void callbackFaceDetection(sp<Camera2Client> client,
                               camera_frame_metadata &metadata);
};


}; //namespace camera2
}; //namespace android

#endif
