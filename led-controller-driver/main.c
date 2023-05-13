#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/rwsem.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>

MODULE_AUTHOR("Tiago Teixeira");
MODULE_LICENSE("Dual BSD/GPL");

/*
	Module Params/Kobjects
*/

/* if debugging */
//#define LED_INITIAL_STATE 1
/* for production */
#define LED_INITIAL_STATE 0


static int parent_kobj_init(void);

#define LEDS_MAX 32


struct led {
	struct kobject kobj;
	s16 pin_number;
	struct gpio_desc *gpio;
	struct rw_semaphore pin_number_sem;
};

static void led_kobj_release(struct kobject *kobj)
{
	struct led *led = container_of(kobj, struct led, kobj);
	printk(KERN_DEBUG "ledcontroller: releasing led kobj %hd\n", led->pin_number);
	if(led->gpio)
		gpio_free(led->pin_number);
	kfree(led);
}

struct attribute led_pin_number_attr = {
	.name = "pin",
	.mode = 0644
	/* the rest is default? */
};
static struct attribute *led_pin_number_attrs[] = {
	&led_pin_number_attr,
	NULL
};
ATTRIBUTE_GROUPS(led_pin_number);

static ssize_t show_attr(struct kobject *kobj, struct attribute *attr, char *buffer)
{
	/* TODO ensure attr->name == "pin" (only 1 attribute, so far) */
	ssize_t ret;
	struct led *led = container_of(kobj, struct led, kobj);
	printk(KERN_DEBUG "ledcontroller: show attr 'led'\n");
	down_read(&led->pin_number_sem);
	ret = sprintf(buffer, "%hd\n", led->pin_number);
	up_read(&led->pin_number_sem);
	return ret;
}

static ssize_t store_attr(struct kobject *kobj, struct attribute *attr, const char *buffer, size_t count)
{
	s16 pin_number;
	ssize_t ret;
	struct gpio_desc *prev;
	struct led *led = container_of(kobj, struct led, kobj);
	/* TODO ensure attr->name == "pin" (only 1 attribute, so far) */
	/* TODO ensure states linked-list is empty */
	printk(KERN_DEBUG "ledcontroller: store attr 'led'\n");
	if((ret = kstrtos16(buffer, 10, &pin_number)) != 0)
		return ret;
	if(pin_number < 0)
		pin_number = -1;
	if(pin_number == led->pin_number)
	{
		// nothing to do
		return count;
	}
	// else, store it
	down_write(&led->pin_number_sem);
	prev = led->gpio;
	// get new GPIO desc
	if(pin_number >= 0)
	{
		// request legacy
		if((ret = gpio_request((unsigned)pin_number, NULL)))
		{
			up_write(&led->pin_number_sem);
			return ret;
		}
		// legacy -> new
		led->gpio = gpio_to_desc((unsigned)pin_number);
		if(!led->gpio)
		{
			// reset
			led->gpio = prev;
			up_write(&led->pin_number_sem);
			gpio_free((unsigned)pin_number);
			return -EINVAL;
		}
		// configure
		if((ret = gpiod_direction_output(led->gpio, LED_INITIAL_STATE)))
		{
			led->gpio = prev;
			up_write(&led->pin_number_sem);
			gpio_free((unsigned)pin_number);
			return ret;
		}
	}
	else
	{
		led->gpio = NULL;
	}
	// release previous desc
	if(prev)
	{
		// reset to 0, just in case
		gpiod_set_value(prev, 0);
		gpio_free((unsigned)led->pin_number);
		prev = NULL;
	}
	// finally, update the pin number on the structure
	led->pin_number = pin_number;
	up_write(&led->pin_number_sem);
	return count;
}

static const struct sysfs_ops led_attr_ops = {
	.show = show_attr,
	.store = store_attr
};

static struct kobj_type led_kobj_type = {
	.release = led_kobj_release,
	.sysfs_ops = &led_attr_ops,
	.default_groups = led_pin_number_groups
};

struct leds {
	/* the number of LEDs */
	int led_count;
	/* since we don't have a lot of LEDs, we can have an array of pointers */
	struct led *leds[LEDS_MAX];
	struct rw_semaphore rw_semaphore;	/* semaphore for `leds` array */
	/* sysfs entry/folder for 'leds' */
	struct kobject kobj;
};

static void leds_kobj_release(struct kobject *kobj)
{
	// nothing done here, it's statically allocated
	printk(KERN_DEBUG "ledcontroller: releasing main kobject\n");
}

static struct kobj_type leds_kobj_type = {
	.release = leds_kobj_release
};

static int _update_led_count(struct leds *, int);

static int num_leds_set(const char *val, const struct kernel_param *kp)
{
	// TODO (future) check if states linked-list has values, if !empty, return some error
	struct leds *leds = (struct leds*)kp->arg;
	int count_new;
	if(kstrtoint(val, 10, &count_new) || count_new < 0 || count_new > LEDS_MAX) {
		return -EINVAL;
	}

	printk(KERN_DEBUG "ledcontroller: setting led_count to %d\n", count_new);

	if(leds->led_count < 0)
	{
		// got value from CLI
		leds->led_count = count_new;
		// module initialization shall do the rest
	}
	else
	{
		_update_led_count(leds, count_new);
	}
	return 0;
}

static int _update_led_count(struct leds *leds, int count_new)
{
	int count_current = leds->led_count, count_diff;

	// free/create kobjects as necessary
	count_diff = count_new - count_current;

	down_write(&leds->rw_semaphore);
	if(count_diff > 0)
	{
		// add new objects
		int i, ret;
		printk(KERN_DEBUG "ledcontroller: creating %d new leds\n", count_diff);
		for(i=0; i<count_diff; i++)
		{
			int pin_number = i+count_current;
			struct led *led;
			/* set name */
			char *pin_number_str = kmalloc(4, GFP_KERNEL);
			if(!pin_number_str)
			{
				up_write(&leds->rw_semaphore);
				return -ENOMEM;
			}
			sprintf(pin_number_str, "%d", pin_number);
			/* init structure */
			led = kmalloc(sizeof(struct led), GFP_KERNEL);
			if(!led)
			{
				kfree(pin_number_str);
				up_write(&leds->rw_semaphore);
				return -ENOMEM;
			}
			memset(led, 0, sizeof(struct led));
			init_rwsem(&led->pin_number_sem);
			led->pin_number = -1;
			/* init kobject */
			kobject_init(&led->kobj, &led_kobj_type);
			if((ret=kobject_set_name(&led->kobj, pin_number_str))!=0)
			{
				kfree(pin_number_str);
				kobject_put(&led->kobj);
				up_write(&leds->rw_semaphore);
				return ret;
			}
			/* add to parent */
			if((ret = kobject_add(&led->kobj, &leds->kobj, NULL)))
			{
				kobject_put(&led->kobj);
				up_write(&leds->rw_semaphore);
				return ret;
			}
			// notify userspace
			if((ret = kobject_uevent(&led->kobj, KOBJ_ADD)))
			{
				kobject_del(&led->kobj);
				kobject_put(&led->kobj);
				up_write(&leds->rw_semaphore);
				return ret;
			}
			leds->leds[i] = led;
			/* do each iteration as it can fail anytime */
			leds->led_count++;
		}
	}
	else if(count_diff < 0)
	{
		// remove objects
		int i;
		printk(KERN_DEBUG "ledcontroller: deleting %d leds\n", -count_diff);
		for(i=count_new;i<count_current;i++)
		{
			struct kobject *kobj = &leds->leds[i]->kobj;
			kobject_del(kobj);
			kobject_put(kobj);
			leds->leds[i] = NULL;
			// TODO uevent?
		}
		leds->led_count = count_new;
	}
	up_write(&leds->rw_semaphore);
	// else, count_diff == 0, no changes

	return 0;
}

static int num_leds_get(char *buffer, const struct kernel_param *kp)
{
	struct leds *leds = (struct leds*)kp->arg;
	// only up to 4 bytes
	sprintf(buffer, "%d\n", leds->led_count);
	return strlen(buffer);
}

struct leds leds = { -1 };
static const struct kernel_param_ops leds_ops = {
	/* .flags = 0, */
	.set = num_leds_set,
	.get = num_leds_get
};
module_param_cb(led_count, &leds_ops, &leds, 0644);


/*
	Init/Exit functions
*/

static int parent_kobj_init(void)
{
	int ret;
	kobject_init(&leds.kobj, &leds_kobj_type);
	init_rwsem(&leds.rw_semaphore);

	if((ret = kobject_set_name(&leds.kobj, "leds")))
		goto _fail_1;
	if((ret = kobject_add(&leds.kobj, &THIS_MODULE->mkobj.kobj, NULL)))
		goto _fail_1;
	if((ret = kobject_uevent(&leds.kobj, KOBJ_ADD)))
		goto _fail_2;
	return 0;
_fail_2:
	kobject_del(&leds.kobj);
_fail_1:
	kobject_put(&leds.kobj);
	return ret;
}

int lc_init_module(void)
{
	int ret, i;
	printk(KERN_DEBUG "hello from led-controller module!\n");

	if((ret = parent_kobj_init()))
		goto _fail_0;

	if(leds.led_count < 0)
	{
		leds.led_count = 0;
	}
	else
	{
		/* already have value from parameters, init kobjects */
		int count_new = leds.led_count;
		leds.led_count = 0;	// reset
		if((ret = _update_led_count(&leds, count_new)))
			goto _fail_1;
	}

	return 0;

_fail_1:
	/* delete already created LEDs */
	for(i=0;i<leds.led_count;i++)
	{
		kobject_del(&leds.leds[i]->kobj);
		kobject_put(&leds.leds[i]->kobj);
	}
	/* and the parent kobject */
	kobject_del(&leds.kobj);
	kobject_put(&leds.kobj);
_fail_0:
	return ret;
}

void lc_exit_module(void)
{
	int i;
	struct kobject *kobj;
	for(i=0;i<leds.led_count;i++)
	{
		kobj = &leds.leds[i]->kobj;
		kobject_del(kobj);
		kobject_put(kobj);
	}
	kobj = &leds.kobj;
	kobject_del(kobj);
	kobject_put(kobj);

	printk(KERN_DEBUG "goodbye ...\n");
}

module_init(lc_init_module);
module_exit(lc_exit_module);
