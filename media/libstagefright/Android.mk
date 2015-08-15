#
# This file was modified by DTS, Inc. The portions of the
# code that are surrounded by "DTS..." are copyrighted and
# licensed separately, as follows:
#
#  (C) 2014 DTS, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

include frameworks/av/media/libstagefright/codecs/common/Config.mk

LOCAL_SRC_FILES:=                         \
        ACodec.cpp                        \
        AACExtractor.cpp                  \
        AACWriter.cpp                     \
        AMRExtractor.cpp                  \
        AMRWriter.cpp                     \
        AudioPlayer.cpp                   \
        AudioSource.cpp                   \
        AwesomePlayer.cpp                 \
        CameraSource.cpp                  \
        CameraSourceTimeLapse.cpp         \
        ClockEstimator.cpp                \
        CodecBase.cpp                     \
        DataSource.cpp                    \
        DataURISource.cpp                 \
        DRMExtractor.cpp                  \
        ESDS.cpp                          \
        FileSource.cpp                    \
        FLACExtractor.cpp                 \
        HTTPBase.cpp                      \
        JPEGSource.cpp                    \
        MP3Extractor.cpp                  \
        MPEG2TSWriter.cpp                 \
        MPEG4Extractor.cpp                \
        MPEG4Writer.cpp                   \
        MediaAdapter.cpp                  \
        MediaBuffer.cpp                   \
        MediaBufferGroup.cpp              \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaCodecSource.cpp              \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
        http/MediaHTTP.cpp                \
        MediaMuxer.cpp                    \
        MediaSource.cpp                   \
        MetaData.cpp                      \
        NuCachedSource2.cpp               \
        NuMediaExtractor.cpp              \
        OMXClient.cpp                     \
        OMXCodec.cpp                      \
        ExtendedCodec.cpp                 \
        OggExtractor.cpp                  \
        SampleIterator.cpp                \
        SampleTable.cpp                   \
        SkipCutBuffer.cpp                 \
        StagefrightMediaScanner.cpp       \
        StagefrightMetadataRetriever.cpp  \
        SurfaceMediaSource.cpp            \
        ThrottledSource.cpp               \
        TimeSource.cpp                    \
        TimedEventQueue.cpp               \
        Utils.cpp                         \
        VBRISeeker.cpp                    \
        WAVExtractor.cpp                  \
        WAVEWriter.cpp                    \
        WVMExtractor.cpp                  \
        XINGSeeker.cpp                    \
        avc_utils.cpp                     \
        ExtendedExtractor.cpp             \
        ExtendedUtils.cpp                 \
        ExtendedStats.cpp                 \
        APE.cpp                           \
        FFMPEGSoftCodec.cpp               \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/openssl/include \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/system/netd/include \
        $(TOP)/external/icu/icu4c/source/common \
        $(TOP)/external/icu/icu4c/source/i18n \

LOCAL_SHARED_LIBRARIES := \
        libbinder \
        libcamera_client \
        libcutils \
        libdl \
        libdrmframework \
        libexpat \
        libgui \
        libicui18n \
        libicuuc \
        liblog \
        libmedia \
        libnetd_client \
        libopus \
        libsonivox \
        libssl \
        libstagefright_omx \
        libstagefright_yuv \
        libsync \
        libui \
        libutils \
        libvorbisidec \
        libz \
        libpowermanager

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
ifneq ($(filter msm7x30 msm8660 msm8960,$(TARGET_BOARD_PLATFORM)),)
ifeq ($(BOARD_USES_LEGACY_ALSA_AUDIO),true)
   ifeq ($(USE_TUNNEL_MODE),true)
        LOCAL_CFLAGS += -DUSE_TUNNEL_MODE
   endif
   ifeq ($(NO_TUNNEL_MODE_FOR_MULTICHANNEL),true)
        LOCAL_CFLAGS += -DNO_TUNNEL_MODE_FOR_MULTICHANNEL
   endif
   LOCAL_SRC_FILES += LPAPlayerALSA.cpp TunnelPlayer.cpp
endif
endif
endif

ifeq ($(TARGET_BOARD_PLATFORM),omap4)
LOCAL_CFLAGS += -DBOARD_CANT_REALLOCATE_OMX_BUFFERS
endif

#QTI FLAC Decoder
ifeq ($(call is-vendor-board-platform,QCOM),true)
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FLAC_DECODER)),true)
LOCAL_SRC_FILES += FLACDecoder.cpp
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-flac
LOCAL_CFLAGS += -DQTI_FLAC_DECODER
endif
endif

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_webm \
        libstagefright_timedtext \
        libvpx \
        libwebm \
        libstagefright_mpeg2ts \
        libstagefright_id3 \
        libFLAC \
        libmedia_helper

ifeq ($(call is-vendor-board-platform,QCOM),true)

ifeq ($(TARGET_USES_QCOM_BSP), true)
    LOCAL_C_INCLUDES += $(call project-path-for,qcom-display)/libgralloc
endif

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
       LOCAL_CFLAGS     += -DENABLE_AV_ENHANCEMENTS
       LOCAL_C_INCLUDES += $(TOP)/$(call project-path-for,qcom-media)/mm-core/inc
       LOCAL_C_INCLUDES += $(TOP)/frameworks/av/media/libstagefright/include
       LOCAL_SRC_FILES  += ExtendedMediaDefs.cpp
       LOCAL_SRC_FILES  += ExtendedWriter.cpp
       LOCAL_SRC_FILES  += FMA2DPWriter.cpp
endif #TARGET_ENABLE_AV_ENHANCEMENTS

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD)),true)
       LOCAL_CFLAGS     += -DFLAC_OFFLOAD_ENABLED
endif
endif

ifeq ($(BOARD_USE_S3D_SUPPORT), true)
ifeq ($(BOARD_USES_HWC_SERVICES), true)

ifeq ($(TARGET_SLSI_VARIANT),cm)
SLSI_DIR := samsung_slsi-cm
PLATFORM_DIR := $(TARGET_BOARD_PLATFORM)
else
SLSI_DIR := samsung_slsi
PLATFORM_DIR := $(TARGET_BOARD_PLATFORM)-$(TARGET_SLSI_VARIANT)
endif

LOCAL_CFLAGS += -DUSE_S3D_SUPPORT -DHWC_SERVICES
LOCAL_C_INCLUDES += \
        $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include \
        $(TOP)/hardware/$(SLSI_DIR)/openmax/include/exynos \
        $(TOP)/hardware/$(SLSI_DIR)/$(PLATFORM_DIR)/libhwcService \
        $(TOP)/hardware/$(SLSI_DIR)/$(PLATFORM_DIR)/libhwc \
        $(TOP)/hardware/$(SLSI_DIR)/$(PLATFORM_DIR)/include \
        $(TOP)/hardware/$(SLSI_DIR)/$(TARGET_SOC)/libhwcmodule \
        $(TOP)/hardware/$(SLSI_DIR)/$(TARGET_SOC)/include \
        $(TOP)/hardware/$(SLSI_DIR)/exynos/libexynosutils \
        $(TOP)/hardware/$(SLSI_DIR)/exynos/include \
        $(TOP)/hardware/$(SLSI_DIR)/exynos/libhwc

LOCAL_ADDITIONAL_DEPENDENCIES := \
        $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr

LOCAL_SHARED_LIBRARIES += \
        libExynosHWCService
endif
endif

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl

LOCAL_CFLAGS += -Wno-multichar

ifeq ($(DTS_CODEC_M_), true)
  LOCAL_SRC_FILES+= DTSUtils.cpp
  LOCAL_CFLAGS += -DDTS_CODEC_M_
endif

ifeq ($(BOARD_USE_SAMSUNG_COLORFORMAT), true)
LOCAL_CFLAGS += -DUSE_SAMSUNG_COLORFORMAT

# Include native color format header path
LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung/exynos4/hal/include \
	$(TOP)/hardware/samsung/exynos4/include
endif

# FFMPEG plugin
LOCAL_C_INCLUDES += \
	$(TOP)/external/stagefright-plugins/include

ifeq ($(BOARD_USE_ALP_AUDIO),  true)
LOCAL_CFLAGS += -DUSE_ALP_AUDIO
endif

ifeq ($(BOARD_USE_SEIREN_AUDIO), true)
LOCAL_CFLAGS += -DUSE_SEIREN_AUDIO
endif

LOCAL_MODULE:= libstagefright

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
