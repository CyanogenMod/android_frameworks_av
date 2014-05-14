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
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    AesCtrDecryptor.cpp \
    InitDataParser.cpp \
    JsonWebKey.cpp \
    ClearKeyUUID.cpp \
    CreatePluginFactories.cpp \
    CryptoFactory.cpp \
    CryptoPlugin.cpp \
    DrmFactory.cpp \
    DrmPlugin.cpp \
    InitDataParser.cpp \
    JsonWebKey.cpp \
    Session.cpp \
    SessionLibrary.cpp \
    Utils.cpp \

LOCAL_C_INCLUDES := \
    bionic \
    external/jsmn \
    external/openssl/include \
    frameworks/av/drm/mediadrm/plugins/clearkey \
    frameworks/av/include \
    frameworks/native/include \

LOCAL_MODULE := libdrmclearkeyplugin

LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_SHARED_LIBRARIES)/mediadrm

LOCAL_SHARED_LIBRARIES := \
    libcrypto \
    liblog \
    libstagefright_foundation \
    libutils \

LOCAL_STATIC_LIBRARIES := \
    libjsmn \

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

#########################################################################
# Build unit tests

include $(LOCAL_PATH)/tests/Android.mk
