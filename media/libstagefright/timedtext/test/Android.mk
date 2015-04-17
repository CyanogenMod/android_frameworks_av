LOCAL_PATH:= $(call my-dir)

# ================================================================
# Unit tests for libstagefright_timedtext
# ================================================================

# ================================================================
# A test for TimedTextSRTSource
# ================================================================
include $(CLEAR_VARS)

LOCAL_MODULE := TimedTextSRTSource_test

LOCAL_MODULE_TAGS := eng tests

LOCAL_SRC_FILES := TimedTextSRTSource_test.cpp

LOCAL_C_INCLUDES := \
    $(TOP)/external/expat/lib \
    $(TOP)/frameworks/av/media/libstagefright/timedtext

LOCAL_SHARED_LIBRARIES := \
    libbinder \
    libexpat \
    libstagefright \
    libstagefright_foundation \
    libutils

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true

include $(BUILD_NATIVE_TEST)
