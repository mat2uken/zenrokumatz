################################################################################
#
# cardrelay - local card socket to remote bcasd bridge
#
################################################################################

CARDRELAY_VERSION = local
CARDRELAY_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/package/cardrelay/src
CARDRELAY_SITE_METHOD = local
CARDRELAY_LICENSE = GPL-2.0

define CARDRELAY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 \
		-D_FILE_OFFSET_BITS=64 -o $(@D)/cardrelay $(@D)/cardrelay.c
endef

define CARDRELAY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cardrelay $(TARGET_DIR)/usr/bin/cardrelay
endef

$(eval $(generic-package))
