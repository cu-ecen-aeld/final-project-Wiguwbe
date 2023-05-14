#!/bin/sh

module=ledcontroller
device=ledc
set -e

rmmod $module

rm -f /dev/${device}
