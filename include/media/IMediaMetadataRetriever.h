/*
**
** Copyright (C) 2008 The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_IMEDIAMETADATARETRIEVER_H
#define ANDROID_IMEDIAMETADATARETRIEVER_H

#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <binder/IMemory.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>

namespace android {

struct IMediaHTTPService;

class IMediaMetadataRetriever: public IInterface
{
public:
    DECLARE_META_INTERFACE(MediaMetadataRetriever);
    virtual void            disconnect() = 0;

    virtual status_t        setDataSource(
            const sp<IMediaHTTPService> &httpService,
            const char *srcUrl,
            const KeyedVector<String8, String8> *headers = NULL) = 0;

    virtual status_t        setDataSource(int fd, int64_t offset, int64_t length) = 0;
    virtual sp<IMemory>     getFrameAtTime(int64_t timeUs, int option) = 0;
    virtual sp<IMemory>     extractAlbumArt() = 0;
    virtual const char*     extractMetadata(int keyCode) = 0;
};

// ----------------------------------------------------------------------------

class BnMediaMetadataRetriever: public BnInterface<IMediaMetadataRetriever>
{
public:
    virtual status_t    onTransact(uint32_t code,
                                   const Parcel& data,
                                   Parcel* reply,
                                   uint32_t flags = 0);
};

}; // namespace android

#endif // ANDROID_IMEDIAMETADATARETRIEVER_H
