LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        ColorConverter.cpp            \
        SoftwareRenderer.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/hardware/msm7k

ifeq ($(BOARD_USE_V4L2_ION), true)
LOCAL_CFLAGS += -DUSE_V4L2_ION
endif

LOCAL_MODULE:= libstagefright_color_conversion

include $(BUILD_STATIC_LIBRARY)
