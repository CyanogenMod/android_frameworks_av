CAMERA_CLIENT_LOCAL_PATH:= $(call my-dir)
include $(call all-subdir-makefiles)
include $(CLEAR_VARS)

LOCAL_PATH := $(CAMERA_CLIENT_LOCAL_PATH)

LOCAL_SRC_FILES:= \
	Camera.cpp \
	CameraMetadata.cpp \
	CameraParameters.cpp \
	CameraParameters2.cpp \
	ICamera.cpp \
	ICameraClient.cpp \
	ICameraService.cpp \
	ICameraServiceListener.cpp \
	ICameraRecordingProxy.cpp \
	ICameraRecordingProxyListener.cpp \
	IProCameraUser.cpp \
	IProCameraCallbacks.cpp \
	camera2/ICameraDeviceUser.cpp \
	camera2/ICameraDeviceCallbacks.cpp \
	camera2/CaptureRequest.cpp \
	ProCamera.cpp \
	CameraBase.cpp \

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

ifeq ($(BOARD_CAMERA_HAVE_ISO),true)
	LOCAL_CFLAGS += -DHAVE_ISO
endif

ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
	LOCAL_CFLAGS += -DQCOM_HARDWARE
endif

LOCAL_MODULE:= libcamera_client

include $(BUILD_SHARED_LIBRARY)
