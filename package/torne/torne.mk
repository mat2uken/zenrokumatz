TORNE_VERSION = 861d550
TORNE_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/vendor/torne
TORNE_SITE_METHOD = local
TORNE_DEPENDENCIES = libusb
TORNE_LICENSE = GPL-2.0, Apache-2.0
TORNE_LICENSE_FILES = third_party/torne/LICENSE third_party/arib25/LICENSE

define TORNE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		USB_CFLAGS="-I$(STAGING_DIR)/usr/include/libusb-1.0" \
		USB_LIBS="-L$(STAGING_DIR)/usr/lib -lusb-1.0" \
		sh $(BR2_EXTERNAL_X96MAX_PATH)/package/torne/build.sh $(@D)
endef

define TORNE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/out/torne-c $(TARGET_DIR)/usr/bin/torne-c
	$(INSTALL) -D -m 0755 $(@D)/out/torne-b25 $(TARGET_DIR)/usr/bin/torne-b25
endef

$(eval $(generic-package))
