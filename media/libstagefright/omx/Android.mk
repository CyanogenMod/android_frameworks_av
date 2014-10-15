LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_DEVICE), manta)
    LOCAL_CFLAGS += -DSURFACE_IS_BGR32
endif

LOCAL_SRC_FILES:=                     \
        GraphicBufferSource.cpp       \
        OMX.cpp                       \
        OMXMaster.cpp                 \
        OMXNodeInstance.cpp           \
        SimpleSoftOMXComponent.cpp    \
        SoftOMXComponent.cpp          \
        SoftOMXPlugin.cpp             \
        SoftVideoDecoderOMXComponent.cpp \
        SoftVideoEncoderOMXComponent.cpp \

LOCAL_C_INCLUDES += \
        $(TOP)/frameworks/av/media/libstagefright \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES :=               \
        libbinder                       \
        libhardware                     \
        libmedia                        \
        libutils                        \
        liblog                          \
        libui                           \
        libgui                          \
        libcutils                       \
        libstagefright_foundation       \
        libdl

LOCAL_MODULE:= libstagefright_omx

include $(BUILD_SHARED_LIBRARY)

################################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
