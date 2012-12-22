LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        OMX.cpp                       \
        OMXMaster.cpp                 \
        OMXNodeInstance.cpp           \
        SimpleSoftOMXComponent.cpp    \
        SoftOMXComponent.cpp          \
        SoftOMXPlugin.cpp             \

LOCAL_C_INCLUDES += \
        $(TOP)/frameworks/av/media/libstagefright \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES :=               \
        libbinder                       \
        libmedia                        \
        libutils                        \
        libui                           \
        libcutils                       \
        libstagefright_foundation       \
        libdl

LOCAL_MODULE:= libstagefright_omx

ifeq ($(BOARD_USE_SAMSUNG_OMX_LIBS), true)
LOCAL_CFLAGS += -DBOARD_USE_SAMSUNG_OMX_LIBS
endif

include $(BUILD_SHARED_LIBRARY)

################################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
