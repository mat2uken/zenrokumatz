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

$(eval $(kernel-module))
$(eval $(generic-package))
