/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef NUPLAYER_CCDECODER_H_

#define NUPLAYER_CCDECODER_H_

#include "NuPlayer.h"

namespace android {

struct NuPlayer::CCDecoder : public RefBase {
    enum {
        kWhatClosedCaptionData,
        kWhatTrackAdded,
    };

    CCDecoder(const sp<AMessage> &notify);

    size_t getTrackCount() const;
    sp<AMessage> getTrackInfo(size_t index) const;
    status_t selectTrack(size_t index, bool select);
    bool isSelected() const;
    void decode(const sp<ABuffer> &accessUnit);
    void display(int64_t timeUs);
    void flush();

private:
    sp<AMessage> mNotify;
    KeyedVector<int64_t, sp<ABuffer> > mCCMap;
    size_t mCurrentChannel;
    int32_t mSelectedTrack;
    int32_t mTrackIndices[4];
    Vector<size_t> mFoundChannels;

    bool isTrackValid(size_t index) const;
    int32_t getTrackIndex(size_t channel) const;
    bool extractFromSEI(const sp<ABuffer> &accessUnit);
    bool parseSEINalUnit(int64_t timeUs, const uint8_t *nalStart, size_t nalSize);
    sp<ABuffer> filterCCBuf(const sp<ABuffer> &ccBuf, size_t index);

    DISALLOW_EVIL_CONSTRUCTORS(CCDecoder);
};

}  // namespace android

#endif  // NUPLAYER_CCDECODER_H_
