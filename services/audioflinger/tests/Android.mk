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
	libaudioutils \
	libaudioresampler

LOCAL_C_INCLUDES := \
	$(call include-path-for, audio-utils) \
	frameworks/av/services/audioflinger

LOCAL_SRC_FILES := \
	resampler_tests.cpp

LOCAL_MODULE := resampler_tests
LOCAL_MODULE_TAGS := tests

include $(BUILD_NATIVE_TEST)

#
# audio mixer test tool
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	test-mixer.cpp \
	../AudioMixer.cpp.arm \
	../BufferProviders.cpp

LOCAL_C_INCLUDES := \
	$(call include-path-for, audio-effects) \
	$(call include-path-for, audio-utils) \
	frameworks/av/services/audioflinger \
	external/sonic

LOCAL_STATIC_LIBRARIES := \
	libsndfile

LOCAL_SHARED_LIBRARIES := \
	libeffects \
	libnbaio \
	libaudioresampler \
	libaudioutils \
	libdl \
	libcutils \
	libutils \
	liblog \
	libsonic

LOCAL_MODULE:= test-mixer

LOCAL_MODULE_TAGS := optional

LOCAL_CXX_STL := libc++

include $(BUILD_EXECUTABLE)
