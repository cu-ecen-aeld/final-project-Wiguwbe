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
#include <linux/fcntl.h>
// for hrtimer
#include <linux/hrtimer.h>
#include <linux/ktime.h>
// for gpio outputs
#include <linux/wait.h>
#include <linux/kthread.h>

#include "structs.h"

MODULE_AUTHOR("Tiago Teixeira");
MODULE_LICENSE("Dual BSD/GPL");

/*
	Global stuff
*/

int lc_dev_major = 0;
int lc_dev_minor = 0;
#define LC_MINOR_COUNT 1

struct lc_states_dev lc_states_dev;

/*
	Timers and GPIO
*/

struct hrtimer states_hrtimer;

// default/empty
#define TSIGNAL_NOT 0
// continue scheduling, timer finished
#define TSIGNAL_CNT 1
// cancel scheduling, cleanup, wait for new events
#define TSIGNAL_CAN 2
// cleanup and exit thread
#define TSIGNAL_EXT 3
static int timer_signal = 0;

DECLARE_WAIT_QUEUE_HEAD(wq);

static enum hrtimer_restart _states_hrtimer_callback(struct hrtimer *timer)
{
	printk(KERN_DEBUG "ledcontroller: signal in timer callback\n");
	timer_signal = TSIGNAL_CNT;
	wake_up(&wq);
	return HRTIMER_NORESTART;
}

static int _thread_gpio_runner(void *data)
{
	struct lc_states_dev *dev = (struct lc_states_dev*)data;
	int i;
	// 1. infinite loop:
	printk(KERN_DEBUG "ledcontroller-t: running thread\n");
	while(1)
	{
		printk(KERN_DEBUG "ledcontroller-t: got signal: %d\n", timer_signal);
		switch(timer_signal)
		{
			case TSIGNAL_NOT:	// first iteration
			{
				// do nothing
				break;
			}
			case TSIGNAL_CNT:
			{
				down_read(&dev->semaphore);
				// move cursor
				if(!dev->cur||dev->cur==dev->tail)
					// unititialized/end
					dev->cur = dev->head;
				else
					dev->cur = dev->cur->next;
				if(!dev->cur)
				{
					printk(KERN_WARNING "ledcontroller-t: timer IRQ on empty state list\n");
					up_read(&dev->semaphore);
					break;
				}
				// output gpio
				down_read(&dev->leds->rw_semaphore);
				for(i=0;i<dev->leds->led_count;i++)
				{
					if(dev->leds->leds[i]->gpio)
					{
						printk(KERN_DEBUG "ledcontroller-t: led %d value %d\n", i, dev->cur->led_values[i]);
						gpiod_set_value(dev->leds->leds[i]->gpio, dev->cur->led_values[i]);
					}
					else
						printk(KERN_WARNING "ledcontroller-t: GPIO output on unitialized LED\n");
				}
				up_read(&dev->leds->rw_semaphore);
				// setup new timer
				hrtimer_start(&states_hrtimer, ms_to_ktime(dev->cur->time * 1000), HRTIMER_MODE_REL);
				up_read(&dev->semaphore);
				break;
			}
			case TSIGNAL_CAN:
			case TSIGNAL_EXT:
			{
				// clean out GPIO
				down_read(&dev->leds->rw_semaphore);
				for(i=0;i<dev->leds->led_count;i++)
				{
					if(dev->leds->leds[i]->gpio)
						gpiod_set_value(dev->leds->leds[i]->gpio, 0);
					else
						printk(KERN_WARNING "ledcontroller: GPIO output on unitialized LED\n");
				}
				up_read(&dev->leds->rw_semaphore);
				if(timer_signal==TSIGNAL_EXT)
				{
					printk(KERN_DEBUG "ledcontroller-t: exit on signal\n");
					return 0;
				}
				break;
			}
		}
		printk(KERN_DEBUG "ledcontroller-t: waiting for new event\n");
		timer_signal = 0;
		wait_event(wq, timer_signal);
	}
	printk(KERN_WARNING "ledcontroller-t: weird exit from thread function!\n");
	return 0;
}


/*
	Linked List, CharDev I/O
*/

static int lc_states_open(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_cdev;
	if(!(filp->f_flags & O_APPEND) && (filp->f_mode&FMODE_WRITE))
	{
		/* write without append, assume truncate: clean state */
		struct lc_states_dev *dev = (struct lc_states_dev*)inode->i_cdev;
		//struct leds *leds = dev->leds;
		/* stop timers and clear LED outputs */
		printk(KERN_DEBUG "ledcontroller: file open as TRUNC, cancelling timers\n");
		hrtimer_cancel(&states_hrtimer);
		timer_signal = TSIGNAL_CAN;
		wake_up(&wq);
		printk(KERN_DEBUG "ledcontroller: continuing `open`\n");
		/* clear linked-lists */
		down_write(&dev->semaphore);
		/* `cur` is iterator, `head` is next*/
		for(dev->cur = dev->head; dev->cur; dev->cur = dev->head)
		{
			dev->head = dev->cur->next;
			kfree(dev->cur->led_values);
			kfree(dev->cur);
		}
		dev->head = dev->cur = dev->tail = NULL;
		up_write(&dev->semaphore);
	}
	// else, just append

	return 0;
}

static int lc_states_release(struct inode *inode, struct file *filp)
{
	/* nothing to be done here */
	return 0;
}

static ssize_t lc_states_read(struct file *filp, char __user *buf, size_t count, loff_t *fpos)
{
	struct lc_states_dev *dev = (struct lc_states_dev*)filp->private_data;
	size_t total_read = 0,
	       entry_offset = 0,
		   space_left = count;
	size_t read_offset = (size_t)*fpos;
	size_t to_skip = (size_t)*fpos;
	struct ll_node *ptr;
	down_read(&dev->semaphore);
	// 1. skip to target offset
	for(ptr = dev->head; ptr; ptr = ptr->next)
	{
		if(to_skip < ptr->repr_size)
		{
			entry_offset = to_skip;
			break;
		}
		// else
		to_skip -= ptr->repr_size;
	}
	// 2. read while it fits in buffer
	while(space_left > 0 && ptr)
	{
		// 2.1 transform state into str
		// get state buffer
		unsigned buffer_offset = 0, buffer_length = ptr->repr_size+1;
		size_t needed = ptr->repr_size - entry_offset;
		int i;
		char *state_buffer = (char*)kmalloc(buffer_length, GFP_KERNEL);
		if(!state_buffer)
		{
			up_read(&dev->semaphore);
			return -ENOMEM;
		}
		// led values
		for(i=0;i<dev->leds->led_count;i++)
		{
			buffer_offset += snprintf(
				state_buffer+buffer_offset,
				buffer_length-buffer_offset,
				"%hhu,", ptr->led_values[i]
			);
		}
		// and the time
		snprintf(state_buffer+buffer_offset, buffer_length-buffer_offset, "%u\n", ptr->time);
		// 2.2 copy data to user
		size_t to_copy = space_left > needed ? needed : space_left;
		size_t copied = to_copy - copy_to_user(buf + total_read, state_buffer+entry_offset, to_copy);
		kfree(state_buffer); state_buffer = NULL;
		total_read += copied;
		read_offset += copied;
		space_left -= copied;
		if(copied != to_copy)
		{
			/* TODO: consider having this comparing to 0 (zero) instead */
			// failed to copy the full thing
			// let's just return
			break;
		}
		// prepare next loop
		entry_offset += copied;
		if(entry_offset >= ptr->repr_size)
			/* it should never be ` > repr_size` */
			entry_offset = 0;
		ptr = ptr->next;
	}
	up_read(&dev->semaphore);
	// 3. return
	*fpos = (loff_t)read_offset;
	return (ssize_t)total_read;
}

static int _next_line(struct lc_states_dev *dev, char *newline, int part_size)
{
	char *prev = dev->partial;
	int new_size = dev->partial_len - part_size;
	if(new_size > 0)
	{
		dev->partial = kmalloc(new_size*sizeof(char), GFP_KERNEL);
		if(!dev->partial)
		{
			dev->partial = prev;
			up_write(&dev->semaphore);
			mutex_unlock(&dev->partial_mx);
			return -ENOMEM;
		}
		// prev is always !NULL here
		// `newline` still points to somewhere in `prev`
		memcpy(dev->partial, newline+1, new_size);
	}
	else
		dev->partial = NULL;
	dev->partial_len = new_size;
	if(prev)
		kfree(prev);
	return 0;
}

static ssize_t lc_states_write(struct file *filp, const char __user *buf, size_t count, loff_t *fpos)
{
	/* we ignore `fpos`, we only allow append */
	ssize_t retval = -ENOMEM;
	char *newline;
	int all_set, i;
	struct lc_states_dev *dev = (struct lc_states_dev*)filp->private_data;
	/* don't allow writes if all pins are not set */
	down_read(&dev->leds->rw_semaphore);
	all_set = 1;
	for(i=0;i<dev->leds->led_count;i++)
	{
		if(!dev->leds->leds[i]->gpio)
			all_set = 0;
	}
	up_read(&dev->leds->rw_semaphore);
	if(!all_set)
	{
		printk(KERN_WARNING "ledcontroller: attempt to write before all pins are set\n");
		return -ENXIO;
	}


	if(mutex_lock_interruptible(&dev->partial_mx))
		return -EINTR;


	{ /* append to buffer */
		char *prev = dev->partial;
		// recreate with bigger size
		dev->partial = (char*)kmalloc((dev->partial_len+count)*sizeof(char), GFP_KERNEL);
		if(!dev->partial)
		{
			// reset
			dev->partial = prev;
			mutex_unlock(&dev->partial_mx);
			return -ENOMEM;
		}
		// copy from previous
		if(prev)
		{
			memcpy(dev->partial, prev, dev->partial_len);
			kfree(prev);
		}

		/*
			we don't retry copies.
			worse case scenario the partial buffer will have unused space
			at the end, but it will be freed on a new write or at the end,
			since we don't use the actual size of the buffer for re-sizes
			but the string length
		*/
		retval = count - copy_from_user(dev->partial+dev->partial_len, buf, count);
		dev->partial_len += retval;
	}

	// check for '\n'(s)
	down_write(&dev->semaphore);
	while((newline = memchr(dev->partial, '\n', dev->partial_len)))
	{
		char *it;
		int i, offset, part_size;
		struct ll_node *node;
		part_size = newline + 1 - dev->partial;
		if(newline == dev->partial)
		{
			// empty line, ignore it
			goto _next_line_tag;
		}
		node = (struct ll_node*)kmalloc(sizeof(struct ll_node), GFP_KERNEL);
		if(!node)
		{
			up_write(&dev->semaphore);
			mutex_unlock(&dev->partial_mx);
			return -ENOMEM;
		}
		memset(node, 0, sizeof(struct ll_node));
		node->repr_size = part_size;
		node->led_values = kmalloc(sizeof(unsigned char)*dev->leds->led_count, GFP_KERNEL);
		if(!node->led_values)
		{
			kfree(node);
			up_write(&dev->semaphore);
			mutex_unlock(&dev->partial_mx);
			return -ENOMEM;
		}
		// now parse from input (leds)
		for(i=0, offset=0, it=dev->partial;i<dev->leds->led_count;i++)
		{
			int err;
			char *comma = memchr(it, ',', part_size-offset);
			if(!comma || comma > newline)
			{
				// invalid
				kfree(node->led_values);
				kfree(node);
				// remove this line
				_next_line(dev, newline, part_size);
				up_write(&dev->semaphore);
				mutex_unlock(&dev->partial_mx);
				return -EINVAL;
			}
			offset += comma+1-it;
			*comma = 0;	// set NULL byte
			// parse
			err = kstrtos8(it, 10, node->led_values+i);
			if(err == -ERANGE || err == -EINVAL)
			{
				// invalid
				kfree(node->led_values);
				kfree(node);
				// remove this line
				_next_line(dev, newline, part_size);
				up_write(&dev->semaphore);
				mutex_unlock(&dev->partial_mx);
				return -EINVAL;
			}
			// next item
			it = comma +1;
		}
		// parse time value
		{
			// `it` should be at the sweet spot
			int err;
			// set null byte
			*newline = 0;
			// parse
			err = kstrtou32(it, 10, &node->time);
			if(err == -ERANGE || err == -EINVAL || node->time < 1)
			{
				// invalid
				kfree(node->led_values);
				kfree(node);
				// remove this line
				_next_line(dev, newline, part_size);
				up_write(&dev->semaphore);
				mutex_unlock(&dev->partial_mx);
				return -EINVAL;
			}
		}

		// all seems good, append new state
		if(!dev->head)
		{
			dev->head = dev->tail = node;
		}
		else
		{
			dev->tail->next = node;
			dev->tail = node;
		}

		// remove part of the buffer
	_next_line_tag:
		if(_next_line(dev, newline, part_size))
			return -ENOMEM;
	}
	downgrade_write(&dev->semaphore);
	if(dev->head && !hrtimer_active(&states_hrtimer))
	{
		printk(KERN_DEBUG "ledcontroller: initializing timer\n");
		timer_signal = TSIGNAL_CNT;
		wake_up(&wq);
	}
	up_read(&dev->semaphore);
	mutex_unlock(&dev->partial_mx);

	return retval;
}

struct file_operations lc_states_fops = {
	.owner   = THIS_MODULE,
	.read    = lc_states_read,
	.write   = lc_states_write,
	.open    = lc_states_open,
	.release = lc_states_release
};

static int lc_states_dev_setup(struct lc_states_dev *dev)
{
	int err;
	int devno = MKDEV(lc_dev_major, lc_dev_minor);

	cdev_init(&dev->cdev, &lc_states_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &lc_states_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if(err)
		printk(KERN_ERR "ledcontroller: failed to add states cdev: %d\n", err);
	return err;
}

/* just for allowing folding */
#if 1 /* Module params/Kobjects */
/*
	Module Params/Kobjects
*/

/* if debugging */
//#define LED_INITIAL_STATE 1
/* for production */
#define LED_INITIAL_STATE 0


static int parent_kobj_init(void);

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
	/* TODO: ensure attr->name == "pin" (only 1 attribute, so far) */
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
	/* TODO: ensure attr->name == "pin" (only 1 attribute, so far) */

	/* ensure states linked-list is empty */
	down_read(&lc_states_dev.semaphore);
	if(lc_states_dev.head)
	{
		// not empty
		up_read(&lc_states_dev.semaphore);
		return -EBUSY;
	}
	up_read(&lc_states_dev.semaphore);

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
	struct leds *leds = (struct leds*)kp->arg;
	int count_new;

	// check if states linked-list has values, if !empty, return some error
	if(leds->led_count >= 0)
	{
		// we only check if it is already initialized
		down_read(&lc_states_dev.semaphore);
		if(lc_states_dev.head)
		{
			up_read(&lc_states_dev.semaphore);
			return -EBUSY;
		}
		up_read(&lc_states_dev.semaphore);
	}


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

#endif /* Module params/Kobjects */

/*
	Init/Exit functions
*/

struct task_struct *gpio_thread;

int lc_init_module(void)
{
	int ret, i;
	dev_t dev = 0;
	printk(KERN_DEBUG "hello from led-controller module!\n");

	{ /* params/leds init */
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
	}

	{ /* cdev init */
		ret = alloc_chrdev_region(&dev, lc_dev_minor, LC_MINOR_COUNT, "ledcontroller");
		lc_dev_major = MAJOR(dev);
		if(ret < 0)
		{
			printk(KERN_ERR "ledcontroller: can't get major for device: %d\n", ret);
			goto _fail_1;
		}
		memset(&lc_states_dev, 0, sizeof(lc_states_dev));
		// init semaphores
		init_rwsem(&lc_states_dev.semaphore);
		mutex_init(&lc_states_dev.partial_mx);
		lc_states_dev.leds = &leds;

		ret = lc_states_dev_setup(&lc_states_dev);
		if(ret)
			goto _fail_2;
	}

	hrtimer_init(&states_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	states_hrtimer.function = _states_hrtimer_callback;

	if((gpio_thread = kthread_run(_thread_gpio_runner, &lc_states_dev, "ledc-gpio-runner")) == ERR_PTR(-ENOMEM))
	{
		ret = -ENOMEM;
		goto _fail_3;
	}

	return 0;

_fail_3:
	hrtimer_cancel(&states_hrtimer);
_fail_2:
	unregister_chrdev_region(dev, LC_MINOR_COUNT);
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
	dev_t devno;
	struct lc_states_dev *dev = &lc_states_dev;	// minify

	// stop timer
	hrtimer_cancel(&states_hrtimer);
	if(hrtimer_cancel(&states_hrtimer))
	{
		printk(KERN_WARNING "ledcontroller: timer was cancelled twice, may be an error\n");
	}

	printk(KERN_DEBUG "ledcontroller: signalling thread to exit\n");
	timer_signal = TSIGNAL_EXT;
	wake_up(&wq);
	kthread_stop(gpio_thread);

	/* free linked-list */
	for(dev->cur=dev->head;dev->cur;dev->cur=dev->head)
	{
		dev->head = dev->cur->next;
		kfree(dev->cur->led_values);
		kfree(dev->cur);
	}
	if(dev->partial)
		kfree(dev->partial);

	devno = MKDEV(lc_dev_major, lc_dev_minor);
	cdev_del(&lc_states_dev.cdev);

	unregister_chrdev_region(devno, LC_MINOR_COUNT);

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
