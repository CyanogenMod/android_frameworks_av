LOCAL_PATH:= $(call my-dir)

#
# libcameraservice
#

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    CameraService.cpp \
    CameraClient.cpp \
    Camera2Client.cpp \
    ProCamera2Client.cpp \
    Camera2ClientBase.cpp \
    CameraDeviceBase.cpp \
    Camera2Device.cpp \
    Camera3Device.cpp \
    camera2/Parameters.cpp \
    camera2/FrameProcessor.cpp \
    camera2/StreamingProcessor.cpp \
    camera2/JpegProcessor.cpp \
    camera2/CallbackProcessor.cpp \
    camera2/ZslProcessor.cpp \
    camera2/BurstCapture.cpp \
    camera2/JpegCompressor.cpp \
    camera2/CaptureSequencer.cpp \
    camera2/ProFrameProcessor.cpp \
    camera2/ZslProcessor3.cpp \
    camera3/Camera3Stream.cpp \
    camera3/Camera3IOStreamBase.cpp \
    camera3/Camera3InputStream.cpp \
    camera3/Camera3OutputStream.cpp \
    camera3/Camera3ZslStream.cpp \
    gui/RingBufferConsumer.cpp \

LOCAL_SHARED_LIBRARIES:= \
    libui \
    liblog \
    libutils \
    libbinder \
    libcutils \
    libmedia \
    libcamera_client \
    libgui \
    libhardware \
    libsync \
    libcamera_metadata \
    libjpeg

LOCAL_C_INCLUDES += \
    system/media/camera/include \
    external/jpeg


LOCAL_CFLAGS += -Wall -Wextra

ifeq ($(BOARD_USES_QCOM_LEGACY_CAM_PARAMS),true)
    LOCAL_CFLAGS += -DQCOM_LEGACY_CAM_PARAMS
endif

ifeq ($(BOARD_HAVE_HTC_FFC),true)
    LOCAL_CFLAGS += -DBOARD_HAVE_HTC_FFC
endif

LOCAL_MODULE:= libcameraservice

include $(BUILD_SHARED_LIBRARY)
