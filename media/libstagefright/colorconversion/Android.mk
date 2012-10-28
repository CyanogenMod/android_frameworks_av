LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        ColorConverter.cpp            \
        SoftwareRenderer.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/hardware/msm7k

ifeq ($(TARGET_QCOM_LEGACY_OMX),true)
        LOCAL_CFLAGS += -DQCOM_LEGACY_OMX
ifneq ($(TARGET_QCOM_DISPLAY_VARIANT),)
        LOCAL_C_INCLUDES += $(TOP)/hardware/qcom/display-$(TARGET_QCOM_DISPLAY_VARIANT)/libgralloc
else
        LOCAL_C_INCLUDES += $(TOP)/hardware/qcom/display/libgralloc
endif
endif

LOCAL_MODULE:= libstagefright_color_conversion

include $(BUILD_STATIC_LIBRARY)
