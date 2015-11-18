LOCAL_PATH:= $(call my-dir)

ifeq ($(TARGET_IGNORE_VENDOR_AUDIO_EFFECTS_CONF),true)
  LOCAL_CFLAGS += -DIGNORE_VENDOR_AUDIO_EFFECTS_CONF
endif

# Effect factory library
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	EffectsFactory.c

LOCAL_SHARED_LIBRARIES := \
	libcutils liblog

LOCAL_MODULE:= libeffects

LOCAL_SHARED_LIBRARIES += libdl

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-effects)

include $(BUILD_SHARED_LIBRARY)
