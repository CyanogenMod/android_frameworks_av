LOCAL_PATH := $(call my-dir)

###########################
# convert static lib target

include $(CLEAR_VARS)

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/

LOCAL_MODULE := libutilities_convert

include $(BUILD_STATIC_LIBRARY)

#########################
# convert static lib host

include $(CLEAR_VARS)

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/

LOCAL_MODULE := libutilities_convert_host

LOCAL_CFLAGS = -O0 --coverage

LOCAL_LDFLAGS = --coverage

LOCAL_MODULE_TAGS := tests

include $(BUILD_HOST_STATIC_LIBRARY)

