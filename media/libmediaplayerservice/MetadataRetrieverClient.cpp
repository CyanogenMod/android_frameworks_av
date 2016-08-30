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

//#define LOG_NDEBUG 0
#define LOG_TAG "MetadataRetrieverClient"
#include <utils/Log.h>

#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <string.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <media/IMediaHTTPService.h>
#include <media/MediaMetadataRetrieverInterface.h>
#include <media/MediaPlayerInterface.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/Utils.h>
#include <private/media/VideoFrame.h>
#include "MetadataRetrieverClient.h"
#include "StagefrightMetadataRetriever.h"
#include "MediaPlayerFactory.h"

namespace android {

MetadataRetrieverClient::MetadataRetrieverClient(pid_t pid)
{
    ALOGV("MetadataRetrieverClient constructor pid(%d)", pid);
    mPid = pid;
    mThumbnail = NULL;
    mAlbumArt = NULL;
    mRetriever = NULL;
}

MetadataRetrieverClient::~MetadataRetrieverClient()
{
    ALOGV("MetadataRetrieverClient destructor");
    disconnect();
}

status_t MetadataRetrieverClient::dump(int fd, const Vector<String16>& /*args*/)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append(" MetadataRetrieverClient\n");
    snprintf(buffer, 255, "  pid(%d)\n", mPid);
    result.append(buffer);
    write(fd, result.string(), result.size());
    write(fd, "\n", 1);
    return NO_ERROR;
}

void MetadataRetrieverClient::disconnect()
{
    ALOGV("disconnect from pid %d", mPid);
    Mutex::Autolock lock(mLock);
    mRetriever.clear();
    mThumbnail.clear();
    mAlbumArt.clear();
    IPCThreadState::self()->flushCommands();
}

static sp<MediaMetadataRetrieverBase> createRetriever(player_type playerType)
{
    sp<MediaMetadataRetrieverBase> p;
    switch (playerType) {
        case STAGEFRIGHT_PLAYER:
        case NU_PLAYER:
        {
            p = new StagefrightMetadataRetriever;
            break;
        }
        default:
            // TODO:
            // support for TEST_PLAYER
            ALOGE("player type %d is not supported",  playerType);
            break;
    }
    if (p == NULL) {
        ALOGE("failed to create a retriever object");
    }
    return p;
}

status_t MetadataRetrieverClient::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers)
{
    ALOGV("setDataSource(%s)", url);
    Mutex::Autolock lock(mLock);
    if (url == NULL) {
        return UNKNOWN_ERROR;
    }

    // When asking the MediaPlayerFactory subsystem to choose a media player for
    // a given URL, a pointer to an outer IMediaPlayer can be passed to the
    // factory system to be taken into consideration along with the URL.  In the
    // case of choosing an instance of a MediaPlayerBase for a
    // MetadataRetrieverClient, there is no outer IMediaPlayer which will
    // eventually encapsulate the result of this selection.  In this case, just
    // pass NULL to getPlayerType to indicate that there is no outer
    // IMediaPlayer to consider during selection.
    player_type playerType =
        MediaPlayerFactory::getPlayerType(NULL /* client */, url);
    ALOGV("player type = %d", playerType);
    sp<MediaMetadataRetrieverBase> p = createRetriever(playerType);
    if (p == NULL) return NO_INIT;
    status_t ret = p->setDataSource(httpService, url, headers);
    if (ret == NO_ERROR) mRetriever = p;
    return ret;
}

status_t MetadataRetrieverClient::setDataSource(int fd, int64_t offset, int64_t length)
{
    ALOGV("setDataSource fd=%d, offset=%" PRId64 ", length=%" PRId64 "", fd, offset, length);
    Mutex::Autolock lock(mLock);
    struct stat sb;
    int ret = fstat(fd, &sb);
    if (ret != 0) {
        ALOGE("fstat(%d) failed: %d, %s", fd, ret, strerror(errno));
        return BAD_VALUE;
    }
    ALOGV("st_dev  = %" PRIu64 "", static_cast<uint64_t>(sb.st_dev));
    ALOGV("st_mode = %u", sb.st_mode);
    ALOGV("st_uid  = %lu", static_cast<unsigned long>(sb.st_uid));
    ALOGV("st_gid  = %lu", static_cast<unsigned long>(sb.st_gid));
    ALOGV("st_size = %" PRIu64 "", sb.st_size);

    if (offset >= sb.st_size) {
        ALOGE("offset (%" PRId64 ") bigger than file size (%" PRIu64 ")", offset, sb.st_size);
        return BAD_VALUE;
    }
    if (offset + length > sb.st_size) {
        length = sb.st_size - offset;
        ALOGV("calculated length = %" PRId64 "", length);
    }

    player_type playerType =
        MediaPlayerFactory::getPlayerType(NULL /* client */,
                                          fd,
                                          offset,
                                          length);
    ALOGV("player type = %d", playerType);
    sp<MediaMetadataRetrieverBase> p = createRetriever(playerType);
    if (p == NULL) {
        return NO_INIT;
    }
    status_t status = p->setDataSource(fd, offset, length);
    if (status == NO_ERROR) mRetriever = p;
    return status;
}

status_t MetadataRetrieverClient::setDataSource(
        const sp<IDataSource>& source)
{
    ALOGV("setDataSource(IDataSource)");
    Mutex::Autolock lock(mLock);

    sp<DataSource> dataSource = DataSource::CreateFromIDataSource(source);
    player_type playerType =
        MediaPlayerFactory::getPlayerType(NULL /* client */, dataSource);
    ALOGV("player type = %d", playerType);
    sp<MediaMetadataRetrieverBase> p = createRetriever(playerType);
    if (p == NULL) return NO_INIT;
    status_t ret = p->setDataSource(dataSource);
    if (ret == NO_ERROR) mRetriever = p;
    return ret;
}

Mutex MetadataRetrieverClient::sLock;

sp<IMemory> MetadataRetrieverClient::getFrameAtTime(int64_t timeUs, int option)
{
    ALOGV("getFrameAtTime: time(%" PRId64 " us) option(%d)", timeUs, option);
    Mutex::Autolock lock(mLock);
    Mutex::Autolock glock(sLock);
    mThumbnail.clear();
    if (mRetriever == NULL) {
        ALOGE("retriever is not initialized");
        return NULL;
    }
    VideoFrame *frame = mRetriever->getFrameAtTime(timeUs, option);
    if (frame == NULL) {
        ALOGE("failed to capture a video frame");
        return NULL;
    }
    size_t size = sizeof(VideoFrame) + frame->mSize;
    sp<MemoryHeapBase> heap = new MemoryHeapBase(size, 0, "MetadataRetrieverClient");
    if (heap == NULL) {
        ALOGE("failed to create MemoryDealer");
        delete frame;
        return NULL;
    }
    mThumbnail = new MemoryBase(heap, 0, size);
    if (mThumbnail == NULL) {
        ALOGE("not enough memory for VideoFrame size=%zu", size);
        delete frame;
        return NULL;
    }
    VideoFrame *frameCopy = static_cast<VideoFrame *>(mThumbnail->pointer());
    frameCopy->mWidth = frame->mWidth;
    frameCopy->mHeight = frame->mHeight;
    frameCopy->mDisplayWidth = frame->mDisplayWidth;
    frameCopy->mDisplayHeight = frame->mDisplayHeight;
    frameCopy->mSize = frame->mSize;
    frameCopy->mRotationAngle = frame->mRotationAngle;
    ALOGV("rotation: %d", frameCopy->mRotationAngle);
    frameCopy->mData = (uint8_t *)frameCopy + sizeof(VideoFrame);
    memcpy(frameCopy->mData, frame->mData, frame->mSize);
    frameCopy->mData = 0;
    delete frame;  // Fix memory leakage
    return mThumbnail;
}

sp<IMemory> MetadataRetrieverClient::extractAlbumArt()
{
    ALOGV("extractAlbumArt");
    Mutex::Autolock lock(mLock);
    mAlbumArt.clear();
    if (mRetriever == NULL) {
        ALOGE("retriever is not initialized");
        return NULL;
    }
    MediaAlbumArt *albumArt = mRetriever->extractAlbumArt();
    if (albumArt == NULL) {
        ALOGE("failed to extract an album art");
        return NULL;
    }
    size_t size = sizeof(MediaAlbumArt) + albumArt->size();
    sp<MemoryHeapBase> heap = new MemoryHeapBase(size, 0, "MetadataRetrieverClient");
    if (heap == NULL) {
        ALOGE("failed to create MemoryDealer object");
        delete albumArt;
        return NULL;
    }
    mAlbumArt = new MemoryBase(heap, 0, size);
    if (mAlbumArt == NULL) {
        ALOGE("not enough memory for MediaAlbumArt size=%zu", size);
        delete albumArt;
        return NULL;
    }
    MediaAlbumArt::init((MediaAlbumArt *) mAlbumArt->pointer(),
                        albumArt->size(), albumArt->data());
    delete albumArt;  // We've taken our copy.
    return mAlbumArt;
}

const char* MetadataRetrieverClient::extractMetadata(int keyCode)
{
    ALOGV("extractMetadata");
    Mutex::Autolock lock(mLock);
    if (mRetriever == NULL) {
        ALOGE("retriever is not initialized");
        return NULL;
    }
    return mRetriever->extractMetadata(keyCode);
}

}; // namespace android
