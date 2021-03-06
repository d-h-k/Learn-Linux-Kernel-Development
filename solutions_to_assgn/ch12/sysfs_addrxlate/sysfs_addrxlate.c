/*
 * solutions_to_assgn/ch13/sysfs_addrxlate/sysfs_addrxlate.c
 ***************************************************************
 * This program is part of the source code released for the book
 *  "Learn Linux Kernel Development"
 *  (c) Author: Kaiwan N Billimoria
 *  Publisher:  Packt
 *  GitHub repository:
 *  https://github.com/PacktPublishing/Learn-Linux-Kernel-Development
 *
 * From: Ch 13 : User-Kernel communication pathways
 ****************************************************************
 * Brief Description:
 * This is an assignment from the chapter:
 *
 * sysfs_addrxlate: sysfs assignment #2 (a bit more advanced):
 * Address translation: exploiting the knowledge gained from this chapter and
 * Ch 7 section 'Direct-mapped RAM and address translation', write a simple
 * platform driver that provides two sysfs interface files called
 * addrxlate_kva2pa and addrxlate_pa2kva; the way it should work: writing a
 * kva (kernel virtual address) into the sysfs file addrxlate_kva2pa should
 * have the driver read and translate the kva into it's corresponding physical
 * address (pa); then reading from the same file should cause the pa to be
 * displayed. Vice-versa with the addrxlate_pa2kva sysfs file.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/mm.h>	    // for high_memory

// copy_[to|from]_user()
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 11, 0)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif

MODULE_AUTHOR("Kaiwan N Billimoria");
MODULE_DESCRIPTION
    ("LLKD book:solutions_to_assgn/ch12/sysfs_addrxlate: simple sysfs interfacing to translate linear addr");
/*
 * We *require* the module to be released under GPL license (as well) to please
 * several core driver routines (like sysfs_create_group,
 * platform_device_register_simple, etc which are exported to GPL only (using
 * the EXPORT_SYMBOL_GPL() macro)
 */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

#define OURMODNAME	"sysfs_addrxlate"
#define SYSFS_FILE1	addrxlate_kva2pa
#define SYSFS_FILE2	addrxlate_pa2kva

//--- our MSG() macro
#ifdef DEBUG
#define MSG(string, args...)  do {                       \
	pr_info("%s:%s():%d: " string,                   \
		OURMODNAME, __func__, __LINE__, ##args); \
} while (0)
#else
#define MSG(string, args...)
#endif

#if(BITS_PER_LONG == 32)
	#define FMTSPC		"%08x"
	#define TYPECST		unsigned long
	typedef u32 addr_t;
#elif(BITS_PER_LONG == 64)
	#define FMTSPC		"%016llx"
	#define TYPECST	    	unsigned long long
	typedef u64 addr_t;
#endif

/* We use a mutex lock; details in Ch 15 and Ch 16 */
static DEFINE_MUTEX(mtx1);
static DEFINE_MUTEX(mtx2);
static struct platform_device *sysfs_demo_platdev;	/* Device structure */

/* Note that in both the show and store methods, the buffer 'buf' is
 * a *kernel*-space buffer. (So don't try copy_[from|to]_user stuff!)
 *
 * From linux/device.h:
--snip--
// interface for exporting device attributes
struct device_attribute {
	struct attribute        attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count);
};
*/

#define MANUALLY
#define ADDR_MAXLEN	20
static phys_addr_t gxlated_addr_kva2pa;
static addr_t gxlated_addr_pa2kva;

/*------------------ sysfs file 2 (RW) -------------------------------------*/

/* xlateaddr_pa2kva: sysfs entry point for the 'show' (read) callback */
static ssize_t addrxlate_pa2kva_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int n;

	if (mutex_lock_interruptible(&mtx2))
		return -ERESTARTSYS;
	MSG("In the 'show' method\n");
	n = snprintf(buf, ADDR_MAXLEN, "0x" FMTSPC "\n", gxlated_addr_pa2kva);
	mutex_unlock(&mtx2);

	return n;
}

/* xlateaddr_pa2kva: sysfs entry point for the 'store' (write) callback */
static ssize_t addrxlate_pa2kva_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int ret = (int)count;
	char s_addr[ADDR_MAXLEN];
	phys_addr_t pa = 0x0;

	if (mutex_lock_interruptible(&mtx2))
		return -ERESTARTSYS;

	memset(s_addr, 0, ADDR_MAXLEN);
	strncpy(s_addr, buf, ADDR_MAXLEN);
	s_addr[strlen(s_addr) - 1] = '\0';	// rm trailing newline char
	if (count == 0 || count > ADDR_MAXLEN) {
		ret = -EINVAL;
		goto out;
	}

#if(BITS_PER_LONG == 32)
	ret = kstrtoul(s_addr, 0, (long unsigned int *)&pa);
#else
	ret = kstrtoull(s_addr, 0, &pa);
#endif
	if (ret < 0) {
		mutex_unlock(&mtx2);
		pr_warn("%s:%s:%d: kstrtoull failed!\n",
			OURMODNAME, __func__, __LINE__);
		return ret;
	}

	/* Verify that the passed pa is valid
	 * WARNING! the below validity checks are very simplistic; YMMV!
	 */
	if (pa > PAGE_OFFSET) {
		pr_info("%s(): invalid physical address (0x" FMTSPC ")?\n", __func__, pa);
		return -EFAULT;
	}

	/* All okay (fingers crossed), perform the address translation! */
	gxlated_addr_pa2kva = (TYPECST)phys_to_virt(pa);
	pr_debug(" pa 0x" FMTSPC " = kva 0x" FMTSPC "\n", pa, gxlated_addr_pa2kva);

#ifdef MANUALLY
	/* 'Manually' perform the address translation */
	pr_info("%s: manually:  pa 0x" FMTSPC " = kva 0x" FMTSPC "\n",
		OURMODNAME, pa,
#if(BITS_PER_LONG == 32)
		(unsigned int)(pa + PAGE_OFFSET));
#else
		(pa + PAGE_OFFSET));
#endif
#endif
	ret = count;
 out:
	mutex_unlock(&mtx2);
	return ret;
}

static DEVICE_ATTR_RW(SYSFS_FILE2);	/* it's show/store callbacks are above */

/*------------------ sysfs file 1 (RW) -------------------------------------*/

/* xlateaddr_kva2pa: sysfs entry point for the 'show' (read) callback */
static ssize_t addrxlate_kva2pa_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int n;

	if (mutex_lock_interruptible(&mtx1))
		return -ERESTARTSYS;
	MSG("In the 'show' method\n");
	n = snprintf(buf, ADDR_MAXLEN, "0x" FMTSPC "\n", gxlated_addr_kva2pa);
	//n = snprintf(buf, ADDR_MAXLEN, "0x%llx\n", gxlated_addr_kva2pa);
	mutex_unlock(&mtx1);

	return n;
}

/* xlateaddr_kva2pa: sysfs entry point for the 'store' (write) callback */
static ssize_t addrxlate_kva2pa_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int ret = (int)count, valid = 1;
	char s_addr[ADDR_MAXLEN];
	addr_t kva = 0x0;
	//unsigned long long kva = 0x0;

	if (mutex_lock_interruptible(&mtx1))
		return -ERESTARTSYS;

	memset(s_addr, 0, ADDR_MAXLEN);
	strncpy(s_addr, buf, ADDR_MAXLEN);
	s_addr[strlen(s_addr) - 1] = '\0';	// rm trailing newline char
	if (count == 0 || count > ADDR_MAXLEN) {
		ret = -EINVAL;
		goto out;
	}

#if(BITS_PER_LONG == 32)
	ret = kstrtoul(s_addr, 0, (long unsigned int *)&kva);
#else
	ret = kstrtoull(s_addr, 0, &kva);
#endif
	if (ret < 0) {
		mutex_unlock(&mtx1);
		pr_warn("%s:%s:%d: kstrtoull failed!\n",
			OURMODNAME, __func__, __LINE__);
		return ret;
	}

	/* Verify that the passed kva is valid, a linear address, i.e, it
	 * lies within the lowmem segment, i.e., between PAGE_OFFSET and
	 * high_memory; x86 has a builtin validity checker func that we use,
	 * for other we do our own (simplistic) checking.
	 */
#ifdef CONFIG_X86
	if (!virt_addr_valid(kva))
		valid = 0;
#else
	// WARNING! the below validity checks are very simplistic; YMMV!
	if ((kva < PAGE_OFFSET) || (kva > (addr_t)high_memory))
		valid = 0;
#endif
	if (!valid) {
		pr_info("%s(): invalid virtual address (0x" FMTSPC "),"
		" must be a valid linear addr within the kernel lowmem region\n"
		" IOW, *only* kernel direct mapped RAM locations are valid\n",
			__func__, kva);
		return -EFAULT;
	}

	/* All okay (fingers crossed), perform the address translation! */
	gxlated_addr_kva2pa = virt_to_phys((volatile void *)kva);
	pr_debug("kva 0x" FMTSPC " =  pa 0x" FMTSPC "\n", kva, gxlated_addr_kva2pa);

#ifdef MANUALLY
	/* 'Manually' perform the address translation */
	pr_info("%s: manually: kva 0x" FMTSPC " =  pa 0x" FMTSPC "\n",
		OURMODNAME, kva,
#if(BITS_PER_LONG == 32)
		(unsigned int)(kva - PAGE_OFFSET));
#else
		(kva - PAGE_OFFSET));
#endif
#endif
	ret = count;
 out:
	mutex_unlock(&mtx1);
	return ret;
}

/* The DEVICE_ATTR{_RW|RO|WO}() macro instantiates a struct device_attribute
 * dev_attr_<name> (as the comments below help explain ...) here...
 */
static DEVICE_ATTR_RW(SYSFS_FILE1);	/* it's show/store callbacks are above */

/*
 * From <linux/device.h>:
DEVICE_ATTR{_RW} helper interfaces (linux/device.h):
--snip--
#define DEVICE_ATTR_RW(_name) \
    struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
--snip--
and in <linux/sysfs.h>:
#define __ATTR(_name, _mode, _show, _store) {              \
	.attr = {.name = __stringify(_name),               \
		.mode = VERIFY_OCTAL_PERMISSIONS(_mode) }, \
	.show   = _show,                                   \
	.store  = _store,                                  \
}
 */

static int __init sysfs_addrxlate_init(void)
{
	int stat = 0;

	if (!IS_ENABLED(CONFIG_SYSFS)) {
		pr_warn("%s: sysfs unsupported! Aborting ...\n", OURMODNAME);
		return -EINVAL;
	}

	/* 0. Register a (dummy) platform device; required as we need a
	 * struct device *dev pointer to create the sysfs file with
	 * the device_create_file() API
	 */
#define PLAT_NAME	"llkd_sysfs_addrxlate"
	sysfs_demo_platdev =
	    platform_device_register_simple(PLAT_NAME, -1, NULL, 0);
	if (IS_ERR(sysfs_demo_platdev)) {
		stat = PTR_ERR(sysfs_demo_platdev);
		pr_info
		    ("%s: error (%d) registering our platform device, aborting\n",
		     OURMODNAME, stat);
		goto out1;
	}

	// 1. Create our first sysfile file : addrxlate_kva2pa
	/* The device_create_file() API creates a sysfs attribute file for
	 * given device (1st parameter); the second parameter is the pointer
	 * to it's struct device_attribute structure dev_attr_<name> which was
	 * instantiated by our DEV_ATTR{_RW|RO} macros above ...
	 */
	stat =
	    device_create_file(&sysfs_demo_platdev->dev, &dev_attr_SYSFS_FILE1);
	if (stat) {
		pr_info
		    ("%s: device_create_file [1] failed (%d), aborting now\n",
		     OURMODNAME, stat);
		goto out2;
	}
	pr_info("sysfs file [1] (/sys/devices/platform/%s/%s) created\n",
	    PLAT_NAME, __stringify(SYSFS_FILE1));

	// 2. Create our second sysfile file : addrxlate_pa2kva
	stat =
	    device_create_file(&sysfs_demo_platdev->dev, &dev_attr_SYSFS_FILE2);
	if (stat) {
		pr_info
		    ("%s: device_create_file [2] failed (%d), aborting now\n",
		     OURMODNAME, stat);
		goto out3;
	}
	pr_info("sysfs file [2] (/sys/devices/platform/%s/%s) created\n",
	    PLAT_NAME, __stringify(SYSFS_FILE2));

	pr_info("%s initialized\n", OURMODNAME);
	return 0;		/* success */

 out3:
	device_remove_file(&sysfs_demo_platdev->dev, &dev_attr_SYSFS_FILE1);
 out2:
	platform_device_unregister(sysfs_demo_platdev);
 out1:
	return stat;
}

static void __exit sysfs_addrxlate_cleanup(void)
{
	/* Cleanup sysfs nodes */
	device_remove_file(&sysfs_demo_platdev->dev, &dev_attr_SYSFS_FILE2);
	device_remove_file(&sysfs_demo_platdev->dev, &dev_attr_SYSFS_FILE1);
	/* Unregister the (dummy) platform device */
	platform_device_unregister(sysfs_demo_platdev);
	pr_info("%s removed\n", OURMODNAME);
}

module_init(sysfs_addrxlate_init);
module_exit(sysfs_addrxlate_cleanup);
