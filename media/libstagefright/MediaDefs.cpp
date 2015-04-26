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
 *
 * This file was modified by Dolby Laboratories, Inc. The portions of the
 * code that are surrounded by "DOLBY..." are copyrighted and
 * licensed separately, as follows:
 *
 *  (C) 2011-2014 Dolby Laboratories, Inc.
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

#include <media/stagefright/MediaDefs.h>

namespace android {

const char *MEDIA_MIMETYPE_IMAGE_JPEG = "image/jpeg";

const char *MEDIA_MIMETYPE_VIDEO_VP8 = "video/x-vnd.on2.vp8";
const char *MEDIA_MIMETYPE_VIDEO_VP9 = "video/x-vnd.on2.vp9";
const char *MEDIA_MIMETYPE_VIDEO_AVC = "video/avc";
const char *MEDIA_MIMETYPE_VIDEO_HEVC = "video/hevc";
const char *MEDIA_MIMETYPE_VIDEO_MPEG4 = "video/mp4v-es";
const char *MEDIA_MIMETYPE_VIDEO_MPEG4_DP = "video/mp4v-esdp";
const char *MEDIA_MIMETYPE_VIDEO_H263 = "video/3gpp";
const char *MEDIA_MIMETYPE_VIDEO_MPEG2 = "video/mpeg2";
const char *MEDIA_MIMETYPE_VIDEO_RAW = "video/raw";

const char *MEDIA_MIMETYPE_AUDIO_AMR_NB = "audio/3gpp";
const char *MEDIA_MIMETYPE_AUDIO_AMR_WB = "audio/amr-wb";
const char *MEDIA_MIMETYPE_AUDIO_MPEG = "audio/mpeg";
const char *MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I = "audio/mpeg-L1";
const char *MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II = "audio/mpeg-L2";
const char *MEDIA_MIMETYPE_AUDIO_AAC = "audio/mp4a-latm";
const char *MEDIA_MIMETYPE_AUDIO_QCELP = "audio/qcelp";
const char *MEDIA_MIMETYPE_AUDIO_VORBIS = "audio/vorbis";
const char *MEDIA_MIMETYPE_AUDIO_OPUS = "audio/opus";
const char *MEDIA_MIMETYPE_AUDIO_G711_ALAW = "audio/g711-alaw";
const char *MEDIA_MIMETYPE_AUDIO_G711_MLAW = "audio/g711-mlaw";
const char *MEDIA_MIMETYPE_AUDIO_RAW = "audio/raw";
const char *MEDIA_MIMETYPE_AUDIO_FLAC = "audio/flac";
const char *MEDIA_MIMETYPE_AUDIO_AAC_ADTS = "audio/aac-adts";
const char *MEDIA_MIMETYPE_AUDIO_MSGSM = "audio/gsm";
const char *MEDIA_MIMETYPE_AUDIO_AC3 = "audio/ac3";
const char *MEDIA_MIMETYPE_AUDIO_EAC3 = "audio/eac3";

const char *MEDIA_MIMETYPE_CONTAINER_MPEG4 = "video/mp4";
const char *MEDIA_MIMETYPE_CONTAINER_WAV = "audio/x-wav";
const char *MEDIA_MIMETYPE_CONTAINER_OGG = "application/ogg";
const char *MEDIA_MIMETYPE_CONTAINER_MATROSKA = "video/x-matroska";
const char *MEDIA_MIMETYPE_CONTAINER_MPEG2TS = "video/mp2ts";
const char *MEDIA_MIMETYPE_CONTAINER_AVI = "video/avi";
const char *MEDIA_MIMETYPE_CONTAINER_MPEG2PS = "video/mp2p";

const char *MEDIA_MIMETYPE_CONTAINER_WVM = "video/wvm";

const char *MEDIA_MIMETYPE_TEXT_3GPP = "text/3gpp-tt";
const char *MEDIA_MIMETYPE_TEXT_SUBRIP = "application/x-subrip";
const char *MEDIA_MIMETYPE_TEXT_VTT = "text/vtt";
const char *MEDIA_MIMETYPE_TEXT_CEA_608 = "text/cea-608";
#ifdef DOLBY_UDC
const char *MEDIA_MIMETYPE_AUDIO_EAC3_JOC = "audio/eac3-joc";
#endif // DOLBY_END

const char *MEDIA_MIMETYPE_VIDEO_DIVX = "video/divx";
const char *MEDIA_MIMETYPE_VIDEO_DIVX311 = "video/divx311";
const char *MEDIA_MIMETYPE_VIDEO_DIVX4 = "video/divx4";
const char *MEDIA_MIMETYPE_VIDEO_FLV1 = "video/x-flv";
const char *MEDIA_MIMETYPE_VIDEO_MJPEG = "video/x-jpeg";
const char *MEDIA_MIMETYPE_VIDEO_RV = "video/vnd.rn-realvideo";
const char *MEDIA_MIMETYPE_VIDEO_VC1 = "video/vc1";
const char *MEDIA_MIMETYPE_VIDEO_WMV = "video/x-ms-wmv";
const char *MEDIA_MIMETYPE_VIDEO_FFMPEG = "video/ffmpeg";

const char *MEDIA_MIMETYPE_AUDIO_APE = "audio/x-ape";
const char *MEDIA_MIMETYPE_AUDIO_DTS = "audio/vnd.dts";
const char *MEDIA_MIMETYPE_AUDIO_PCM = "audio/x-pcm";
const char *MEDIA_MIMETYPE_AUDIO_RA = "audio/vnd.rn-realaudio";
const char *MEDIA_MIMETYPE_AUDIO_WMA = "audio/x-ms-wma";
const char *MEDIA_MIMETYPE_AUDIO_FFMPEG = "audio/ffmpeg";

const char *MEDIA_MIMETYPE_CONTAINER_APE = "audio/x-ape";
const char *MEDIA_MIMETYPE_CONTAINER_ASF = "video/x-ms-asf";
const char *MEDIA_MIMETYPE_CONTAINER_DIVX = "video/divx";
const char *MEDIA_MIMETYPE_CONTAINER_DTS = "audio/vnd.dts";
const char *MEDIA_MIMETYPE_CONTAINER_FLAC = "audio/flac";
const char *MEDIA_MIMETYPE_CONTAINER_FLV = "video/x-flv";
const char *MEDIA_MIMETYPE_CONTAINER_MOV = "video/quicktime";
const char *MEDIA_MIMETYPE_CONTAINER_MP2 = "audio/mpeg2";
const char *MEDIA_MIMETYPE_CONTAINER_MPG = "video/mpeg";
const char *MEDIA_MIMETYPE_CONTAINER_RA = "audio/vnd.rn-realaudio";
const char *MEDIA_MIMETYPE_CONTAINER_RM = "video/vnd.rn-realvideo";
const char *MEDIA_MIMETYPE_CONTAINER_TS = "video/mp2t";
const char *MEDIA_MIMETYPE_CONTAINER_WEBM = "video/webm";
const char *MEDIA_MIMETYPE_CONTAINER_WMA = "audio/x-ms-wma";
const char *MEDIA_MIMETYPE_CONTAINER_WMV = "video/x-ms-wmv";
const char *MEDIA_MIMETYPE_CONTAINER_VC1 = "video/vc1";
const char *MEDIA_MIMETYPE_CONTAINER_HEVC = "video/hevc";
const char *MEDIA_MIMETYPE_CONTAINER_FFMPEG = "video/ffmpeg";

}  // namespace android
