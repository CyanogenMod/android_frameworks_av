/*
 * Copyright (C) 2011 The Android Open Source Project
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
 *  (C) 2011-2015 Dolby Laboratories, Inc.
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
 **
 ** This file was modified by DTS, Inc. The portions of the
 ** code that are surrounded by "DTS..." are copyrighted and
 ** licensed separately, as follows:
 **
 **  (C) 2014 DTS, Inc.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **    http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftOMXPlugin"
#include <utils/Log.h>

#include "SoftOMXPlugin.h"
#include "include/SoftOMXComponent.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

#include <dlfcn.h>

namespace android {

static const struct {
    const char *mName;
    const char *mLibNameSuffix;
    const char *mRole;

} kComponents[] = {
    { "OMX.ffmpeg.mpeg2.decoder", "ffmpegvdec", "video_decoder.mpeg2" },
    { "OMX.ffmpeg.h263.decoder", "ffmpegvdec", "video_decoder.h263" },
    { "OMX.ffmpeg.mpeg4.decoder", "ffmpegvdec", "video_decoder.mpeg4" },
    { "OMX.ffmpeg.wmv.decoder", "ffmpegvdec", "video_decoder.wmv" },
    { "OMX.ffmpeg.rv.decoder", "ffmpegvdec", "video_decoder.rv" },
    { "OMX.ffmpeg.h264.decoder", "ffmpegvdec", "video_decoder.avc" },
    { "OMX.ffmpeg.vp8.decoder", "ffmpegvdec", "video_decoder.vp8" },
    { "OMX.ffmpeg.vc1.decoder", "ffmpegvdec", "video_decoder.vc1" },
    { "OMX.ffmpeg.flv1.decoder", "ffmpegvdec", "video_decoder.flv1" },
    { "OMX.ffmpeg.divx.decoder", "ffmpegvdec", "video_decoder.divx" },
    { "OMX.ffmpeg.hevc.decoder", "ffmpegvdec", "video_decoder.hevc" },
    { "OMX.ffmpeg.vtrial.decoder", "ffmpegvdec", "video_decoder.trial" },
    { "OMX.ffmpeg.aac.decoder", "ffmpegadec", "audio_decoder.aac" },
    { "OMX.ffmpeg.mp3.decoder", "ffmpegadec", "audio_decoder.mp3" },
    { "OMX.ffmpeg.vorbis.decoder", "ffmpegadec", "audio_decoder.vorbis" },
    { "OMX.ffmpeg.wma.decoder", "ffmpegadec", "audio_decoder.wma" },
    { "OMX.ffmpeg.ra.decoder", "ffmpegadec", "audio_decoder.ra" },
    { "OMX.ffmpeg.flac.decoder", "ffmpegadec", "audio_decoder.flac" },
    { "OMX.ffmpeg.mp2.decoder", "ffmpegadec", "audio_decoder.mp2" },
    { "OMX.ffmpeg.ac3.decoder", "ffmpegadec", "audio_decoder.ac3" },
    { "OMX.ffmpeg.ape.decoder", "ffmpegadec", "audio_decoder.ape" },
    { "OMX.ffmpeg.dts.decoder", "ffmpegadec", "audio_decoder.dts" },
    { "OMX.ffmpeg.atrial.decoder", "ffmpegadec", "audio_decoder.trial" },
    { "OMX.google.aac.decoder", "aacdec", "audio_decoder.aac" },
    { "OMX.google.aac.encoder", "aacenc", "audio_encoder.aac" },
    { "OMX.google.amrnb.decoder", "amrdec", "audio_decoder.amrnb" },
    { "OMX.google.amrnb.encoder", "amrnbenc", "audio_encoder.amrnb" },
    { "OMX.google.amrwb.decoder", "amrdec", "audio_decoder.amrwb" },
    { "OMX.google.amrwb.encoder", "amrwbenc", "audio_encoder.amrwb" },
    { "OMX.google.h264.decoder", "h264dec", "video_decoder.avc" },
    { "OMX.google.h264.encoder", "h264enc", "video_encoder.avc" },
    { "OMX.google.hevc.decoder", "hevcdec", "video_decoder.hevc" },
    { "OMX.google.g711.alaw.decoder", "g711dec", "audio_decoder.g711alaw" },
    { "OMX.google.g711.mlaw.decoder", "g711dec", "audio_decoder.g711mlaw" },
    { "OMX.google.h263.decoder", "mpeg4dec", "video_decoder.h263" },
    { "OMX.google.h263.encoder", "mpeg4enc", "video_encoder.h263" },
    { "OMX.google.mpeg4.decoder", "mpeg4dec", "video_decoder.mpeg4" },
    { "OMX.google.mpeg4.encoder", "mpeg4enc", "video_encoder.mpeg4" },
    { "OMX.google.mp3.decoder", "mp3dec", "audio_decoder.mp3" },
    { "OMX.google.vorbis.decoder", "vorbisdec", "audio_decoder.vorbis" },
    { "OMX.google.opus.decoder", "opusdec", "audio_decoder.opus" },
    { "OMX.google.vp8.decoder", "vpxdec", "video_decoder.vp8" },
    { "OMX.google.vp9.decoder", "vpxdec", "video_decoder.vp9" },
    { "OMX.google.vp8.encoder", "vpxenc", "video_encoder.vp8" },
    { "OMX.google.raw.decoder", "rawdec", "audio_decoder.raw" },
    { "OMX.google.flac.encoder", "flacenc", "audio_encoder.flac" },
    { "OMX.google.gsm.decoder", "gsmdec", "audio_decoder.gsm" },
#ifdef DOLBY_UDC
    { "OMX.dolby.ac3.decoder", "ddpdec", "audio_decoder.ac3" },
    { "OMX.dolby.eac3.decoder", "ddpdec", "audio_decoder.eac3" },
    { "OMX.dolby.eac3_joc.decoder", "ddpdec", "audio_decoder.eac3_joc" },
#endif // DOLBY_END
#ifdef DTS_CODEC_M_
    { "OMX.DTS.audio_decoder.dts", "dtsdec", "audio_decoder.dts" },
#endif
};

static const size_t kNumComponents =
    sizeof(kComponents) / sizeof(kComponents[0]);

SoftOMXPlugin::SoftOMXPlugin() {
}

OMX_ERRORTYPE SoftOMXPlugin::makeComponentInstance(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    ALOGV("makeComponentInstance '%s'", name);

    dlerror(); // clear any existing error
    for (size_t i = 0; i < kNumComponents; ++i) {
        if (strcmp(name, kComponents[i].mName)) {
            continue;
        }

        AString libName = "libstagefright_soft_";
        libName.append(kComponents[i].mLibNameSuffix);
        libName.append(".so");

        void *libHandle = dlopen(libName.c_str(), RTLD_NOW);

        if (libHandle == NULL) {
            ALOGE("unable to dlopen %s: %s", libName.c_str(), dlerror());

            return OMX_ErrorComponentNotFound;
        }

        ALOGV("load component %s for %s", libName.c_str(), name);

        typedef SoftOMXComponent *(*CreateSoftOMXComponentFunc)(
                const char *, const OMX_CALLBACKTYPE *,
                OMX_PTR, OMX_COMPONENTTYPE **);

        CreateSoftOMXComponentFunc createSoftOMXComponent =
            (CreateSoftOMXComponentFunc)dlsym(
                    libHandle,
                    "_Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPE"
                    "PvPP17OMX_COMPONENTTYPE");

        if (const char *error = dlerror()) {
            ALOGE("unable to dlsym %s: %s", libName.c_str(), error);
            dlclose(libHandle);
            libHandle = NULL;

            return OMX_ErrorComponentNotFound;
        }

        sp<SoftOMXComponent> codec =
            (*createSoftOMXComponent)(name, callbacks, appData, component);

        if (codec == NULL) {
            dlclose(libHandle);
            libHandle = NULL;

            return OMX_ErrorInsufficientResources;
        }

        OMX_ERRORTYPE err = codec->initCheck();
        if (err != OMX_ErrorNone) {
            dlclose(libHandle);
            libHandle = NULL;

            return err;
        }

        codec->incStrong(this);
        codec->setLibHandle(libHandle);

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

OMX_ERRORTYPE SoftOMXPlugin::destroyComponentInstance(
        OMX_COMPONENTTYPE *component) {
    SoftOMXComponent *me =
        (SoftOMXComponent *)
            ((OMX_COMPONENTTYPE *)component)->pComponentPrivate;

    me->prepareForDestruction();

    void *libHandle = me->libHandle();

    CHECK_EQ(me->getStrongCount(), 1);
    me->decStrong(this);
    me = NULL;

    dlclose(libHandle);
    libHandle = NULL;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftOMXPlugin::enumerateComponents(
        OMX_STRING name,
        size_t /* size */,
        OMX_U32 index) {
    if (index >= kNumComponents) {
        return OMX_ErrorNoMore;
    }

    strcpy(name, kComponents[index].mName);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftOMXPlugin::getRolesOfComponent(
        const char *name,
        Vector<String8> *roles) {
    for (size_t i = 0; i < kNumComponents; ++i) {
        if (strcmp(name, kComponents[i].mName)) {
            continue;
        }

        roles->clear();
        roles->push(String8(kComponents[i].mRole));

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

}  // namespace android
