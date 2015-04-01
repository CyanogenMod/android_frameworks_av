
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

#######################################################################
# Recursive call sub-folder Android.mk
#
include $(call all-makefiles-under,$(LOCAL_PATH))

