/*
 * Copyright (c) 2013 - 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _AV_EXTENSIONS_H_
#define _AV_EXTENSIONS_H_

#include <media/stagefright/DataSource.h>
#include <common/AVExtensionsCommon.h>
#include <system/audio.h>

namespace android {

struct ACodec;
class MediaExtractor;
struct MediaCodec;
class AudioParameter;

/*
 * Factory to create objects of base-classes in libstagefright
 */
struct AVFactory {
    virtual sp<ACodec> createACodec();
    virtual MediaExtractor* createExtendedExtractor(
            const sp<DataSource> &source, const char *mime);

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVFactory);
};

/*
 * Common delegate to the classes in libstagefright
 */
struct AVUtils {

    virtual status_t convertMetaDataToMessage(
            const sp<MetaData> &meta, sp<AMessage> *format);
    virtual DataSource::SnifferFunc getExtendedSniffer();
    virtual status_t mapMimeToAudioFormat( audio_format_t& format, const char* mime);
    virtual status_t sendMetaDataToHal(const sp<MetaData>& meta, AudioParameter *param);

    virtual sp<MediaCodec> createCustomComponentByName(const sp<ALooper> &looper,
                const char* mime, bool encoder);

    virtual bool is24bitPCMOffloadEnabled();
    virtual bool is16bitPCMOffloadEnabled();
    virtual int getPcmSampleBits(const sp<MetaData> &);
    virtual int getPcmSampleBits(const sp<AMessage> &);
    virtual void setPcmSampleBits(const sp<MetaData> &, int32_t /*bitWidth*/);
    virtual void setPcmSampleBits(const sp<AMessage> &, int32_t /*bitWidth*/);

    virtual audio_format_t updateAudioFormat(audio_format_t audioFormat,
            const sp<MetaData> &);

    virtual audio_format_t updateAudioFormat(audio_format_t audioFormat,
            const sp<AMessage> &);

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVUtils);
};

}

#endif // _AV_EXTENSIONS__H_
