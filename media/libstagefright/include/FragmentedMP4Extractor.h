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

#ifndef FRAGMENTED_MP4_EXTRACTOR_H_

#define FRAGMENTED_MP4_EXTRACTOR_H_

#include "include/FragmentedMP4Parser.h"

#include <media/stagefright/MediaExtractor.h>
#include <utils/Vector.h>
#include <utils/String8.h>

namespace android {

struct AMessage;
class DataSource;
class SampleTable;
class String8;

class FragmentedMP4Extractor : public MediaExtractor {
public:
    // Extractor assumes ownership of "source".
    FragmentedMP4Extractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);
    virtual sp<MetaData> getMetaData();
    virtual uint32_t flags() const;

protected:
    virtual ~FragmentedMP4Extractor();

private:
    sp<ALooper> mLooper;
    sp<FragmentedMP4Parser> mParser;
    sp<DataSource> mDataSource;
    status_t mInitCheck;
    size_t mAudioTrackIndex;
    size_t mTrackCount;

    sp<MetaData> mFileMetaData;

    Vector<uint32_t> mPath;

    FragmentedMP4Extractor(const FragmentedMP4Extractor &);
    FragmentedMP4Extractor &operator=(const FragmentedMP4Extractor &);
};

bool SniffFragmentedMP4(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *);

}  // namespace android

#endif  // MPEG4_EXTRACTOR_H_
