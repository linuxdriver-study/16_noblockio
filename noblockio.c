#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/types.h>
#include <linux/ide.h>
#include <linux/irq.h>
#include <linux/of_irq.h>
#include <linux/wait.h>
#include <linux/poll.h>

#define DEVICE_CNT      1
#define DEVICE_NAME     "keyirq"

#define KEY_NUM         1
#define KEY0VALUE       0x01
#define INVAKEY         0xFF

struct irq_keydesc {
        int gpio;
        int irqnum;
        unsigned char value;
        char name[10];
        irqreturn_t (*handler)(int, void *);
};

struct keyirq_device_struct {
        int major;
        int minor;
        dev_t devid;
        struct cdev keyirq_cdev;
        struct class *class;
        struct device *device;
        struct device_node *nd;
        struct irq_keydesc keyirq[KEY_NUM];
        struct timer_list timer;
        atomic_t releasekey;
        wait_queue_head_t r_wait;
};
static struct keyirq_device_struct keyirq_dev;

static int keyirq_open(struct inode *inode, struct file *file);
static ssize_t keyirq_read(struct file *file,
                        char __user *user,
                        size_t count,
                        loff_t *loff);
static int keyirq_release(struct inode *inode, struct file *file);
static unsigned int keyirq_poll(struct file *filp, struct poll_table_struct *wait);

static struct file_operations ops = {
        .owner = THIS_MODULE,
        .open = keyirq_open,
        .read = keyirq_read,
        .release = keyirq_release,
        .poll = keyirq_poll,
};

static int keyirq_open(struct inode *inode, struct file *file)
{
        file->private_data = &keyirq_dev;
        return 0;
}

static ssize_t keyirq_read(struct file *file,
                        char __user *user,
                        size_t count,
                        loff_t *loff)
{
        int ret = 0;
        int releasekey = 0;
        struct keyirq_device_struct *dev = file->private_data;

        if (file->f_flags & O_NONBLOCK) {
                if (atomic_read(&dev->releasekey) == 0)
                        return -EAGAIN;
        } else {
                ret = wait_event_interruptible(dev->r_wait, atomic_read(&dev->releasekey));
                if (ret)
                        goto error;
        }
        
        releasekey = atomic_read(&dev->releasekey); 
        if (releasekey == 1) {
                atomic_set(&dev->releasekey, 0);
                ret = copy_to_user(user, &releasekey, sizeof(releasekey));
                if (ret != 0) {
                        printk("copy to user error!\n");
                        goto error;
                }
                ret = sizeof(releasekey);
        }
error:
        return ret;
}

static unsigned int keyirq_poll(struct file *filp, struct poll_table_struct *wait)
{
        unsigned int mask = 0;
        struct keyirq_device_struct *dev = (struct keyirq_device_struct *)filp->private_data;

        /* 调用poll_wait不会引起阻塞，只是将应用程序添加到poll_table中 */
        poll_wait(filp, &dev->r_wait, wait);

        if (atomic_read(&dev->releasekey))
                mask = POLLIN | POLLRDNORM;

        printk("poll release! mask = %d\n", mask);
        return mask;
}

static int keyirq_release(struct inode *inode, struct file *file)
{
        file->private_data = NULL;
        return 0;
}

void timer_func(unsigned long arg)
{
        int value = 0;
        struct keyirq_device_struct *dev = (struct keyirq_device_struct *)arg;

        printk("Enter timer!\n");

        value = gpio_get_value(dev->keyirq[0].gpio);
        if (value == 0) {
                atomic_set(&dev->releasekey, 1);
                //printk("KEY0 Press!\n");
        } else if (value == 1) {
                //printk("KEY0 Release!\n");
                wake_up(&dev->r_wait);
        }
        //wake_up(&dev->r_wait);
}

static irqreturn_t key0_handler(int irq, void *dev_id)
{
        struct keyirq_device_struct *dev = dev_id;

        dev->timer.data = (unsigned long)dev;
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(10));

        return IRQ_RETVAL(IRQ_HANDLED);
}

int key_io_config(struct keyirq_device_struct *dev)
{
        int ret = 0;
        int i = 0;
        int num = 0;
        int num_irq = 0;

        dev->nd = of_find_node_by_path("/key");
        if (dev->nd == NULL) {
                printk("fail find node!\n");
                ret = -EINVAL;
                goto fail_nd;
        }

        for (i = 0; i < KEY_NUM; i++) {
                dev->keyirq[i].gpio =of_get_named_gpio(dev->nd, "key-gpios", i);
                if (dev->keyirq[i].gpio < 0) {
                        printk("fail get named!\n");
                        ret = -EFAULT;
                        goto fail_get_named;
                }
        }

        for (i = 0; i < KEY_NUM; i++) {
                memset(dev->keyirq[i].name, 0, sizeof(dev->keyirq[i].name));
                sprintf(dev->keyirq[i].name, "KEY%d", i);
                ret = gpio_request(dev->keyirq[i].gpio, dev->keyirq[i].name);
                if (ret != 0) {
                        printk("gpio request error!\n");
                        ret = -EFAULT;
                        goto fail_request;
                }
                num ++;
        }

        for (i = 0; i < KEY_NUM; i++) {
                ret = gpio_direction_input(dev->keyirq[i].gpio);
                if (ret != 0) {
                        printk("gpio set dir error!\n");
                        ret = -EFAULT;
                        goto fail_set_dir;
                }
        }

        for (i = 0; i < KEY_NUM; i++) {
#if 1
                dev->keyirq[i].irqnum = gpio_to_irq(dev->keyirq[i].gpio);
#else
                dev->keyirq[i].irqnum = irq_of_parse_and_map(dev->nd, i);
#endif
                printk("key%d:gpio=%d, irqnum=%d\n", i, dev->keyirq[i].gpio,
                       dev->keyirq[i].irqnum);
        }

        dev->keyirq[0].handler = key0_handler;
        dev->keyirq[0].value = KEY0VALUE;

        for (i = 0; i < KEY_NUM; i++) {
                ret = request_irq(dev->keyirq[i].irqnum, 
                                  dev->keyirq[i].handler, 
                                  IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING,
                                  dev->keyirq[i].name,
                                  dev);
                if (ret) {
                        printk("irq%d request error!\n", i);
                        goto fail_request_irq;
                }
                num_irq ++;
        }
        goto success;

fail_request_irq:
        for (i = 0; i < num_irq; i++)
                free_irq(dev->keyirq[i].irqnum, dev);
fail_set_dir:
fail_request:
        for (i = 0; i < num; i++)
                gpio_free(dev->keyirq[i].gpio);
fail_get_named:
fail_nd:
success:
        return ret;
}

static int __init keyirq_init(void)
{
        int ret = 0;
        if (keyirq_dev.major) {
                keyirq_dev.devid = MKDEV(keyirq_dev.major, keyirq_dev.minor);
                ret = register_chrdev_region(keyirq_dev.devid, DEVICE_CNT, DEVICE_NAME);
        } else {
                ret = alloc_chrdev_region(&keyirq_dev.devid, 0, DEVICE_CNT, DEVICE_NAME);
        }
        if (ret < 0) {
                printk("chrdev region error!\n");
                goto fail_chrdev_region;
        }
        keyirq_dev.major = MAJOR(keyirq_dev.devid);
        keyirq_dev.minor = MINOR(keyirq_dev.devid);
        printk("major:%d minor:%d\n", keyirq_dev.major, keyirq_dev.minor);

        cdev_init(&keyirq_dev.keyirq_cdev, &ops);
        ret = cdev_add(&keyirq_dev.keyirq_cdev, keyirq_dev.devid, DEVICE_CNT);
        if (ret < 0) {
                printk("cdev add error!\n");
                goto fail_cdev_add;
        }
        keyirq_dev.class = class_create(THIS_MODULE, DEVICE_NAME);
        if (IS_ERR(keyirq_dev.class)) {
                printk("class create error!\n");
                ret = -EINVAL;
                goto fail_class_create;
        }
        keyirq_dev.device = device_create(keyirq_dev.class, NULL,
                                       keyirq_dev.devid, NULL, DEVICE_NAME);
        if (IS_ERR(keyirq_dev.device)) {
                printk("device create error!\n");
                ret = -EINVAL;
                goto fail_device_create;
        }

        atomic_set(&keyirq_dev.releasekey, 0);
        init_waitqueue_head(&keyirq_dev.r_wait);

        init_timer(&keyirq_dev.timer);
        keyirq_dev.timer.function = timer_func;

        ret = key_io_config(&keyirq_dev);
        if (ret != 0) {
                printk("key io config error!\n");
                goto fail_io_config;
        }

        goto success;
        
fail_io_config:
        del_timer(&keyirq_dev.timer);
        device_destroy(keyirq_dev.class, keyirq_dev.devid);
fail_device_create:
        class_destroy(keyirq_dev.class);
fail_class_create:
        cdev_del(&keyirq_dev.keyirq_cdev);
fail_cdev_add:
        unregister_chrdev_region(keyirq_dev.devid, DEVICE_CNT);
fail_chrdev_region:
success:
        return ret;
}

static void __exit keyirq_exit(void)
{
        int i = 0;

        del_timer(&keyirq_dev.timer);
        for (i = 0; i < KEY_NUM; i++)
                free_irq(keyirq_dev.keyirq[i].irqnum, &keyirq_dev);
        for (i = 0; i < KEY_NUM; i++)
                gpio_free(keyirq_dev.keyirq[i].gpio);
        device_destroy(keyirq_dev.class, keyirq_dev.devid);
        class_destroy(keyirq_dev.class);
        cdev_del(&keyirq_dev.keyirq_cdev);
        unregister_chrdev_region(keyirq_dev.devid, DEVICE_CNT);
}

module_init(keyirq_init);
module_exit(keyirq_exit);
MODULE_AUTHOR("wanglei");
MODULE_LICENSE("GPL");
