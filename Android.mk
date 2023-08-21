# Copyright 2006 The Android Open Source Project

# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)
$(shell touch $(LOCAL_PATH)/*)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    reference-ril.c \
    atchannel.c \
    pppd.c \
    misc.c \
    at_tok.c \
    usim-fcp.c \
    usb_find.c \
    diagsaver.c

LOCAL_SHARED_LIBRARIES := \
    libcutils libutils libril libnetutils

# for asprinf
LOCAL_CFLAGS += -D_GNU_SOURCE -Wno-return-stack-address
LOCAL_CFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-unused-label
LOCAL_CFLAGS += -Wno-incompatible-pointer-types -Wno-format -Wno-unused-function -Wno-return-stack-address
LOCAL_LDLIBS += -llog
#LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

ifeq ($(TARGET_DEVICE),sooner)
  LOCAL_CFLAGS += -DOMAP_CSMI_POWER_CONTROL -DUSE_TI_COMMANDS
endif

ifeq ($(TARGET_DEVICE),surf)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

ifeq ($(TARGET_DEVICE),dream)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

LOCAL_CFLAGS += -Wno-unused-parameter
ifeq (foo,foo)
  #build shared library
  LOCAL_CFLAGS += -DRIL_SHLIB
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES
  LOCAL_MODULE:= libreference-ril
  include $(BUILD_SHARED_LIBRARY)
else
  #build executable
  LOCAL_MODULE_CLASS := EXECUTABLES
  LOCAL_MODULE:= reference-ril
  include $(BUILD_EXECUTABLE)
endif



include $(CLEAR_VARS)
LOCAL_SRC_FILES:= chat.c
LOCAL_PROPRIETARY_MODULE := true
LOCAL_CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-unused-const-variable
LOCAL_CFLAGS += -pie -fPIE
LOCAL_LDFLAGS += -pie -fPIE
#ifeq ($(USE_NDK),1)
LOCAL_CFLAGS += -DUSE_NDK
LOCAL_LDLIBS += -llog
#else
LOCAL_SHARED_LIBRARIES := libcutils libutils
#endif
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE_PATH:= $(TARGET_OUT_ETC)/ppp
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE:= chat
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_SRC_FILES:= ip-up.c
ifeq ($(USE_NDK),1)
LOCAL_CFLAGS += -DUSE_NDK
LOCAL_LDLIBS += -llog
else
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SHARED_LIBRARIES += libcutils libutils
endif
LOCAL_CFLAGS += -pie -fPIE
LOCAL_CFLAGS += -Wno-self-assign
LOCAL_LDFLAGS += -pie -fPIE
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_PATH:= $(TARGET_OUT_ETC)/ppp
LOCAL_MODULE:= ip-up
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_SRC_FILES:= ip-down.c
ifeq ($(USE_NDK),1)
LOCAL_CFLAGS += -DUSE_NDK
LOCAL_LDLIBS += -llog
else
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SHARED_LIBRARIES += libcutils libutils
endif
LOCAL_CFLAGS += -pie -fPIE
LOCAL_CFLAGS += -Wno-self-assign
LOCAL_LDFLAGS += -pie -fPIE
LOCAL_MODULE_TAGS:= optional
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_PATH:= $(TARGET_OUT_ETC)/ppp
LOCAL_MODULE:= ip-down
include $(BUILD_EXECUTABLE)
