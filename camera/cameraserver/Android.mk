# Copyright 2015 The Android Open Source Project
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

ifeq ($(TARGET_HAS_LEGACY_CAMERA_HAL1),true)
$(warning Target has integrated cameraserver into mediaserver. This is weakening security measures introduced in 7.0)
else
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	main_cameraserver.cpp

LOCAL_SHARED_LIBRARIES := \
	libcameraservice \
	libcutils \
	libutils \
	libbinder \
	libcamera_client

LOCAL_MODULE:= cameraserver
LOCAL_32_BIT_ONLY := true

LOCAL_CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter

LOCAL_INIT_RC := cameraserver.rc

include $(BUILD_EXECUTABLE)
endif
