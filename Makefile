TARGET := iphone:clang:10.3:6.0
ARCHS = armv7

include $(THEOS)/makefiles/common.mk

SUBPROJECTS = daemon app
include $(THEOS_MAKE_PATH)/aggregate.mk

before-all::
	@test -d btstack || ./get_btstack.sh
