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
# ----------------------------------------------------------------
# Builds ClearKey Drm Tests
#
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := ClearKeyDrmUnitTest
LOCAL_MODULE_TAGS := tests

LOCAL_SRC_FILES := \
    AesCtrDecryptorUnittest.cpp \
    InitDataParserUnittest.cpp \
    JsonWebKeyUnittest.cpp \

LOCAL_C_INCLUDES := \
    bionic \
    external/gtest/include \
    external/jsmn \
    external/openssl/include \
    external/stlport/stlport \
    frameworks/av/drm/mediadrm/plugins/clearkey \
    frameworks/av/include \
    frameworks/native/include \

LOCAL_STATIC_LIBRARIES := \
    libgtest \
    libgtest_main \

LOCAL_SHARED_LIBRARIES := \
    libcrypto \
    libdrmclearkeyplugin \
    liblog \
    libstagefright_foundation \
    libstlport \
    libutils \

include $(BUILD_NATIVE_TEST)
