TARGET := iphone:clang:10.3:6.0
ARCHS = armv7

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = A2DPSink
A2DPSink_FILES = main.m AppDelegate.m RootViewController.m
A2DPSink_FRAMEWORKS = UIKit Foundation
A2DPSink_CFLAGS = -fobjc-arc
# iOS 9.3 stubs: the 10.3 SDK ones lack armv7-only symbols (objc_msgSend_stret, __divsi3...)
A2DPSink_LDFLAGS = ../stubs/libobjc.A.tbd ../stubs/libcompiler_rt.tbd

include $(THEOS_MAKE_PATH)/application.mk
