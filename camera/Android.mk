# Copyright 2010 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

CAMERA_CLIENT_LOCAL_PATH:= $(call my-dir)
include $(call all-subdir-makefiles)
include $(CLEAR_VARS)

LOCAL_PATH := $(CAMERA_CLIENT_LOCAL_PATH)

LOCAL_SRC_FILES:= \
	Camera.cpp \
	CameraMetadata.cpp \
	CaptureResult.cpp \
	CameraParameters2.cpp \
	ICamera.cpp \
	ICameraClient.cpp \
	ICameraService.cpp \
	ICameraServiceListener.cpp \
	ICameraServiceProxy.cpp \
	ICameraRecordingProxy.cpp \
	ICameraRecordingProxyListener.cpp \
	camera2/ICameraDeviceUser.cpp \
	camera2/ICameraDeviceCallbacks.cpp \
	camera2/CaptureRequest.cpp \
	camera2/OutputConfiguration.cpp \
	CameraBase.cpp \
	CameraUtils.cpp \
	VendorTagDescriptor.cpp

ifeq ($(BOARD_HAS_MTK_HARDWARE),true)
	LOCAL_SRC_FILES+= \
		MtkCameraParameters.cpp
endif

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	liblog \
	libbinder \
	libhardware \
	libui \
	libgui \
	libcamera_metadata \

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \

ifneq ($(TARGET_SPECIFIC_CAMERA_PARAMETER_LIBRARY),)
LOCAL_WHOLE_STATIC_LIBRARIES += $(TARGET_SPECIFIC_CAMERA_PARAMETER_LIBRARY)
else
LOCAL_WHOLE_STATIC_LIBRARIES += libcamera_parameters
endif

LOCAL_MODULE:= libcamera_client

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	CameraParameters.cpp

LOCAL_MODULE := libcamera_parameters

include $(BUILD_STATIC_LIBRARY)
