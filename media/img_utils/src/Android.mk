# Copyright 2014 The Android Open Source Project
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

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
  EndianUtils.cpp \
  FileInput.cpp \
  FileOutput.cpp \
  SortedEntryVector.cpp \
  Input.cpp \
  Output.cpp \
  Orderable.cpp \
  TiffIfd.cpp \
  TiffWritable.cpp \
  TiffWriter.cpp \
  TiffEntry.cpp \
  TiffEntryImpl.cpp \
  ByteArrayOutput.cpp \
  DngUtils.cpp \

LOCAL_SHARED_LIBRARIES := \
  libexpat \
  libutils \
  libcutils \
  libcamera_metadata \
  libcamera_client

LOCAL_C_INCLUDES += \
  $(LOCAL_PATH)/../include \
  system/media/camera/include

LOCAL_CFLAGS += \
  -Wall \
  -Wextra \
  -Werror \
  -fvisibility=hidden

ifneq ($(filter userdebug eng,$(TARGET_BUILD_VARIANT)),)
    # Enable assert() in eng builds
    LOCAL_CFLAGS += -UNDEBUG -DLOG_NDEBUG=1
endif

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/../include

LOCAL_MODULE := libimg_utils

include $(BUILD_SHARED_LIBRARY)
