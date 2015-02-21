LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                       \
        GenericSource.cpp               \
        HTTPLiveSource.cpp              \
        NuPlayer.cpp                    \
        NuPlayerDecoder.cpp             \
        NuPlayerDecoderPassThrough.cpp  \
        NuPlayerDriver.cpp              \
        NuPlayerRenderer.cpp            \
        NuPlayerStreamListener.cpp      \
        RTSPSource.cpp                  \
        StreamingSource.cpp             \

LOCAL_C_INCLUDES := \
	$(TOP)/frameworks/av/media/libstagefright/httplive            \
	$(TOP)/frameworks/av/media/libstagefright/include             \
	$(TOP)/frameworks/av/media/libstagefright/mpeg2ts             \
	$(TOP)/frameworks/av/media/libstagefright/rtsp                \
	$(TOP)/frameworks/av/media/libstagefright/timedtext           \
	$(TOP)/frameworks/av/media/libmediaplayerservice              \
	$(TOP)/frameworks/native/include/media/openmax

#LOCAL_CFLAGS += -DLOG_NDEBUG=0

ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(TARGET_ENABLE_QC_AV_ENHANCEMENTS),true)
LOCAL_CFLAGS += -DENABLE_AV_ENHANCEMENTS
endif

# QTI FLAC Decoder
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_FLAC_DECODER)),true)
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-flac
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio
LOCAL_CFLAGS += -DQTI_FLAC_DECODER
endif

endif

LOCAL_MODULE:= libstagefright_nuplayer

LOCAL_MODULE_TAGS := eng

include $(BUILD_STATIC_LIBRARY)

