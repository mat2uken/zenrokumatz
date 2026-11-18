PIX_TUNER_VERSION = a866c61067f03557701914a2e48a3a11bfbb706e
PIX_TUNER_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/vendor/pix-userland
PIX_TUNER_SITE_METHOD = local
PIX_TUNER_DEPENDENCIES = libusb
PIX_TUNER_LICENSE = GPL-2.0+
PIX_TUNER_LICENSE_FILES = LICENSE THIRD_PARTY_NOTICES.md

# vendor/pix-userland は上流の固定コミットのままにする。こちらの変更は
# board/x96max/patches/pix-userland/ のパッチだけで持つ。
define PIX_TUNER_APPLY_LOCAL_PATCHES
	$(BR2_EXTERNAL_X96MAX_PATH)/scripts/apply-vendor-patches.sh $(@D)
endef
PIX_TUNER_PRE_BUILD_HOOKS += PIX_TUNER_APPLY_LOCAL_PATCHES

# 局ロゴの抽出は TS を読むだけなので libusb に依存しない。サービス名の
# デコードだけ stb_streamer と同じ実装 (arib_text.c) を使う。
PIX_TUNER_LOGO_DIR = $(BR2_EXTERNAL_X96MAX_PATH)/package/pix-tuner/logo
PIX_TUNER_ARIB_DIR = $(BR2_EXTERNAL_X96MAX_PATH)/package/stb-streamer/src

define PIX_TUNER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) pix-ts-legacy \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -DDT295_CAPTURE_TESTING" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		USB_CFLAGS="-I$(STAGING_DIR)/usr/include/libusb-1.0" \
		USB_LIBS="-L$(STAGING_DIR)/usr/lib -lusb-1.0"
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L \
		-I$(PIX_TUNER_LOGO_DIR) -I$(PIX_TUNER_ARIB_DIR) \
		-o $(@D)/pix-dt295-logo \
		$(PIX_TUNER_LOGO_DIR)/pix_dt295_logo.c \
		$(PIX_TUNER_LOGO_DIR)/logo_carousel.c \
		$(PIX_TUNER_LOGO_DIR)/logo_cdt.c \
		$(PIX_TUNER_LOGO_DIR)/logo_store.c \
		$(PIX_TUNER_LOGO_DIR)/ts_section.c \
		$(PIX_TUNER_ARIB_DIR)/arib_text.c
endef

define PIX_TUNER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pix-ts-legacy $(TARGET_DIR)/usr/bin/pix-ts-legacy
	$(INSTALL) -D -m 0755 $(@D)/pix-dt295-logo $(TARGET_DIR)/usr/bin/pix-dt295-logo
	$(INSTALL) -D -m 0644 $(@D)/firmware/dt295.bin $(TARGET_DIR)/usr/share/pix-tuner/dt295.assets
endef

$(eval $(generic-package))
