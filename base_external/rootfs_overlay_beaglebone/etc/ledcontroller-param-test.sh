#!/bin/sh
set -ex

moddir=/sys/module/ledcontroller

function test_led_count() {
	led_count=${1:-0}
	test $led_count -eq $(cat $moddir/parameters/led_count)
	test $led_count -eq $(ls -1 $moddir/leds | wc -l)
}


function do_test_a() {

	# initial number should be 0
	test_led_count 0

	# setting parameters to 2
	echo 2 > $moddir/parameters/led_count

	# now it should be 2
	test_led_count 2

	# test invalid arguments (should be 0-32)
	echo -30 > $moddir/parameters/led_count 2>/dev/null || true
	test_led_count 2
	echo 40 > $moddir/parameters/led_count 2>/dev/null || true
	test_led_count 2

	# and check their values
	for led in $moddir/leds/*
	do
		test -1 -eq $(cat $led/pin)
	done

	# set first pin
	echo 15 > $moddir/leds/0/pin
	# set second pin
	echo 3 > $moddir/leds/1/pin

	# read them
	test 15 -eq $(cat $moddir/leds/0/pin)
	test 3 -eq $(cat $moddir/leds/1/pin)

	# set led_count to 1
	echo 1 > $moddir/parameters/led_count
	test_led_count 1
	# should preserve led 0
	test 15 -eq $(cat $moddir/leds/0/pin)

	# reset to 2
	echo 2 > $moddir/parameters/led_count
	# first should be the same
	test 15 -eq $(cat $moddir/leds/0/pin)
	# but second should be -1 again (not kept from previous)
	test -1 -eq $(cat $moddir/leds/1/pin)

}

function do_test_b() {

	# now it should be 2
	test_led_count 2

	# and check their values
	for led in $moddir/leds/*
	do
		test -1 -eq $(cat $led/pin)
	done

	# set first pin
	echo 15 > $moddir/leds/0/pin
	# set second pin
	echo 3 > $moddir/leds/1/pin

	# read them
	test 15 -eq $(cat $moddir/leds/0/pin)
	test 3 -eq $(cat $moddir/leds/1/pin)

	# set led_count to 1
	echo 1 > $moddir/parameters/led_count
	test_led_count 1
	# should preserve led 0
	test 15 -eq $(cat $moddir/leds/0/pin)

	# reset to 2
	echo 2 > $moddir/parameters/led_count
	# first should be the same
	test 15 -eq $(cat $moddir/leds/0/pin)
	# but second should be -1 again (not kept from previous)
	test -1 -eq $(cat $moddir/leds/1/pin)
}

modprobe ledcontroller
# always remove mod
do_test_a || { rmmod ledcontroller; exit 1; }
# all good then
rmmod ledcontroller

# test with initial values
modprobe ledcontroller led_count=2
do_test_b || { rmmod ledcontroller; exit 1; }
rmmod ledcontroller
