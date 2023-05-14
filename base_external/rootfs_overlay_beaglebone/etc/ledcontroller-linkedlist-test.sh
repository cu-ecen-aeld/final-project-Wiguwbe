#!/bin/sh
#
# test linked-list implementation

set -ex

dev=/dev/ledc
moddir=/sys/module/ledcontroller

# load module with 0 leds (default)
/etc/ledcontroller-load.sh

# it actually allows to write when there are no states (lol?)
echo 10 > $dev
echo 20 >> $dev

test $(cat $dev | wc -l) -eq 2

# clear
> $dev

# empty
test $(cat $dev | wc -l) -eq 0

# anyway, lets try with number of leds
/etc/ledcontroller-unload.sh
/etc/ledcontroller-load.sh led_count=2

# test single line
echo 1,0,10 > $dev
# append
echo 0,1,5 >> $dev

test $(cat $dev | wc -l) -eq 2

# test truncate
echo 1,1,8 > $dev
# and append again
echo 1,0,2 >> $dev
# (and again)
echo 1,1,5 >> $dev
test $(cat $dev | wc -l) -eq 3

# test wrong inputs
# not CSV
! echo "hello" > $dev
# not integers
! echo "a,b,c" > $dev
# too many/little
! echo "1,2" > $dev
! echo "1,1,0,4" > $dev

# test multiline input
test_data_size=$(cat /etc/test_data/linklist_0 | wc -l)
cat /etc/test_data/linklist_0 > $dev
test $(cat $dev | wc -l) -eq $test_data_size
cat /etc/test_data/linklist_0 >> $dev
test $(cat $dev | wc -l) -eq $(echo $test_data_size + $test_data_size | bc -q)

# test changing led parameters when LL is set
! echo 1 > $moddir/parameters/led_count

# test changing pin number
! echo 20 > $moddir/leds/0/pin

# clear LL and try again
> $dev

echo 1 > $moddir/parameters/led_count
echo 20 > $moddir/leds/0/pin
# reset, just in case
echo '-1' > $moddir/leds/0/pin

# and cleanup

/etc/ledcontroller-unload.sh

echo "All tests passed!"
