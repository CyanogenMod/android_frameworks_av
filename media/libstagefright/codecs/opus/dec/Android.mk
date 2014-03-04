LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        SoftOpus.cpp

LOCAL_C_INCLUDES := \
        external/libopus/include \
        frameworks/av/media/libstagefright/include \
        frameworks/native/include/media/openmax \

LOCAL_SHARED_LIBRARIES := \
        libopus libstagefright libstagefright_omx \
        libstagefright_foundation libutils liblog

LOCAL_MODULE := libstagefright_soft_opusdec
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)