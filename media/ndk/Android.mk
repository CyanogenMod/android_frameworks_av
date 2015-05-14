#
# Copyright (C) 2014 The Android Open Source Project
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

LOCAL_SRC_FILES:=                                       \
                  NdkMediaCodec.cpp                     \
                  NdkMediaCrypto.cpp                    \
                  NdkMediaExtractor.cpp                 \
                  NdkMediaFormat.cpp                    \
                  NdkMediaMuxer.cpp                     \
                  NdkMediaDrm.cpp                       \

LOCAL_MODULE:= libmediandk

LOCAL_C_INCLUDES := \
    bionic/libc/private \
    frameworks/base/core/jni \
    frameworks/av/include/ndk

LOCAL_CFLAGS += -fvisibility=hidden -D EXPORT='__attribute__ ((visibility ("default")))'

LOCAL_SHARED_LIBRARIES := \
    libbinder \
    libmedia \
    libstagefright \
    libstagefright_foundation \
    liblog \
    libutils \
    libandroid_runtime \
    libbinder \

include $(BUILD_SHARED_LIBRARY)

endif
