################################################################################################
#
# @NOTE:
# Audio Policy Engine configurable example for generic device build
#
# Any vendor shall have its own configuration within the corresponding device folder
#
################################################################################################


LOCAL_PATH := $(call my-dir)

PFW_CORE := external/parameter-framework
BUILD_PFW_SETTINGS := $(PFW_CORE)/support/android/build_pfw_settings.mk
PFW_DEFAULT_SCHEMAS_DIR := $(PFW_CORE)/Schemas
PFW_SCHEMAS_DIR := $(PFW_DEFAULT_SCHEMAS_DIR)

##################################################################
# CONFIGURATION FILES
##################################################################
######### Policy PFW top level file #########

include $(CLEAR_VARS)
LOCAL_MODULE := ParameterFrameworkConfigurationPolicy.xml
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/parameter-framework
LOCAL_SRC_FILES := $(LOCAL_MODULE)
include $(BUILD_PREBUILT)


########## Policy PFW Structures #########

include $(CLEAR_VARS)
LOCAL_MODULE := PolicyClass.xml
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/parameter-framework/Structure/Policy
LOCAL_SRC_FILES := Structure/$(LOCAL_MODULE)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := PolicySubsystem.xml
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_REQUIRED_MODULES := \
    PolicySubsystem-CommonTypes.xml \
    PolicySubsystem-Volume.xml \
    libpolicy-subsystem \

LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/parameter-framework/Structure/Policy
LOCAL_SRC_FILES := Structure/$(LOCAL_MODULE)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := PolicySubsystem-CommonTypes.xml
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/parameter-framework/Structure/Policy
LOCAL_SRC_FILES := Structure/$(LOCAL_MODULE)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := PolicySubsystem-Volume.xml
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/parameter-framework/Structure/Policy
LOCAL_SRC_FILES := Structure/$(LOCAL_MODULE)
include $(BUILD_PREBUILT)

######### Policy PFW Settings #########
include $(CLEAR_VARS)
LOCAL_MODULE := parameter-framework.policy
LOCAL_MODULE_STEM := PolicyConfigurableDomains.xml
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_RELATIVE_PATH := parameter-framework/Settings/Policy
LOCAL_ADDITIONAL_DEPENDENCIES := \
        PolicyClass.xml \
        PolicySubsystem.xml \
        ParameterFrameworkConfigurationPolicy.xml

ifeq ($(pfw_rebuild_settings),true)
PFW_TOPLEVEL_FILE := $(TARGET_OUT_ETC)/parameter-framework/ParameterFrameworkConfigurationPolicy.xml
PFW_CRITERIA_FILE := $(LOCAL_PATH)/policy_criteria.txt
PFW_EDD_FILES := \
        $(LOCAL_PATH)/Settings/device_for_strategy_media.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_phone.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_sonification.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_sonification_respectful.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_dtmf.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_enforced_audible.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_transmitted_through_speaker.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_accessibility.pfw \
        $(LOCAL_PATH)/Settings/device_for_strategy_rerouting.pfw \
        $(LOCAL_PATH)/Settings/strategy_for_stream.pfw \
        $(LOCAL_PATH)/Settings/strategy_for_usage.pfw \
        $(LOCAL_PATH)/Settings/device_for_input_source.pfw \
        $(LOCAL_PATH)/Settings/volumes.pfw

include $(BUILD_PFW_SETTINGS)
else
# Use the existing file
LOCAL_SRC_FILES := Settings/$(LOCAL_MODULE_STEM)
include $(BUILD_PREBUILT)
endif # pfw_rebuild_settings
