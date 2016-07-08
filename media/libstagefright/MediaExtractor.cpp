/*
 * Copyright (C) 2009 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaExtractor"
#include <utils/Log.h>
#include <inttypes.h>
#include <pwd.h>

#include "include/AMRExtractor.h"
#include "include/MP3Extractor.h"
#include "include/MPEG4Extractor.h"
#include "include/WAVExtractor.h"
#include "include/OggExtractor.h"
#include "include/MPEG2PSExtractor.h"
#include "include/MPEG2TSExtractor.h"
#include "include/DRMExtractor.h"
#include "include/WVMExtractor.h"
#include "include/FLACExtractor.h"
#include "include/AACExtractor.h"
#include "include/MidiExtractor.h"

#include "matroska/MatroskaExtractor.h"

#include <binder/IServiceManager.h>
#include <binder/MemoryDealer.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <media/IMediaExtractorService.h>
#include <cutils/properties.h>
#include <utils/String8.h>
#include <private/android_filesystem_config.h>


namespace android {

MediaExtractor::MediaExtractor():
    mIsDrm(false) {
    if (!LOG_NDEBUG) {
        uid_t uid = getuid();
        struct passwd *pw = getpwuid(uid);
        ALOGI("extractor created in uid: %d (%s)", getuid(), pw->pw_name);
    }

}


sp<MetaData> MediaExtractor::getMetaData() {
    return new MetaData;
}

uint32_t MediaExtractor::flags() const {
    return CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_PAUSE | CAN_SEEK;
}



class RemoteDataSource : public BnDataSource {
public:
    enum {
        kBufferSize = 64 * 1024,
    };

    static sp<IDataSource> wrap(const sp<DataSource> &source);
    virtual ~RemoteDataSource();

    virtual sp<IMemory> getIMemory();
    virtual ssize_t readAt(off64_t offset, size_t size);
    virtual status_t getSize(off64_t* size);
    virtual void close();
    virtual uint32_t getFlags();
    virtual String8 toString();
    virtual sp<DecryptHandle> DrmInitialization(const char *mime);

private:
    sp<IMemory> mMemory;
    sp<DataSource> mSource;
    String8 mName;
    RemoteDataSource(const sp<DataSource> &source);
    DISALLOW_EVIL_CONSTRUCTORS(RemoteDataSource);
};


sp<IDataSource> RemoteDataSource::wrap(const sp<DataSource> &source) {
    return new RemoteDataSource(source);
}
RemoteDataSource::RemoteDataSource(const sp<DataSource> &source) {
    mSource = source;
    sp<MemoryDealer> memoryDealer = new MemoryDealer(kBufferSize, "RemoteDataSource");
    mMemory = memoryDealer->allocate(kBufferSize);
    if (mMemory == NULL) {
        ALOGE("Failed to allocate memory!");
    }
    mName = String8::format("RemoteDataSource(%s)", mSource->toString().string());
}
RemoteDataSource::~RemoteDataSource() {
    close();
}
sp<IMemory> RemoteDataSource::getIMemory() {
    return mMemory;
}
ssize_t RemoteDataSource::readAt(off64_t offset, size_t size) {
    ALOGV("readAt(%" PRId64 ", %zu)", offset, size);
    return mSource->readAt(offset, mMemory->pointer(), size);
}
status_t RemoteDataSource::getSize(off64_t* size) {
    return mSource->getSize(size);
}
void RemoteDataSource::close() {
    mSource = NULL;
}
uint32_t RemoteDataSource::getFlags() {
    return mSource->flags();
}

String8 RemoteDataSource::toString() {
    return mName;
}

sp<DecryptHandle> RemoteDataSource::DrmInitialization(const char *mime) {
    return mSource->DrmInitialization(mime);
}

// static
sp<IMediaExtractor> MediaExtractor::Create(
        const sp<DataSource> &source, const char *mime) {
    ALOGV("MediaExtractor::Create %s", mime);

    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.extractremote", value, NULL)
            && (!strcmp("0", value) || !strcasecmp("false", value))) {
        // local extractor
        ALOGW("creating media extractor in calling process");
        return CreateFromService(source, mime);
    } else {
        // Check if it's WVM, since WVMExtractor needs to be created in the media server process,
        // not the extractor process.
        String8 mime8;
        float confidence;
        sp<AMessage> meta;
        if (SniffWVM(source, &mime8, &confidence, &meta) &&
                !strcasecmp(mime8, MEDIA_MIMETYPE_CONTAINER_WVM)) {
            return new WVMExtractor(source);
        }

        // Check if it's es-based DRM, since DRMExtractor needs to be created in the media server
        // process, not the extractor process.
        if (SniffDRM(source, &mime8, &confidence, &meta)) {
            const char *drmMime = mime8.string();
            ALOGV("Detected media content as '%s' with confidence %.2f", drmMime, confidence);
            if (!strncmp(drmMime, "drm+es_based+", 13)) {
                // DRMExtractor sets container metadata kKeyIsDRM to 1
                return new DRMExtractor(source, drmMime + 14);
            }
        }

        // remote extractor
        ALOGV("get service manager");
        sp<IBinder> binder = defaultServiceManager()->getService(String16("media.extractor"));

        if (binder != 0) {
            sp<IMediaExtractorService> mediaExService(interface_cast<IMediaExtractorService>(binder));
            sp<IMediaExtractor> ex = mediaExService->makeExtractor(RemoteDataSource::wrap(source), mime);
            return ex;
        } else {
            ALOGE("extractor service not running");
            return NULL;
        }
    }
    return NULL;
}

sp<MediaExtractor> MediaExtractor::CreateFromService(
        const sp<DataSource> &source, const char *mime) {

    ALOGV("MediaExtractor::CreateFromService %s", mime);
    DataSource::RegisterDefaultSniffers();

    sp<AMessage> meta;

    String8 tmp;
    if (mime == NULL) {
        float confidence;
        if (!source->sniff(&tmp, &confidence, &meta)) {
            ALOGV("FAILED to autodetect media content.");

            return NULL;
        }

        mime = tmp.string();
        ALOGV("Autodetected media content as '%s' with confidence %.2f",
             mime, confidence);
    }

    bool isDrm = false;
    // DRM MIME type syntax is "drm+type+original" where
    // type is "es_based" or "container_based" and
    // original is the content's cleartext MIME type
    if (!strncmp(mime, "drm+", 4)) {
        const char *originalMime = strchr(mime+4, '+');
        if (originalMime == NULL) {
            // second + not found
            return NULL;
        }
        ++originalMime;
        if (!strncmp(mime, "drm+es_based+", 13)) {
            // DRMExtractor sets container metadata kKeyIsDRM to 1
            return new DRMExtractor(source, originalMime);
        } else if (!strncmp(mime, "drm+container_based+", 20)) {
            mime = originalMime;
            isDrm = true;
        } else {
            return NULL;
        }
    }

    MediaExtractor *ret = NULL;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)
            || !strcasecmp(mime, "audio/mp4")) {
        ret = new MPEG4Extractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
        ret = new MP3Extractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_NB)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        ret = new AMRExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)) {
        ret = new FLACExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WAV)) {
        ret = new WAVExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_OGG)) {
        ret = new OggExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)) {
        ret = new MatroskaExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) {
        ret = new MPEG2TSExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WVM) && getuid() == AID_MEDIA) {
        // Return now.  WVExtractor should not have the DrmFlag set in the block below.
        return new WVMExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC_ADTS)) {
        ret = new AACExtractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2PS)) {
        ret = new MPEG2PSExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MIDI)) {
        ret = new MidiExtractor(source);
    }

    if (ret != NULL) {
       if (isDrm) {
           ret->setDrmFlag(true);
       } else {
           ret->setDrmFlag(false);
       }
    }

    return ret;
}

}  // namespace android
