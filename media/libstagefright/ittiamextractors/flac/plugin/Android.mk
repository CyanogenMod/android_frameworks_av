LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_SUFFIX := .a
LOCAL_MODULE := libittiam_flacparser
LOCAL_SRC_FILES := $(LOCAL_MODULE)$(LOCAL_MODULE_SUFFIX)
LOCAL_MODULE_CLASS := STATIC_LIBRARIES
LOCAL_MODULE_TAGS := optional
include $(BUILD_PREBUILT)


include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_SUFFIX := .a
LOCAL_MODULE := libIttiamFLACExtractor
LOCAL_SRC_FILES := $(LOCAL_MODULE)$(LOCAL_MODULE_SUFFIX)
LOCAL_MODULE_CLASS := STATIC_LIBRARIES
LOCAL_MODULE_TAGS := optional
include $(BUILD_PREBUILT)

#include $(CLEAR_VARS)

#LOCAL_SRC_FILES := \
#    IttiamFLACExtractor.cpp

#LOCAL_MODULE := libIttiamFLACExtractor
#LOCAL_ARM_MODE := arm
#LOCAL_MODULE_TAGS := optional

#LOCAL_C_INCLUDES := \
#    $(TOP)/frameworks/av/media/libstagefright/ittiamextractors/flac/component/inc \
#    $(TOP)/frameworks/av/include \

#LOCAL_CFLAGS += -DUSE_ALP_AUDIO
#LOCAL_CFLAGS += -DUSE_SEIREN_AUDIO

#include $(BUILD_STATIC_LIBRARY)
