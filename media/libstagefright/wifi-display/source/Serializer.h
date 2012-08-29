/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SERIALIZER_H_

#define SERIALIZER_H_

#include <media/stagefright/foundation/AHandler.h>
#include <utils/Vector.h>

namespace android {

struct AMessage;
struct MediaSource;

// After adding a number of MediaSource objects and starting the Serializer,
// it'll emit their access units in order of increasing timestamps.
struct Serializer : public AHandler {
    enum {
        kWhatEOS,
        kWhatAccessUnit
    };

    // In throttled operation, data is emitted at a pace corresponding
    // to the incoming media timestamps.
    Serializer(bool throttle, const sp<AMessage> &notify);

    ssize_t addSource(const sp<MediaSource> &source);

    status_t start();
    status_t stop();

protected:
    virtual void onMessageReceived(const sp<AMessage> &what);
    virtual ~Serializer();

private:
    enum {
        kWhatAddSource,
        kWhatStart,
        kWhatStop,
        kWhatPoll
    };

    struct Track;

    bool mThrottle;
    sp<AMessage> mNotify;
    Vector<sp<Track> > mTracks;

    int32_t mPollGeneration;

    int64_t mStartTimeUs;

    status_t postSynchronouslyAndReturnError(const sp<AMessage> &msg);

    ssize_t onAddSource(const sp<AMessage> &msg);
    status_t onStart();
    status_t onStop();
    int64_t onPoll();

    void schedulePoll(int64_t delayUs = 0ll);
    void cancelPoll();

    DISALLOW_EVIL_CONSTRUCTORS(Serializer);
};

}  // namespace android

#endif  // SERIALIZER_H_

