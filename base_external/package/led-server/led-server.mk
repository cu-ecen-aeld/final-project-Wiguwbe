
LED_SERVER_VERSION = '0.1'

LED_SERVER_SITE = $(TOPDIR)/../led-server
LED_SERVER_SITE_METHOD = local

define LED_SERVER_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) clean all
endef

define LED_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 $(@D)/ledserver $(TARGET_DIR)/usr/bin
endef

$(eval $(generic-package))
