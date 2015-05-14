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

include $(BUILD_STATIC_LIBRARY)
