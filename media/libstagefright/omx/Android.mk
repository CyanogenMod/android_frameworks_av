LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        GraphicBufferSource.cpp       \
        OMX.cpp                       \
        OMXMaster.cpp                 \
        OMXNodeInstance.cpp           \
        SimpleSoftOMXComponent.cpp    \
        SoftOMXComponent.cpp          \
        SoftOMXPlugin.cpp             \
        SoftVideoDecoderOMXComponent.cpp \

LOCAL_C_INCLUDES += \
        $(TOP)/frameworks/av/media/libstagefright \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES :=               \
        libbinder                       \
        libmedia                        \
        libutils                        \
        liblog                          \
        libui                           \
        libgui                          \
        libcutils                       \
        libstagefright_foundation       \
        libdl

ifeq ($(BOARD_HAS_MTK_HARDWARE),true)
    LOCAL_C_INCLUDES += \
        $(TOP)/hardware/mediatek/media/include

    LOCAL_SHARED_LIBRARIES += \
        libstagefright_memutil

    LOCAL_STATIC_LIBRARIES += \
        libstagefright_bufferallocator
endif

ifeq ($(BOARD_USES_PROPRIETARY_OMX),SAMSUNG)
LOCAL_CFLAGS     += -DSAMSUNG_OMX
endif

LOCAL_MODULE:= libstagefright_omx

include $(BUILD_SHARED_LIBRARY)

################################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
