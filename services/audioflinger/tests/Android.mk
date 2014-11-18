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

# Clang++ aborts on AudioMixer.cpp,
# b/18373866, "do not know how to split this operator."
ifeq ($(filter $(TARGET_ARCH),arm arm64),$(TARGET_ARCH))
    LOCAL_CLANG := false
endif

LOCAL_SRC_FILES:= \
	test-mixer.cpp \
	../AudioMixer.cpp.arm \

LOCAL_C_INCLUDES := \
	$(call include-path-for, audio-effects) \
	$(call include-path-for, audio-utils) \
	frameworks/av/services/audioflinger

LOCAL_STATIC_LIBRARIES := \
	libsndfile

LOCAL_SHARED_LIBRARIES := \
	libeffects \
	libnbaio \
	libcommon_time_client \
	libaudioresampler \
	libaudioutils \
	libdl \
	libcutils \
	libutils \
	liblog

LOCAL_MODULE:= test-mixer

LOCAL_MODULE_TAGS := optional

LOCAL_CXX_STL := libc++

include $(BUILD_EXECUTABLE)
