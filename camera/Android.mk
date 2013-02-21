CAMERA_CLIENT_LOCAL_PATH:= $(call my-dir)
include $(call all-subdir-makefiles)
include $(CLEAR_VARS)

LOCAL_PATH := $(CAMERA_CLIENT_LOCAL_PATH)

LOCAL_SRC_FILES:= \
	Camera.cpp \
	CameraMetadata.cpp \
	CameraParameters.cpp \
	ICamera.cpp \
	ICameraClient.cpp \
	ICameraService.cpp \
	ICameraRecordingProxy.cpp \
	ICameraRecordingProxyListener.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libbinder \
	libhardware \
	libui \
	libgui \
	libcamera_metadata \

LOCAL_C_INCLUDES += \
	system/media/camera/include \

LOCAL_MODULE:= libcamera_client

include $(BUILD_SHARED_LIBRARY)
