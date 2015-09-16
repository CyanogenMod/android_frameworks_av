LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
        YUVImage.cpp            \
        YUVCanvas.cpp

LOCAL_SHARED_LIBRARIES :=       \
        libcutils \
        liblog

LOCAL_MODULE:= libstagefright_yuv


LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true
LOCAL_SANITIZE := signed-integer-overflow

include $(BUILD_SHARED_LIBRARY)
