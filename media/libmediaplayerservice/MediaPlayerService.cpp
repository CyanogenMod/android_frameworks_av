/*
**
** Copyright 2008, The Android Open Source Project
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

// Proxy for media player implementations

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaPlayerService"
#include <utils/Log.h>

#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

#include <string.h>

#include <cutils/atomic.h>
#include <cutils/properties.h> // for property_get

#include <utils/misc.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryHeapBase.h>
#include <binder/MemoryBase.h>
#include <gui/Surface.h>
#include <utils/Errors.h>  // for status_t
#include <utils/String8.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include <media/AudioPolicyHelper.h>
#include <media/IMediaHTTPService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/MediaPlayerInterface.h>
#include <media/mediarecorder.h>
#include <media/MediaMetadataRetrieverInterface.h>
#include <media/Metadata.h>
#include <media/AudioTrack.h>
#include <media/MemoryLeakTrackUtil.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/AudioPlayer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooperRoster.h>
#include <mediautils/BatteryNotifier.h>

#include <system/audio.h>

#include <private/android_filesystem_config.h>

#include "ActivityManager.h"
#include "MediaRecorderClient.h"
#include "MediaPlayerService.h"
#include "MetadataRetrieverClient.h"
#include "MediaPlayerFactory.h"

#include "TestPlayerStub.h"
#include "nuplayer/NuPlayerDriver.h"

#include <OMX.h>

#include "Crypto.h"
#include "Drm.h"
#include "HDCP.h"
#include "HTTPBase.h"
#include "RemoteDisplay.h"

namespace {
using android::media::Metadata;
using android::status_t;
using android::OK;
using android::BAD_VALUE;
using android::NOT_ENOUGH_DATA;
using android::Parcel;

// Max number of entries in the filter.
const int kMaxFilterSize = 64;  // I pulled that out of thin air.

// FIXME: Move all the metadata related function in the Metadata.cpp


// Unmarshall a filter from a Parcel.
// Filter format in a parcel:
//
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       number of entries (n)                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       metadata type 1                         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       metadata type 2                         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  ....
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       metadata type n                         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// @param p Parcel that should start with a filter.
// @param[out] filter On exit contains the list of metadata type to be
//                    filtered.
// @param[out] status On exit contains the status code to be returned.
// @return true if the parcel starts with a valid filter.
bool unmarshallFilter(const Parcel& p,
                      Metadata::Filter *filter,
                      status_t *status)
{
    int32_t val;
    if (p.readInt32(&val) != OK)
    {
        ALOGE("Failed to read filter's length");
        *status = NOT_ENOUGH_DATA;
        return false;
    }

    if( val > kMaxFilterSize || val < 0)
    {
        ALOGE("Invalid filter len %d", val);
        *status = BAD_VALUE;
        return false;
    }

    const size_t num = val;

    filter->clear();
    filter->setCapacity(num);

    size_t size = num * sizeof(Metadata::Type);


    if (p.dataAvail() < size)
    {
        ALOGE("Filter too short expected %zu but got %zu", size, p.dataAvail());
        *status = NOT_ENOUGH_DATA;
        return false;
    }

    const Metadata::Type *data =
            static_cast<const Metadata::Type*>(p.readInplace(size));

    if (NULL == data)
    {
        ALOGE("Filter had no data");
        *status = BAD_VALUE;
        return false;
    }

    // TODO: The stl impl of vector would be more efficient here
    // because it degenerates into a memcpy on pod types. Try to
    // replace later or use stl::set.
    for (size_t i = 0; i < num; ++i)
    {
        filter->add(*data);
        ++data;
    }
    *status = OK;
    return true;
}

// @param filter Of metadata type.
// @param val To be searched.
// @return true if a match was found.
bool findMetadata(const Metadata::Filter& filter, const int32_t val)
{
    // Deal with empty and ANY right away
    if (filter.isEmpty()) return false;
    if (filter[0] == Metadata::kAny) return true;

    return filter.indexOf(val) >= 0;
}

}  // anonymous namespace


namespace {
using android::Parcel;
using android::String16;

// marshalling tag indicating flattened utf16 tags
// keep in sync with frameworks/base/media/java/android/media/AudioAttributes.java
const int32_t kAudioAttributesMarshallTagFlattenTags = 1;

// Audio attributes format in a parcel:
//
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       usage                                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       content_type                            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       source                                  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       flags                                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       kAudioAttributesMarshallTagFlattenTags  | // ignore tags if not found
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                       flattened tags in UTF16                 |
// |                         ...                                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// @param p Parcel that contains audio attributes.
// @param[out] attributes On exit points to an initialized audio_attributes_t structure
// @param[out] status On exit contains the status code to be returned.
void unmarshallAudioAttributes(const Parcel& parcel, audio_attributes_t *attributes)
{
    attributes->usage = (audio_usage_t) parcel.readInt32();
    attributes->content_type = (audio_content_type_t) parcel.readInt32();
    attributes->source = (audio_source_t) parcel.readInt32();
    attributes->flags = (audio_flags_mask_t) parcel.readInt32();
    const bool hasFlattenedTag = (parcel.readInt32() == kAudioAttributesMarshallTagFlattenTags);
    if (hasFlattenedTag) {
        // the tags are UTF16, convert to UTF8
        String16 tags = parcel.readString16();
        ssize_t realTagSize = utf16_to_utf8_length(tags.string(), tags.size());
        if (realTagSize <= 0) {
            strcpy(attributes->tags, "");
        } else {
            // copy the flattened string into the attributes as the destination for the conversion:
            // copying array size -1, array for tags was calloc'd, no need to NULL-terminate it
            size_t tagSize = realTagSize > AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1 ?
                    AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1 : realTagSize;
            utf16_to_utf8(tags.string(), tagSize, attributes->tags,
                    sizeof(attributes->tags) / sizeof(attributes->tags[0]));
        }
    } else {
        ALOGE("unmarshallAudioAttributes() received unflattened tags, ignoring tag values");
        strcpy(attributes->tags, "");
    }
}
} // anonymous namespace


namespace android {

extern ALooperRoster gLooperRoster;


static bool checkPermission(const char* permissionString) {
#ifndef HAVE_ANDROID_OS
    return true;
#endif
    if (getpid() == IPCThreadState::self()->getCallingPid()) return true;
    bool ok = checkCallingPermission(String16(permissionString));
    if (!ok) ALOGE("Request requires %s", permissionString);
    return ok;
}

// TODO: Find real cause of Audio/Video delay in PV framework and remove this workaround
/* static */ int MediaPlayerService::AudioOutput::mMinBufferCount = 4;
/* static */ bool MediaPlayerService::AudioOutput::mIsOnEmulator = false;

void MediaPlayerService::instantiate() {
    defaultServiceManager()->addService(
            String16("media.player"), new MediaPlayerService());
}

MediaPlayerService::MediaPlayerService()
{
    ALOGV("MediaPlayerService created");
    mNextConnId = 1;

    mBatteryAudio.refCount = 0;
    for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
        mBatteryAudio.deviceOn[i] = 0;
        mBatteryAudio.lastTime[i] = 0;
        mBatteryAudio.totalTime[i] = 0;
    }
    // speaker is on by default
    mBatteryAudio.deviceOn[SPEAKER] = 1;

    // reset battery stats
    // if the mediaserver has crashed, battery stats could be left
    // in bad state, reset the state upon service start.
    BatteryNotifier& notifier(BatteryNotifier::getInstance());
    notifier.noteResetVideo();
    notifier.noteResetAudio();

    MediaPlayerFactory::registerBuiltinFactories();
}

MediaPlayerService::~MediaPlayerService()
{
    ALOGV("MediaPlayerService destroyed");
}

sp<IMediaRecorder> MediaPlayerService::createMediaRecorder(const String16 &opPackageName)
{
    pid_t pid = IPCThreadState::self()->getCallingPid();
    sp<MediaRecorderClient> recorder = new MediaRecorderClient(this, pid, opPackageName);
    wp<MediaRecorderClient> w = recorder;
    Mutex::Autolock lock(mLock);
    mMediaRecorderClients.add(w);
    ALOGV("Create new media recorder client from pid %d", pid);
    return recorder;
}

void MediaPlayerService::removeMediaRecorderClient(wp<MediaRecorderClient> client)
{
    Mutex::Autolock lock(mLock);
    mMediaRecorderClients.remove(client);
    ALOGV("Delete media recorder client");
}

sp<IMediaMetadataRetriever> MediaPlayerService::createMetadataRetriever()
{
    pid_t pid = IPCThreadState::self()->getCallingPid();
    sp<MetadataRetrieverClient> retriever = new MetadataRetrieverClient(pid);
    ALOGV("Create new media retriever from pid %d", pid);
    return retriever;
}

sp<IMediaPlayer> MediaPlayerService::create(const sp<IMediaPlayerClient>& client,
        int audioSessionId)
{
    pid_t pid = IPCThreadState::self()->getCallingPid();
    int32_t connId = android_atomic_inc(&mNextConnId);

    sp<Client> c = new Client(
            this, pid, connId, client, audioSessionId,
            IPCThreadState::self()->getCallingUid());

    ALOGV("Create new client(%d) from pid %d, uid %d, ", connId, pid,
         IPCThreadState::self()->getCallingUid());

    wp<Client> w = c;
    {
        Mutex::Autolock lock(mLock);
        mClients.add(w);
    }
    return c;
}

sp<IMediaCodecList> MediaPlayerService::getCodecList() const {
    return MediaCodecList::getLocalInstance();
}

sp<IOMX> MediaPlayerService::getOMX() {
    Mutex::Autolock autoLock(mLock);

    if (mOMX.get() == NULL) {
        mOMX = new OMX;
    }

    return mOMX;
}

sp<ICrypto> MediaPlayerService::makeCrypto() {
    return new Crypto;
}

sp<IDrm> MediaPlayerService::makeDrm() {
    return new Drm;
}

sp<IHDCP> MediaPlayerService::makeHDCP(bool createEncryptionModule) {
    return new HDCP(createEncryptionModule);
}

sp<IRemoteDisplay> MediaPlayerService::listenForRemoteDisplay(
        const String16 &opPackageName,
        const sp<IRemoteDisplayClient>& client, const String8& iface) {
    if (!checkPermission("android.permission.CONTROL_WIFI_DISPLAY")) {
        return NULL;
    }

    return new RemoteDisplay(opPackageName, client, iface.string());
}

status_t MediaPlayerService::AudioOutput::dump(int fd, const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append(" AudioOutput\n");
    snprintf(buffer, 255, "  stream type(%d), left - right volume(%f, %f)\n",
            mStreamType, mLeftVolume, mRightVolume);
    result.append(buffer);
    snprintf(buffer, 255, "  msec per frame(%f), latency (%d)\n",
            mMsecsPerFrame, (mTrack != 0) ? mTrack->latency() : -1);
    result.append(buffer);
    snprintf(buffer, 255, "  aux effect id(%d), send level (%f)\n",
            mAuxEffectId, mSendLevel);
    result.append(buffer);

    ::write(fd, result.string(), result.size());
    if (mTrack != 0) {
        mTrack->dump(fd, args);
    }
    return NO_ERROR;
}

status_t MediaPlayerService::Client::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append(" Client\n");
    snprintf(buffer, 255, "  pid(%d), connId(%d), status(%d), looping(%s)\n",
            mPid, mConnId, mStatus, mLoop?"true": "false");
    result.append(buffer);
    write(fd, result.string(), result.size());
    if (mPlayer != NULL) {
        mPlayer->dump(fd, args);
    }
    if (mAudioOutput != 0) {
        mAudioOutput->dump(fd, args);
    }
    write(fd, "\n", 1);
    return NO_ERROR;
}

/**
 * The only arguments this understands right now are -c, -von and -voff,
 * which are parsed by ALooperRoster::dump()
 */
status_t MediaPlayerService::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    SortedVector< sp<Client> > clients; //to serialise the mutex unlock & client destruction.
    SortedVector< sp<MediaRecorderClient> > mediaRecorderClients;

    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        snprintf(buffer, SIZE, "Permission Denial: "
                "can't dump MediaPlayerService from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
        result.append(buffer);
    } else {
        Mutex::Autolock lock(mLock);
        for (int i = 0, n = mClients.size(); i < n; ++i) {
            sp<Client> c = mClients[i].promote();
            if (c != 0) c->dump(fd, args);
            clients.add(c);
        }
        if (mMediaRecorderClients.size() == 0) {
                result.append(" No media recorder client\n\n");
        } else {
            for (int i = 0, n = mMediaRecorderClients.size(); i < n; ++i) {
                sp<MediaRecorderClient> c = mMediaRecorderClients[i].promote();
                if (c != 0) {
                    snprintf(buffer, 255, " MediaRecorderClient pid(%d)\n", c->mPid);
                    result.append(buffer);
                    write(fd, result.string(), result.size());
                    result = "\n";
                    c->dump(fd, args);
                    mediaRecorderClients.add(c);
                }
            }
        }

        result.append(" Files opened and/or mapped:\n");
        snprintf(buffer, SIZE, "/proc/%d/maps", getpid());
        FILE *f = fopen(buffer, "r");
        if (f) {
            while (!feof(f)) {
                fgets(buffer, SIZE, f);
                if (strstr(buffer, " /storage/") ||
                    strstr(buffer, " /system/sounds/") ||
                    strstr(buffer, " /data/") ||
                    strstr(buffer, " /system/media/")) {
                    result.append("  ");
                    result.append(buffer);
                }
            }
            fclose(f);
        } else {
            result.append("couldn't open ");
            result.append(buffer);
            result.append("\n");
        }

        snprintf(buffer, SIZE, "/proc/%d/fd", getpid());
        DIR *d = opendir(buffer);
        if (d) {
            struct dirent *ent;
            while((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name,".") && strcmp(ent->d_name,"..")) {
                    snprintf(buffer, SIZE, "/proc/%d/fd/%s", getpid(), ent->d_name);
                    struct stat s;
                    if (lstat(buffer, &s) == 0) {
                        if ((s.st_mode & S_IFMT) == S_IFLNK) {
                            char linkto[256];
                            int len = readlink(buffer, linkto, sizeof(linkto));
                            if(len > 0) {
                                if(len > 255) {
                                    linkto[252] = '.';
                                    linkto[253] = '.';
                                    linkto[254] = '.';
                                    linkto[255] = 0;
                                } else {
                                    linkto[len] = 0;
                                }
                                if (strstr(linkto, "/storage/") == linkto ||
                                    strstr(linkto, "/system/sounds/") == linkto ||
                                    strstr(linkto, "/data/") == linkto ||
                                    strstr(linkto, "/system/media/") == linkto) {
                                    result.append("  ");
                                    result.append(buffer);
                                    result.append(" -> ");
                                    result.append(linkto);
                                    result.append("\n");
                                }
                            }
                        } else {
                            result.append("  unexpected type for ");
                            result.append(buffer);
                            result.append("\n");
                        }
                    }
                }
            }
            closedir(d);
        } else {
            result.append("couldn't open ");
            result.append(buffer);
            result.append("\n");
        }

        gLooperRoster.dump(fd, args);

        bool dumpMem = false;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == String16("-m")) {
                dumpMem = true;
            }
        }
        if (dumpMem) {
            dumpMemoryAddresses(fd);
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

void MediaPlayerService::removeClient(wp<Client> client)
{
    Mutex::Autolock lock(mLock);
    mClients.remove(client);
}

MediaPlayerService::Client::Client(
        const sp<MediaPlayerService>& service, pid_t pid,
        int32_t connId, const sp<IMediaPlayerClient>& client,
        int audioSessionId, uid_t uid)
{
    ALOGV("Client(%d) constructor", connId);
    mPid = pid;
    mConnId = connId;
    mService = service;
    mClient = client;
    mLoop = false;
    mStatus = NO_INIT;
    mAudioSessionId = audioSessionId;
    mUID = uid;
    mRetransmitEndpointValid = false;
    mAudioAttributes = NULL;

#if CALLBACK_ANTAGONIZER
    ALOGD("create Antagonizer");
    mAntagonizer = new Antagonizer(notify, this);
#endif
}

MediaPlayerService::Client::~Client()
{
    ALOGV("Client(%d) destructor pid = %d", mConnId, mPid);
    mAudioOutput.clear();
    wp<Client> client(this);
    disconnect();
    mService->removeClient(client);
    if (mAudioAttributes != NULL) {
        free(mAudioAttributes);
    }
}

void MediaPlayerService::Client::disconnect()
{
    ALOGV("disconnect(%d) from pid %d", mConnId, mPid);
    // grab local reference and clear main reference to prevent future
    // access to object
    sp<MediaPlayerBase> p;
    {
        Mutex::Autolock l(mLock);
        p = mPlayer;
        mClient.clear();
    }

    mPlayer.clear();

    // clear the notification to prevent callbacks to dead client
    // and reset the player. We assume the player will serialize
    // access to itself if necessary.
    if (p != 0) {
        p->setNotifyCallback(0, 0);
#if CALLBACK_ANTAGONIZER
        ALOGD("kill Antagonizer");
        mAntagonizer->kill();
#endif
        p->reset();
    }

    disconnectNativeWindow();

    IPCThreadState::self()->flushCommands();
}

sp<MediaPlayerBase> MediaPlayerService::Client::createPlayer(player_type playerType)
{
    // determine if we have the right player type
    sp<MediaPlayerBase> p = mPlayer;
    if ((p != NULL) && (p->playerType() != playerType)) {
        ALOGV("delete player");
        p.clear();
    }
    if (p == NULL) {
        p = MediaPlayerFactory::createPlayer(playerType, this, notify, mPid);
    }

    if (p != NULL) {
        p->setUID(mUID);
    }

    return p;
}

sp<MediaPlayerBase> MediaPlayerService::Client::setDataSource_pre(
        player_type playerType)
{
    ALOGV("player type = %d", playerType);

    // create the right type of player
    sp<MediaPlayerBase> p = createPlayer(playerType);
    if (p == NULL) {
        return p;
    }

    if (!p->hardwareOutput()) {
        Mutex::Autolock l(mLock);
        mAudioOutput = new AudioOutput(mAudioSessionId, IPCThreadState::self()->getCallingUid(),
                mPid, mAudioAttributes);
        static_cast<MediaPlayerInterface*>(p.get())->setAudioSink(mAudioOutput);
    }

    return p;
}

void MediaPlayerService::Client::setDataSource_post(
        const sp<MediaPlayerBase>& p,
        status_t status)
{
    ALOGV(" setDataSource");
    mStatus = status;
    if (mStatus != OK) {
        ALOGE("  error: %d", mStatus);
        return;
    }

    // Set the re-transmission endpoint if one was chosen.
    if (mRetransmitEndpointValid) {
        mStatus = p->setRetransmitEndpoint(&mRetransmitEndpoint);
        if (mStatus != NO_ERROR) {
            ALOGE("setRetransmitEndpoint error: %d", mStatus);
        }
    }

    if (mStatus == OK) {
        mPlayer = p;
    }
}

status_t MediaPlayerService::Client::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers)
{
    ALOGV("setDataSource(%s)", url);
    if (url == NULL)
        return UNKNOWN_ERROR;

    if ((strncmp(url, "http://", 7) == 0) ||
        (strncmp(url, "https://", 8) == 0) ||
        (strncmp(url, "rtsp://", 7) == 0)) {
        if (!checkPermission("android.permission.INTERNET")) {
            return PERMISSION_DENIED;
        }
    }

    if (strncmp(url, "content://", 10) == 0) {
        // get a filedescriptor for the content Uri and
        // pass it to the setDataSource(fd) method

        String16 url16(url);
        int fd = android::openContentProviderFile(url16);
        if (fd < 0)
        {
            ALOGE("Couldn't open fd for %s", url);
            return UNKNOWN_ERROR;
        }
        setDataSource(fd, 0, 0x7fffffffffLL); // this sets mStatus
        close(fd);
        return mStatus;
    } else {
        player_type playerType = MediaPlayerFactory::getPlayerType(this, url);
        sp<MediaPlayerBase> p = setDataSource_pre(playerType);
        if (p == NULL) {
            return NO_INIT;
        }

        setDataSource_post(p, p->setDataSource(httpService, url, headers));
        return mStatus;
    }
}

status_t MediaPlayerService::Client::setDataSource(int fd, int64_t offset, int64_t length)
{
    ALOGV("setDataSource fd=%d, offset=%" PRId64 ", length=%" PRId64 "", fd, offset, length);
    struct stat sb;
    int ret = fstat(fd, &sb);
    if (ret != 0) {
        ALOGE("fstat(%d) failed: %d, %s", fd, ret, strerror(errno));
        return UNKNOWN_ERROR;
    }

    ALOGV("st_dev  = %" PRIu64 "", static_cast<uint64_t>(sb.st_dev));
    ALOGV("st_mode = %u", sb.st_mode);
    ALOGV("st_uid  = %lu", static_cast<unsigned long>(sb.st_uid));
    ALOGV("st_gid  = %lu", static_cast<unsigned long>(sb.st_gid));
    ALOGV("st_size = %" PRId64 "", sb.st_size);

    if (offset >= sb.st_size) {
        ALOGE("offset error");
        return UNKNOWN_ERROR;
    }
    if (offset + length > sb.st_size) {
        length = sb.st_size - offset;
        ALOGV("calculated length = %" PRId64 "", length);
    }

    player_type playerType = MediaPlayerFactory::getPlayerType(this,
                                                               fd,
                                                               offset,
                                                               length);
    sp<MediaPlayerBase> p = setDataSource_pre(playerType);
    if (p == NULL) {
        return NO_INIT;
    }

    // now set data source
    setDataSource_post(p, p->setDataSource(fd, offset, length));
    return mStatus;
}

status_t MediaPlayerService::Client::setDataSource(
        const sp<IStreamSource> &source) {
    // create the right type of player
    player_type playerType = MediaPlayerFactory::getPlayerType(this, source);
    sp<MediaPlayerBase> p = setDataSource_pre(playerType);
    if (p == NULL) {
        return NO_INIT;
    }

    // now set data source
    setDataSource_post(p, p->setDataSource(source));
    return mStatus;
}

status_t MediaPlayerService::Client::setDataSource(
        const sp<IDataSource> &source) {
    sp<DataSource> dataSource = DataSource::CreateFromIDataSource(source);
    player_type playerType = MediaPlayerFactory::getPlayerType(this, dataSource);
    sp<MediaPlayerBase> p = setDataSource_pre(playerType);
    if (p == NULL) {
        return NO_INIT;
    }
    // now set data source
    setDataSource_post(p, p->setDataSource(dataSource));
    return mStatus;
}

void MediaPlayerService::Client::disconnectNativeWindow() {
    if (mConnectedWindow != NULL) {
        status_t err = native_window_api_disconnect(mConnectedWindow.get(),
                NATIVE_WINDOW_API_MEDIA);

        if (err != OK) {
            ALOGW("native_window_api_disconnect returned an error: %s (%d)",
                    strerror(-err), err);
        }
    }
    mConnectedWindow.clear();
}

status_t MediaPlayerService::Client::setVideoSurfaceTexture(
        const sp<IGraphicBufferProducer>& bufferProducer)
{
    ALOGV("[%d] setVideoSurfaceTexture(%p)", mConnId, bufferProducer.get());
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;

    sp<IBinder> binder(IInterface::asBinder(bufferProducer));
    if (mConnectedWindowBinder == binder) {
        return OK;
    }

    sp<ANativeWindow> anw;
    if (bufferProducer != NULL) {
        anw = new Surface(bufferProducer, true /* controlledByApp */);
        status_t err = native_window_api_connect(anw.get(),
                NATIVE_WINDOW_API_MEDIA);

        if (err != OK) {
            ALOGE("setVideoSurfaceTexture failed: %d", err);
            // Note that we must do the reset before disconnecting from the ANW.
            // Otherwise queue/dequeue calls could be made on the disconnected
            // ANW, which may result in errors.
            reset();

            disconnectNativeWindow();

            return err;
        }
    }

    // Note that we must set the player's new GraphicBufferProducer before
    // disconnecting the old one.  Otherwise queue/dequeue calls could be made
    // on the disconnected ANW, which may result in errors.
    status_t err = p->setVideoSurfaceTexture(bufferProducer);

    disconnectNativeWindow();

    mConnectedWindow = anw;

    if (err == OK) {
        mConnectedWindowBinder = binder;
    } else {
        disconnectNativeWindow();
    }

    return err;
}

status_t MediaPlayerService::Client::invoke(const Parcel& request,
                                            Parcel *reply)
{
    sp<MediaPlayerBase> p = getPlayer();
    if (p == NULL) return UNKNOWN_ERROR;
    return p->invoke(request, reply);
}

// This call doesn't need to access the native player.
status_t MediaPlayerService::Client::setMetadataFilter(const Parcel& filter)
{
    status_t status;
    media::Metadata::Filter allow, drop;

    if (unmarshallFilter(filter, &allow, &status) &&
        unmarshallFilter(filter, &drop, &status)) {
        Mutex::Autolock lock(mLock);

        mMetadataAllow = allow;
        mMetadataDrop = drop;
    }
    return status;
}

status_t MediaPlayerService::Client::getMetadata(
        bool update_only, bool /*apply_filter*/, Parcel *reply)
{
    sp<MediaPlayerBase> player = getPlayer();
    if (player == 0) return UNKNOWN_ERROR;

    status_t status;
    // Placeholder for the return code, updated by the caller.
    reply->writeInt32(-1);

    media::Metadata::Filter ids;

    // We don't block notifications while we fetch the data. We clear
    // mMetadataUpdated first so we don't lose notifications happening
    // during the rest of this call.
    {
        Mutex::Autolock lock(mLock);
        if (update_only) {
            ids = mMetadataUpdated;
        }
        mMetadataUpdated.clear();
    }

    media::Metadata metadata(reply);

    metadata.appendHeader();
    status = player->getMetadata(ids, reply);

    if (status != OK) {
        metadata.resetParcel();
        ALOGE("getMetadata failed %d", status);
        return status;
    }

    // FIXME: Implement filtering on the result. Not critical since
    // filtering takes place on the update notifications already. This
    // would be when all the metadata are fetch and a filter is set.

    // Everything is fine, update the metadata length.
    metadata.updateLength();
    return OK;
}

status_t MediaPlayerService::Client::prepareAsync()
{
    ALOGV("[%d] prepareAsync", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    status_t ret = p->prepareAsync();
#if CALLBACK_ANTAGONIZER
    ALOGD("start Antagonizer");
    if (ret == NO_ERROR) mAntagonizer->start();
#endif
    return ret;
}

status_t MediaPlayerService::Client::start()
{
    ALOGV("[%d] start", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    p->setLooping(mLoop);
    return p->start();
}

status_t MediaPlayerService::Client::stop()
{
    ALOGV("[%d] stop", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->stop();
}

status_t MediaPlayerService::Client::pause()
{
    ALOGV("[%d] pause", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->pause();
}

status_t MediaPlayerService::Client::isPlaying(bool* state)
{
    *state = false;
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    *state = p->isPlaying();
    ALOGV("[%d] isPlaying: %d", mConnId, *state);
    return NO_ERROR;
}

status_t MediaPlayerService::Client::setPlaybackSettings(const AudioPlaybackRate& rate)
{
    ALOGV("[%d] setPlaybackSettings(%f, %f, %d, %d)",
            mConnId, rate.mSpeed, rate.mPitch, rate.mFallbackMode, rate.mStretchMode);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->setPlaybackSettings(rate);
}

status_t MediaPlayerService::Client::getPlaybackSettings(AudioPlaybackRate* rate /* nonnull */)
{
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    status_t ret = p->getPlaybackSettings(rate);
    if (ret == NO_ERROR) {
        ALOGV("[%d] getPlaybackSettings(%f, %f, %d, %d)",
                mConnId, rate->mSpeed, rate->mPitch, rate->mFallbackMode, rate->mStretchMode);
    } else {
        ALOGV("[%d] getPlaybackSettings returned %d", mConnId, ret);
    }
    return ret;
}

status_t MediaPlayerService::Client::setSyncSettings(
        const AVSyncSettings& sync, float videoFpsHint)
{
    ALOGV("[%d] setSyncSettings(%u, %u, %f, %f)",
            mConnId, sync.mSource, sync.mAudioAdjustMode, sync.mTolerance, videoFpsHint);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->setSyncSettings(sync, videoFpsHint);
}

status_t MediaPlayerService::Client::getSyncSettings(
        AVSyncSettings* sync /* nonnull */, float* videoFps /* nonnull */)
{
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    status_t ret = p->getSyncSettings(sync, videoFps);
    if (ret == NO_ERROR) {
        ALOGV("[%d] getSyncSettings(%u, %u, %f, %f)",
                mConnId, sync->mSource, sync->mAudioAdjustMode, sync->mTolerance, *videoFps);
    } else {
        ALOGV("[%d] getSyncSettings returned %d", mConnId, ret);
    }
    return ret;
}

status_t MediaPlayerService::Client::getCurrentPosition(int *msec)
{
    ALOGV("getCurrentPosition");
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    status_t ret = p->getCurrentPosition(msec);
    if (ret == NO_ERROR) {
        ALOGV("[%d] getCurrentPosition = %d", mConnId, *msec);
    } else {
        ALOGE("getCurrentPosition returned %d", ret);
    }
    return ret;
}

status_t MediaPlayerService::Client::getDuration(int *msec)
{
    ALOGV("getDuration");
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    status_t ret = p->getDuration(msec);
    if (ret == NO_ERROR) {
        ALOGV("[%d] getDuration = %d", mConnId, *msec);
    } else {
        ALOGE("getDuration returned %d", ret);
    }
    return ret;
}

status_t MediaPlayerService::Client::setNextPlayer(const sp<IMediaPlayer>& player) {
    ALOGV("setNextPlayer");
    Mutex::Autolock l(mLock);
    sp<Client> c = static_cast<Client*>(player.get());
    mNextClient = c;

    if (c != NULL) {
        if (mAudioOutput != NULL) {
            mAudioOutput->setNextOutput(c->mAudioOutput);
        } else if ((mPlayer != NULL) && !mPlayer->hardwareOutput()) {
            ALOGE("no current audio output");
        }

        if ((mPlayer != NULL) && (mNextClient->getPlayer() != NULL)) {
            mPlayer->setNextPlayer(mNextClient->getPlayer());
        }
    }

    return OK;
}

status_t MediaPlayerService::Client::seekTo(int msec)
{
    ALOGV("[%d] seekTo(%d)", mConnId, msec);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->seekTo(msec);
}

status_t MediaPlayerService::Client::reset()
{
    ALOGV("[%d] reset", mConnId);
    mRetransmitEndpointValid = false;
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->reset();
}

status_t MediaPlayerService::Client::setAudioStreamType(audio_stream_type_t type)
{
    ALOGV("[%d] setAudioStreamType(%d)", mConnId, type);
    // TODO: for hardware output, call player instead
    Mutex::Autolock l(mLock);
    if (mAudioOutput != 0) mAudioOutput->setAudioStreamType(type);
    return NO_ERROR;
}

status_t MediaPlayerService::Client::setAudioAttributes_l(const Parcel &parcel)
{
    if (mAudioAttributes != NULL) { free(mAudioAttributes); }
    mAudioAttributes = (audio_attributes_t *) calloc(1, sizeof(audio_attributes_t));
    if (mAudioAttributes == NULL) {
        return NO_MEMORY;
    }
    unmarshallAudioAttributes(parcel, mAudioAttributes);

    ALOGV("setAudioAttributes_l() usage=%d content=%d flags=0x%x tags=%s",
            mAudioAttributes->usage, mAudioAttributes->content_type, mAudioAttributes->flags,
            mAudioAttributes->tags);

    if (mAudioOutput != 0) {
        mAudioOutput->setAudioAttributes(mAudioAttributes);
    }
    return NO_ERROR;
}

status_t MediaPlayerService::Client::setLooping(int loop)
{
    ALOGV("[%d] setLooping(%d)", mConnId, loop);
    mLoop = loop;
    sp<MediaPlayerBase> p = getPlayer();
    if (p != 0) return p->setLooping(loop);
    return NO_ERROR;
}

status_t MediaPlayerService::Client::setVolume(float leftVolume, float rightVolume)
{
    ALOGV("[%d] setVolume(%f, %f)", mConnId, leftVolume, rightVolume);

    // for hardware output, call player instead
    sp<MediaPlayerBase> p = getPlayer();
    {
      Mutex::Autolock l(mLock);
      if (p != 0 && p->hardwareOutput()) {
          MediaPlayerHWInterface* hwp =
                  reinterpret_cast<MediaPlayerHWInterface*>(p.get());
          return hwp->setVolume(leftVolume, rightVolume);
      } else {
          if (mAudioOutput != 0) mAudioOutput->setVolume(leftVolume, rightVolume);
          return NO_ERROR;
      }
    }

    return NO_ERROR;
}

status_t MediaPlayerService::Client::setAuxEffectSendLevel(float level)
{
    ALOGV("[%d] setAuxEffectSendLevel(%f)", mConnId, level);
    Mutex::Autolock l(mLock);
    if (mAudioOutput != 0) return mAudioOutput->setAuxEffectSendLevel(level);
    return NO_ERROR;
}

status_t MediaPlayerService::Client::attachAuxEffect(int effectId)
{
    ALOGV("[%d] attachAuxEffect(%d)", mConnId, effectId);
    Mutex::Autolock l(mLock);
    if (mAudioOutput != 0) return mAudioOutput->attachAuxEffect(effectId);
    return NO_ERROR;
}

status_t MediaPlayerService::Client::setParameter(int key, const Parcel &request) {
    ALOGV("[%d] setParameter(%d)", mConnId, key);
    switch (key) {
    case KEY_PARAMETER_AUDIO_ATTRIBUTES:
    {
        Mutex::Autolock l(mLock);
        return setAudioAttributes_l(request);
    }
    default:
        sp<MediaPlayerBase> p = getPlayer();
        if (p == 0) { return UNKNOWN_ERROR; }
        return p->setParameter(key, request);
    }
}

status_t MediaPlayerService::Client::getParameter(int key, Parcel *reply) {
    ALOGV("[%d] getParameter(%d)", mConnId, key);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == 0) return UNKNOWN_ERROR;
    return p->getParameter(key, reply);
}

status_t MediaPlayerService::Client::setRetransmitEndpoint(
        const struct sockaddr_in* endpoint) {

    if (NULL != endpoint) {
        uint32_t a = ntohl(endpoint->sin_addr.s_addr);
        uint16_t p = ntohs(endpoint->sin_port);
        ALOGV("[%d] setRetransmitEndpoint(%u.%u.%u.%u:%hu)", mConnId,
                (a >> 24), (a >> 16) & 0xFF, (a >> 8) & 0xFF, (a & 0xFF), p);
    } else {
        ALOGV("[%d] setRetransmitEndpoint = <none>", mConnId);
    }

    sp<MediaPlayerBase> p = getPlayer();

    // Right now, the only valid time to set a retransmit endpoint is before
    // player selection has been made (since the presence or absence of a
    // retransmit endpoint is going to determine which player is selected during
    // setDataSource).
    if (p != 0) return INVALID_OPERATION;

    if (NULL != endpoint) {
        mRetransmitEndpoint = *endpoint;
        mRetransmitEndpointValid = true;
    } else {
        mRetransmitEndpointValid = false;
    }

    return NO_ERROR;
}

status_t MediaPlayerService::Client::getRetransmitEndpoint(
        struct sockaddr_in* endpoint)
{
    if (NULL == endpoint)
        return BAD_VALUE;

    sp<MediaPlayerBase> p = getPlayer();

    if (p != NULL)
        return p->getRetransmitEndpoint(endpoint);

    if (!mRetransmitEndpointValid)
        return NO_INIT;

    *endpoint = mRetransmitEndpoint;

    return NO_ERROR;
}

void MediaPlayerService::Client::notify(
        void* cookie, int msg, int ext1, int ext2, const Parcel *obj)
{
    Client* client = static_cast<Client*>(cookie);
    if (client == NULL) {
        return;
    }

    sp<IMediaPlayerClient> c;
    {
        Mutex::Autolock l(client->mLock);
        c = client->mClient;
        if (msg == MEDIA_PLAYBACK_COMPLETE && client->mNextClient != NULL) {
            if (client->mAudioOutput != NULL)
                client->mAudioOutput->switchToNextOutput();
            ALOGD("gapless:current track played back");
            ALOGD("gapless:try to do a gapless switch to next track");
            status_t ret;
            ret = client->mNextClient->start();
            if (ret == NO_ERROR) {
                client->mNextClient->mClient->notify(MEDIA_INFO,
                        MEDIA_INFO_STARTED_AS_NEXT, 0, obj);
            } else {
                client->mClient->notify(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN , 0, obj);
                ALOGW("gapless:start playback for next track failed");
            }
        }
    }

    if (MEDIA_INFO == msg &&
        MEDIA_INFO_METADATA_UPDATE == ext1) {
        const media::Metadata::Type metadata_type = ext2;

        if(client->shouldDropMetadata(metadata_type)) {
            return;
        }

        // Update the list of metadata that have changed. getMetadata
        // also access mMetadataUpdated and clears it.
        client->addNewMetadataUpdate(metadata_type);
    }

    if (c != NULL) {
        ALOGV("[%d] notify (%p, %d, %d, %d)", client->mConnId, cookie, msg, ext1, ext2);
        c->notify(msg, ext1, ext2, obj);
    }
}


bool MediaPlayerService::Client::shouldDropMetadata(media::Metadata::Type code) const
{
    Mutex::Autolock lock(mLock);

    if (findMetadata(mMetadataDrop, code)) {
        return true;
    }

    if (mMetadataAllow.isEmpty() || findMetadata(mMetadataAllow, code)) {
        return false;
    } else {
        return true;
    }
}


void MediaPlayerService::Client::addNewMetadataUpdate(media::Metadata::Type metadata_type) {
    Mutex::Autolock lock(mLock);
    if (mMetadataUpdated.indexOf(metadata_type) < 0) {
        mMetadataUpdated.add(metadata_type);
    }
}

status_t MediaPlayerService::Client::suspend()
{
    ALOGV("[%d] suspend", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == NULL) return NO_INIT;
    return p->suspend();
}

status_t MediaPlayerService::Client::resume()
{
    ALOGV("[%d] resume", mConnId);
    sp<MediaPlayerBase> p = getPlayer();
    if (p == NULL) return NO_INIT;
    return p->resume();
}

#if CALLBACK_ANTAGONIZER
const int Antagonizer::interval = 10000; // 10 msecs

Antagonizer::Antagonizer(notify_callback_f cb, void* client) :
    mExit(false), mActive(false), mClient(client), mCb(cb)
{
    createThread(callbackThread, this);
}

void Antagonizer::kill()
{
    Mutex::Autolock _l(mLock);
    mActive = false;
    mExit = true;
    mCondition.wait(mLock);
}

int Antagonizer::callbackThread(void* user)
{
    ALOGD("Antagonizer started");
    Antagonizer* p = reinterpret_cast<Antagonizer*>(user);
    while (!p->mExit) {
        if (p->mActive) {
            ALOGV("send event");
            p->mCb(p->mClient, 0, 0, 0);
        }
        usleep(interval);
    }
    Mutex::Autolock _l(p->mLock);
    p->mCondition.signal();
    ALOGD("Antagonizer stopped");
    return 0;
}
#endif

#undef LOG_TAG
#define LOG_TAG "AudioSink"
MediaPlayerService::AudioOutput::AudioOutput(int sessionId, int uid, int pid,
        const audio_attributes_t* attr)
    : mCallback(NULL),
      mCallbackCookie(NULL),
      mCallbackData(NULL),
      mBytesWritten(0),
      mStreamType(AUDIO_STREAM_MUSIC),
      mLeftVolume(1.0),
      mRightVolume(1.0),
      mPlaybackRate(AUDIO_PLAYBACK_RATE_DEFAULT),
      mSampleRateHz(0),
      mMsecsPerFrame(0),
      mFrameSize(0),
      mSessionId(sessionId),
      mUid(uid),
      mPid(pid),
      mSendLevel(0.0),
      mAuxEffectId(0),
      mFlags(AUDIO_OUTPUT_FLAG_NONE)
{
    ALOGV("AudioOutput(%d)", sessionId);
    if (attr != NULL) {
        mAttributes = (audio_attributes_t *) calloc(1, sizeof(audio_attributes_t));
        if (mAttributes != NULL) {
            memcpy(mAttributes, attr, sizeof(audio_attributes_t));
            mStreamType = audio_attributes_to_stream_type(attr);
        }
    } else {
        mAttributes = NULL;
    }

    setMinBufferCount();
    mBitWidth = 16;
}

MediaPlayerService::AudioOutput::~AudioOutput()
{
    close();
    free(mAttributes);
    delete mCallbackData;
}

//static
void MediaPlayerService::AudioOutput::setMinBufferCount()
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get("ro.kernel.qemu", value, 0)) {
        mIsOnEmulator = true;
        mMinBufferCount = 12;  // to prevent systematic buffer underrun for emulator
    }
}

// static
bool MediaPlayerService::AudioOutput::isOnEmulator()
{
    setMinBufferCount(); // benign race wrt other threads
    return mIsOnEmulator;
}

// static
int MediaPlayerService::AudioOutput::getMinBufferCount()
{
    setMinBufferCount(); // benign race wrt other threads
    return mMinBufferCount;
}

ssize_t MediaPlayerService::AudioOutput::bufferSize() const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->frameCount() * mFrameSize;
}

ssize_t MediaPlayerService::AudioOutput::frameCount() const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->frameCount();
}

ssize_t MediaPlayerService::AudioOutput::channelCount() const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->channelCount();
}

ssize_t MediaPlayerService::AudioOutput::frameSize() const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mFrameSize;
}

uint32_t MediaPlayerService::AudioOutput::latency () const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return 0;
    return mTrack->latency();
}

float MediaPlayerService::AudioOutput::msecsPerFrame() const
{
    Mutex::Autolock lock(mLock);
    return mMsecsPerFrame;
}

status_t MediaPlayerService::AudioOutput::getPosition(uint32_t *position) const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->getPosition(position);
}

status_t MediaPlayerService::AudioOutput::getTimestamp(AudioTimestamp &ts) const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->getTimestamp(ts);
}

status_t MediaPlayerService::AudioOutput::getFramesWritten(uint32_t *frameswritten) const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    *frameswritten = mBytesWritten / mFrameSize;
    return OK;
}

status_t MediaPlayerService::AudioOutput::setParameters(const String8& keyValuePairs)
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return NO_INIT;
    return mTrack->setParameters(keyValuePairs);
}

String8  MediaPlayerService::AudioOutput::getParameters(const String8& keys)
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return String8::empty();
    return mTrack->getParameters(keys);
}

void MediaPlayerService::AudioOutput::setAudioAttributes(const audio_attributes_t * attributes) {
    Mutex::Autolock lock(mLock);
    if (attributes == NULL) {
        free(mAttributes);
        mAttributes = NULL;
    } else {
        if (mAttributes == NULL) {
            mAttributes = (audio_attributes_t *) calloc(1, sizeof(audio_attributes_t));
        }
        memcpy(mAttributes, attributes, sizeof(audio_attributes_t));
        mStreamType = audio_attributes_to_stream_type(attributes);
    }
}

void MediaPlayerService::AudioOutput::setAudioStreamType(audio_stream_type_t streamType)
{
    Mutex::Autolock lock(mLock);
    // do not allow direct stream type modification if attributes have been set
    if (mAttributes == NULL) {
        mStreamType = streamType;
    }
}

void MediaPlayerService::AudioOutput::deleteRecycledTrack_l()
{
    ALOGV("deleteRecycledTrack_l");
    if (mRecycledTrack != 0) {

        if (mCallbackData != NULL) {
            mCallbackData->setOutput(NULL);
            mCallbackData->endTrackSwitch();
        }

        if ((mRecycledTrack->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0) {
            mRecycledTrack->flush();
        }
        // An offloaded track isn't flushed because the STREAM_END is reported
        // slightly prematurely to allow time for the gapless track switch
        // but this means that if we decide not to recycle the track there
        // could be a small amount of residual data still playing. We leave
        // AudioFlinger to drain the track.

        mRecycledTrack.clear();
        close_l();
        delete mCallbackData;
        mCallbackData = NULL;
    }
}

void MediaPlayerService::AudioOutput::close_l()
{
    mTrack.clear();
}

status_t MediaPlayerService::AudioOutput::open(
        uint32_t sampleRate, int channelCount, audio_channel_mask_t channelMask,
        audio_format_t format, int bufferCount,
        AudioCallback cb, void *cookie,
        audio_output_flags_t flags,
        const audio_offload_info_t *offloadInfo,
        bool doNotReconnect,
        uint32_t suggestedFrameCount)
{
    ALOGV("open(%u, %d, 0x%x, 0x%x, %d, %d 0x%x)", sampleRate, channelCount, channelMask,
                format, bufferCount, mSessionId, flags);

    // offloading is only supported in callback mode for now.
    // offloadInfo must be present if offload flag is set
    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) &&
            ((cb == NULL) || (offloadInfo == NULL))) {
        return BAD_VALUE;
    }

    // compute frame count for the AudioTrack internal buffer
    size_t frameCount;
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        frameCount = 0; // AudioTrack will get frame count from AudioFlinger
    } else {
        // try to estimate the buffer processing fetch size from AudioFlinger.
        // framesPerBuffer is approximate and generally correct, except when it's not :-).
        uint32_t afSampleRate;
        size_t afFrameCount;
        if (AudioSystem::getOutputFrameCount(&afFrameCount, mStreamType) != NO_ERROR) {
            return NO_INIT;
        }
        if (AudioSystem::getOutputSamplingRate(&afSampleRate, mStreamType) != NO_ERROR) {
            return NO_INIT;
        }
        const size_t framesPerBuffer =
                (unsigned long long)sampleRate * afFrameCount / afSampleRate;

        if (bufferCount == 0) {
            // use suggestedFrameCount
            bufferCount = (suggestedFrameCount + framesPerBuffer - 1) / framesPerBuffer;
        }
        // Check argument bufferCount against the mininum buffer count
        if (bufferCount != 0 && bufferCount < mMinBufferCount) {
            ALOGV("bufferCount (%d) increased to %d", bufferCount, mMinBufferCount);
            bufferCount = mMinBufferCount;
        }
        // if frameCount is 0, then AudioTrack will get frame count from AudioFlinger
        // which will be the minimum size permitted.
        frameCount = bufferCount * framesPerBuffer;
    }

    if (channelMask == CHANNEL_MASK_USE_CHANNEL_ORDER) {
        channelMask = audio_channel_out_mask_from_count(channelCount);
        if (0 == channelMask) {
            ALOGE("open() error, can\'t derive mask for %d audio channels", channelCount);
            return NO_INIT;
        }
    }

    Mutex::Autolock lock(mLock);
    mCallback = cb;
    mCallbackCookie = cookie;

    // Check whether we can recycle the track
    bool reuse = false;
    bool bothOffloaded = false;

    if (mRecycledTrack != 0) {
        // check whether we are switching between two offloaded tracks
        bothOffloaded = (flags & mRecycledTrack->getFlags()
                                & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0;

        // check if the existing track can be reused as-is, or if a new track needs to be created.
        reuse = true;

        if ((mCallbackData == NULL && mCallback != NULL) ||
                (mCallbackData != NULL && mCallback == NULL)) {
            // recycled track uses callbacks but the caller wants to use writes, or vice versa
            ALOGV("can't chain callback and write");
            reuse = false;
        } else if ((mRecycledTrack->getSampleRate() != sampleRate) ||
                (mRecycledTrack->channelCount() != (uint32_t)channelCount) ) {
            ALOGV("samplerate, channelcount differ: %u/%u Hz, %u/%d ch",
                  mRecycledTrack->getSampleRate(), sampleRate,
                  mRecycledTrack->channelCount(), channelCount);
            reuse = false;
        } else if (flags != mFlags) {
            ALOGV("output flags differ %08x/%08x", flags, mFlags);
            reuse = false;
        } else if (mRecycledTrack->format() != format) {
            reuse = false;
        }

        if (bothOffloaded) {
            if (mBitWidth != offloadInfo->bit_width) {
                ALOGV("output bit width differs %d v/s %d",
                      mBitWidth, offloadInfo->bit_width);
                reuse = false;
            }
        }

    } else {
        ALOGV("no track available to recycle");
    }

    ALOGV_IF(bothOffloaded, "both tracks offloaded");

    // If we can't recycle and both tracks are offloaded
    // we must close the previous output before opening a new one
    if (bothOffloaded && !reuse) {
        ALOGV("both offloaded and not recycling");
        deleteRecycledTrack_l();
    }

    sp<AudioTrack> t;
    CallbackData *newcbd = NULL;

    // We don't attempt to create a new track if we are recycling an
    // offloaded track. But, if we are recycling a non-offloaded or we
    // are switching where one is offloaded and one isn't then we create
    // the new track in advance so that we can read additional stream info

    if (!(reuse && bothOffloaded)) {
        ALOGV("creating new AudioTrack");

        if (mCallback != NULL) {
            newcbd = new CallbackData(this);
            t = new AudioTrack(
                    mStreamType,
                    sampleRate,
                    format,
                    channelMask,
                    frameCount,
                    flags,
                    CallbackWrapper,
                    newcbd,
                    0,  // notification frames
                    mSessionId,
                    AudioTrack::TRANSFER_CALLBACK,
                    offloadInfo,
                    mUid,
                    mPid,
                    mAttributes,
                    doNotReconnect);
        } else {
            t = new AudioTrack(
                    mStreamType,
                    sampleRate,
                    format,
                    channelMask,
                    frameCount,
                    flags,
                    NULL, // callback
                    NULL, // user data
                    0, // notification frames
                    mSessionId,
                    AudioTrack::TRANSFER_DEFAULT,
                    NULL, // offload info
                    mUid,
                    mPid,
                    mAttributes,
                    doNotReconnect);
        }

        if ((t == 0) || (t->initCheck() != NO_ERROR)) {
            ALOGE("Unable to create audio track");
            delete newcbd;
            // t goes out of scope, so reference count drops to zero
            return NO_INIT;
        } else {
            // successful AudioTrack initialization implies a legacy stream type was generated
            // from the audio attributes
            mStreamType = t->streamType();
        }
    }

    if (reuse) {
        CHECK(mRecycledTrack != NULL);

        if (!bothOffloaded) {
            if (mRecycledTrack->frameCount() != t->frameCount()) {
                ALOGV("framecount differs: %zu/%zu frames",
                      mRecycledTrack->frameCount(), t->frameCount());
                reuse = false;
            }
        }

        if (reuse) {
            ALOGV("chaining to next output and recycling track");
            close_l();
            mTrack = mRecycledTrack;
            mRecycledTrack.clear();
            if (mCallbackData != NULL) {
                mCallbackData->setOutput(this);
            }
            delete newcbd;
            return OK;
        }
    }

    // we're not going to reuse the track, unblock and flush it
    // this was done earlier if both tracks are offloaded
    if (!bothOffloaded) {
        deleteRecycledTrack_l();
    }

    CHECK((t != NULL) && ((mCallback == NULL) || (newcbd != NULL)));

    mCallbackData = newcbd;
    ALOGV("setVolume");
    t->setVolume(mLeftVolume, mRightVolume);

    mSampleRateHz = sampleRate;
    mFlags = flags;
    mMsecsPerFrame = 1E3f / (mPlaybackRate.mSpeed * sampleRate);
    mFrameSize = t->frameSize();

    if (offloadInfo) {
        mBitWidth = offloadInfo->bit_width;
    } else {
        mBitWidth = 16;
    }

    uint32_t pos;
    if (t->getPosition(&pos) == OK) {
        mBytesWritten = uint64_t(pos) * mFrameSize;
    }
    mTrack = t;

    status_t res = NO_ERROR;
    // Note some output devices may give us a direct track even though we don't specify it.
    // Example: Line application b/17459982.
    if ((t->getFlags() & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT)) == 0) {
        res = t->setPlaybackRate(mPlaybackRate);
        if (res == NO_ERROR) {
            t->setAuxEffectSendLevel(mSendLevel);
            res = t->attachAuxEffect(mAuxEffectId);
        }
    }
    ALOGV("open() DONE status %d", res);
    return res;
}

status_t MediaPlayerService::AudioOutput::start()
{
    ALOGV("start");
    Mutex::Autolock lock(mLock);
    if (mCallbackData != NULL) {
        mCallbackData->endTrackSwitch();
    }
    if (mTrack != 0) {
        mTrack->setVolume(mLeftVolume, mRightVolume);
        mTrack->setAuxEffectSendLevel(mSendLevel);
        return mTrack->start();
    }
    return NO_INIT;
}

void MediaPlayerService::AudioOutput::setNextOutput(const sp<AudioOutput>& nextOutput) {
    Mutex::Autolock lock(mLock);
    mNextOutput = nextOutput;
}

void MediaPlayerService::AudioOutput::switchToNextOutput() {
    ALOGV("switchToNextOutput");

    // Try to acquire the callback lock before moving track (without incurring deadlock).
    const unsigned kMaxSwitchTries = 100;
    Mutex::Autolock lock(mLock);
    for (unsigned tries = 0;;) {
        if (mTrack == 0) {
            return;
        }
        if (mNextOutput != NULL && mNextOutput != this) {
            if (mCallbackData != NULL) {
                // two alternative approaches
#if 1
                CallbackData *callbackData = mCallbackData;
                mLock.unlock();
                // proper acquisition sequence
                callbackData->lock();
                mLock.lock();
                // Caution: it is unlikely that someone deleted our callback or changed our target
                if (callbackData != mCallbackData || mNextOutput == NULL || mNextOutput == this) {
                    // fatal if we are starved out.
                    LOG_ALWAYS_FATAL_IF(++tries > kMaxSwitchTries,
                            "switchToNextOutput() cannot obtain correct lock sequence");
                    callbackData->unlock();
                    continue;
                }
                callbackData->mSwitching = true; // begin track switch
#else
                // tryBeginTrackSwitch() returns false if the callback has the lock.
                if (!mCallbackData->tryBeginTrackSwitch()) {
                    // fatal if we are starved out.
                    LOG_ALWAYS_FATAL_IF(++tries > kMaxSwitchTries,
                            "switchToNextOutput() cannot obtain callback lock");
                    mLock.unlock();
                    usleep(5 * 1000 /* usec */); // allow callback to use AudioOutput
                    mLock.lock();
                    continue;
                }
#endif
            }

            Mutex::Autolock nextLock(mNextOutput->mLock);

            // If the next output track is not NULL, then it has been
            // opened already for playback.
            // This is possible even without the next player being started,
            // for example, the next player could be prepared and seeked.
            //
            // Presuming it isn't advisable to force the track over.
             if (mNextOutput->mTrack == NULL) {
                ALOGD("Recycling track for gapless playback");
                delete mNextOutput->mCallbackData;
                mNextOutput->mCallbackData = mCallbackData;
                mNextOutput->mRecycledTrack = mTrack;
                mNextOutput->mSampleRateHz = mSampleRateHz;
                mNextOutput->mMsecsPerFrame = mMsecsPerFrame;
                mNextOutput->mBytesWritten = mBytesWritten;
                mNextOutput->mFlags = mFlags;
                mNextOutput->mFrameSize = mFrameSize;
                mNextOutput->mBitWidth = mBitWidth;
                close_l();
                mCallbackData = NULL;  // destruction handled by mNextOutput
            } else {
                ALOGW("Ignoring gapless playback because next player has already started");
                // remove track in case resource needed for future players.
                if (mCallbackData != NULL) {
                    mCallbackData->endTrackSwitch();  // release lock for callbacks before close.
                }
                close_l();
            }
        }
        break;
    }
}

ssize_t MediaPlayerService::AudioOutput::write(const void* buffer, size_t size, bool blocking)
{
    Mutex::Autolock lock(mLock);
    LOG_ALWAYS_FATAL_IF(mCallback != NULL, "Don't call write if supplying a callback.");

    //ALOGV("write(%p, %u)", buffer, size);
    if (mTrack != 0) {
        ssize_t ret = mTrack->write(buffer, size, blocking);
        if (ret >= 0) {
            mBytesWritten += ret;
        }
        return ret;
    }
    return NO_INIT;
}

void MediaPlayerService::AudioOutput::stop()
{
    ALOGV("stop");
    Mutex::Autolock lock(mLock);
    mBytesWritten = 0;
    if (mTrack != 0) mTrack->stop();
}

void MediaPlayerService::AudioOutput::flush()
{
    ALOGV("flush");
    Mutex::Autolock lock(mLock);
    mBytesWritten = 0;
    if (mTrack != 0) mTrack->flush();
}

void MediaPlayerService::AudioOutput::pause()
{
    ALOGV("pause");
    Mutex::Autolock lock(mLock);
    if (mTrack != 0) mTrack->pause();
}

void MediaPlayerService::AudioOutput::close()
{
    ALOGV("close");
    sp<AudioTrack> track;
    {
        Mutex::Autolock lock(mLock);
        track = mTrack;
        close_l(); // clears mTrack
    }
    // destruction of the track occurs outside of mutex.
}

void MediaPlayerService::AudioOutput::setVolume(float left, float right)
{
    ALOGV("setVolume(%f, %f)", left, right);
    Mutex::Autolock lock(mLock);
    mLeftVolume = left;
    mRightVolume = right;
    if (mTrack != 0) {
        mTrack->setVolume(left, right);
    }
}

status_t MediaPlayerService::AudioOutput::setPlaybackRate(const AudioPlaybackRate &rate)
{
    ALOGV("setPlaybackRate(%f %f %d %d)",
                rate.mSpeed, rate.mPitch, rate.mFallbackMode, rate.mStretchMode);
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) {
        // remember rate so that we can set it when the track is opened
        mPlaybackRate = rate;
        return OK;
    }
    status_t res = mTrack->setPlaybackRate(rate);
    if (res != NO_ERROR) {
        return res;
    }
    // rate.mSpeed is always greater than 0 if setPlaybackRate succeeded
    CHECK_GT(rate.mSpeed, 0.f);
    mPlaybackRate = rate;
    if (mSampleRateHz != 0) {
        mMsecsPerFrame = 1E3f / (rate.mSpeed * mSampleRateHz);
    }
    return res;
}

status_t MediaPlayerService::AudioOutput::getPlaybackRate(AudioPlaybackRate *rate)
{
    ALOGV("setPlaybackRate");
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) {
        return NO_INIT;
    }
    *rate = mTrack->getPlaybackRate();
    return NO_ERROR;
}

status_t MediaPlayerService::AudioOutput::setAuxEffectSendLevel(float level)
{
    ALOGV("setAuxEffectSendLevel(%f)", level);
    Mutex::Autolock lock(mLock);
    mSendLevel = level;
    if (mTrack != 0) {
        return mTrack->setAuxEffectSendLevel(level);
    }
    return NO_ERROR;
}

status_t MediaPlayerService::AudioOutput::attachAuxEffect(int effectId)
{
    ALOGV("attachAuxEffect(%d)", effectId);
    Mutex::Autolock lock(mLock);
    mAuxEffectId = effectId;
    if (mTrack != 0) {
        return mTrack->attachAuxEffect(effectId);
    }
    return NO_ERROR;
}

// static
void MediaPlayerService::AudioOutput::CallbackWrapper(
        int event, void *cookie, void *info) {
    //ALOGV("callbackwrapper");
    CallbackData *data = (CallbackData*)cookie;
    // lock to ensure we aren't caught in the middle of a track switch.
    data->lock();
    AudioOutput *me = data->getOutput();
    AudioTrack::Buffer *buffer = (AudioTrack::Buffer *)info;
    if (me == NULL) {
        // no output set, likely because the track was scheduled to be reused
        // by another player, but the format turned out to be incompatible.
        data->unlock();
        if (buffer != NULL) {
            buffer->size = 0;
        }
        return;
    }

    switch(event) {
    case AudioTrack::EVENT_MORE_DATA: {
        size_t actualSize = (*me->mCallback)(
                me, buffer->raw, buffer->size, me->mCallbackCookie,
                CB_EVENT_FILL_BUFFER);

        // Log when no data is returned from the callback.
        // (1) We may have no data (especially with network streaming sources).
        // (2) We may have reached the EOS and the audio track is not stopped yet.
        // Note that AwesomePlayer/AudioPlayer will only return zero size when it reaches the EOS.
        // NuPlayerRenderer will return zero when it doesn't have data (it doesn't block to fill).
        //
        // This is a benign busy-wait, with the next data request generated 10 ms or more later;
        // nevertheless for power reasons, we don't want to see too many of these.

        ALOGV_IF(actualSize == 0 && buffer->size > 0, "callbackwrapper: empty buffer returned");

        me->mBytesWritten += actualSize;  // benign race with reader.
        buffer->size = actualSize;
        } break;

    case AudioTrack::EVENT_STREAM_END:
        // currently only occurs for offloaded callbacks
        ALOGV("callbackwrapper: deliver EVENT_STREAM_END");
        (*me->mCallback)(me, NULL /* buffer */, 0 /* size */,
                me->mCallbackCookie, CB_EVENT_STREAM_END);
        break;

    case AudioTrack::EVENT_NEW_IAUDIOTRACK :
        ALOGV("callbackwrapper: deliver EVENT_TEAR_DOWN");
        (*me->mCallback)(me,  NULL /* buffer */, 0 /* size */,
                me->mCallbackCookie, CB_EVENT_TEAR_DOWN);
        break;

    case AudioTrack::EVENT_UNDERRUN:
        // This occurs when there is no data available, typically
        // when there is a failure to supply data to the AudioTrack.  It can also
        // occur in non-offloaded mode when the audio device comes out of standby.
        //
        // If an AudioTrack underruns it outputs silence. Since this happens suddenly
        // it may sound like an audible pop or glitch.
        //
        // The underrun event is sent once per track underrun; the condition is reset
        // when more data is sent to the AudioTrack.
        ALOGI("callbackwrapper: EVENT_UNDERRUN (discarded)");
        break;

    default:
        ALOGE("received unknown event type: %d inside CallbackWrapper !", event);
    }

    data->unlock();
}

int MediaPlayerService::AudioOutput::getSessionId() const
{
    Mutex::Autolock lock(mLock);
    return mSessionId;
}

uint32_t MediaPlayerService::AudioOutput::getSampleRate() const
{
    Mutex::Autolock lock(mLock);
    if (mTrack == 0) return 0;
    return mTrack->getSampleRate();
}

////////////////////////////////////////////////////////////////////////////////

struct CallbackThread : public Thread {
    CallbackThread(const wp<MediaPlayerBase::AudioSink> &sink,
                   MediaPlayerBase::AudioSink::AudioCallback cb,
                   void *cookie);

protected:
    virtual ~CallbackThread();

    virtual bool threadLoop();

private:
    wp<MediaPlayerBase::AudioSink> mSink;
    MediaPlayerBase::AudioSink::AudioCallback mCallback;
    void *mCookie;
    void *mBuffer;
    size_t mBufferSize;

    CallbackThread(const CallbackThread &);
    CallbackThread &operator=(const CallbackThread &);
};

CallbackThread::CallbackThread(
        const wp<MediaPlayerBase::AudioSink> &sink,
        MediaPlayerBase::AudioSink::AudioCallback cb,
        void *cookie)
    : mSink(sink),
      mCallback(cb),
      mCookie(cookie),
      mBuffer(NULL),
      mBufferSize(0) {
}

CallbackThread::~CallbackThread() {
    if (mBuffer) {
        free(mBuffer);
        mBuffer = NULL;
    }
}

bool CallbackThread::threadLoop() {
    sp<MediaPlayerBase::AudioSink> sink = mSink.promote();
    if (sink == NULL) {
        return false;
    }

    if (mBuffer == NULL) {
        mBufferSize = sink->bufferSize();
        mBuffer = malloc(mBufferSize);
        CHECK(mBuffer != NULL);
    }

    size_t actualSize =
        (*mCallback)(sink.get(), mBuffer, mBufferSize, mCookie,
                MediaPlayerBase::AudioSink::CB_EVENT_FILL_BUFFER);

    if (actualSize > 0) {
        sink->write(mBuffer, actualSize);
        // Could return false on sink->write() error or short count.
        // Not necessarily appropriate but would work for AudioCache behavior.
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

void MediaPlayerService::addBatteryData(uint32_t params)
{
    Mutex::Autolock lock(mLock);

    int32_t time = systemTime() / 1000000L;

    // change audio output devices. This notification comes from AudioFlinger
    if ((params & kBatteryDataSpeakerOn)
            || (params & kBatteryDataOtherAudioDeviceOn)) {

        int deviceOn[NUM_AUDIO_DEVICES];
        for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
            deviceOn[i] = 0;
        }

        if ((params & kBatteryDataSpeakerOn)
                && (params & kBatteryDataOtherAudioDeviceOn)) {
            deviceOn[SPEAKER_AND_OTHER] = 1;
        } else if (params & kBatteryDataSpeakerOn) {
            deviceOn[SPEAKER] = 1;
        } else {
            deviceOn[OTHER_AUDIO_DEVICE] = 1;
        }

        for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
            if (mBatteryAudio.deviceOn[i] != deviceOn[i]){

                if (mBatteryAudio.refCount > 0) { // if playing audio
                    if (!deviceOn[i]) {
                        mBatteryAudio.lastTime[i] += time;
                        mBatteryAudio.totalTime[i] += mBatteryAudio.lastTime[i];
                        mBatteryAudio.lastTime[i] = 0;
                    } else {
                        mBatteryAudio.lastTime[i] = 0 - time;
                    }
                }

                mBatteryAudio.deviceOn[i] = deviceOn[i];
            }
        }
        return;
    }

    // an audio stream is started
    if (params & kBatteryDataAudioFlingerStart) {
        // record the start time only if currently no other audio
        // is being played
        if (mBatteryAudio.refCount == 0) {
            for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
                if (mBatteryAudio.deviceOn[i]) {
                    mBatteryAudio.lastTime[i] -= time;
                }
            }
        }

        mBatteryAudio.refCount ++;
        return;

    } else if (params & kBatteryDataAudioFlingerStop) {
        if (mBatteryAudio.refCount <= 0) {
            ALOGW("Battery track warning: refCount is <= 0");
            return;
        }

        // record the stop time only if currently this is the only
        // audio being played
        if (mBatteryAudio.refCount == 1) {
            for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
                if (mBatteryAudio.deviceOn[i]) {
                    mBatteryAudio.lastTime[i] += time;
                    mBatteryAudio.totalTime[i] += mBatteryAudio.lastTime[i];
                    mBatteryAudio.lastTime[i] = 0;
                }
            }
        }

        mBatteryAudio.refCount --;
        return;
    }

    int uid = IPCThreadState::self()->getCallingUid();
    if (uid == AID_MEDIA) {
        return;
    }
    int index = mBatteryData.indexOfKey(uid);

    if (index < 0) { // create a new entry for this UID
        BatteryUsageInfo info;
        info.audioTotalTime = 0;
        info.videoTotalTime = 0;
        info.audioLastTime = 0;
        info.videoLastTime = 0;
        info.refCount = 0;

        if (mBatteryData.add(uid, info) == NO_MEMORY) {
            ALOGE("Battery track error: no memory for new app");
            return;
        }
    }

    BatteryUsageInfo &info = mBatteryData.editValueFor(uid);

    if (params & kBatteryDataCodecStarted) {
        if (params & kBatteryDataTrackAudio) {
            info.audioLastTime -= time;
            info.refCount ++;
        }
        if (params & kBatteryDataTrackVideo) {
            info.videoLastTime -= time;
            info.refCount ++;
        }
    } else {
        if (info.refCount == 0) {
            ALOGW("Battery track warning: refCount is already 0");
            return;
        } else if (info.refCount < 0) {
            ALOGE("Battery track error: refCount < 0");
            mBatteryData.removeItem(uid);
            return;
        }

        if (params & kBatteryDataTrackAudio) {
            info.audioLastTime += time;
            info.refCount --;
        }
        if (params & kBatteryDataTrackVideo) {
            info.videoLastTime += time;
            info.refCount --;
        }

        // no stream is being played by this UID
        if (info.refCount == 0) {
            info.audioTotalTime += info.audioLastTime;
            info.audioLastTime = 0;
            info.videoTotalTime += info.videoLastTime;
            info.videoLastTime = 0;
        }
    }
}

status_t MediaPlayerService::pullBatteryData(Parcel* reply) {
    Mutex::Autolock lock(mLock);

    // audio output devices usage
    int32_t time = systemTime() / 1000000L; //in ms
    int32_t totalTime;

    for (int i = 0; i < NUM_AUDIO_DEVICES; i++) {
        totalTime = mBatteryAudio.totalTime[i];

        if (mBatteryAudio.deviceOn[i]
            && (mBatteryAudio.lastTime[i] != 0)) {
                int32_t tmpTime = mBatteryAudio.lastTime[i] + time;
                totalTime += tmpTime;
        }

        reply->writeInt32(totalTime);
        // reset the total time
        mBatteryAudio.totalTime[i] = 0;
   }

    // codec usage
    BatteryUsageInfo info;
    int size = mBatteryData.size();

    reply->writeInt32(size);
    int i = 0;

    while (i < size) {
        info = mBatteryData.valueAt(i);

        reply->writeInt32(mBatteryData.keyAt(i)); //UID
        reply->writeInt32(info.audioTotalTime);
        reply->writeInt32(info.videoTotalTime);

        info.audioTotalTime = 0;
        info.videoTotalTime = 0;

        // remove the UID entry where no stream is being played
        if (info.refCount <= 0) {
            mBatteryData.removeItemsAt(i);
            size --;
            i --;
        }
        i++;
    }
    return NO_ERROR;
}
} // namespace android
