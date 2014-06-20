# Build the unit tests for audioflinger

#
# resampler unit test
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SHARED_LIBRARIES := \
	liblog \
	libutils \
	libcutils \
	libstlport \
	libaudioutils \
	libaudioresampler

LOCAL_STATIC_LIBRARIES := \
	libgtest \
	libgtest_main

LOCAL_C_INCLUDES := \
	bionic \
	bionic/libstdc++/include \
	external/gtest/include \
	external/stlport/stlport \
	$(call include-path-for, audio-utils) \
	frameworks/av/services/audioflinger

LOCAL_SRC_FILES := \
	resampler_tests.cpp

LOCAL_MODULE := resampler_tests
LOCAL_MODULE_TAGS := tests

include $(BUILD_EXECUTABLE)
