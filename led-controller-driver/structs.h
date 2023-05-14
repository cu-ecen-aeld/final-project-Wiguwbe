/*
	Structs for device driver
*/
#ifndef _LED_CONTROLLER_DRIVER_H_
#define _LED_CONTROLLER_DRIVER_H_

#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/rwsem.h>
#include <linux/gpio/consumer.h>

#include <linux/cdev.h>
#include <linux/mutex.h>

/*
	Leds/Kobjects
*/

#define LEDS_MAX 32

struct led {
	struct kobject kobj;
	s16 pin_number;
	struct gpio_desc *gpio;
	struct rw_semaphore pin_number_sem;
};

/* for params */
struct leds {
	/* the number of leds*/
	int led_count;
	/* we limit the amount of leds */
	struct led *leds[LEDS_MAX];
	struct rw_semaphore rw_semaphore;
	/* sysfs entry/folder for 'leds' */
	struct kobject kobj;
};

/*
	LinkedList/CharDev
*/

/* single linked-list */
struct ll_node {
	struct ll_node *next;
	/* the led values */
	unsigned char *led_values;
	/* how long to hold this state */
	unsigned time;
	/*
		the size of the string representation,
		including the final newline,
		excluding NULL byte
	*/
	unsigned repr_size;
};

struct lc_states_dev {
	struct cdev cdev;

	/*
		the states:
		- head -> the list
		- tail -> for fast appending
		- cur  -> tracker of current state
	*/
	struct ll_node *head, *tail, *cur;
	//unsigned states_len;
	/* semaphore to access it */
	struct rw_semaphore semaphore;

	/* buffer for partial writes */
	char *partial;
	int partial_len;
	struct mutex partial_mx;

	/* and the leds */
	struct leds *leds;
};

#endif
