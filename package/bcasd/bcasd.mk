################################################################################
#
# bcasd - multi-client B-CAS APDU server
#
################################################################################

BCASD_VERSION = local
BCASD_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/package/bcasd/src
BCASD_SITE_METHOD = local
BCASD_LICENSE = GPL-2.0
BCASD_DEPENDENCIES = pcsc-lite

define BCASD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 \
		-D_FILE_OFFSET_BITS=64 -I$(STAGING_DIR)/usr/include/PCSC \
		-o $(@D)/bcasd $(@D)/bcasd.c \
		-L$(STAGING_DIR)/usr/lib -lpcsclite -pthread
endef

define BCASD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bcasd $(TARGET_DIR)/usr/bin/bcasd
endef

$(eval $(generic-package))
