TARGET := iphone:clang:10.3:6.0
ARCHS = armv7

include $(THEOS)/makefiles/common.mk

BTSTACK_ROOT ?= ../btstack
include sources.mk

TOOL_NAME = a2dpsinkd
a2dpsinkd_FILES = $(DAEMON_SOURCES) $(addprefix $(BTSTACK_ROOT)/,$(BTSTACK_SOURCES))
a2dpsinkd_CFLAGS = -I. -include ios9_compat.h $(addprefix -I$(BTSTACK_ROOT)/,$(BTSTACK_INCLUDES)) \
	-Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-deprecated-declarations -Wno-error
# iOS 9.3 stubs for armv7-only helpers the 10.3 SDK stubs lack:
#  libcompiler_rt: integer division (armv7 has no hardware divide)
#  libunwind:      setjmp/longjmp exception helpers used by @try/@catch on armv7
a2dpsinkd_LDFLAGS = ../stubs/libcompiler_rt.tbd ../stubs/libunwind.tbd
# MediaPlayer / MediaRemote are loaded at runtime (dlopen), see media_controls.m
a2dpsinkd_FRAMEWORKS = CoreFoundation Foundation AudioToolbox IOKit
a2dpsinkd_INSTALL_PATH = /usr/libexec

include $(THEOS_MAKE_PATH)/tool.mk
