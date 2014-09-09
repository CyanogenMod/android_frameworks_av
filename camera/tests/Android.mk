LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	ProCameraTests.cpp \

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/native/include \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_client_test
LOCAL_MODULE_TAGS := tests

include $(BUILD_NATIVE_TEST)
