TARGET := iphone:clang:10.3:6.0
ARCHS = armv7

include $(THEOS)/makefiles/common.mk

BTSTACK_ROOT ?= ../btstack
include sources.mk

TOOL_NAME = a2dpsinkd
a2dpsinkd_FILES = $(DAEMON_SOURCES) $(addprefix $(BTSTACK_ROOT)/,$(BTSTACK_SOURCES))
a2dpsinkd_CFLAGS = -I. -include ios9_compat.h $(addprefix -I$(BTSTACK_ROOT)/,$(BTSTACK_INCLUDES)) \
	-Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-deprecated-declarations -Wno-error
# armv7 has no hardware divide: link the iOS 9.3 compiler_rt stub (the 10.3 SDK one lacks the armv7 helpers)
a2dpsinkd_LDFLAGS = ../stubs/libcompiler_rt.tbd
a2dpsinkd_FRAMEWORKS = CoreFoundation Foundation AudioToolbox MediaPlayer IOKit
a2dpsinkd_PRIVATE_FRAMEWORKS = MediaRemote
a2dpsinkd_INSTALL_PATH = /usr/libexec

include $(THEOS_MAKE_PATH)/tool.mk
