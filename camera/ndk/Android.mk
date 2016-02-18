#
# Copyright (C) 2015 The Android Open Source Project
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
#

LOCAL_PATH:= $(call my-dir)

ifneq ($(TARGET_BUILD_PDK), true)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                  \
    NdkCameraManager.cpp           \
    NdkCameraMetadata.cpp          \
    NdkCameraDevice.cpp            \
    NdkCaptureRequest.cpp          \
    NdkCameraCaptureSession.cpp    \
    impl/ACameraManager.cpp        \
    impl/ACameraMetadata.cpp       \
    impl/ACameraDevice.cpp         \
    impl/ACameraCaptureSession.cpp

LOCAL_MODULE:= libcamera2ndk

LOCAL_C_INCLUDES := \
    frameworks/av/include/camera/ndk \
    frameworks/av/include/ndk

LOCAL_CFLAGS += -fvisibility=hidden -D EXPORT='__attribute__ ((visibility ("default")))'
LOCAL_CFLAGS += -Wall -Wextra -Werror

LOCAL_SHARED_LIBRARIES := \
    libbinder \
    liblog \
    libgui \
    libutils \
    libandroid_runtime \
    libcamera_client \
    libstagefright_foundation \
    libcutils \

LOCAL_CLANG := true

include $(BUILD_SHARED_LIBRARY)

endif
