LOCAL_PATH := $(call my-dir)

# service library
include $(CLEAR_VARS)
LOCAL_SRC_FILES := MediaCodecService.cpp
LOCAL_SHARED_LIBRARIES := libmedia libbinder libutils liblog libstagefright_omx
LOCAL_C_INCLUDES := \
    $(TOP)/frameworks/av/media/libstagefright \
    $(TOP)/frameworks/native/include/media/openmax
LOCAL_MODULE:= libmediacodecservice
LOCAL_32_BIT_ONLY := true
include $(BUILD_SHARED_LIBRARY)


# service executable
include $(CLEAR_VARS)
LOCAL_SRC_FILES := main_codecservice.cpp
LOCAL_SHARED_LIBRARIES := libmedia libmediacodecservice libbinder libutils liblog
LOCAL_C_INCLUDES := \
    $(TOP)/frameworks/av/media/libstagefright \
    $(TOP)/frameworks/native/include/media/openmax
LOCAL_MODULE:= mediacodec
LOCAL_32_BIT_ONLY := true
LOCAL_INIT_RC := mediacodec.rc
include $(BUILD_EXECUTABLE)


