LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_NDK_STL_VARIANT := stlport_static

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
        $(TOP)/bionic \
        $(TOP)/bionic/libstdc++/include \
        $(TOP)/external/stlport/stlport \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/frameworks/rs/cpp \
        $(TOP)/frameworks/rs \

intermediates := $(call intermediates-dir-for,STATIC_LIBRARIES,libRS,TARGET,)
LOCAL_C_INCLUDES += $(intermediates)

LOCAL_CFLAGS += -Wno-multichar

LOCAL_MODULE:= libstagefright_mediafilter

include $(BUILD_STATIC_LIBRARY)
