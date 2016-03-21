LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    AudioResamplerCoefficients.cpp

LOCAL_MODULE := libaudio-resampler

LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES  := libutils liblog

LOCAL_CFLAGS += -Werror -Wall

include $(BUILD_SHARED_LIBRARY)
