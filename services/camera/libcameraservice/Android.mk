LOCAL_PATH:= $(call my-dir)

#
# libcameraservice
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    CameraService.cpp

LOCAL_SHARED_LIBRARIES:= \
    libui \
    libutils \
    libbinder \
    libcutils \
    libmedia \
    libmedia_native \
    libcamera_client \
    libgui \
    libhardware

ifeq ($(BOARD_USE_SAMSUNG_V4L2_ION), true)
LOCAL_CFLAGS += -DBOARD_USE_SAMSUNG_V4L2_ION
endif

LOCAL_MODULE:= libcameraservice

include $(BUILD_SHARED_LIBRARY)
