LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        ColorConvert.cpp          \
        GraphicBufferListener.cpp \
        IntrinsicBlurFilter.cpp   \
        MediaFilter.cpp           \
        RSFilter.cpp              \
        SaturationFilter.cpp      \
        saturationARGB.rs         \
        SimpleFilter.cpp          \
        ZeroFilter.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/frameworks/rs/cpp \
        $(TOP)/frameworks/rs \

intermediates := $(call intermediates-dir-for,STATIC_LIBRARIES,libRS,TARGET,)
LOCAL_C_INCLUDES += $(intermediates)

LOCAL_CFLAGS += -Wno-multichar -Werror -Wall
LOCAL_CLANG := true

LOCAL_MODULE:= libstagefright_mediafilter

include $(BUILD_STATIC_LIBRARY)
