# Copyright 2005 Google Inc. All Rights Reserved.
#
# Android.mk for resampler_tools 
#


LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	fir.cpp

LOCAL_MODULE := fir

include $(BUILD_HOST_EXECUTABLE)


