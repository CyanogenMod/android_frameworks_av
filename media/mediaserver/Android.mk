LOCAL_PATH:= $(call my-dir)

ifneq ($(BOARD_USE_CUSTOM_MEDIASERVEREXTENSIONS),true)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := register.cpp
LOCAL_MODULE := libregistermsext
LOCAL_MODULE_TAGS := optional
include $(BUILD_STATIC_LIBRARY)
endif

include $(CLEAR_VARS)

ifeq ($(QCOM_LISTEN_FEATURE),true)
  LOCAL_CFLAGS += -DQCOM_LISTEN_FEATURE_ENABLE
endif

LOCAL_SRC_FILES:= \
	main_mediaserver.cpp 

LOCAL_SHARED_LIBRARIES := \
	libaudioflinger \
	libcameraservice \
	libmedialogservice \
	libcutils \
	libnbaio \
	libmedia \
	libmediaplayerservice \
	libutils \
	liblog \
	libbinder

LOCAL_STATIC_LIBRARIES := \
	libregistermsext

ifeq ($(BOARD_USE_SECTVOUT),true)
	LOCAL_CFLAGS += -DSECTVOUT
	LOCAL_SHARED_LIBRARIES += libTVOut
endif

LOCAL_C_INCLUDES := \
    frameworks/av/media/libmediaplayerservice \
    frameworks/av/services/medialog \
    frameworks/av/services/audioflinger \
    frameworks/av/services/camera/libcameraservice

LOCAL_MODULE:= mediaserver

ifeq ($(QCOM_LISTEN_FEATURE),true)
  LOCAL_SHARED_LIBRARIES += liblisten
  LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-listen
endif

include $(BUILD_EXECUTABLE)
