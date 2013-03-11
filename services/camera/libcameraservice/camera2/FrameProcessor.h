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
#include <utils/List.h>
#include <camera/CameraMetadata.h>

#include "ProFrameProcessor.h"

struct camera_frame_metadata;

namespace android {

class Camera2Client;

namespace camera2 {

/* Output frame metadata processing thread.  This thread waits for new
 * frames from the device, and analyzes them as necessary.
 */
class FrameProcessor : public ProFrameProcessor {
  public:
    FrameProcessor(wp<CameraDeviceBase> device, wp<Camera2Client> client);
    ~FrameProcessor();

  private:
    wp<Camera2Client> mClient;
    int mLastFrameNumberOfFaces;

    void processNewFrames(const sp<Camera2Client> &client);

    virtual bool processSingleFrame(CameraMetadata &frame,
                                    const sp<CameraDeviceBase> &device);

    status_t processFaceDetect(const CameraMetadata &frame,
            const sp<Camera2Client> &client);

    // Emit FaceDetection event to java if faces changed
    void callbackFaceDetection(sp<Camera2Client> client,
                               const camera_frame_metadata &metadata);
};


}; //namespace camera2
}; //namespace android

#endif
