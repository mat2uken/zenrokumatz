################################################################################
#
# meson H.264 hardware encoder (amvenc_avc)
#
################################################################################

MESON_VENC_VERSION = eb7a44464af1c7793f793698e309abe4e6ad82da
MESON_VENC_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/vendor/meson-sm1-codecs
MESON_VENC_SITE_METHOD = local
MESON_VENC_LICENSE = GPL-2.0
MESON_VENC_LICENSE_FILES = LICENSE
MESON_VENC_MODULE_SUBDIRS = venc-h264

# The encoder locates its DOS window through the decoder node, and the vendored
# code only knows the G12A compatible. This board describes the decoder as
# "amlogic,sm1-vdec" so mainline binds vdec_platform_sm1, leaving the encoder
# with nothing to find. SITE_METHOD=local skips the usual patch step, so apply
# the changes here before the module is built.
MESON_VENC_PATCHES = $(BR2_EXTERNAL_X96MAX_PATH)/package/meson-venc/patches

define MESON_VENC_APPLY_LOCAL_PATCHES
	for p in $(MESON_VENC_PATCHES)/*.patch; do \
		echo "Applying $$(basename $$p)" ; \
		patch -d $(@D) -p1 --forward --batch < $$p || exit 1 ; \
	done
endef
MESON_VENC_POST_RSYNC_HOOKS += MESON_VENC_APPLY_LOCAL_PATCHES

$(eval $(kernel-module))
$(eval $(generic-package))
