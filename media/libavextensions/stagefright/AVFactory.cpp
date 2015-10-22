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

#define LOG_TAG "AVFactory"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaHTTP.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/CameraSource.h>
#include <media/stagefright/CameraSourceTimeLapse.h>
#include <camera/CameraParameters.h>

#include "common/ExtensionsLoader.hpp"
#include "stagefright/AVExtensions.h"
#include "include/NuCachedSource2.h"

namespace android {

sp<ACodec> AVFactory::createACodec() {
    return new ACodec;
}

MediaExtractor* AVFactory::createExtendedExtractor(
         const sp<DataSource> &, const char *, const sp<AMessage> &) {
    return NULL;
}

sp<MediaExtractor> AVFactory::updateExtractor(
            sp<MediaExtractor> ext, const sp<DataSource> &,
            const char *, const sp<AMessage> &) {
    return ext;
}

sp<NuCachedSource2> AVFactory::createCachedSource(
            const sp<DataSource> &source,
            const char *cacheConfig,
            bool disconnectAtHighwatermark) {
    return new NuCachedSource2(source, cacheConfig, disconnectAtHighwatermark);
}

MediaHTTP* AVFactory::createMediaHTTP(
         const sp<IMediaHTTPConnection> &conn) {
    return new MediaHTTP(conn);
}

AudioSource* AVFactory::createAudioSource(
            audio_source_t inputSource,
            const String16 &opPackageName,
            uint32_t sampleRate,
            uint32_t channels,
            uint32_t outSampleRate) {
    return new AudioSource(inputSource, opPackageName, sampleRate,
                            channels, outSampleRate);
}

CameraSource* AVFactory::CreateCameraSourceFromCamera(
            const sp<ICamera> &camera,
            const sp<ICameraRecordingProxy> &proxy,
            int32_t cameraId,
            const String16& clientName,
            uid_t clientUid,
            Size videoSize,
            int32_t frameRate,
            const sp<IGraphicBufferProducer>& surface,
            bool storeMetaDataInVideoBuffers) {
    return CameraSource::CreateFromCamera(camera, proxy, cameraId,
            clientName, clientUid, videoSize, frameRate, surface,
            storeMetaDataInVideoBuffers);
}

CameraSourceTimeLapse* AVFactory::CreateCameraSourceTimeLapseFromCamera(
        const sp<ICamera> &camera,
        const sp<ICameraRecordingProxy> &proxy,
        int32_t cameraId,
        const String16& clientName,
        uid_t clientUid,
        Size videoSize,
        int32_t videoFrameRate,
        const sp<IGraphicBufferProducer>& surface,
        int64_t timeBetweenFrameCaptureUs,
        bool storeMetaDataInVideoBuffers) {
    return CameraSourceTimeLapse::CreateFromCamera(camera, proxy, cameraId,
            clientName, clientUid, videoSize, videoFrameRate, surface,
            timeBetweenFrameCaptureUs, storeMetaDataInVideoBuffers);
}
// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVFactory::AVFactory() {
}

AVFactory::~AVFactory() {
}

//static
AVFactory *AVFactory::sInst =
        ExtensionsLoader<AVFactory>::createInstance("createExtendedFactory");

} //namespace android

