#!/bin/sh
#
# script to setup pin leds for testing
#

/etc/ledcontroller-load.sh led_count=6

function pin() {
	echo $1 > /sys/module/ledcontroller/leds/$2/pin
}

pin 67 0
pin 68 1
pin 44 2
pin 26 3
pin 46 4
pin 65 5
