ifeq ($(if $(wildcard external/libmpeg2),1,0),1)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE            := libstagefright_soft_mpeg2dec
LOCAL_MODULE_TAGS       := optional

LOCAL_STATIC_LIBRARIES  := libmpeg2dec
LOCAL_SRC_FILES         := SoftMPEG2.cpp

LOCAL_C_INCLUDES := $(TOP)/external/libmpeg2/decoder
LOCAL_C_INCLUDES += $(TOP)/external/libmpeg2/common
LOCAL_C_INCLUDES += $(TOP)/frameworks/av/media/libstagefright/include
LOCAL_C_INCLUDES += $(TOP)/frameworks/native/include/media/openmax

LOCAL_SHARED_LIBRARIES  := libstagefright
LOCAL_SHARED_LIBRARIES  += libstagefright_omx
LOCAL_SHARED_LIBRARIES  += libstagefright_foundation
LOCAL_SHARED_LIBRARIES  += libutils
LOCAL_SHARED_LIBRARIES  += liblog

LOCAL_LDFLAGS := -Wl,-Bsymbolic

include $(BUILD_SHARED_LIBRARY)

endif
