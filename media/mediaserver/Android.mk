LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	main_mediaserver.cpp 

LOCAL_SHARED_LIBRARIES := \
	libaudioflinger \
	libcameraservice \
	libmediaplayerservice \
	libutils \
	libbinder

ifeq ($(BOARD_USE_SECTVOUT),true)
	LOCAL_CFLAGS += -DSECTVOUT
	LOCAL_SHARED_LIBRARIES += libTVOut
endif

ifeq ($(TARGET_QCOM_AUDIO_VARIANT),caf)
	LOCAL_CFLAGS += -DQCOM_ENHANCED_AUDIO
endif

# FIXME The duplicate audioflinger is temporary
LOCAL_C_INCLUDES := \
    frameworks/av/media/libmediaplayerservice \
    frameworks/av/services/audioflinger \
    frameworks/av/services/camera/libcameraservice \
    frameworks/native/services/audioflinger

LOCAL_MODULE:= mediaserver

include $(BUILD_EXECUTABLE)
