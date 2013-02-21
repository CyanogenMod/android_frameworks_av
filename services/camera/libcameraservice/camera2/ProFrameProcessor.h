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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2_PROFRAMEPROCESSOR_H
#define ANDROID_SERVERS_CAMERA_CAMERA2_PROFRAMEPROCESSOR_H

#include <utils/Thread.h>
#include <utils/String16.h>
#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include <utils/List.h>
#include <camera/CameraMetadata.h>

struct camera_frame_metadata;

namespace android {

class ProCamera2Client;

namespace camera2 {

/* Output frame metadata processing thread.  This thread waits for new
 * frames from the device, and analyzes them as necessary.
 */
class ProFrameProcessor: public Thread {
  public:
    ProFrameProcessor(wp<ProCamera2Client> client);
    ~ProFrameProcessor();

    struct FilteredListener: virtual public RefBase {
        virtual void onFrameAvailable(int32_t frameId,
                const CameraMetadata &frame) = 0;
    };

    // Register a listener for a range of IDs [minId, maxId). Multiple listeners
    // can be listening to the same range
    status_t registerListener(int32_t minId, int32_t maxId, wp<FilteredListener> listener);
    status_t removeListener(int32_t minId, int32_t maxId, wp<FilteredListener> listener);

    void dump(int fd, const Vector<String16>& args);
  private:
    static const nsecs_t kWaitDuration = 10000000; // 10 ms
    wp<ProCamera2Client> mClient;

    virtual bool threadLoop();

    Mutex mInputMutex;

    struct RangeListener {
        int32_t minId;
        int32_t maxId;
        wp<FilteredListener> listener;
    };
    List<RangeListener> mRangeListeners;

    void processNewFrames(sp<ProCamera2Client> &client);

    status_t processListeners(const CameraMetadata &frame,
            sp<ProCamera2Client> &client);

    CameraMetadata mLastFrame;
};


}; //namespace camera2
}; //namespace android

#endif
