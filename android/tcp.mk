LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_ARM_MODE := arm

tcp_LOCAL_SRC_FILES_BASE:= \
	gst/tcp/gsttcpplugin.c  	\
	gst/tcp/gsttcp.c 			\
	gst/tcp/gstmultifdsink.c 	\
	gst/tcp/gsttcpclientsrc.c 	\
	gst/tcp/gsttcpclientsink.c 	\
	gst/tcp/gsttcpserversrc.c 	\
	gst/tcp/gsttcpserversink.c 	
tcp_LOCAL_SRC_FILES_ANDROID:= \
	gst/tcp/gsttcp-enumtypes.c 	\
	gst/tcp/gsttcp-marshal.c

LOCAL_SRC_FILES:= $(addprefix ../,$(tcp_LOCAL_SRC_FILES_BASE)) \
				  $(addprefix ../android/,$(tcp_LOCAL_SRC_FILES_ANDROID))

LOCAL_SHARED_LIBRARIES := \
    libgstreamer-0.11       \
    libgstbase-0.11         \
    libglib-2.0             \
    libgthread-2.0          \
    libgmodule-2.0          \
    libgobject-2.0 			\
	libgstdataprotocol-0.11

LOCAL_MODULE:= libgsttcp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../gst/tcp    			\
    $(LOCAL_PATH)/../gst-libs 				\
    $(LOCAL_PATH)/..         				\
    $(LOCAL_PATH)   	  					\
	$(LOCAL_PATH)/gst/tcp    				\
    $(TARGET_OUT_HEADERS)/gstreamer-0.11    \
	$(TARGET_OUT_HEADERS)/glib-2.0 			\
    $(TARGET_OUT_HEADERS)/glib-2.0/glib     \
	external/libxml2/include 

ifeq ($(STECONF_ANDROID_VERSION),"FROYO")
LOCAL_SHARED_LIBRARIES += libicuuc 
LOCAL_C_INCLUDES += external/icu4c/common
endif

LOCAL_CFLAGS := -DHAVE_CONFIG_H  -DGSTREAMER_BUILT_FOR_ANDROID
#
# define LOCAL_PRELINK_MODULE to false to not use pre-link map
#
LOCAL_PRELINK_MODULE := false

#It's a gstreamer plugins, and it must be installed on ..../lib/gstreamer-0.11
LOCAL_MODULE_PATH := $(TARGET_OUT)/lib/gstreamer-0.11

include $(BUILD_SHARED_LIBRARY)