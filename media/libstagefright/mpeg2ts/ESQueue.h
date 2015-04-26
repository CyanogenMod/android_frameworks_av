/*
 * Copyright (C) 2010 The Android Open Source Project
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
 *
 * This file was modified by Dolby Laboratories, Inc. The portions of the
 * code that are surrounded by "DOLBY..." are copyrighted and
 * licensed separately, as follows:
 *
 *  (C) 2011-2012 Dolby Laboratories, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef ES_QUEUE_H_

#define ES_QUEUE_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/RefBase.h>

namespace android {

struct ABuffer;
struct MetaData;

struct ElementaryStreamQueue {
    enum Mode {
        H264,
        H265,
        AAC,
        AC3,
        MPEG_AUDIO,
        MPEG_VIDEO,
        MPEG4_VIDEO,
        PCM_AUDIO,
#if defined(DOLBY_UDC) && defined(DOLBY_UDC_STREAMING_HLS)
        DDP_EC3_AUDIO,
#endif // DOLBY_END
    };

    enum Flags {
        // Data appended to the queue is always at access unit boundaries.
        kFlag_AlignedData = 1,
    };
    ElementaryStreamQueue(Mode mode, uint32_t flags = 0);

    status_t appendData(const void *data, size_t size, int64_t timeUs);
    void clear(bool clearFormat);

    sp<ABuffer> dequeueAccessUnit();

    sp<MetaData> getFormat();

private:
    struct RangeInfo {
        int64_t mTimestampUs;
        size_t mLength;
    };

    Mode mMode;
    uint32_t mFlags;

    sp<ABuffer> mBuffer;
    List<RangeInfo> mRangeInfos;

    sp<MetaData> mFormat;

    sp<ABuffer> dequeueAccessUnitH264();
    sp<ABuffer> dequeueAccessUnitH265();
    sp<ABuffer> dequeueAccessUnitAAC();
    sp<ABuffer> dequeueAccessUnitAC3();
    sp<ABuffer> dequeueAccessUnitMPEGAudio();
    sp<ABuffer> dequeueAccessUnitMPEGVideo();
    sp<ABuffer> dequeueAccessUnitMPEG4Video();
    sp<ABuffer> dequeueAccessUnitPCMAudio();
#if defined(DOLBY_UDC) && defined(DOLBY_UDC_STREAMING_HLS)
    sp<ABuffer> dequeueAccessUnitDDP();
#endif // DOLBY_END

    // consume a logical (compressed) access unit of size "size",
    // returns its timestamp in us (or -1 if no time information).
    int64_t fetchTimestamp(size_t size);
    int64_t fetchTimestampAAC(size_t size);

    DISALLOW_EVIL_CONSTRUCTORS(ElementaryStreamQueue);
};

}  // namespace android

#endif  // ES_QUEUE_H_
