#
# Copyright (C) 2011 The Android Open Source Project
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

LOCAL_SRC_FILES:= \
    VideoEditorVideoDecoder.cpp \
    VideoEditorAudioDecoder.cpp \
    VideoEditorMp3Reader.cpp \
    VideoEditor3gpReader.cpp \
    VideoEditorUtils.cpp \
    VideoEditorBuffer.c \
    VideoEditorVideoEncoder.cpp \
    VideoEditorAudioEncoder.cpp

LOCAL_C_INCLUDES += \
    $(TOP)/frameworks/base/core/jni \
    $(TOP)/frameworks/base/include \
    $(TOP)/frameworks/base/include/media \
    $(TOP)/frameworks/base/media/libmediaplayerservice \
    $(TOP)/frameworks/base/media/libstagefright \
    $(TOP)/frameworks/base/media/libstagefright/include \
    $(TOP)/frameworks/base/media/libstagefright/rtsp \
    $(JNI_H_INCLUDE) \
    $(call include-path-for, corecg graphics) \
    $(TOP)/frameworks/base/include/media/stagefright/openmax \
    $(TOP)/frameworks/base/core/jni/mediaeditor \
    $(TOP)/frameworks/media/libvideoeditor/vss/inc \
    $(TOP)/frameworks/media/libvideoeditor/vss/common/inc \
    $(TOP)/frameworks/media/libvideoeditor/vss/mcs/inc \
    $(TOP)/frameworks/media/libvideoeditor/lvpp \
    $(TOP)/frameworks/media/libvideoeditor/osal/inc \
    $(TOP)/frameworks/media/libvideoeditor/include \
    $(TOP)/frameworks/media/libvideoeditor/vss/stagefrightshells/inc

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libandroid_runtime \
    libnativehelper \
    libmedia \
    libbinder \
    libstagefright \
    libstagefright_omx \
    libgui \
    libvideoeditorplayer

LOCAL_CFLAGS += \



LOCAL_STATIC_LIBRARIES := \
    libvideoeditor_osal \
    libstagefright_color_conversion


LOCAL_MODULE:= libvideoeditor_stagefrightshells

LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
