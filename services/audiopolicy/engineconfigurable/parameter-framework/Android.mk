LOCAL_PATH := $(call my-dir)

#######################################################################
# Recursive call sub-folder Android.mk
#######################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
