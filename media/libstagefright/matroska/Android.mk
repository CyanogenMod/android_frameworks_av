LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                 \
        MatroskaExtractor.cpp

LOCAL_C_INCLUDES:= \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/frameworks/av/media/libstagefright/include \

LOCAL_CFLAGS += -Wno-multichar -Werror

LOCAL_MODULE:= libstagefright_matroska

ifeq ($(AUDIO_FEATURE_OFFLOAD_PCM_ENABLED_24),true)
LOCAL_CFLAGS += -DPCM_OFFLOAD_ENABLED_24;
endif

include $(BUILD_STATIC_LIBRARY)
