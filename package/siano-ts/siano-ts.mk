################################################################################
#
# siano-ts - user-space ISDB-T driver for PX-S1UD
#
################################################################################

SIANO_TS_VERSION = 505a2f7267ae064ee40c6f3fea6e3ab0e88b70f3
SIANO_TS_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/vendor/siano-userland
SIANO_TS_SITE_METHOD = local
SIANO_TS_LICENSE = GPL-2.0-or-later
SIANO_TS_LICENSE_FILES = COPYING
SIANO_TS_DEPENDENCIES = libusb
# The snapshot is pinned to commit 505a2f7 in sources.lock.json and carries
# one local change: TS_QUEUE_SLOTS in siano-device.h is 1024 instead of 256
# so the tuner survives the few seconds the downstream transcode needs to
# start. SITE_METHOD=local skips package patches, so the change is kept in
# the snapshot itself.

SIANO_TS_CFLAGS = $(TARGET_CFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64

define SIANO_TS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(SIANO_TS_CFLAGS) \
		-I$(STAGING_DIR)/usr/include/libusb-1.0 \
		-o $(@D)/siano-ts \
		$(@D)/siano-ts.c $(@D)/siano-device.c $(@D)/protocol.c \
		$(@D)/stream-state.c $(@D)/control-parse.c $(@D)/control-input.c \
		$(@D)/device-selector.c $(@D)/exit-codes.c $(@D)/queue-policy.c \
		$(@D)/output-writer.c \
		-L$(STAGING_DIR)/usr/lib -lusb-1.0 -pthread
endef

define SIANO_TS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/siano-ts $(TARGET_DIR)/usr/bin/siano-ts
endef

$(eval $(generic-package))
