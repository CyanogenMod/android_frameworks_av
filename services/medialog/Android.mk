LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := MediaLogService.cpp

LOCAL_SHARED_LIBRARIES := libmedia libbinder libutils liblog libnbaio

LOCAL_MULTILIB := $(AUDIOSERVER_MULTILIB)

LOCAL_MODULE:= libmedialogservice

LOCAL_C_INCLUDES := $(call include-path-for, audio-utils)

include $(BUILD_SHARED_LIBRARY)
