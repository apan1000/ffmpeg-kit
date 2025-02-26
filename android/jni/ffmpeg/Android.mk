ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
    MY_BUILD_DIR := $(ARMV7_BUILD_PATH)
endif
ifeq ($(TARGET_ARCH_ABI), arm64-v8a)
    MY_BUILD_DIR := $(ARM64_BUILD_PATH)
endif
ifeq ($(TARGET_ARCH_ABI), x86)
    MY_BUILD_DIR := $(X86_BUILD_PATH)
endif
ifeq ($(TARGET_ARCH_ABI), x86_64)
    MY_BUILD_DIR := $(X86_64_BUILD_PATH)
endif
LOCAL_PATH := $(call my-dir)/../../../prebuilt/$(MY_BUILD_DIR)/ffmpeg/lib

MY_ARM_MODE := arm

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libavcodec
LOCAL_SRC_FILES := libavcodec.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libavfilter
LOCAL_SRC_FILES := libavfilter.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libavdevice
LOCAL_SRC_FILES := libavdevice.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libavformat
LOCAL_SRC_FILES := libavformat.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libavutil
LOCAL_SRC_FILES := libavutil.so
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../include
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libswresample
LOCAL_SRC_FILES := libswresample.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_ARM_MODE := $(MY_ARM_MODE)
LOCAL_MODULE := libswscale
LOCAL_SRC_FILES := libswscale.so
include $(PREBUILT_SHARED_LIBRARY)
