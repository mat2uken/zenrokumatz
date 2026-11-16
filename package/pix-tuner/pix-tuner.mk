PIX_TUNER_VERSION = a866c61067f03557701914a2e48a3a11bfbb706e
PIX_TUNER_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/vendor/pix-userland
PIX_TUNER_SITE_METHOD = local
PIX_TUNER_DEPENDENCIES = libusb
PIX_TUNER_LICENSE = GPL-2.0+
PIX_TUNER_LICENSE_FILES = LICENSE THIRD_PARTY_NOTICES.md

define PIX_TUNER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) pix-ts-legacy \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -DDT295_CAPTURE_TESTING" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		USB_CFLAGS="-I$(STAGING_DIR)/usr/include/libusb-1.0" \
		USB_LIBS="-L$(STAGING_DIR)/usr/lib -lusb-1.0"
endef

define PIX_TUNER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pix-ts-legacy $(TARGET_DIR)/usr/bin/pix-ts-legacy
	$(INSTALL) -D -m 0644 $(@D)/firmware/dt295.bin $(TARGET_DIR)/usr/share/pix-tuner/dt295.assets
endef

$(eval $(generic-package))
