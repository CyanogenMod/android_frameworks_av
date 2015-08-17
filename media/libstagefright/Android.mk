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
        CallbackDataSource.cpp            \
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
        FrameRenderTracker.cpp            \
        HTTPBase.cpp                      \
        JPEGSource.cpp                    \
        MP3Extractor.cpp                  \
        MPEG2TSWriter.cpp                 \
        MPEG4Extractor.cpp                \
        MPEG4Writer.cpp                   \
        MediaAdapter.cpp                  \
        MediaBuffer.cpp                   \
        MediaBufferGroup.cpp              \
        MediaClock.cpp                    \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaCodecListOverrides.cpp       \
        MediaCodecSource.cpp              \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
        MediaSync.cpp                     \
        MidiExtractor.cpp                 \
        http/MediaHTTP.cpp                \
        MediaMuxer.cpp                    \
        MediaSource.cpp                   \
        MetaData.cpp                      \
        NuCachedSource2.cpp               \
        NuMediaExtractor.cpp              \
        OMXClient.cpp                     \
        OMXCodec.cpp                      \
        OggExtractor.cpp                  \
        ProcessInfo.cpp                   \
        SampleIterator.cpp                \
        SampleTable.cpp                   \
        SkipCutBuffer.cpp                 \
        StagefrightMediaScanner.cpp       \
        StagefrightMetadataRetriever.cpp  \
        SurfaceMediaSource.cpp            \
        SurfaceUtils.cpp                  \
        ThrottledSource.cpp               \
        TimeSource.cpp                    \
        TimedEventQueue.cpp               \
        Utils.cpp                         \
        VBRISeeker.cpp                    \
        VideoFrameScheduler.cpp           \
        WAVExtractor.cpp                  \
        WVMExtractor.cpp                  \
        XINGSeeker.cpp                    \
        avc_utils.cpp                     \
        APE.cpp                           \
        FFMPEGSoftCodec.cpp               \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/media/libavextensions \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/system/netd/include \

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
        libmediautils \
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
        libpowermanager \

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_mediafilter \
        libstagefright_webm \
        libstagefright_timedtext \
        libvpx \
        libwebm \
        libstagefright_mpeg2ts \
        libstagefright_id3 \
        libFLAC \
        libmedia_helper \

LOCAL_WHOLE_STATIC_LIBRARIES := libavextensions

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
        libdl \
        libRScpp \

LOCAL_CFLAGS += -Wno-multichar -Werror -Wno-error=deprecated-declarations -Wall

# enable experiments only in userdebug and eng builds
ifneq (,$(filter userdebug eng,$(TARGET_BUILD_VARIANT)))
LOCAL_CFLAGS += -DENABLE_STAGEFRIGHT_EXPERIMENTS
endif

LOCAL_CLANG := true

ifeq ($(BOARD_USE_SAMSUNG_CAMERAFORMAT_NV21), true)
# This needs flag requires the following string constant in
# CameraParametersExtra.h:
#
# const char CameraParameters::PIXEL_FORMAT_YUV420SP_NV21[] = "nv21";
LOCAL_CFLAGS += -DUSE_SAMSUNG_CAMERAFORMAT_NV21
endif

# FFMPEG plugin
LOCAL_C_INCLUDES += $(TOP)/external/stagefright-plugins/include

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
