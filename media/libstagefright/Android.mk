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

#QTI FLAC Decoder
ifeq ($(call is-vendor-board-platform,QCOM),true)
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FLAC_DECODER)),true)
LOCAL_SRC_FILES += FLACDecoder.cpp
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-flac
LOCAL_CFLAGS := -DQTI_FLAC_DECODER
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

ifeq ($(TARGET_USES_QCOM_BSP), true)
    LOCAL_C_INCLUDES += $(call project-path-for,qcom-display)/libgralloc
    LOCAL_CFLAGS += -DQCOM_BSP
endif

ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
       LOCAL_CFLAGS     += -DENABLE_AV_ENHANCEMENTS
       LOCAL_C_INCLUDES += $(TOP)/$(call project-path-for,qcom-media)/mm-core/inc
       LOCAL_C_INCLUDES += $(TOP)/frameworks/av/media/libstagefright/include
       LOCAL_SRC_FILES  += ExtendedMediaDefs.cpp
       LOCAL_SRC_FILES  += ExtendedWriter.cpp
       LOCAL_SRC_FILES  += FMA2DPWriter.cpp
endif #TARGET_ENABLE_AV_ENHANCEMENTS

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_PCM_OFFLOAD_24)),true)
       LOCAL_CFLAGS     += -DPCM_OFFLOAD_ENABLED_24
       LOCAL_C_INCLUDES += $(TOP)/$(call project-path-for,qcom-media)/mm-core/inc
endif
endif

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_FLAC_OFFLOAD)),true)
       LOCAL_CFLAGS     += -DFLAC_OFFLOAD_ENABLED
endif
endif

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl

LOCAL_CFLAGS += -Wno-multichar

LOCAL_MODULE:= libstagefright

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
