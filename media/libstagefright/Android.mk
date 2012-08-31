LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(BOARD_USES_ALSA_AUDIO),true)
    ifeq ($(TARGET_BOARD_PLATFORM),msm8960)
        LOCAL_CFLAGS += -DUSE_TUNNEL_MODE
    endif
endif

include frameworks/av/media/libstagefright/codecs/common/Config.mk

ifeq ($(TARGET_SOC),exynos4210)
LOCAL_CFLAGS += -DCONFIG_MFC_FPS
endif

ifeq ($(TARGET_SOC),exynos4x12)
LOCAL_CFLAGS += -DCONFIG_MFC_FPS
endif

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
        DataSource.cpp                    \
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
        MediaBuffer.cpp                   \
        MediaBufferGroup.cpp              \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
        MediaSource.cpp                   \
        MetaData.cpp                      \
        NuCachedSource2.cpp               \
        NuMediaExtractor.cpp              \
        OMXClient.cpp                     \
        OMXCodec.cpp                      \
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
        WVMExtractor.cpp                  \
        XINGSeeker.cpp                    \
        avc_utils.cpp                     \

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
LOCAL_SRC_FILES+=                         \
        ExtendedExtractor.cpp             \
        ExtendedWriter.cpp                \
        TunnelPlayer.cpp

ifeq ($(TARGET_BOARD_PLATFORM),msm8960)
   LOCAL_SRC_FILES += LPAPlayerALSA.cpp
else
   LOCAL_SRC_FILES += LPAPlayer.cpp
endif

ifeq ($(BOARD_HAVE_QCOM_FM),true)
LOCAL_SRC_FILES+=                         \
        FMA2DPWriter.cpp
endif
ifeq ($(BOARD_USES_ALSA_AUDIO),true)
    LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/libalsa-intf
    LOCAL_SHARED_LIBRARIES += libalsa-intf
endif
endif

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/expat/lib \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/openssl/include \
        $(TOP)/hardware/qcom/display/libgralloc \
        $(TOP)/hardware/qcom/media/mm-core/inc \
        $(TOP)/system/core/include

LOCAL_SHARED_LIBRARIES := \
        libbinder \
        libcamera_client \
        libcrypto \
        libcutils \
        libdl \
        libdrmframework \
        libexpat \
        libgui \
        libicui18n \
        libicuuc \
        liblog \
        libmedia \
        libmedia_native \
        libsonivox \
        libssl \
        libstagefright_omx \
        libstagefright_yuv \
        libui \
        libutils \
        libvorbisidec \
        libz \

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_timedtext \
        libvpx \
        libstagefright_mpeg2ts \
        libstagefright_httplive \
        libstagefright_id3 \
        libFLAC \

ifeq ($(call is-vendor-board-platform,QCOM),true)
endif

ifneq ($(TARGET_BUILD_PDK), true)
LOCAL_STATIC_LIBRARIES += \
	libstagefright_chromium_http
LOCAL_SHARED_LIBRARIES += \
        libchromium_net
LOCAL_CPPFLAGS += -DCHROMIUM_AVAILABLE=1
endif

LOCAL_SHARED_LIBRARIES += libstlport
include external/stlport/libstlport.mk

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl

LOCAL_CFLAGS += -Wno-multichar

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
    LOCAL_C_INCLUDES += $(TOP)/hardware/qcom/display/libgralloc
    ifeq ($(BOARD_CAMERA_USE_MM_HEAP),true)
        LOCAL_CFLAGS += -DCAMERA_MM_HEAP
    endif
endif

ifeq ($(filter-out exynos4 exynos5,$(TARGET_BOARD_PLATFORM)),)
LOCAL_CFLAGS += -DSAMSUNG_ANDROID_PATCH
endif

ifeq ($(TARGET_BOARD_PLATFORM), exynos4)
LOCAL_C_INCLUDES += $(TOP)/hardware/samsung/exynos4/hal/include
endif

ifeq ($(BOARD_USE_SAMSUNG_COLORFORMAT), true)
# Include native color format header path
LOCAL_C_INCLUDES += $(TARGET_HAL_PATH)/include

LOCAL_CFLAGS += -DUSE_SAMSUNG_COLORFORMAT
endif

ifeq ($(BOARD_FIX_NATIVE_COLOR_FORMAT), true)
# Add native color format patch definition
LOCAL_CFLAGS += -DNATIVE_COLOR_FORMAT_PATCH

endif # ifeq ($(BOARD_FIX_NATIVE_COLOR_FORMAT), true)

ifeq ($(BOARD_USE_SAMSUNG_V4L2_ION), true)
LOCAL_CFLAGS += -DBOARD_USE_SAMSUNG_V4L2_ION
endif

ifeq ($(TARGET_BOARD_PLATFORM), exynos4)
ifeq ($(BOARD_USE_SAMSUNG_V4L2_ION), false)
ifeq ($(BOARD_USE_S3D_SUPPORT), true)
LOCAL_CFLAGS += -DS3D_SUPPORT
endif
endif
endif

ifeq ($(TARGET_BOARD_PLATFORM), exynos5)
ifeq ($(BOARD_USE_S3D_SUPPORT), true)
LOCAL_CFLAGS += -DS3D_SUPPORT
endif
endif

ifeq ($(filter-out s5pc110 s5pv210,$(TARGET_SOC)),)
ifeq ($(BOARD_USE_V4L2), false)
ifeq ($(BOARD_USE_S3D_SUPPORT), true)
LOCAL_CFLAGS += -DS3D_SUPPORT
endif
endif
endif

ifeq ($(TARGET_SOC),exynos4x12)
LOCAL_CFLAGS += -DSAMSUNG_EXYNOS4x12
endif

ifeq ($(TARGET_SOC),exynos5250)
LOCAL_CFLAGS += -DSAMSUNG_EXYNOS5250
endif

ifeq ($(BOARD_USES_HDMI),true)
LOCAL_CFLAGS += -DBOARD_USES_HDMI
endif

LOCAL_MODULE:= libstagefright

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
