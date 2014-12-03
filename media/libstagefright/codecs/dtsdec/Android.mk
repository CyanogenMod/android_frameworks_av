LOCAL_PATH:= $(call my-dir)

ifeq ($(DTS_CODEC_M_), true)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
      SoftDTSDec.cpp

LOCAL_C_INCLUDES := \
      frameworks/av/media/libstagefright/include \
      frameworks/native/include/media/openmax

LOCAL_CFLAGS :=

LOCAL_CFLAGS += -Werror

LOCAL_SHARED_LIBRARIES := \
      libomx-dts \
      libstagefright_omx libstagefright_foundation libutils libcutils libdl

LOCAL_MODULE := libstagefright_soft_dtsdec
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := 32

include $(BUILD_SHARED_LIBRARY)

endif
