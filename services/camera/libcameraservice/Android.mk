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

LOCAL_PATH:= $(call my-dir)

#
# libcameraservice
#

include $(CLEAR_VARS)

# Camera service source

LOCAL_SRC_FILES :=  \
    CameraService.cpp \
    CameraFlashlight.cpp \
    common/Camera2ClientBase.cpp \
    common/CameraDeviceBase.cpp \
    common/CameraModule.cpp \
    common/FrameProcessorBase.cpp \
    api1/CameraClient.cpp \
    api1/Camera2Client.cpp \
    api1/client2/Parameters.cpp \
    api1/client2/FrameProcessor.cpp \
    api1/client2/StreamingProcessor.cpp \
    api1/client2/JpegProcessor.cpp \
    api1/client2/CallbackProcessor.cpp \
    api1/client2/JpegCompressor.cpp \
    api1/client2/CaptureSequencer.cpp \
    api1/client2/ZslProcessor.cpp \
    api2/CameraDeviceClient.cpp \
    device3/Camera3Device.cpp \
    device3/Camera3Stream.cpp \
    device3/Camera3IOStreamBase.cpp \
    device3/Camera3InputStream.cpp \
    device3/Camera3OutputStream.cpp \
    device3/Camera3ZslStream.cpp \
    device3/Camera3DummyStream.cpp \
    device3/StatusTracker.cpp \
    device3/Camera3BufferManager.cpp \
    gui/RingBufferConsumer.cpp \
    utils/CameraTraces.cpp \
    utils/AutoConditionLock.cpp \
    utils/TagMonitor.cpp

LOCAL_SHARED_LIBRARIES:= \
    libui \
    liblog \
    libutils \
    libbinder \
    libcutils \
    libmedia \
    libmediautils \
    libcamera_client \
    libgui \
    libhardware \
    libsync \
    libcamera_metadata \
    libjpeg \
    libmemunreachable

LOCAL_C_INCLUDES += \
    system/media/private/camera/include \
    frameworks/native/include/media/openmax \
    external/jpeg

LOCAL_EXPORT_C_INCLUDE_DIRS := \
    frameworks/av/services/camera/libcameraservice

LOCAL_CFLAGS += -Wall -Wextra -Werror

ifneq ($(BOARD_NUMBER_OF_CAMERAS),)
    LOCAL_CFLAGS += -DMAX_CAMERAS=$(BOARD_NUMBER_OF_CAMERAS)
endif

ifeq ($(TARGET_HAS_LEGACY_CAMERA_HAL1),true)
    LOCAL_CFLAGS += -DNO_CAMERA_SERVER
endif

LOCAL_MODULE:= libcameraservice

include $(BUILD_SHARED_LIBRARY)
