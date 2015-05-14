LOCAL_PATH:= $(call my-dir)

# Visualizer library
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	EffectVisualizer.cpp

LOCAL_CFLAGS+= -O2 -fvisibility=hidden

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	liblog \
	libdl

LOCAL_MODULE_RELATIVE_PATH := soundfx
LOCAL_MODULE:= libvisualizer

LOCAL_C_INCLUDES := \
	$(call include-path-for, audio-effects)


include $(BUILD_SHARED_LIBRARY)
