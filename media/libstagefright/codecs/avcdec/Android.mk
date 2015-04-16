#ifeq ($(if $(wildcard external/libh264),1,0),1)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE            := libstagefright_soft_avcdec
LOCAL_MODULE_TAGS       := optional

LOCAL_STATIC_LIBRARIES  := libavcdec
LOCAL_SRC_FILES         := SoftAVCDec.cpp

LOCAL_C_INCLUDES := $(TOP)/external/libavc/decoder
LOCAL_C_INCLUDES += $(TOP)/external/libavc/common
LOCAL_C_INCLUDES += $(TOP)/frameworks/av/media/libstagefright/include
LOCAL_C_INCLUDES += $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES  := libstagefright
LOCAL_SHARED_LIBRARIES  += libstagefright_omx
LOCAL_SHARED_LIBRARIES  += libstagefright_foundation
LOCAL_SHARED_LIBRARIES  += libutils
LOCAL_SHARED_LIBRARIES  += liblog

LOCAL_LDFLAGS := -Wl,-Bsymbolic

include $(BUILD_SHARED_LIBRARY)

#endif
