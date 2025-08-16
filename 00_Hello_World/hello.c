#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
// Has the current struct
#include <linux/sched.h>
// Has the KERNEL_VERSION(major, minor, release) macro and LINUX_VERSION_CODE
#include <linux/version.h>

static char *whom = ":p";
static int howmany = 1;
module_param(howmany, int, S_IRUGO);
module_param(whom, charp, S_IRUGO);

static int __init hello_init(void)
{
    for (__u32 i = 0; i < howmany; i++)
    {
        printk(KERN_INFO "[%i] Hello, \"%s\"!\n", i, whom);
    }
    printk(KERN_INFO "The calculated KERNEL_VERSION is: %i and the LINUX_VERSION_CODE is: %i", KERNEL_VERSION(6, 12, 10), LINUX_VERSION_CODE);
    printk(KERN_INFO "The process calling this module is: \"%s\" (pid %i)", current->comm, current->pid);
    return 0;
}

static void __exit hello_exit(void)
{
    printk(KERN_INFO "Goodbye, World!\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(":p");
MODULE_DESCRIPTION("Hello From Kernel Module");
MODULE_VERSION("1.0");
