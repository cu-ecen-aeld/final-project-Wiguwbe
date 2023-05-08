#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>

MODULE_AUTHOR("Tiago Teixeira");
MODULE_LICENSE("Dual BSD/GPL");


int lc_init_module(void)
{
	printk(KERN_DEBUG "hello from led-controller module!\n");
	return 0;
}

void lc_exit_module(void)
{
	printk(KERN_DEBUG "goodbye ...\n");
}

module_init(lc_init_module);
module_exit(lc_exit_module);
