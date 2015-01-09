#
# This file was modified by DTS, Inc. The portions of the
# code that are surrounded by "DTS..." are copyrighted and
# licensed separately, as follows:
#
#  (C) 2013 DTS, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License
#

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ISchedulingPolicyService.cpp \
    SchedulingPolicyService.cpp

# FIXME Move this library to frameworks/native
LOCAL_MODULE := libscheduling_policy

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ServiceUtilities.cpp

# FIXME Move this library to frameworks/native
LOCAL_MODULE := libserviceutility

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libbinder

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    AudioFlinger.cpp            \
    Threads.cpp                 \
    Tracks.cpp                  \
    Effects.cpp                 \
    AudioMixer.cpp.arm          \
    PatchPanel.cpp

LOCAL_SRC_FILES += StateQueue.cpp

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audiopolicy \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudioutils \
    libcommon_time_client \
    libcutils \
    libutils \
    liblog \
    libbinder \
    libmedia \
    libnbaio \
    libhardware \
    libhardware_legacy \
    libeffects \
    libpowermanager \
    libserviceutility

LOCAL_STATIC_LIBRARIES := \
    libscheduling_policy \
    libcpustats \
    libmedia_helper

#QTI Resampler
ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_RESAMPLER)),true)
LOCAL_CFLAGS += -DQTI_RESAMPLER
endif
endif
#QTI Resampler

LOCAL_CFLAGS += -Os

LOCAL_MODULE:= libaudioflinger
LOCAL_32_BIT_ONLY := true

LOCAL_SRC_FILES += FastMixer.cpp FastMixerState.cpp AudioWatchdog.cpp
LOCAL_SRC_FILES += FastThread.cpp FastThreadState.cpp
LOCAL_SRC_FILES += FastCapture.cpp FastCaptureState.cpp

LOCAL_CFLAGS += -DSTATE_QUEUE_INSTANTIATIONS='"StateQueueInstantiations.cpp"'

# Define ANDROID_SMP appropriately. Used to get inline tracing fast-path.
ifeq ($(TARGET_CPU_SMP),true)
    LOCAL_CFLAGS += -DANDROID_SMP=1
else
    LOCAL_CFLAGS += -DANDROID_SMP=0
endif

LOCAL_CFLAGS += -fvisibility=hidden

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_HDMI_PASSTHROUGH)),true)
    LOCAL_CFLAGS += -DHDMI_PASSTHROUGH_ENABLED
endif

ifeq ($(strip $(BOARD_USES_SRS_TRUEMEDIA)),true)
LOCAL_SHARED_LIBRARIES += libsrsprocessing
LOCAL_CFLAGS += -DSRS_PROCESSING
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-effects
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_HW_ACCELERATED_EFFECTS)), true)
LOCAL_CFLAGS += -DHW_ACC_EFFECTS
LOCAL_WHOLE_STATIC_LIBRARIES := libhwacceffectswrapper
LOCAL_C_INCLUDES += hardware/qcom/audio/post_proc
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_DTS_EAGLE)), true)
LOCAL_CFLAGS += -DHW_ACC_HPX
endif
endif

include $(BUILD_SHARED_LIBRARY)

#
# build audio resampler test tool
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    test-resample.cpp           \

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)

LOCAL_STATIC_LIBRARIES := \
    libsndfile

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudioutils \
    libdl \
    libcutils \
    libutils \
    liblog

LOCAL_MODULE:= test-resample

LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioResampler.cpp.arm \
    AudioResamplerCubic.cpp.arm \
    AudioResamplerSinc.cpp.arm \
    AudioResamplerDyn.cpp.arm

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libdl \
    liblog

#QTI Resampler
ifeq ($(call is-vendor-board-platform,QCOM),true)
ifeq ($(strip $(AUDIO_FEATURE_ENABLED_EXTN_RESAMPLER)),true)
ifdef TARGET_2ND_ARCH
LOCAL_SRC_FILES_$(TARGET_2ND_ARCH) += AudioResamplerQTI.cpp.arm
LOCAL_C_INCLUDES_$(TARGET_2ND_ARCH) += $(TARGET_OUT_HEADERS)/mm-audio/audio-src
LOCAL_SHARED_LIBRARIES_$(TARGET_2ND_ARCH) += libqct_resampler
LOCAL_CFLAGS_$(TARGET_2ND_ARCH) += -DQTI_RESAMPLER
else
LOCAL_SRC_FILES += AudioResamplerQTI.cpp.arm
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-src
LOCAL_SHARED_LIBRARIES += libqct_resampler
LOCAL_CFLAGS += -DQTI_RESAMPLER
endif
endif
endif
#QTI Resampler

LOCAL_MODULE := libaudioresampler

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
