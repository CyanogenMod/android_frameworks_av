LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

include frameworks/av/media/libstagefright/codecs/common/Config.mk

ifeq ($(BOARD_HTC_3D_SUPPORT),true)
   LOCAL_CFLAGS += -DHTC_3D_SUPPORT
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
        MediaAdapter.cpp                  \
        MediaBuffer.cpp                   \
        MediaBufferGroup.cpp              \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
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
        mp4/FragmentedMP4Parser.cpp       \
        mp4/TrackFragment.cpp             \
        ExtendedExtractor.cpp             \
        QCUtils.cpp                       \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/openssl/include

ifneq ($(TI_CUSTOM_DOMX_PATH),)
LOCAL_C_INCLUDES += $(TI_CUSTOM_DOMX_PATH)/omx_core/inc
LOCAL_CPPFLAGS += -DUSE_TI_CUSTOM_DOMX
else
LOCAL_C_INCLUDES += $(TOP)/frameworks/native/include/media/openmax
endif

ifeq ($(BOARD_USES_STE_FMRADIO),true)
LOCAL_SRC_FILES += \
        FMRadioSource.cpp                 \
        PCMExtractor.cpp
endif

ifneq ($(filter caf bfam,$(TARGET_QCOM_AUDIO_VARIANT)),)
    ifeq ($(BOARD_USES_ALSA_AUDIO),true)
        LOCAL_SRC_FILES += LPAPlayerALSA.cpp
        ifeq ($(call is-chipset-in-board-platform,msm8960),true)
            LOCAL_SRC_FILES += TunnelPlayer.cpp
            LOCAL_CFLAGS += -DUSE_TUNNEL_MODE
            LOCAL_CFLAGS += -DTUNNEL_MODE_SUPPORTS_AMRWB
        endif
        ifeq ($(call is-chipset-in-board-platform,msm8974),true)
            LOCAL_SRC_FILES += TunnelPlayer.cpp
            LOCAL_CFLAGS += -DUSE_TUNNEL_MODE
        endif
        ifeq ($(NO_TUNNEL_MODE_FOR_MULTICHANNEL),true)
            LOCAL_CFLAGS += -DNO_TUNNEL_MODE_FOR_MULTICHANNEL
        endif
    else
        LOCAL_SRC_FILES += LPAPlayer.cpp
        LOCAL_CFLAGS += -DLEGACY_LPA
    endif
    LOCAL_CFLAGS += -DQCOM_ENHANCED_AUDIO
endif

ifneq ($(TARGET_QCOM_MEDIA_VARIANT),)
LOCAL_C_INCLUDES += \
        $(TOP)/hardware/qcom/media-$(TARGET_QCOM_MEDIA_VARIANT)/mm-core/inc
else
LOCAL_C_INCLUDES += \
        $(TOP)/hardware/qcom/media/mm-core/inc
endif

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
        libsonivox \
        libssl \
        libstagefright_omx \
        libstagefright_yuv \
        libsync \
        libui \
        libutils \
        libvorbisidec \
        libz \

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libstagefright_mp3dec \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_timedtext \
        libvpx \
        libwebm \
        libstagefright_mpeg2ts \
        libstagefright_httplive \
        libstagefright_id3 \
        libFLAC \

LOCAL_SRC_FILES += \
        chromium_http_stub.cpp
LOCAL_CPPFLAGS += -DCHROMIUM_AVAILABLE=1

LOCAL_SHARED_LIBRARIES += libstlport
include external/stlport/libstlport.mk

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl

LOCAL_CFLAGS += -Wno-multichar

ifeq ($(BOARD_USE_SAMSUNG_COLORFORMAT), true)
LOCAL_CFLAGS += -DUSE_SAMSUNG_COLORFORMAT

# Include native color format header path
LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung/exynos4/hal/include \
	$(TOP)/hardware/samsung/exynos4/include

endif

ifeq ($(BOARD_USE_TI_DUCATI_H264_PROFILE), true)
LOCAL_CFLAGS += -DUSE_TI_DUCATI_H264_PROFILE
endif

LOCAL_MODULE:= libstagefright

LOCAL_MODULE_TAGS := optional


ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
    LOCAL_CFLAGS += -DENABLE_QC_AV_ENHANCEMENTS
    LOCAL_SRC_FILES  += ExtendedWriter.cpp
    LOCAL_SRC_FILES  += QCMediaDefs.cpp
    ifneq ($(TARGET_QCOM_MEDIA_VARIANT),)
        LOCAL_C_INCLUDES += \
            $(TOP)/hardware/qcom/media-$(TARGET_QCOM_MEDIA_VARIANT)/mm-core/inc
    else
        LOCAL_C_INCLUDES += \
            $(TOP)/hardware/qcom/media/mm-core/inc
    endif
endif #TARGET_ENABLE_QC_AV_ENHANCEMENTS

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
