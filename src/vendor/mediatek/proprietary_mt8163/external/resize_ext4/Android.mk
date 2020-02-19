LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

CORE_SRC_FILES := \
	main.c

LOCAL_SRC_FILES := \
	$(CORE_SRC_FILES)

LOCAL_C_INCLUDES := $(TOP)/external/e2fsprogs/lib/

LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT_SBIN)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_SBIN_UNSTRIPPED)

LOCAL_STATIC_LIBRARIES  := \
	liblogwrap \
	libcutils \
	liblog \
	libc \
	libext2fs \
	libext2_com_err \
	libext2_e2p

LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_MODULE := resize_ext4
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)
