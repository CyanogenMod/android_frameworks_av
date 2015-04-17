LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES =       \
	OMXHarness.cpp  \

LOCAL_SHARED_LIBRARIES := \
	libstagefright libbinder libmedia libutils liblog libstagefright_foundation

LOCAL_C_INCLUDES := \
	$(TOP)/frameworks/av/media/libstagefright \
	$(TOP)/frameworks/native/include/media/openmax

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true

LOCAL_MODULE := omx_tests

LOCAL_MODULE_TAGS := tests

LOCAL_32_BIT_ONLY := true

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := FrameDropper_test

LOCAL_MODULE_TAGS := tests

LOCAL_SRC_FILES := \
	FrameDropper_test.cpp \

LOCAL_SHARED_LIBRARIES := \
	libstagefright_omx \
	libutils \

LOCAL_C_INCLUDES := \
	frameworks/av/media/libstagefright/omx \

LOCAL_CFLAGS += -Werror -Wall
LOCAL_CLANG := true

include $(BUILD_NATIVE_TEST)
