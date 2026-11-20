################################################################################
#
# cfw - host tools for MB8AC031-CFW (PIX-DT295 replacement firmware)
#
################################################################################

# Upstream: https://github.com/soralis0912/MB8AC031-CFW (MIT).
# The sources under src/ are taken verbatim from that repository at the
# release the firmware container was built from. They are vendored rather
# than fetched because the container and the tools have to agree: frc1_load
# checks the container's loader crc32 and hook fingerprint against the chip,
# and a tool built from a different revision could disagree.
#
# BUILDINFO of the release the container came from:
#   commit   d14dfdd99a16f3c96d901f88055e1f5e7e64ef5f
#   ref      refs/tags/v0.0.1
#   frc1-dt295.frc sha256 (see package/cfw/SHA256SUMS)
CFW_VERSION = v0.0.1

CFW_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/package/cfw/src
CFW_SITE_METHOD = local
CFW_LICENSE = MIT
CFW_LICENSE_FILES = $(BR2_EXTERNAL_X96MAX_PATH)/package/cfw/LICENSE
CFW_DEPENDENCIES = libusb

# chipio.h needs this pair to compile its libusb transport in.
CFW_DEFS = -D_POSIX_C_SOURCE=200809L -DCHIPIO_HAVE_LIBUSB

define CFW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=c11 \
		$(CFW_DEFS) -I$(STAGING_DIR)/usr/include/libusb-1.0 \
		-o $(@D)/frc1_load $(@D)/frc1_load.c $(TARGET_LDFLAGS) -lusb-1.0
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=c11 \
		$(CFW_DEFS) -I$(STAGING_DIR)/usr/include/libusb-1.0 \
		-o $(@D)/ts_grab $(@D)/ts_grab.c $(TARGET_LDFLAGS) -lusb-1.0 -pthread
endef

define CFW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/frc1_load $(TARGET_DIR)/usr/bin/frc1_load
	$(INSTALL) -D -m 0755 $(@D)/ts_grab $(TARGET_DIR)/usr/bin/pix-cfw-grab
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_X96MAX_PATH)/package/cfw/frc1-dt295.frc \
		$(TARGET_DIR)/usr/share/cfw/frc1-dt295.frc
endef
$(eval $(generic-package))
