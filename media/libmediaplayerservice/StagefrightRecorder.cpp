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
#define LOG_TAG "StagefrightRecorder"
#include <inttypes.h>
#include <utils/Log.h>

#include <inttypes.h>
#include "WebmWriter.h"
#include "StagefrightRecorder.h"

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>

#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/AMRWriter.h>
#include <media/stagefright/AACWriter.h>
#include <media/stagefright/CameraSource.h>
#include <media/stagefright/CameraSourceTimeLapse.h>
#include <media/stagefright/MPEG2TSWriter.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaCodecSource.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/FLACWriter.h>
#include <media/stagefright/WAVEWriter.h>
#include <media/MediaProfiles.h>
#include <camera/ICamera.h>
#include <camera/CameraParameters.h>

#include <utils/Errors.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>

#include <system/audio.h>

#include "ARTPWriter.h"
#include <stagefright/AVExtensions.h>

namespace android {

static const int64_t kMax32BitFileSize = 0x00ffffffffLL; // 4GB

// To collect the encoder usage for the battery app
static void addBatteryData(uint32_t params) {
    sp<IBinder> binder =
        defaultServiceManager()->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    CHECK(service.get() != NULL);

    service->addBatteryData(params);
}


StagefrightRecorder::StagefrightRecorder(const String16 &opPackageName)
    : MediaRecorderBase(opPackageName),
      mWriter(NULL),
      mOutputFd(-1),
      mAudioSource(AUDIO_SOURCE_CNT),
      mVideoSource(VIDEO_SOURCE_LIST_END),
      mStarted(false),
      mRecPaused(false) {

    ALOGV("Constructor");
    reset();
}

StagefrightRecorder::~StagefrightRecorder() {
    ALOGV("Destructor");
    stop();

    if (mLooper != NULL) {
        mLooper->stop();
    }
}

status_t StagefrightRecorder::init() {
    ALOGV("init");

    mLooper = new ALooper;
    mLooper->setName("recorder_looper");
    mLooper->start();

    return OK;
}

// The client side of mediaserver asks it to creat a SurfaceMediaSource
// and return a interface reference. The client side will use that
// while encoding GL Frames
sp<IGraphicBufferProducer> StagefrightRecorder::querySurfaceMediaSource() const {
    ALOGV("Get SurfaceMediaSource");
    return mGraphicBufferProducer;
}

status_t StagefrightRecorder::setAudioSource(audio_source_t as) {
    ALOGV("setAudioSource: %d", as);
    if (as < AUDIO_SOURCE_DEFAULT ||
        (as >= AUDIO_SOURCE_CNT && as != AUDIO_SOURCE_FM_TUNER)) {
        ALOGE("Invalid audio source: %d", as);
        return BAD_VALUE;
    }

    if (as == AUDIO_SOURCE_DEFAULT) {
        mAudioSource = AUDIO_SOURCE_MIC;
    } else {
        mAudioSource = as;
    }

    return OK;
}

status_t StagefrightRecorder::setVideoSource(video_source vs) {
    ALOGV("setVideoSource: %d", vs);
    if (vs < VIDEO_SOURCE_DEFAULT ||
        vs >= VIDEO_SOURCE_LIST_END) {
        ALOGE("Invalid video source: %d", vs);
        return BAD_VALUE;
    }

    if (vs == VIDEO_SOURCE_DEFAULT) {
        mVideoSource = VIDEO_SOURCE_CAMERA;
    } else {
        mVideoSource = vs;
    }

    return OK;
}

status_t StagefrightRecorder::setOutputFormat(output_format of) {
    ALOGV("setOutputFormat: %d", of);
    if (of < OUTPUT_FORMAT_DEFAULT ||
        of >= OUTPUT_FORMAT_LIST_END) {
        ALOGE("Invalid output format: %d", of);
        return BAD_VALUE;
    }

    if (of == OUTPUT_FORMAT_DEFAULT) {
        mOutputFormat = OUTPUT_FORMAT_THREE_GPP;
    } else {
        mOutputFormat = of;
    }

    return OK;
}

status_t StagefrightRecorder::setAudioEncoder(audio_encoder ae) {
    ALOGV("setAudioEncoder: %d", ae);
    if (ae < AUDIO_ENCODER_DEFAULT ||
        ae >= AUDIO_ENCODER_LIST_END) {
        ALOGE("Invalid audio encoder: %d", ae);
        return BAD_VALUE;
    }

    if (ae == AUDIO_ENCODER_DEFAULT) {
        mAudioEncoder = AUDIO_ENCODER_AMR_NB;
    } else {
        mAudioEncoder = ae;
    }

    return OK;
}

status_t StagefrightRecorder::setVideoEncoder(video_encoder ve) {
    ALOGV("setVideoEncoder: %d", ve);
    if (ve < VIDEO_ENCODER_DEFAULT ||
        (ve >= VIDEO_ENCODER_LIST_END && ve <= VIDEO_ENCODER_LIST_VENDOR_START) ||
        ve >= VIDEO_ENCODER_LIST_VENDOR_END) {
        ALOGE("Invalid video encoder: %d", ve);
        return BAD_VALUE;
    }

    mVideoEncoder = ve;

    return OK;
}

status_t StagefrightRecorder::setVideoSize(int width, int height) {
    ALOGV("setVideoSize: %dx%d", width, height);
    if (width <= 0 || height <= 0) {
        ALOGE("Invalid video size: %dx%d", width, height);
        return BAD_VALUE;
    }

    // Additional check on the dimension will be performed later
    mVideoWidth = width;
    mVideoHeight = height;

    return OK;
}

status_t StagefrightRecorder::setVideoFrameRate(int frames_per_second) {
    ALOGV("setVideoFrameRate: %d", frames_per_second);
    if ((frames_per_second <= 0 && frames_per_second != -1) ||
        frames_per_second > kMaxHighSpeedFps) {
        ALOGE("Invalid video frame rate: %d", frames_per_second);
        return BAD_VALUE;
    }

    // Additional check on the frame rate will be performed later
    mFrameRate = frames_per_second;

    return OK;
}

status_t StagefrightRecorder::setCamera(const sp<ICamera> &camera,
                                        const sp<ICameraRecordingProxy> &proxy) {
    ALOGV("setCamera");
    if (camera == 0) {
        ALOGE("camera is NULL");
        return BAD_VALUE;
    }
    if (proxy == 0) {
        ALOGE("camera proxy is NULL");
        return BAD_VALUE;
    }

    mCamera = camera;
    mCameraProxy = proxy;
    return OK;
}

status_t StagefrightRecorder::setPreviewSurface(const sp<IGraphicBufferProducer> &surface) {
    ALOGV("setPreviewSurface: %p", surface.get());
    mPreviewSurface = surface;

    return OK;
}

status_t StagefrightRecorder::setInputSurface(
        const sp<IGraphicBufferConsumer>& surface) {
    mPersistentSurface = surface;

    return OK;
}

status_t StagefrightRecorder::setOutputFile(int fd, int64_t offset, int64_t length) {
    ALOGV("setOutputFile: %d, %" PRId64 ", %" PRId64 "", fd, offset, length);
    // These don't make any sense, do they?
    CHECK_EQ(offset, 0ll);
    CHECK_EQ(length, 0ll);

    if (fd < 0) {
        ALOGE("Invalid file descriptor: %d", fd);
        return -EBADF;
    }

    // start with a clean, empty file
    ftruncate(fd, 0);

    if (mOutputFd >= 0) {
        ::close(mOutputFd);
    }
    mOutputFd = dup(fd);

    return OK;
}

// Attempt to parse an float literal optionally surrounded by whitespace,
// returns true on success, false otherwise.
static bool safe_strtof(const char *s, float *val) {
    char *end;

    // It is lame, but according to man page, we have to set errno to 0
    // before calling strtof().
    errno = 0;
    *val = strtof(s, &end);

    if (end == s || errno == ERANGE) {
        return false;
    }

    // Skip trailing whitespace
    while (isspace(*end)) {
        ++end;
    }

    // For a successful return, the string must contain nothing but a valid
    // float literal optionally surrounded by whitespace.

    return *end == '\0';
}

// Attempt to parse an int64 literal optionally surrounded by whitespace,
// returns true on success, false otherwise.
static bool safe_strtoi64(const char *s, int64_t *val) {
    char *end;

    // It is lame, but according to man page, we have to set errno to 0
    // before calling strtoll().
    errno = 0;
    *val = strtoll(s, &end, 10);

    if (end == s || errno == ERANGE) {
        return false;
    }

    // Skip trailing whitespace
    while (isspace(*end)) {
        ++end;
    }

    // For a successful return, the string must contain nothing but a valid
    // int64 literal optionally surrounded by whitespace.

    return *end == '\0';
}

// Return true if the value is in [0, 0x007FFFFFFF]
static bool safe_strtoi32(const char *s, int32_t *val) {
    int64_t temp;
    if (safe_strtoi64(s, &temp)) {
        if (temp >= 0 && temp <= 0x007FFFFFFF) {
            *val = static_cast<int32_t>(temp);
            return true;
        }
    }
    return false;
}

// Trim both leading and trailing whitespace from the given string.
static void TrimString(String8 *s) {
    size_t num_bytes = s->bytes();
    const char *data = s->string();

    size_t leading_space = 0;
    while (leading_space < num_bytes && isspace(data[leading_space])) {
        ++leading_space;
    }

    size_t i = num_bytes;
    while (i > leading_space && isspace(data[i - 1])) {
        --i;
    }

    s->setTo(String8(&data[leading_space], i - leading_space));
}

status_t StagefrightRecorder::setParamAudioSamplingRate(int32_t sampleRate) {
    ALOGV("setParamAudioSamplingRate: %d", sampleRate);
    if (sampleRate <= 0) {
        ALOGE("Invalid audio sampling rate: %d", sampleRate);
        return BAD_VALUE;
    }

    // Additional check on the sample rate will be performed later.
    mSampleRate = sampleRate;
    return OK;
}

status_t StagefrightRecorder::setParamAudioNumberOfChannels(int32_t channels) {
    ALOGV("setParamAudioNumberOfChannels: %d", channels);
    if (channels <= 0 || channels >= 7) {
        ALOGE("Invalid number of audio channels: %d", channels);
        return BAD_VALUE;
    }

    // Additional check on the number of channels will be performed later.
    mAudioChannels = channels;
    return OK;
}

status_t StagefrightRecorder::setParamAudioEncodingBitRate(int32_t bitRate) {
    ALOGV("setParamAudioEncodingBitRate: %d", bitRate);
    if (bitRate <= 0) {
        ALOGE("Invalid audio encoding bit rate: %d", bitRate);
        return BAD_VALUE;
    }

    // The target bit rate may not be exactly the same as the requested.
    // It depends on many factors, such as rate control, and the bit rate
    // range that a specific encoder supports. The mismatch between the
    // the target and requested bit rate will NOT be treated as an error.
    mAudioBitRate = bitRate;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncodingBitRate(int32_t bitRate) {
    ALOGV("setParamVideoEncodingBitRate: %d", bitRate);
    if (bitRate <= 0) {
        ALOGE("Invalid video encoding bit rate: %d", bitRate);
        return BAD_VALUE;
    }

    // The target bit rate may not be exactly the same as the requested.
    // It depends on many factors, such as rate control, and the bit rate
    // range that a specific encoder supports. The mismatch between the
    // the target and requested bit rate will NOT be treated as an error.
    mVideoBitRate = bitRate;
    return OK;
}

// Always rotate clockwise, and only support 0, 90, 180 and 270 for now.
status_t StagefrightRecorder::setParamVideoRotation(int32_t degrees) {
    ALOGV("setParamVideoRotation: %d", degrees);
    if (degrees < 0 || degrees % 90 != 0) {
        ALOGE("Unsupported video rotation angle: %d", degrees);
        return BAD_VALUE;
    }
    mRotationDegrees = degrees % 360;
    return OK;
}

status_t StagefrightRecorder::setParamMaxFileDurationUs(int64_t timeUs) {
    ALOGV("setParamMaxFileDurationUs: %" PRId64 " us", timeUs);

    // This is meant for backward compatibility for MediaRecorder.java
    if (timeUs <= 0) {
        ALOGW("Max file duration is not positive: %" PRId64 " us. Disabling duration limit.", timeUs);
        timeUs = 0; // Disable the duration limit for zero or negative values.
    } else if (timeUs <= 100000LL) {  // XXX: 100 milli-seconds
        ALOGE("Max file duration is too short: %" PRId64 " us", timeUs);
        return BAD_VALUE;
    }

    if (timeUs <= 15 * 1000000LL) {
        ALOGW("Target duration (%" PRId64 " us) too short to be respected", timeUs);
    }
    mMaxFileDurationUs = timeUs;
    return OK;
}

status_t StagefrightRecorder::setParamMaxFileSizeBytes(int64_t bytes) {
    ALOGV("setParamMaxFileSizeBytes: %" PRId64 " bytes", bytes);

    // This is meant for backward compatibility for MediaRecorder.java
    if (bytes <= 0) {
        ALOGW("Max file size is not positive: %" PRId64 " bytes. "
             "Disabling file size limit.", bytes);
        bytes = 0; // Disable the file size limit for zero or negative values.
    } else if (bytes <= 1024) {  // XXX: 1 kB
        ALOGE("Max file size is too small: %" PRId64 " bytes", bytes);
        return BAD_VALUE;
    }

    if (bytes <= 100 * 1024) {
        ALOGW("Target file size (%" PRId64 " bytes) is too small to be respected", bytes);
    }

    mMaxFileSizeBytes = bytes;

    // If requested size is >4GB, force 64-bit offsets
    mUse64BitFileOffset |= (bytes >= kMax32BitFileSize);

    return OK;
}

status_t StagefrightRecorder::setParamInterleaveDuration(int32_t durationUs) {
    ALOGV("setParamInterleaveDuration: %d", durationUs);
    if (durationUs <= 500000) {           //  500 ms
        // If interleave duration is too small, it is very inefficient to do
        // interleaving since the metadata overhead will count for a significant
        // portion of the saved contents
        ALOGE("Audio/video interleave duration is too small: %d us", durationUs);
        return BAD_VALUE;
    } else if (durationUs >= 10000000) {  // 10 seconds
        // If interleaving duration is too large, it can cause the recording
        // session to use too much memory since we have to save the output
        // data before we write them out
        ALOGE("Audio/video interleave duration is too large: %d us", durationUs);
        return BAD_VALUE;
    }
    mInterleaveDurationUs = durationUs;
    return OK;
}

// If seconds <  0, only the first frame is I frame, and rest are all P frames
// If seconds == 0, all frames are encoded as I frames. No P frames
// If seconds >  0, it is the time spacing (seconds) between 2 neighboring I frames
status_t StagefrightRecorder::setParamVideoIFramesInterval(int32_t seconds) {
    ALOGV("setParamVideoIFramesInterval: %d seconds", seconds);
    mIFramesIntervalSec = seconds;
    return OK;
}

status_t StagefrightRecorder::setParam64BitFileOffset(bool use64Bit) {
    ALOGV("setParam64BitFileOffset: %s",
        use64Bit? "use 64 bit file offset": "use 32 bit file offset");
    mUse64BitFileOffset = use64Bit;
    return OK;
}

status_t StagefrightRecorder::setParamVideoCameraId(int32_t cameraId) {
    ALOGV("setParamVideoCameraId: %d", cameraId);
    if (cameraId < 0) {
        return BAD_VALUE;
    }
    mCameraId = cameraId;
    return OK;
}

status_t StagefrightRecorder::setParamTrackTimeStatus(int64_t timeDurationUs) {
    ALOGV("setParamTrackTimeStatus: %" PRId64 "", timeDurationUs);
    if (timeDurationUs < 20000) {  // Infeasible if shorter than 20 ms?
        ALOGE("Tracking time duration too short: %" PRId64 " us", timeDurationUs);
        return BAD_VALUE;
    }
    mTrackEveryTimeDurationUs = timeDurationUs;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncoderProfile(int32_t profile) {
    ALOGV("setParamVideoEncoderProfile: %d", profile);

    // Additional check will be done later when we load the encoder.
    // For now, we are accepting values defined in OpenMAX IL.
    mVideoEncoderProfile = profile;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncoderLevel(int32_t level) {
    ALOGV("setParamVideoEncoderLevel: %d", level);

    // Additional check will be done later when we load the encoder.
    // For now, we are accepting values defined in OpenMAX IL.
    mVideoEncoderLevel = level;
    return OK;
}

status_t StagefrightRecorder::setParamMovieTimeScale(int32_t timeScale) {
    ALOGV("setParamMovieTimeScale: %d", timeScale);

    // The range is set to be the same as the audio's time scale range
    // since audio's time scale has a wider range.
    if (timeScale < 600 || timeScale > 96000) {
        ALOGE("Time scale (%d) for movie is out of range [600, 96000]", timeScale);
        return BAD_VALUE;
    }
    mMovieTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamVideoTimeScale(int32_t timeScale) {
    ALOGV("setParamVideoTimeScale: %d", timeScale);

    // 60000 is chosen to make sure that each video frame from a 60-fps
    // video has 1000 ticks.
    if (timeScale < 600 || timeScale > 60000) {
        ALOGE("Time scale (%d) for video is out of range [600, 60000]", timeScale);
        return BAD_VALUE;
    }
    mVideoTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamAudioTimeScale(int32_t timeScale) {
    ALOGV("setParamAudioTimeScale: %d", timeScale);

    // 96000 Hz is the highest sampling rate support in AAC.
    if (timeScale < 600 || timeScale > 96000) {
        ALOGE("Time scale (%d) for audio is out of range [600, 96000]", timeScale);
        return BAD_VALUE;
    }
    mAudioTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamCaptureFpsEnable(int32_t captureFpsEnable) {
    ALOGV("setParamCaptureFpsEnable: %d", captureFpsEnable);

    if(captureFpsEnable == 0) {
        mCaptureFpsEnable = false;
    } else if (captureFpsEnable == 1) {
        mCaptureFpsEnable = true;
    } else {
        return BAD_VALUE;
    }
    return OK;
}

status_t StagefrightRecorder::setParamCaptureFps(float fps) {
    ALOGV("setParamCaptureFps: %.2f", fps);

    int64_t timeUs = (int64_t) (1000000.0f / fps + 0.5f);

    // Not allowing time more than a day
    if (timeUs <= 0 || timeUs > 86400*1E6) {
        ALOGE("Time between frame capture (%" PRId64 ") is out of range [0, 1 Day]", timeUs);
        return BAD_VALUE;
    }

    mCaptureFps = fps;
    mTimeBetweenCaptureUs = timeUs;
    return OK;
}

status_t StagefrightRecorder::setParamGeoDataLongitude(
    int64_t longitudex10000) {

    if (longitudex10000 > 1800000 || longitudex10000 < -1800000) {
        return BAD_VALUE;
    }
    mLongitudex10000 = longitudex10000;
    return OK;
}

status_t StagefrightRecorder::setParamGeoDataLatitude(
    int64_t latitudex10000) {

    if (latitudex10000 > 900000 || latitudex10000 < -900000) {
        return BAD_VALUE;
    }
    mLatitudex10000 = latitudex10000;
    return OK;
}

status_t StagefrightRecorder::setParameter(
        const String8 &key, const String8 &value) {
    ALOGV("setParameter: key (%s) => value (%s)", key.string(), value.string());
    if (key == "max-duration") {
        int64_t max_duration_ms;
        if (safe_strtoi64(value.string(), &max_duration_ms)) {
            return setParamMaxFileDurationUs(1000LL * max_duration_ms);
        }
    } else if (key == "max-filesize") {
        int64_t max_filesize_bytes;
        if (safe_strtoi64(value.string(), &max_filesize_bytes)) {
            return setParamMaxFileSizeBytes(max_filesize_bytes);
        }
    } else if (key == "interleave-duration-us") {
        int32_t durationUs;
        if (safe_strtoi32(value.string(), &durationUs)) {
            return setParamInterleaveDuration(durationUs);
        }
    } else if (key == "param-movie-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.string(), &timeScale)) {
            return setParamMovieTimeScale(timeScale);
        }
    } else if (key == "param-use-64bit-offset") {
        int32_t use64BitOffset;
        if (safe_strtoi32(value.string(), &use64BitOffset)) {
            return setParam64BitFileOffset(use64BitOffset != 0);
        }
    } else if (key == "param-geotag-longitude") {
        int64_t longitudex10000;
        if (safe_strtoi64(value.string(), &longitudex10000)) {
            return setParamGeoDataLongitude(longitudex10000);
        }
    } else if (key == "param-geotag-latitude") {
        int64_t latitudex10000;
        if (safe_strtoi64(value.string(), &latitudex10000)) {
            return setParamGeoDataLatitude(latitudex10000);
        }
    } else if (key == "param-track-time-status") {
        int64_t timeDurationUs;
        if (safe_strtoi64(value.string(), &timeDurationUs)) {
            return setParamTrackTimeStatus(timeDurationUs);
        }
    } else if (key == "audio-param-sampling-rate") {
        int32_t sampling_rate;
        if (safe_strtoi32(value.string(), &sampling_rate)) {
            return setParamAudioSamplingRate(sampling_rate);
        }
    } else if (key == "audio-param-number-of-channels") {
        int32_t number_of_channels;
        if (safe_strtoi32(value.string(), &number_of_channels)) {
            return setParamAudioNumberOfChannels(number_of_channels);
        }
    } else if (key == "audio-param-encoding-bitrate") {
        int32_t audio_bitrate;
        if (safe_strtoi32(value.string(), &audio_bitrate)) {
            return setParamAudioEncodingBitRate(audio_bitrate);
        }
    } else if (key == "audio-param-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.string(), &timeScale)) {
            return setParamAudioTimeScale(timeScale);
        }
    } else if (key == "video-param-encoding-bitrate") {
        int32_t video_bitrate;
        if (safe_strtoi32(value.string(), &video_bitrate)) {
            return setParamVideoEncodingBitRate(video_bitrate);
        }
    } else if (key == "video-param-rotation-angle-degrees") {
        int32_t degrees;
        if (safe_strtoi32(value.string(), &degrees)) {
            return setParamVideoRotation(degrees);
        }
    } else if (key == "video-param-i-frames-interval") {
        int32_t seconds;
        if (safe_strtoi32(value.string(), &seconds)) {
            return setParamVideoIFramesInterval(seconds);
        }
    } else if (key == "video-param-encoder-profile") {
        int32_t profile;
        if (safe_strtoi32(value.string(), &profile)) {
            return setParamVideoEncoderProfile(profile);
        }
    } else if (key == "video-param-encoder-level") {
        int32_t level;
        if (safe_strtoi32(value.string(), &level)) {
            return setParamVideoEncoderLevel(level);
        }
    } else if (key == "video-param-camera-id") {
        int32_t cameraId;
        if (safe_strtoi32(value.string(), &cameraId)) {
            return setParamVideoCameraId(cameraId);
        }
    } else if (key == "video-param-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.string(), &timeScale)) {
            return setParamVideoTimeScale(timeScale);
        }
    } else if (key == "time-lapse-enable") {
        int32_t captureFpsEnable;
        if (safe_strtoi32(value.string(), &captureFpsEnable)) {
            return setParamCaptureFpsEnable(captureFpsEnable);
        }
    } else if (key == "time-lapse-fps") {
        float fps;
        if (safe_strtof(value.string(), &fps)) {
            return setParamCaptureFps(fps);
        }
    } else {
        ALOGE("setParameter: failed to find key %s", key.string());
    }
    return BAD_VALUE;
}

status_t StagefrightRecorder::setParameters(const String8 &params) {
    ALOGV("setParameters: %s", params.string());
    const char *cparams = params.string();
    const char *key_start = cparams;
    for (;;) {
        const char *equal_pos = strchr(key_start, '=');
        if (equal_pos == NULL) {
            ALOGE("Parameters %s miss a value", cparams);
            return BAD_VALUE;
        }
        String8 key(key_start, equal_pos - key_start);
        TrimString(&key);
        if (key.length() == 0) {
            ALOGE("Parameters %s contains an empty key", cparams);
            return BAD_VALUE;
        }
        const char *value_start = equal_pos + 1;
        const char *semicolon_pos = strchr(value_start, ';');
        String8 value;
        if (semicolon_pos == NULL) {
            value.setTo(value_start);
        } else {
            value.setTo(value_start, semicolon_pos - value_start);
        }
        if (setParameter(key, value) != OK) {
            return BAD_VALUE;
        }
        if (semicolon_pos == NULL) {
            break;  // Reaches the end
        }
        key_start = semicolon_pos + 1;
    }
    return OK;
}

status_t StagefrightRecorder::setListener(const sp<IMediaRecorderClient> &listener) {
    mListener = listener;

    return OK;
}

status_t StagefrightRecorder::setClientName(const String16& clientName) {
    mClientName = clientName;

    return OK;
}

status_t StagefrightRecorder::prepareInternal() {
    ALOGV("prepare");
    if (mOutputFd < 0) {
        ALOGE("Output file descriptor is invalid");
        return INVALID_OPERATION;
    }

    // Get UID here for permission checking
    mClientUid = IPCThreadState::self()->getCallingUid();

    status_t status = OK;

    switch (mOutputFormat) {
        case OUTPUT_FORMAT_DEFAULT:
        case OUTPUT_FORMAT_THREE_GPP:
        case OUTPUT_FORMAT_MPEG_4:
        case OUTPUT_FORMAT_WEBM:
            status = setupMPEG4orWEBMRecording();
            break;

        case OUTPUT_FORMAT_AMR_NB:
        case OUTPUT_FORMAT_AMR_WB:
            status = setupAMRRecording();
            break;

        case OUTPUT_FORMAT_AAC_ADIF:
        case OUTPUT_FORMAT_AAC_ADTS:
            status = setupAACRecording();
            break;

        case OUTPUT_FORMAT_RTP_AVP:
            status = setupRTPRecording();
            break;

        case OUTPUT_FORMAT_MPEG2TS:
            status = setupMPEG2TSRecording();
            break;

        case OUTPUT_FORMAT_WAVE:
            status = setupWAVERecording();
            break;

        case OUTPUT_FORMAT_FLAC:
            status = setupFLACRecording();
            break;

        default:
            if (handleCustomRecording() != OK) {
                ALOGE("Unsupported output file format: %d", mOutputFormat);
                status = UNKNOWN_ERROR;
            }
            break;
    }

    return status;
}

status_t StagefrightRecorder::prepare() {
    if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        return prepareInternal();
    }
    return OK;
}

status_t StagefrightRecorder::start() {
    ALOGV("start");
    if (mOutputFd < 0) {
        ALOGE("Output file descriptor is invalid");
        return INVALID_OPERATION;
    }

    if (mRecPaused == true) {
        status_t err = mWriter->start();
        if (err != OK) {
            ALOGE("Writer start in StagefrightRecorder pause failed");
            return err;
        }

        err = setSourcePause(false);
        if (err != OK) {
            ALOGE("Source start after pause failed");
            return err;
        }

        mRecPaused = false;
        return OK;
    }
    status_t status = OK;

    if (mVideoSource != VIDEO_SOURCE_SURFACE) {
        status = prepareInternal();
        if (status != OK) {
            return status;
        }
    }

    if (mWriter == NULL) {
        ALOGE("File writer is not avaialble");
        return UNKNOWN_ERROR;
    }

    switch (mOutputFormat) {
        case OUTPUT_FORMAT_DEFAULT:
        case OUTPUT_FORMAT_THREE_GPP:
        case OUTPUT_FORMAT_MPEG_4:
        case OUTPUT_FORMAT_WEBM:
        {
            bool isMPEG4 = true;
            if (mOutputFormat == OUTPUT_FORMAT_WEBM) {
                isMPEG4 = false;
            }
            sp<MetaData> meta = new MetaData;
            setupMPEG4orWEBMMetaData(&meta);
            status = mWriter->start(meta.get());
            break;
        }

        case OUTPUT_FORMAT_AMR_NB:
        case OUTPUT_FORMAT_AMR_WB:
        case OUTPUT_FORMAT_AAC_ADIF:
        case OUTPUT_FORMAT_AAC_ADTS:
        case OUTPUT_FORMAT_RTP_AVP:
        case OUTPUT_FORMAT_MPEG2TS:
        case OUTPUT_FORMAT_WAVE:
        case OUTPUT_FORMAT_FLAC:
        {
            status = mWriter->start();
            break;
        }

        default:
        {
            if (handleCustomOutputFormats() != OK) {
                ALOGE("Unsupported output file format: %d", mOutputFormat);
                status = UNKNOWN_ERROR;
            }
            break;
        }
    }

    if (status != OK) {
        mWriter.clear();
        mWriter = NULL;
    }

    if ((status == OK) && (!mStarted)) {
        mStarted = true;

        uint32_t params = IMediaPlayerService::kBatteryDataCodecStarted;
        if (mAudioSource != AUDIO_SOURCE_CNT) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != VIDEO_SOURCE_LIST_END) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }

        addBatteryData(params);
    }

    return status;
}

sp<MediaSource> StagefrightRecorder::createAudioSource() {
    int32_t sourceSampleRate = mSampleRate;

    if (mCaptureFpsEnable && mCaptureFps >= mFrameRate) {
        // Upscale the sample rate for slow motion recording.
        // Fail audio source creation if source sample rate is too high, as it could
        // cause out-of-memory due to large input buffer size. And audio recording
        // probably doesn't make sense in the scenario, since the slow-down factor
        // is probably huge (eg. mSampleRate=48K, mCaptureFps=240, mFrameRate=1).
        const static int32_t SAMPLE_RATE_HZ_MAX = 192000;
        sourceSampleRate =
                (mSampleRate * mCaptureFps + mFrameRate / 2) / mFrameRate;
        if (sourceSampleRate < mSampleRate || sourceSampleRate > SAMPLE_RATE_HZ_MAX) {
            ALOGE("source sample rate out of range! "
                    "(mSampleRate %d, mCaptureFps %.2f, mFrameRate %d",
                    mSampleRate, mCaptureFps, mFrameRate);
            return NULL;
        }
    }

    // If using QCOM extension (Camera 1 HAL) for slow motion recording
    // mCaptureFpsEnable and mCaptureFps will not be set via setCaptureRate
    // We need to query from AVUtil, in order to support slow motion audio recording
    if (mVideoSourceNode != NULL) {
        int hfrRatio =  AVUtils::get()->HFRUtils().getHFRRatio(mVideoSourceNode->getFormat());
        if (hfrRatio != 1) {
            // Upscale the sample rate for slow motion recording.
            // Fail audio source creation if source sample rate is too high, as it could
            // cause out-of-memory due to large input buffer size. And audio recording
            // probably doesn't make sense in the scenario, since the slow-down factor
            // is probably huge (eg. mSampleRate=48K, hfrRatio=240, mFrameRate=1).
            const static int32_t SAMPLE_RATE_HZ_MAX = 192000;
            sourceSampleRate =
                    (mSampleRate * hfrRatio + mFrameRate / 2) / mFrameRate;
            if (sourceSampleRate < mSampleRate || sourceSampleRate > SAMPLE_RATE_HZ_MAX) {
                ALOGE("source sample rate out of range! "
                        "(mSampleRate %d, hfrRatio %d, mFrameRate %d",
                        mSampleRate, hfrRatio, mFrameRate);
                return NULL;
            }
        }
    }


    sp<AudioSource> audioSource = AVFactory::get()->createAudioSource(
        mAudioSource,
        mOpPackageName,
        sourceSampleRate,
        mAudioChannels,
        mSampleRate);

    status_t err = audioSource->initCheck();

    if (err != OK) {
        ALOGE("audio source is not initialized");
        return NULL;
    }

    sp<AMessage> format = new AMessage;
    switch (mAudioEncoder) {
        case AUDIO_ENCODER_AMR_NB:
        case AUDIO_ENCODER_DEFAULT:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_NB);
            break;
        case AUDIO_ENCODER_AMR_WB:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_WB);
            break;
        case AUDIO_ENCODER_AAC:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectLC);
            break;
        case AUDIO_ENCODER_HE_AAC:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectHE);
            break;
        case AUDIO_ENCODER_AAC_ELD:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectELD);
            break;
        case AUDIO_ENCODER_LPCM:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_RAW);
            break;
        case AUDIO_ENCODER_FLAC:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_FLAC);
            break;

        default:
            if (handleCustomAudioSource(format) != OK) {
                ALOGE("Unknown audio encoder: %d", mAudioEncoder);
                return NULL;
            }
    }

    int32_t maxInputSize;
    CHECK(audioSource->getFormat()->findInt32(
                kKeyMaxInputSize, &maxInputSize));

    format->setInt32("max-input-size", maxInputSize);
    format->setInt32("channel-count", mAudioChannels);
    format->setInt32("sample-rate", mSampleRate);
    format->setInt32("bitrate", mAudioBitRate);
    if (mAudioTimeScale > 0) {
        format->setInt32("time-scale", mAudioTimeScale);
    }
    format->setInt32("priority", 0 /* realtime */);

    sp<MediaSource> audioEncoder =
            MediaCodecSource::Create(mLooper, format, audioSource);
    // If encoder could not be created (as in LPCM), then
    // use the AudioSource directly as the MediaSource.
    if (audioEncoder == NULL &&
        mAudioEncoder == AUDIO_ENCODER_LPCM) {
        ALOGD("No encoder is needed for linear PCM format");
        audioEncoder = audioSource;
    }
    mAudioSourceNode = audioSource;

    if (audioEncoder == NULL) {
        ALOGE("Failed to create audio encoder");
    }

    mAudioEncoderOMX = audioEncoder;
    return audioEncoder;
}

status_t StagefrightRecorder::setupAACRecording() {
    // FIXME:
    // Add support for OUTPUT_FORMAT_AAC_ADIF
    CHECK_EQ(mOutputFormat, OUTPUT_FORMAT_AAC_ADTS);

    CHECK(mAudioEncoder == AUDIO_ENCODER_AAC ||
          mAudioEncoder == AUDIO_ENCODER_HE_AAC ||
          mAudioEncoder == AUDIO_ENCODER_AAC_ELD);
    CHECK(mAudioSource != AUDIO_SOURCE_CNT);

    mWriter = new AACWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupFLACRecording() {
    CHECK_EQ(mOutputFormat, OUTPUT_FORMAT_FLAC);
    CHECK(mAudioEncoder == AUDIO_ENCODER_FLAC);

    mWriter = new FLACWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupAMRRecording() {
    CHECK(mOutputFormat == OUTPUT_FORMAT_AMR_NB ||
          mOutputFormat == OUTPUT_FORMAT_AMR_WB);

    if (mOutputFormat == OUTPUT_FORMAT_AMR_NB) {
        if (mAudioEncoder != AUDIO_ENCODER_DEFAULT &&
            mAudioEncoder != AUDIO_ENCODER_AMR_NB) {
            ALOGE("Invalid encoder %d used for AMRNB recording",
                    mAudioEncoder);
            return BAD_VALUE;
        }
    } else {  // mOutputFormat must be OUTPUT_FORMAT_AMR_WB
        if (mAudioEncoder != AUDIO_ENCODER_AMR_WB) {
            ALOGE("Invlaid encoder %d used for AMRWB recording",
                    mAudioEncoder);
            return BAD_VALUE;
        }
    }

    mWriter = new AMRWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupRawAudioRecording() {
    if (mAudioSource >= AUDIO_SOURCE_CNT && mAudioSource != AUDIO_SOURCE_FM_TUNER) {
        ALOGE("Invalid audio source: %d", mAudioSource);
        return BAD_VALUE;
    }

    status_t status = BAD_VALUE;
    if (OK != (status = checkAudioEncoderCapabilities())) {
        return status;
    }

    sp<MediaSource> audioEncoder = createAudioSource();
    if (audioEncoder == NULL) {
        return UNKNOWN_ERROR;
    }

    CHECK(mWriter != 0);
    mWriter->addSource(audioEncoder);

    if (mMaxFileDurationUs != 0) {
        mWriter->setMaxFileDuration(mMaxFileDurationUs);
    }
    if (mMaxFileSizeBytes != 0) {
        mWriter->setMaxFileSize(mMaxFileSizeBytes);
    }
    mWriter->setListener(mListener);

    return OK;
}

status_t StagefrightRecorder::setupRTPRecording() {
    CHECK_EQ(mOutputFormat, OUTPUT_FORMAT_RTP_AVP);

    if ((mAudioSource != AUDIO_SOURCE_CNT
                && mVideoSource != VIDEO_SOURCE_LIST_END)
            || (mAudioSource == AUDIO_SOURCE_CNT
                && mVideoSource == VIDEO_SOURCE_LIST_END)) {
        // Must have exactly one source.
        return BAD_VALUE;
    }

    if (mOutputFd < 0) {
        return BAD_VALUE;
    }

    sp<MediaSource> source;

    if (mAudioSource != AUDIO_SOURCE_CNT) {
        source = createAudioSource();
    } else {
        setDefaultVideoEncoderIfNecessary();

        sp<MediaSource> mediaSource;
        status_t err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        err = setupVideoEncoder(mediaSource, &source);
        if (err != OK) {
            return err;
        }
    }

    mWriter = new ARTPWriter(mOutputFd);
    mWriter->addSource(source);
    mWriter->setListener(mListener);

    return OK;
}

status_t StagefrightRecorder::setupMPEG2TSRecording() {
    CHECK_EQ(mOutputFormat, OUTPUT_FORMAT_MPEG2TS);

    sp<MediaWriter> writer = new MPEG2TSWriter(mOutputFd);

    if (mAudioSource != AUDIO_SOURCE_CNT) {
        if (mAudioEncoder != AUDIO_ENCODER_AAC &&
            mAudioEncoder != AUDIO_ENCODER_HE_AAC &&
            mAudioEncoder != AUDIO_ENCODER_AAC_ELD) {
            return ERROR_UNSUPPORTED;
        }

        status_t err = setupAudioEncoder(writer);

        if (err != OK) {
            return err;
        }
    }

    if (mVideoSource < VIDEO_SOURCE_LIST_END) {
        if (mVideoEncoder != VIDEO_ENCODER_H264) {
            ALOGE("MPEG2TS recording only supports H.264 encoding!");
            return ERROR_UNSUPPORTED;
        }

        sp<MediaSource> mediaSource;
        status_t err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        sp<MediaSource> encoder;
        err = setupVideoEncoder(mediaSource, &encoder);

        if (err != OK) {
            return err;
        }

        writer->addSource(encoder);
    }

    if (mMaxFileDurationUs != 0) {
        writer->setMaxFileDuration(mMaxFileDurationUs);
    }

    if (mMaxFileSizeBytes != 0) {
        writer->setMaxFileSize(mMaxFileSizeBytes);
    }

    mWriter = writer;

    return OK;
}

status_t StagefrightRecorder::setupWAVERecording() {
    CHECK(mOutputFormat == OUTPUT_FORMAT_WAVE);
    CHECK(mAudioEncoder == AUDIO_ENCODER_LPCM);
    CHECK(mAudioSource != AUDIO_SOURCE_CNT);

    mWriter = new WAVEWriter(mOutputFd);
    return setupRawAudioRecording();
}

void StagefrightRecorder::clipVideoFrameRate() {
    ALOGV("clipVideoFrameRate: encoder %d", mVideoEncoder);
    if (mFrameRate == -1) {
        mFrameRate = mEncoderProfiles->getCamcorderProfileParamByName(
                "vid.fps", mCameraId, CAMCORDER_QUALITY_LOW);
        ALOGW("Using default video fps %d", mFrameRate);
    }

    int minFrameRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.fps.min", mVideoEncoder);
    int maxFrameRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.fps.max", mVideoEncoder);
    if (mFrameRate < minFrameRate && minFrameRate != -1) {
        ALOGW("Intended video encoding frame rate (%d fps) is too small"
             " and will be set to (%d fps)", mFrameRate, minFrameRate);
        mFrameRate = minFrameRate;
    } else if (mFrameRate > maxFrameRate && maxFrameRate != -1) {
        ALOGW("Intended video encoding frame rate (%d fps) is too large"
             " and will be set to (%d fps)", mFrameRate, maxFrameRate);
        mFrameRate = maxFrameRate;
    }
}

void StagefrightRecorder::clipVideoBitRate() {
    ALOGV("clipVideoBitRate: encoder %d", mVideoEncoder);
    int minBitRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.bps.min", mVideoEncoder);
    int maxBitRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.bps.max", mVideoEncoder);
    if (mVideoBitRate < minBitRate && minBitRate != -1) {
        ALOGW("Intended video encoding bit rate (%d bps) is too small"
             " and will be set to (%d bps)", mVideoBitRate, minBitRate);
        mVideoBitRate = minBitRate;
    } else if (mVideoBitRate > maxBitRate && maxBitRate != -1) {
        ALOGW("Intended video encoding bit rate (%d bps) is too large"
             " and will be set to (%d bps)", mVideoBitRate, maxBitRate);
        mVideoBitRate = maxBitRate;
    }
}

void StagefrightRecorder::clipVideoFrameWidth() {
    ALOGV("clipVideoFrameWidth: encoder %d", mVideoEncoder);
    int minFrameWidth = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.width.min", mVideoEncoder);
    int maxFrameWidth = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.width.max", mVideoEncoder);
    if (mVideoWidth < minFrameWidth && minFrameWidth != -1) {
        ALOGW("Intended video encoding frame width (%d) is too small"
             " and will be set to (%d)", mVideoWidth, minFrameWidth);
        mVideoWidth = minFrameWidth;
    } else if (mVideoWidth > maxFrameWidth && maxFrameWidth != -1) {
        ALOGW("Intended video encoding frame width (%d) is too large"
             " and will be set to (%d)", mVideoWidth, maxFrameWidth);
        mVideoWidth = maxFrameWidth;
    }
}

status_t StagefrightRecorder::checkVideoEncoderCapabilities() {
    /* hardware codecs must support camera source meta data mode */
    Vector<CodecCapabilities> codecs;
    OMXClient client;
    CHECK_EQ(client.connect(), (status_t)OK);
    QueryCodecs(
            client.interface(),
            (mVideoEncoder == VIDEO_ENCODER_H263 ? MEDIA_MIMETYPE_VIDEO_H263 :
             mVideoEncoder == VIDEO_ENCODER_MPEG_4_SP ? MEDIA_MIMETYPE_VIDEO_MPEG4 :
             mVideoEncoder == VIDEO_ENCODER_VP8 ? MEDIA_MIMETYPE_VIDEO_VP8 :
             mVideoEncoder == VIDEO_ENCODER_H264 ? MEDIA_MIMETYPE_VIDEO_AVC :
             mVideoEncoder == VIDEO_ENCODER_H265 ? MEDIA_MIMETYPE_VIDEO_HEVC : ""),
            false /* decoder */, true /* hwCodec */, &codecs);

    if (!mCaptureFpsEnable) {
        // Dont clip for time lapse capture as encoder will have enough
        // time to encode because of slow capture rate of time lapse.
        clipVideoBitRate();
        clipVideoFrameRate();
        clipVideoFrameWidth();
        clipVideoFrameHeight();
        setDefaultProfileIfNecessary();
    }
    return OK;
}

// Set to use AVC baseline profile if the encoding parameters matches
// CAMCORDER_QUALITY_LOW profile; this is for the sake of MMS service.
void StagefrightRecorder::setDefaultProfileIfNecessary() {
    ALOGV("setDefaultProfileIfNecessary");

    camcorder_quality quality = CAMCORDER_QUALITY_LOW;

    int64_t durationUs   = mEncoderProfiles->getCamcorderProfileParamByName(
                                "duration", mCameraId, quality) * 1000000LL;

    int fileFormat       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "file.format", mCameraId, quality);

    int videoCodec       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.codec", mCameraId, quality);

    int videoBitRate     = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.bps", mCameraId, quality);

    int videoFrameRate   = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.fps", mCameraId, quality);

    int videoFrameWidth  = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.width", mCameraId, quality);

    int videoFrameHeight = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.height", mCameraId, quality);

    int audioCodec       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.codec", mCameraId, quality);

    int audioBitRate     = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.bps", mCameraId, quality);

    int audioSampleRate  = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.hz", mCameraId, quality);

    int audioChannels    = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.ch", mCameraId, quality);

    if (durationUs == mMaxFileDurationUs &&
        fileFormat == mOutputFormat &&
        videoCodec == mVideoEncoder &&
        videoBitRate == mVideoBitRate &&
        videoFrameRate == mFrameRate &&
        videoFrameWidth == mVideoWidth &&
        videoFrameHeight == mVideoHeight &&
        audioCodec == mAudioEncoder &&
        audioBitRate == mAudioBitRate &&
        audioSampleRate == mSampleRate &&
        audioChannels == mAudioChannels) {
        if (videoCodec == VIDEO_ENCODER_H264) {
            ALOGI("Force to use AVC baseline profile");
            setParamVideoEncoderProfile(OMX_VIDEO_AVCProfileBaseline);
            // set 0 for invalid levels - this will be rejected by the
            // codec if it cannot handle it during configure
            setParamVideoEncoderLevel(ACodec::getAVCLevelFor(
                    videoFrameWidth, videoFrameHeight, videoFrameRate, videoBitRate));
        }
    }
}

void StagefrightRecorder::setDefaultVideoEncoderIfNecessary() {
    if (mVideoEncoder == VIDEO_ENCODER_DEFAULT) {
        if (mOutputFormat == OUTPUT_FORMAT_WEBM) {
            // default to VP8 for WEBM recording
            mVideoEncoder = VIDEO_ENCODER_VP8;
        } else {
            // pick the default encoder for CAMCORDER_QUALITY_LOW
            int videoCodec = mEncoderProfiles->getCamcorderProfileParamByName(
                    "vid.codec", mCameraId, CAMCORDER_QUALITY_LOW);

            if ((videoCodec > VIDEO_ENCODER_DEFAULT &&
                 videoCodec < VIDEO_ENCODER_LIST_END) ||
                (videoCodec > VIDEO_ENCODER_LIST_VENDOR_START &&
                 videoCodec < VIDEO_ENCODER_LIST_VENDOR_END)) {
                mVideoEncoder = (video_encoder)videoCodec;
            } else {
                // default to H.264 if camcorder profile not available
                mVideoEncoder = VIDEO_ENCODER_H264;
            }
        }
    }
}

status_t StagefrightRecorder::checkAudioEncoderCapabilities() {
    clipAudioBitRate();
    clipAudioSampleRate();
    clipNumberOfAudioChannels();
    return OK;
}

void StagefrightRecorder::clipAudioBitRate() {
    ALOGV("clipAudioBitRate: encoder %d", mAudioEncoder);

    int minAudioBitRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.bps.min", mAudioEncoder);
    if (minAudioBitRate != -1 && mAudioBitRate < minAudioBitRate) {
        ALOGW("Intended audio encoding bit rate (%d) is too small"
            " and will be set to (%d)", mAudioBitRate, minAudioBitRate);
        mAudioBitRate = minAudioBitRate;
    }

    int maxAudioBitRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.bps.max", mAudioEncoder);
    if (maxAudioBitRate != -1 && mAudioBitRate > maxAudioBitRate) {
        ALOGW("Intended audio encoding bit rate (%d) is too large"
            " and will be set to (%d)", mAudioBitRate, maxAudioBitRate);
        mAudioBitRate = maxAudioBitRate;
    }
}

void StagefrightRecorder::clipAudioSampleRate() {
    ALOGV("clipAudioSampleRate: encoder %d", mAudioEncoder);

    int minSampleRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.hz.min", mAudioEncoder);
    if (minSampleRate != -1 && mSampleRate < minSampleRate) {
        ALOGW("Intended audio sample rate (%d) is too small"
            " and will be set to (%d)", mSampleRate, minSampleRate);
        mSampleRate = minSampleRate;
    }

    int maxSampleRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.hz.max", mAudioEncoder);
    if (maxSampleRate != -1 && mSampleRate > maxSampleRate) {
        ALOGW("Intended audio sample rate (%d) is too large"
            " and will be set to (%d)", mSampleRate, maxSampleRate);
        mSampleRate = maxSampleRate;
    }
}

void StagefrightRecorder::clipNumberOfAudioChannels() {
    ALOGV("clipNumberOfAudioChannels: encoder %d", mAudioEncoder);

    int minChannels =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.ch.min", mAudioEncoder);
    if (minChannels != -1 && mAudioChannels < minChannels) {
        ALOGW("Intended number of audio channels (%d) is too small"
            " and will be set to (%d)", mAudioChannels, minChannels);
        mAudioChannels = minChannels;
    }

    int maxChannels =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.ch.max", mAudioEncoder);
    if (maxChannels != -1 && mAudioChannels > maxChannels) {
        ALOGW("Intended number of audio channels (%d) is too large"
            " and will be set to (%d)", mAudioChannels, maxChannels);
        mAudioChannels = maxChannels;
    }
}

void StagefrightRecorder::clipVideoFrameHeight() {
    ALOGV("clipVideoFrameHeight: encoder %d", mVideoEncoder);
    int minFrameHeight = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.height.min", mVideoEncoder);
    int maxFrameHeight = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.height.max", mVideoEncoder);
    if (minFrameHeight != -1 && mVideoHeight < minFrameHeight) {
        ALOGW("Intended video encoding frame height (%d) is too small"
             " and will be set to (%d)", mVideoHeight, minFrameHeight);
        mVideoHeight = minFrameHeight;
    } else if (maxFrameHeight != -1 && mVideoHeight > maxFrameHeight) {
        ALOGW("Intended video encoding frame height (%d) is too large"
             " and will be set to (%d)", mVideoHeight, maxFrameHeight);
        mVideoHeight = maxFrameHeight;
    }
}

// Set up the appropriate MediaSource depending on the chosen option
status_t StagefrightRecorder::setupMediaSource(
                      sp<MediaSource> *mediaSource) {
    if (mVideoSource == VIDEO_SOURCE_DEFAULT
            || mVideoSource == VIDEO_SOURCE_CAMERA) {
        sp<CameraSource> cameraSource;
        status_t err = setupCameraSource(&cameraSource);
        if (err != OK) {
            return err;
        }
        *mediaSource = cameraSource;
    } else if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        *mediaSource = NULL;
    } else {
        return INVALID_OPERATION;
    }
    return OK;
}

status_t StagefrightRecorder::setupCameraSource(
        sp<CameraSource> *cameraSource) {
    status_t err = OK;
    if ((err = checkVideoEncoderCapabilities()) != OK) {
        return err;
    }
    Size videoSize;
    videoSize.width = mVideoWidth;
    videoSize.height = mVideoHeight;
    if (mCaptureFpsEnable) {
        if (mTimeBetweenCaptureUs < 0) {
            ALOGE("Invalid mTimeBetweenTimeLapseFrameCaptureUs value: %" PRId64 "",
                mTimeBetweenCaptureUs);
            return BAD_VALUE;
        }

        mCameraSourceTimeLapse = AVFactory::get()->CreateCameraSourceTimeLapseFromCamera(
                mCamera, mCameraProxy, mCameraId, mClientName, mClientUid,
                videoSize, mFrameRate, mPreviewSurface,
                mTimeBetweenCaptureUs);
        *cameraSource = mCameraSourceTimeLapse;
    } else {
        *cameraSource = AVFactory::get()->CreateCameraSourceFromCamera(
                mCamera, mCameraProxy, mCameraId, mClientName, mClientUid,
                videoSize, mFrameRate,
                mPreviewSurface);
    }
    AVUtils::get()->cacheCaptureBuffers(mCamera, mVideoEncoder);
    mCamera.clear();
    mCameraProxy.clear();
    if (*cameraSource == NULL) {
        return UNKNOWN_ERROR;
    }

    if ((*cameraSource)->initCheck() != OK) {
        (*cameraSource).clear();
        *cameraSource = NULL;
        return NO_INIT;
    }

    // When frame rate is not set, the actual frame rate will be set to
    // the current frame rate being used.
    if (mFrameRate == -1) {
        int32_t frameRate = 0;
        CHECK ((*cameraSource)->getFormat()->findInt32(
                    kKeyFrameRate, &frameRate));
        ALOGI("Frame rate is not explicitly set. Use the current frame "
             "rate (%d fps)", frameRate);
        mFrameRate = frameRate;
    }

    CHECK(mFrameRate != -1);

    mIsMetaDataStoredInVideoBuffers =
        (*cameraSource)->isMetaDataStoredInVideoBuffers();

    return OK;
}

bool StagefrightRecorder::setCustomVideoEncoderMime(const video_encoder videoEncoder,
        sp<AMessage> format) {
    if (videoEncoder == VIDEO_ENCODER_H265) {
        format->setString("mime", MEDIA_MIMETYPE_VIDEO_HEVC);
        return true;
    }
    return false;
}

status_t StagefrightRecorder::setupVideoEncoder(
        sp<MediaSource> cameraSource,
        sp<MediaSource> *source) {
    source->clear();

    sp<AMessage> format = new AMessage();

    switch (mVideoEncoder) {
        case VIDEO_ENCODER_H263:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_H263);
            break;

        case VIDEO_ENCODER_MPEG_4_SP:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_MPEG4);
            break;

        case VIDEO_ENCODER_H264:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
            break;

        case VIDEO_ENCODER_VP8:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_VP8);
            break;

        default:
            if (setCustomVideoEncoderMime(mVideoEncoder, format)) {
                break;
            }
            CHECK(!"Should not be here, unsupported video encoding.");
            break;
    }

    if (cameraSource != NULL) {
        sp<MetaData> meta = cameraSource->getFormat();

        int32_t width, height, stride, sliceHeight, colorFormat;
        CHECK(meta->findInt32(kKeyWidth, &width));
        CHECK(meta->findInt32(kKeyHeight, &height));
        CHECK(meta->findInt32(kKeyStride, &stride));
        CHECK(meta->findInt32(kKeySliceHeight, &sliceHeight));
        CHECK(meta->findInt32(kKeyColorFormat, &colorFormat));

        format->setInt32("width", width);
        format->setInt32("height", height);
        format->setInt32("stride", stride);
        format->setInt32("slice-height", sliceHeight);
        format->setInt32("color-format", colorFormat);
    } else {
        format->setInt32("width", mVideoWidth);
        format->setInt32("height", mVideoHeight);
        format->setInt32("stride", mVideoWidth);
        format->setInt32("slice-height", mVideoHeight);
        format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);

        // set up time lapse/slow motion for surface source
        if (mCaptureFpsEnable) {
            if (mTimeBetweenCaptureUs <= 0) {
                ALOGE("Invalid mTimeBetweenCaptureUs value: %" PRId64 "",
                        mTimeBetweenCaptureUs);
                return BAD_VALUE;
            }
            format->setInt64("time-lapse", mTimeBetweenCaptureUs);
        }
    }

    format->setInt32("bitrate", mVideoBitRate);
    format->setInt32("frame-rate", mFrameRate);
    format->setInt32("i-frame-interval", mIFramesIntervalSec);

    if (cameraSource != NULL) {
        setupCustomVideoEncoderParams(cameraSource, format);
    }

    if (mVideoTimeScale > 0) {
        format->setInt32("time-scale", mVideoTimeScale);
    }
    if (mVideoEncoderProfile != -1) {
        format->setInt32("profile", mVideoEncoderProfile);
    }
    if (mVideoEncoderLevel != -1) {
        format->setInt32("level", mVideoEncoderLevel);
    }

    format->setInt32("priority", 0 /* realtime */);
    if (mCaptureFpsEnable) {
        format->setFloat("operating-rate", mCaptureFps);
    }

    uint32_t flags = 0;
    if (mIsMetaDataStoredInVideoBuffers) {
        flags |= MediaCodecSource::FLAG_USE_METADATA_INPUT;
    }

    if (cameraSource == NULL) {
        flags |= MediaCodecSource::FLAG_USE_SURFACE_INPUT;
    }

    sp<MediaCodecSource> encoder = MediaCodecSource::Create(
            mLooper, format, cameraSource, mPersistentSurface, flags);
    if (encoder == NULL) {
        ALOGE("Failed to create video encoder");
        // When the encoder fails to be created, we need
        // release the camera source due to the camera's lock
        // and unlock mechanism.
        if (cameraSource != NULL) {
            cameraSource->stop();
        }
        return UNKNOWN_ERROR;
    }

    if (cameraSource == NULL) {
        mGraphicBufferProducer = encoder->getGraphicBufferProducer();
    }

    mVideoSourceNode = cameraSource;
    mVideoEncoderOMX = encoder;
    *source = encoder;

    return OK;
}

status_t StagefrightRecorder::setupAudioEncoder(const sp<MediaWriter>& writer) {
    status_t status = BAD_VALUE;
    if (OK != (status = checkAudioEncoderCapabilities())) {
        return status;
    }

    switch(mAudioEncoder) {
        case AUDIO_ENCODER_AMR_NB:
        case AUDIO_ENCODER_AMR_WB:
        case AUDIO_ENCODER_AAC:
        case AUDIO_ENCODER_HE_AAC:
        case AUDIO_ENCODER_AAC_ELD:
        case AUDIO_ENCODER_LPCM:
        case AUDIO_ENCODER_FLAC:
            break;

        default:
            if (handleCustomAudioEncoder() != OK) {
                ALOGE("Unsupported audio encoder: %d", mAudioEncoder);
                return UNKNOWN_ERROR;
            }
    }

    sp<MediaSource> audioEncoder = createAudioSource();
    if (audioEncoder == NULL) {
        return UNKNOWN_ERROR;
    }

    writer->addSource(audioEncoder);
    return OK;
}

status_t StagefrightRecorder::setupMPEG4orWEBMRecording() {
    mWriter.clear();
    mTotalBitRate = 0;

    status_t err = OK;
    sp<MediaWriter> writer;
    sp<MPEG4Writer> mp4writer;
    if (mOutputFormat == OUTPUT_FORMAT_WEBM) {
        writer = new WebmWriter(mOutputFd);
    } else {
        writer = mp4writer = AVFactory::get()->CreateMPEG4Writer(mOutputFd);
    }

    if (mVideoSource < VIDEO_SOURCE_LIST_END) {
        setDefaultVideoEncoderIfNecessary();

        sp<MediaSource> mediaSource;
        err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        sp<MediaSource> encoder;
        err = setupVideoEncoder(mediaSource, &encoder);
        if (err != OK) {
            return err;
        }

        writer->addSource(encoder);
        mTotalBitRate += mVideoBitRate;
    }

    if (mOutputFormat != OUTPUT_FORMAT_WEBM) {
        // Audio source is added at the end if it exists.
        // This help make sure that the "recoding" sound is suppressed for
        // camcorder applications in the recorded files.
        // TODO Audio source is currently unsupported for webm output; vorbis encoder needed.
        // disable audio for time lapse recording
        bool disableAudio = mCaptureFpsEnable && mCaptureFps < mFrameRate;
        if (!disableAudio && mAudioSource != AUDIO_SOURCE_CNT) {
            err = setupAudioEncoder(writer);
            if (err != OK) return err;
            mTotalBitRate += mAudioBitRate;
        }

        if (mCaptureFpsEnable) {
            mp4writer->setCaptureRate(mCaptureFps);
        }

        if (mInterleaveDurationUs > 0) {
            mp4writer->setInterleaveDuration(mInterleaveDurationUs);
        }
        if (mLongitudex10000 > -3600000 && mLatitudex10000 > -3600000) {
            mp4writer->setGeoData(mLatitudex10000, mLongitudex10000);
        }
    }
    if (mMaxFileDurationUs != 0) {
        writer->setMaxFileDuration(mMaxFileDurationUs);
    }
    if (mMaxFileSizeBytes != 0) {
        writer->setMaxFileSize(mMaxFileSizeBytes);
    }
    if (mVideoSource == VIDEO_SOURCE_DEFAULT
            || mVideoSource == VIDEO_SOURCE_CAMERA) {
        mStartTimeOffsetMs = mEncoderProfiles->getStartTimeOffsetMs(mCameraId);
    } else if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        // surface source doesn't need large initial delay
        mStartTimeOffsetMs = 200;
    }
    if (mStartTimeOffsetMs > 0) {
        writer->setStartTimeOffsetMs(mStartTimeOffsetMs);
    }

    writer->setListener(mListener);
    mWriter = writer;
    return OK;
}

void StagefrightRecorder::setupMPEG4orWEBMMetaData(sp<MetaData> *meta) {
    int64_t startTimeUs = systemTime() / 1000;
    (*meta)->setInt64(kKeyTime, startTimeUs);
    int64_t startTimeBootUs = systemTime(SYSTEM_TIME_BOOTTIME) / 1000;
    (*meta)->setInt64(kKeyTimeBoot, startTimeBootUs);
    (*meta)->setInt32(kKeyFileType, mOutputFormat);
    (*meta)->setInt32(kKeyBitRate, mTotalBitRate);
    if (mMovieTimeScale > 0) {
        (*meta)->setInt32(kKeyTimeScale, mMovieTimeScale);
    }
    if (mOutputFormat != OUTPUT_FORMAT_WEBM) {
        (*meta)->setInt32(kKey64BitFileOffset, mUse64BitFileOffset);
        if (mTrackEveryTimeDurationUs > 0) {
            (*meta)->setInt64(kKeyTrackTimeStatus, mTrackEveryTimeDurationUs);
        }
        if (mRotationDegrees != 0) {
            (*meta)->setInt32(kKeyRotation, mRotationDegrees);
        }
    }
}

status_t StagefrightRecorder::pause() {
    ALOGV("pause");
    status_t err = OK;
    if (mWriter == NULL) {
        return UNKNOWN_ERROR;
    }
    err = setSourcePause(true);
    if (err != OK) {
        ALOGE("StagefrightRecorder pause failed");
        return err;
    }

    err = mWriter->pause();
    if (err != OK) {
        ALOGE("Writer pause failed");
        return err;
    }

    mRecPaused = true;

    if (mStarted) {
        mStarted = false;

        uint32_t params = 0;
        if (mAudioSource != AUDIO_SOURCE_CNT) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != VIDEO_SOURCE_LIST_END) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }

        addBatteryData(params);
    }


    return OK;
}

status_t StagefrightRecorder::stop() {
    ALOGV("stop");
    status_t err = OK;

    if (mCaptureFpsEnable && mCameraSourceTimeLapse != NULL) {
        mCameraSourceTimeLapse->startQuickReadReturns();
        mCameraSourceTimeLapse = NULL;
    }

    if (mRecPaused) {
        status_t err = setSourcePause(false);
        if (err != OK) {
            ALOGE("Source start after pause in StagefrightRecorder stop failed");
            return err;
        }

        mRecPaused = false;
    }

    if (mWriter != NULL) {
        err = mWriter->stop();
        mWriter.clear();
    }

    mGraphicBufferProducer.clear();
    mPersistentSurface.clear();

    if (mOutputFd >= 0) {
        ::close(mOutputFd);
        mOutputFd = -1;
    }

    if (mStarted) {
        mStarted = false;

        uint32_t params = 0;
        if (mAudioSource != AUDIO_SOURCE_CNT) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != VIDEO_SOURCE_LIST_END) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }

        addBatteryData(params);
    }

    return err;
}

status_t StagefrightRecorder::close() {
    ALOGV("close");
    stop();

    return OK;
}

status_t StagefrightRecorder::reset() {
    ALOGV("reset");
    stop();

    // No audio or video source by default
    mAudioSource = AUDIO_SOURCE_CNT;
    mVideoSource = VIDEO_SOURCE_LIST_END;

    // Default parameters
    mOutputFormat  = OUTPUT_FORMAT_THREE_GPP;
    mAudioEncoder  = AUDIO_ENCODER_AMR_NB;
    mVideoEncoder  = VIDEO_ENCODER_DEFAULT;
    mVideoWidth    = 176;
    mVideoHeight   = 144;
    mFrameRate     = -1;
    mVideoBitRate  = 192000;
    mSampleRate    = 8000;
    mAudioChannels = 1;
    mAudioBitRate  = 12200;
    mInterleaveDurationUs = 0;
    mIFramesIntervalSec = 1;
    mAudioSourceNode = 0;
    mUse64BitFileOffset = false;
    mMovieTimeScale  = -1;
    mAudioTimeScale  = -1;
    mVideoTimeScale  = -1;
    mCameraId        = 0;
    mStartTimeOffsetMs = -1;
    mVideoEncoderProfile = -1;
    mVideoEncoderLevel   = -1;
    mMaxFileDurationUs = 0;
    mMaxFileSizeBytes = 0;
    mTrackEveryTimeDurationUs = 0;
    mCaptureFpsEnable = false;
    mCaptureFps = 0.0f;
    mTimeBetweenCaptureUs = -1;
    mCameraSourceTimeLapse = NULL;
    mIsMetaDataStoredInVideoBuffers = false;
    mEncoderProfiles = MediaProfiles::getInstance();
    mRotationDegrees = 0;
    mLatitudex10000 = -3600000;
    mLongitudex10000 = -3600000;
    mTotalBitRate = 0;

    mOutputFd = -1;

    return OK;
}

status_t StagefrightRecorder::getMaxAmplitude(int *max) {
    ALOGV("getMaxAmplitude");

    if (max == NULL) {
        ALOGE("Null pointer argument");
        return BAD_VALUE;
    }

    if (mAudioSourceNode != 0) {
        *max = mAudioSourceNode->getMaxAmplitude();
    } else {
        *max = 0;
    }

    return OK;
}

status_t StagefrightRecorder::dump(
        int fd, const Vector<String16>& args) const {
    ALOGV("dump");
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    if (mWriter != 0) {
        mWriter->dump(fd, args);
    } else {
        snprintf(buffer, SIZE, "   No file writer\n");
        result.append(buffer);
    }
    snprintf(buffer, SIZE, "   Recorder: %p\n", this);
    snprintf(buffer, SIZE, "   Output file (fd %d):\n", mOutputFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "     File format: %d\n", mOutputFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max file size (bytes): %" PRId64 "\n", mMaxFileSizeBytes);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max file duration (us): %" PRId64 "\n", mMaxFileDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     File offset length (bits): %d\n", mUse64BitFileOffset? 64: 32);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Interleave duration (us): %d\n", mInterleaveDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Progress notification: %" PRId64 " us\n", mTrackEveryTimeDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "   Audio\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "     Source: %d\n", mAudioSource);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder: %d\n", mAudioEncoder);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Bit rate (bps): %d\n", mAudioBitRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Sampling rate (hz): %d\n", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Number of channels: %d\n", mAudioChannels);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max amplitude: %d\n", mAudioSourceNode == 0? 0: mAudioSourceNode->getMaxAmplitude());
    result.append(buffer);
    snprintf(buffer, SIZE, "   Video\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "     Source: %d\n", mVideoSource);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Camera Id: %d\n", mCameraId);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Start time offset (ms): %d\n", mStartTimeOffsetMs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder: %d\n", mVideoEncoder);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder profile: %d\n", mVideoEncoderProfile);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder level: %d\n", mVideoEncoderLevel);
    result.append(buffer);
    snprintf(buffer, SIZE, "     I frames interval (s): %d\n", mIFramesIntervalSec);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Frame size (pixels): %dx%d\n", mVideoWidth, mVideoHeight);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Frame rate (fps): %d\n", mFrameRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Bit rate (bps): %d\n", mVideoBitRate);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return OK;
}

status_t StagefrightRecorder::setSourcePause(bool pause) {
    status_t err = OK;
    if (pause) {
        if (mVideoEncoderOMX != NULL) {
            err = mVideoEncoderOMX->pause();
            if (err != OK) {
                ALOGE("OMX VideoEncoder pause failed");
                return err;
            }
        }
        if (mAudioEncoderOMX != NULL) {
            if (mAudioEncoderOMX != mAudioSourceNode) {
                err = mAudioEncoderOMX->pause();
                if (err != OK) {
                    ALOGE("OMX AudioEncoder pause failed");
                    return err;
                }
            } else {
                // If AudioSource is the same as MediaSource(as in LPCM),
                // bypass omx encoder pause() call.
                ALOGV("OMX AudioEncoder->pause() bypassed");
            }
        }
        if (mVideoSourceNode != NULL) {
            err = mVideoSourceNode->pause();
            if (err != OK) {
                ALOGE("OMX VideoSourceNode pause failed");
                return err;
            }
        }
        if (mAudioSourceNode != NULL) {
            err = mAudioSourceNode->pause();
            if (err != OK) {
                ALOGE("OMX AudioSourceNode pause failed");
                return err;
            }
        }
    } else {
        if (mVideoSourceNode != NULL) {
            err = mVideoSourceNode->start();
            if (err != OK) {
                ALOGE("OMX VideoSourceNode start failed");
                return err;
            }
        }
        if (mAudioSourceNode != NULL) {
            err = mAudioSourceNode->start();
            if (err != OK) {
                ALOGE("OMX AudioSourceNode start failed");
                return err;
            }
        }
        if (mVideoEncoderOMX != NULL) {
            err = mVideoEncoderOMX->start();
            if (err != OK) {
                ALOGE("OMX VideoEncoder start failed");
                return err;
            }
        }
        if (mAudioEncoderOMX != NULL) {
            if (mAudioEncoderOMX != mAudioSourceNode) {
                err = mAudioEncoderOMX->start();
                if (err != OK) {
                    ALOGE("OMX AudioEncoder start failed");
                    return err;
                }
            } else {
                // If AudioSource is the same as MediaSource(as in LPCM),
                // bypass omx encoder start() call.
                ALOGV("OMX AudioEncoder->start() bypassed");
            }
        }
    }
    return err;
}

void StagefrightRecorder::setupCustomVideoEncoderParams(sp<MediaSource> cameraSource,
        sp<AMessage> &format) {

    // Setup HFR if needed
    AVUtils::get()->HFRUtils().initializeHFR(cameraSource->getFormat(), format,
            mMaxFileDurationUs, mVideoEncoder);
}

}  // namespace android
