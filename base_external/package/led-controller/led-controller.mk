
LED_CONTROLLER_VERSION = '0.1'

LED_CONTROLLER_SITE = $(TOPDIR)/../led-controller-driver
LED_CONTROLLER_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
