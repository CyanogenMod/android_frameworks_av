LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        ColorConverter.cpp            \
        SoftwareRenderer.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/hardware/msm7k

ifeq ($(TARGET_BOARD_PLATFORM), exynos4)
LOCAL_CFLAGS += -DMALI_ALIGNMENT
endif

LOCAL_MODULE:= libstagefright_color_conversion

include $(BUILD_STATIC_LIBRARY)
