################################################################################
#
# stb-streamer - fMP4 spool and HTTP server (dtv_android protocol)
#
################################################################################

STB_STREAMER_VERSION = local
STB_STREAMER_SITE = $(BR2_EXTERNAL_X96MAX_PATH)/package/stb-streamer/src
STB_STREAMER_SITE_METHOD = local
STB_STREAMER_LICENSE = GPL-2.0
STB_STREAMER_DEPENDENCIES =

STB_STREAMER_SRCS = stb_linux_pipe.c feature_tap.c ram_spool.c stb_http.c mdns_announce.c

define STB_STREAMER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 \
		-D_FILE_OFFSET_BITS=64 -I$(@D) \
		-o $(@D)/stb_streamer \
		$(addprefix $(@D)/,$(STB_STREAMER_SRCS)) -pthread
endef

define STB_STREAMER_BUILD_EIT_WATCH
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 \
		-D_FILE_OFFSET_BITS=64 -I$(@D) \
		-o $(@D)/eit_watch $(@D)/eit_watch.c $(@D)/arib_text.c
endef
STB_STREAMER_POST_BUILD_HOOKS += STB_STREAMER_BUILD_EIT_WATCH

define STB_STREAMER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/stb_streamer $(TARGET_DIR)/usr/bin/stb_streamer
	$(INSTALL) -D -m 0755 $(@D)/eit_watch $(TARGET_DIR)/usr/bin/eit_watch
endef

$(eval $(generic-package))
