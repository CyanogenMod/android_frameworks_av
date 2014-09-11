# Build the unit tests.
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_MODULE := SurfaceMediaSource_test

LOCAL_MODULE_TAGS := tests

LOCAL_SRC_FILES := \
    SurfaceMediaSource_test.cpp \
	DummyRecorder.cpp \

LOCAL_SHARED_LIBRARIES := \
	libEGL \
	libGLESv2 \
	libbinder \
	libcutils \
	libgui \
	libmedia \
	libstagefright \
	libstagefright_foundation \
	libstagefright_omx \
	libsync \
	libui \
	libutils \
	liblog

LOCAL_C_INCLUDES := \
	frameworks/av/media/libstagefright \
	frameworks/av/media/libstagefright/include \
	$(TOP)/frameworks/native/include/media/openmax \

LOCAL_32_BIT_ONLY := true

include $(BUILD_NATIVE_TEST)

# Include subdirectory makefiles
# ============================================================

# If we're building with ONE_SHOT_MAKEFILE (mm, mmm), then what the framework
# team really wants is to build the stuff defined by this makefile.
ifeq (,$(ONE_SHOT_MAKEFILE))
include $(call first-makefiles-under,$(LOCAL_PATH))
endif
