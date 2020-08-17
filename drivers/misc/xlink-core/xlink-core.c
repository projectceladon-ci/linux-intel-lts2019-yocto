// SPDX-License-Identifier: GPL-2.0-only
/*
 * xlink Core Driver.
 *
 * Copyright (C) 2018-2019 Intel Corporation
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>

#ifdef CONFIG_XLINK_LOCAL_HOST
#include <linux/xlink-ipc.h>
#endif

#include "xlink-defs.h"
#include "xlink-multiplexer.h"
#include "xlink-dispatcher.h"
#include "xlink-platform.h"

// xlink version number
#define XLINK_VERSION_MAJOR		0
#define XLINK_VERSION_MINOR		1
#define XLINK_VERSION_REVISION	2

// timeout in milliseconds used to wait for the reay message from the VPU
#ifdef CONFIG_XLINK_PSS
#define XLINK_VPU_WAIT_FOR_READY (3000000)
#else
#define XLINK_VPU_WAIT_FOR_READY (3000)
#endif

// device, class, and driver names
#define DEVICE_NAME "xlnk"
#define CLASS_NAME	"xlkcore"
#define DRV_NAME	"xlink-driver"

// used to determine if an API was called from user or kernel space
#define CHANNEL_SET_USER_BIT(chan) (chan |= (1 << 15))
#define CHANNEL_USER_BIT_IS_SET(chan) (chan & (1 << 15))
#define CHANNEL_CLEAR_USER_BIT(chan) (chan &= ~(1 << 15))

static dev_t xdev;
static struct class *dev_class;
static struct cdev xlink_cdev;

static long xlink_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static enum xlink_error xlink_write_data_user(struct xlink_handle *handle,
		uint16_t chan, uint8_t const *pmessage, uint32_t size);

static const struct file_operations fops = {
		.owner			= THIS_MODULE,
		.unlocked_ioctl = xlink_ioctl,
};

struct xlink_link {
	uint32_t id;
	struct xlink_handle handle;
	struct kref refcount;
};

struct keembay_xlink_dev {
	struct platform_device *pdev;
	struct xlink_link links[XLINK_MAX_CONNECTIONS];
	uint32_t nmb_connected_links;
	struct mutex lock;
};

/*
 * global variable pointing to our xlink device.
 *
 * This is meant to be used only when platform_get_drvdata() cannot be used
 * because we lack a reference to our platform_device.
 */
static struct keembay_xlink_dev *xlink;

/*
 * get_next_link() - Searches the list of links to find the next available.
 *
 * Note: This function is only used in xlink_connect, where the xlink mutex is
 * already locked.
 *
 * Return: the next available link, or NULL if maximum connections reached.
 */
static struct xlink_link *get_next_link(void)
{
	int i = 0;
	struct xlink_link *link = NULL;

	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		if (xlink->links[i].handle.sw_device_id
				== XLINK_INVALID_SW_DEVICE_ID) {
			link = &xlink->links[i];
			break;
		}
	}
	return link;
}

/*
 * get_link_by_sw_device_id()
 *
 * Searches the list of links to find a link by sw device id.
 *
 * Return: the handle, or NULL if the handle is not found.
 */
static struct xlink_link *get_link_by_sw_device_id(uint32_t sw_device_id)
{
	int i = 0;
	struct xlink_link *link = NULL;

	mutex_lock(&xlink->lock);
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		if (xlink->links[i].handle.sw_device_id == sw_device_id) {
			link = &xlink->links[i];
			break;
		}
	}
	mutex_unlock(&xlink->lock);
	return link;
}

// For now , do nothing and leave for further consideration
static void release_after_kref_put(struct kref *ref) {}

/* Driver probing. */
static int kmb_xlink_probe(struct platform_device *pdev)
{
	int rc, i;
	struct keembay_xlink_dev *xlink_dev;
	struct device *dev_ret;

	dev_info(&pdev->dev, "KeemBay xlink v%d.%d.%d\n", XLINK_VERSION_MAJOR,
		 XLINK_VERSION_MINOR, XLINK_VERSION_REVISION);

	xlink_dev = devm_kzalloc(&pdev->dev, sizeof(*xlink), GFP_KERNEL);
	if (!xlink_dev)
		return -ENOMEM;

	xlink_dev->pdev = pdev;

	// initialize multiplexer
	rc = xlink_multiplexer_init(xlink_dev->pdev);
	if (rc != X_LINK_SUCCESS) {
		pr_err("Multiplexer initialization failed\n");
		goto r_multiplexer;
	}

	// initialize dispatcher
	rc = xlink_dispatcher_init(xlink_dev->pdev);
	if (rc != X_LINK_SUCCESS) {
		pr_err("Dispatcher initialization failed\n");
		goto r_dispatcher;
	}

	// initialize xlink data structure
	xlink_dev->nmb_connected_links = 0;
	mutex_init(&xlink_dev->lock);
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		xlink_dev->links[i].id = i;
		xlink_dev->links[i].handle.sw_device_id =
				XLINK_INVALID_SW_DEVICE_ID;
	}

	platform_set_drvdata(pdev, xlink_dev);

	/* Set the global reference to our device. */
	xlink = xlink_dev;

	/*Allocating Major number*/
	if ((alloc_chrdev_region(&xdev, 0, 1, "xlinkdev")) < 0) {
		dev_info(&pdev->dev, "Cannot allocate major number\n");
		goto r_dispatcher;
	}
	dev_info(&pdev->dev, "Major = %d Minor = %d\n", MAJOR(xdev),
			MINOR(xdev));

	/*Creating struct class*/
	dev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(dev_class)) {
		dev_info(&pdev->dev, "Cannot create the struct class - Err %ld\n",
				PTR_ERR(dev_class));
		goto r_class;
	}

	/*Creating device*/
	dev_ret = device_create(dev_class, NULL, xdev, NULL, DEVICE_NAME);
	if (IS_ERR(dev_ret)) {
		dev_info(&pdev->dev, "Cannot create the Device 1 - Err %ld\n",
				PTR_ERR(dev_ret));
		goto r_device;
	}
	dev_info(&pdev->dev, "Device Driver Insert...Done!!!\n");

	/*Creating cdev structure*/
	cdev_init(&xlink_cdev, &fops);

	/*Adding character device to the system*/
	if ((cdev_add(&xlink_cdev, xdev, 1)) < 0) {
		dev_info(&pdev->dev, "Cannot add the device to the system\n");
		goto r_class;
	}
	return 0;

r_device:
	class_destroy(dev_class);
r_class:
	unregister_chrdev_region(xdev, 1);
r_dispatcher:
	xlink_dispatcher_destroy();
r_multiplexer:
	xlink_multiplexer_destroy();
	return -1;
}

/* Driver removal. */
static int kmb_xlink_remove(struct platform_device *pdev)
{
	int rc = 0;

	mutex_lock(&xlink->lock);
	// destroy multiplexer
	rc = xlink_multiplexer_destroy();
	if (rc != X_LINK_SUCCESS)
		pr_err("Multiplexer destroy failed\n");

	// stop dispatchers and destroy
	rc = xlink_dispatcher_destroy();
	if (rc != X_LINK_SUCCESS)
		pr_err("Dispatcher destroy failed\n");

	mutex_unlock(&xlink->lock);
	mutex_destroy(&xlink->lock);
	// unregister and destroy device
	unregister_chrdev_region(xdev, 1);
	device_destroy(dev_class, xdev);
	cdev_del(&xlink_cdev);
	class_destroy(dev_class);
	pr_info("XLink Driver removed\n");
	return 0;
}

/*
 * IOCTL function for User Space access to xlink kernel functions
 *
 */

static long xlink_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	int interface = NULL_INTERFACE;
	struct xlink_handle devH = {0};
	struct xlinkopenchannel op = {0};
	struct xlinkwritedata wr = {0};
	struct xlinkreaddata rd = {0};
	struct xlinkreadtobuffer rdtobuf = {0};
	struct xlinkconnect con = {0};
	struct xlinkrelease rel = {0};
	struct xlinkstartvpu startvpu = {0};
	struct xlinkcallback cb = {0};
	struct xlinkgetdevicename devn = {0};
	struct xlinkgetdevicelist devl = {0};
	struct xlinkgetdevicestatus devs = {0};
	struct xlinkbootdevice boot = {0};
	struct xlinkresetdevice res = {0};
	struct xlinkdevmode devm = {0};
	uint8_t *rdaddr;
	uint32_t size;
	uint8_t reladdr;
	uint8_t volbuf[XLINK_MAX_BUF_SIZE];
	char filename[64];
	char name[XLINK_MAX_DEVICE_NAME_SIZE];
	uint32_t sw_device_id_list[XLINK_MAX_DEVICE_LIST_SIZE];
	uint32_t num_devices = 0;
	uint32_t device_status = 0;
	uint32_t device_mode = 0;
	struct xlink_link *link = NULL;

	switch (cmd) {
	case XL_CONNECT:
		if (copy_from_user(&con, (int32_t *)arg,
				sizeof(struct xlinkconnect)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)con.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_connect(&devH);
		if (!rc) {
			if (copy_to_user((struct xlink_handle *)con.handle,
					&devH, sizeof(struct xlink_handle)))
				return -EFAULT;
		}
		if (copy_to_user(con.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_OPEN_CHANNEL:
		if (copy_from_user(&op, (int32_t *)arg,
				sizeof(struct xlinkopenchannel)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)op.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_open_channel(&devH, op.chan, op.mode, op.data_size,
				op.timeout);
		if (copy_to_user(op.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_DATA_READY_CALLBACK:
		if (copy_from_user(&cb, (int32_t *)arg,
				sizeof(struct xlinkcallback)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)cb.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		CHANNEL_SET_USER_BIT(cb.chan); // set MSbit for user space call
		rc = xlink_data_available_event(&devH, cb.chan, cb.callback);
		if (copy_to_user(cb.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_DATA_CONSUMED_CALLBACK:
		if (copy_from_user(&cb, (int32_t *)arg,
				sizeof(struct xlinkcallback)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)cb.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		CHANNEL_SET_USER_BIT(cb.chan); // set MSbit for user space call
		rc = xlink_data_consumed_event(&devH, cb.chan, cb.callback);
		if (copy_to_user(cb.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_READ_DATA:
		if (copy_from_user(&rd, (int32_t *)arg,
				sizeof(struct xlinkreaddata)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)rd.handle,
					sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_read_data(&devH, rd.chan, &rdaddr, &size);
		if (!rc) {
			interface = get_interface_from_sw_device_id(
					devH.sw_device_id);
			if (interface == IPC_INTERFACE) {
				if (copy_to_user(rd.pmessage, (void *)&rdaddr,
						sizeof(uint32_t)))
					return -EFAULT;
			} else {
				if (copy_to_user(rd.pmessage, (void *)rdaddr,
						size))
					return -EFAULT;
			}
			if (copy_to_user(rd.size, (void *)&size, sizeof(size)))
				return -EFAULT;
			// this only releases a packet if we are remote host and
			// rd.chan is a passthru ipc channel
			link = get_link_by_sw_device_id(devH.sw_device_id);
			if (link) {
				rc = core_release_packet_from_channel(link->id,
						rd.chan, rdaddr);
			}
		}
		if (copy_to_user(rd.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_READ_TO_BUFFER:

		if (copy_from_user(&rdtobuf, (int32_t *)arg,
				sizeof(struct xlinkreadtobuffer)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)rdtobuf.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_read_data_to_buffer(&devH, rdtobuf.chan,
				(uint8_t *)volbuf, &size);
		if (!rc) {
			if (copy_to_user(rdtobuf.pmessage, (void *)volbuf,
					size))
				return -EFAULT;
			if (copy_to_user(rdtobuf.size, (void *)&size,
					sizeof(size)))
				return -EFAULT;
		}
		if (copy_to_user(rdtobuf.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_WRITE_DATA:
		if (copy_from_user(&wr, (int32_t *)arg,
				sizeof(struct xlinkwritedata)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)wr.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (wr.size <= XLINK_MAX_DATA_SIZE) {
			rc = xlink_write_data_user(&devH, wr.chan, wr.pmessage,
					wr.size);
			if (copy_to_user(wr.return_code, (void *)&rc,
					sizeof(rc)))
				return -EFAULT;
		} else {
			return -EFAULT;
		}
		break;
	case XL_WRITE_VOLATILE:
		if (copy_from_user(&wr, (int32_t *)arg,
				sizeof(struct xlinkwritedata)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)wr.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (wr.size <= XLINK_MAX_BUF_SIZE) {
			if (copy_from_user(volbuf, (char *)wr.pmessage,
					wr.size))
				return -EFAULT;
			rc = xlink_write_volatile(&devH, wr.chan, volbuf,
					wr.size);
			if (copy_to_user(wr.return_code, (void *)&rc,
					sizeof(rc)))
				return -EFAULT;
		} else {
			return -EFAULT;
		}
		break;
	case XL_WRITE_CONTROL_DATA:
		if (copy_from_user(&wr, (int32_t *)arg,
				sizeof(struct xlinkwritedata)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)wr.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (wr.size <= XLINK_MAX_CONTROL_DATA_SIZE) {
			if (copy_from_user(volbuf, (char *)wr.pmessage,
					wr.size))
				return -EFAULT;
			rc = xlink_write_control_data(&devH, wr.chan, volbuf,
					wr.size);
			if (copy_to_user(wr.return_code,
					(void *)&rc, sizeof(rc)))
				return -EFAULT;
		} else {
			return -EFAULT;
		}
		break;
	case XL_RELEASE_DATA:
		if (copy_from_user(&rel, (int32_t *)arg,
				sizeof(struct xlinkrelease)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)rel.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (rel.addr) {
			if (get_user(reladdr, (uint32_t *const)rel.addr))
				return -EFAULT;
			rc = xlink_release_data(&devH, rel.chan,
					(uint8_t *)&reladdr);
		} else {
			rc = xlink_release_data(&devH, rel.chan, NULL);
		}
		if (copy_to_user(rel.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_CLOSE_CHANNEL:
		if (copy_from_user(&op, (int32_t *)arg,
				sizeof(struct xlinkopenchannel)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)op.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_close_channel(&devH, op.chan);
		if (copy_to_user(op.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_START_VPU:
		if (copy_from_user(&startvpu, (int32_t *)arg,
				sizeof(struct xlinkstartvpu)))
			return -EFAULT;
		if (startvpu.namesize > sizeof(filename))
			return -EINVAL;
		memset(filename, 0, sizeof(filename));
		if (copy_from_user(filename, startvpu.filename,
				startvpu.namesize))
			return -EFAULT;
		rc = xlink_start_vpu(filename);
		if (copy_to_user(startvpu.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_STOP_VPU:
		rc = xlink_stop_vpu();
		break;
	case XL_RESET_VPU:
		rc = xlink_stop_vpu();
		break;
	case XL_DISCONNECT:
		if (copy_from_user(&con, (int32_t *)arg,
				sizeof(struct xlinkconnect)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)con.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_disconnect(&devH);
		if (copy_to_user(con.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_GET_DEVICE_NAME:
		if (copy_from_user(&devn, (int32_t *)arg,
				sizeof(struct xlinkgetdevicename)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)devn.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_get_device_name(&devH, name, devn.name_size);
		if (!rc) {
			if (copy_to_user(devn.name, (void *)name,
					devn.name_size))
				return -EFAULT;
		}
		if (copy_to_user(devn.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_GET_DEVICE_LIST:
		if (copy_from_user(&devl, (int32_t *)arg,
				sizeof(struct xlinkgetdevicelist)))
			return -EFAULT;
		rc = xlink_get_device_list(sw_device_id_list, &num_devices);
		if (!rc && (num_devices <= XLINK_MAX_DEVICE_LIST_SIZE)) {
			/* TODO: this next copy is dangerous! we have no idea
			 * how large the devl.sw_device_id_list buffer is
			 * provided by the user. if num_devices is too large,
			 * the copy will overflow the buffer.
			 */
			if (copy_to_user(devl.sw_device_id_list,
					(void *)sw_device_id_list,
					(sizeof(*sw_device_id_list)
					* num_devices)))
				return -EFAULT;
			if (copy_to_user(devl.num_devices, (void *)&num_devices,
					(sizeof(num_devices))))
				return -EFAULT;
		}
		if (copy_to_user(devl.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_GET_DEVICE_STATUS:
		if (copy_from_user(&devs, (int32_t *)arg,
				sizeof(struct xlinkgetdevicestatus)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)devs.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_get_device_status(&devH, &device_status);
		if (!rc) {
			if (copy_to_user(devs.device_status,
					(void *)&device_status,
					sizeof(device_status)))
				return -EFAULT;
		}
		if (copy_to_user(devs.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_BOOT_DEVICE:
		if (copy_from_user(&boot, (int32_t *)arg,
				sizeof(struct xlinkbootdevice)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)boot.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (boot.binary_name_size > sizeof(filename))
			return -EINVAL;
		memset(filename, 0, sizeof(filename));
		if (copy_from_user(filename, boot.binary_name,
				boot.binary_name_size))
			return -EFAULT;
		rc = xlink_boot_device(&devH, filename);
		if (copy_to_user(boot.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_RESET_DEVICE:
		if (copy_from_user(&res, (int32_t *)arg,
				sizeof(struct xlinkresetdevice)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)res.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_reset_device(&devH);
		if (copy_to_user(res.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_GET_DEVICE_MODE:
		if (copy_from_user(&devm, (int32_t *)arg,
				sizeof(struct xlinkdevmode)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)devm.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		rc = xlink_get_device_mode(&devH, &device_mode);
		if (!rc) {
			if (copy_to_user(devm.device_mode, (void *)&device_mode,
					sizeof(device_mode)))
				return -EFAULT;
		}
		if (copy_to_user(devm.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	case XL_SET_DEVICE_MODE:
		if (copy_from_user(&devm, (int32_t *)arg,
				sizeof(struct xlinkdevmode)))
			return -EFAULT;
		if (copy_from_user(&devH, (struct xlink_handle *)devm.handle,
				sizeof(struct xlink_handle)))
			return -EFAULT;
		if (copy_from_user(&device_mode, (uint32_t *)devm.device_mode,
				sizeof(device_mode)))
			return -EFAULT;
		rc = xlink_set_device_mode(&devH, device_mode);
		if (copy_to_user(devm.return_code, (void *)&rc, sizeof(rc)))
			return -EFAULT;
		break;
	}
	if (rc)
		return -EIO;
	else
		return 0;
}

/*
 * xlink Kernel API.
 */

enum xlink_error xlink_stop_vpu(void)
{
#ifdef CONFIG_XLINK_LOCAL_HOST
	int rc = 0;

	rc = xlink_ipc_reset_device(0x0); // stop vpu slice 0
	if (rc)
		return X_LINK_ERROR;
#endif
	return X_LINK_SUCCESS;
}
EXPORT_SYMBOL(xlink_stop_vpu);

enum xlink_error xlink_start_vpu(char *filename)
{
#ifdef CONFIG_XLINK_LOCAL_HOST
	int rc = 0;

	rc = xlink_ipc_boot_device(0x0, filename); // start vpu slice 0
	if (rc)
		return X_LINK_ERROR;
#endif
	return X_LINK_SUCCESS;
}
EXPORT_SYMBOL(xlink_start_vpu);

enum xlink_error xlink_initialize(void)
{
	return X_LINK_SUCCESS;
}
EXPORT_SYMBOL(xlink_initialize);

enum xlink_error xlink_connect(struct xlink_handle *handle)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	int interface = NULL_INTERFACE;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	mutex_lock(&xlink->lock);
	if (!link) {
		link = get_next_link();
		if (!link) {
			pr_err("max connections reached %d\n",
					XLINK_MAX_CONNECTIONS);
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// platform connect
		interface = get_interface_from_sw_device_id(
				handle->sw_device_id);
		rc = xlink_platform_connect(interface, handle->sw_device_id);
		if (rc) {
			pr_err("platform connect failed %d\n", rc);
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// set link handle reference and link id
		link->handle = *handle;
		xlink->nmb_connected_links++;
		kref_init(&link->refcount);
		if (interface != IPC_INTERFACE) {
			// start dispatcher
			rc = xlink_dispatcher_start(link->id, &link->handle);
			if (rc) {
				pr_err("dispatcher start failed\n");
				goto r_cleanup;
			}
		}
		// initialize multiplexer connection
		rc = xlink_multiplexer_connect(link->id);
		if (rc) {
			pr_err("multiplexer connect failed\n");
			goto r_cleanup;
		}
		pr_info("dev 0x%x connected - dev_type %d - nmb_connected_links %d\n",
				link->handle.sw_device_id,
				link->handle.dev_type,
				xlink->nmb_connected_links);
	} else {
		// already connected
		pr_info("dev 0x%x ALREADY connected - dev_type %d\n",
				link->handle.sw_device_id,
				link->handle.dev_type);
		kref_get(&link->refcount);
		*handle = link->handle;
	}
	mutex_unlock(&xlink->lock);
	// TODO: implement ping
	return X_LINK_SUCCESS;

r_cleanup:
	link->handle.sw_device_id = XLINK_INVALID_SW_DEVICE_ID;
	mutex_unlock(&xlink->lock);
	return X_LINK_ERROR;

}
EXPORT_SYMBOL(xlink_connect);

enum xlink_error xlink_data_available_event(struct xlink_handle *handle,
		uint16_t chan, xlink_event data_available_event)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;
	char origin = 'K';

	if (CHANNEL_USER_BIT_IS_SET(chan))
		origin  = 'U';     // function called from user space
	CHANNEL_CLEAR_USER_BIT(chan);  // restore proper channel value

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_DATA_READY_CALLBACK_REQ,
			&link->handle, chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = data_available_event;
	event->callback_origin = origin;
	if (!data_available_event)
		event->calling_pid = NULL; // disable callbacks on this channel
	else
		event->calling_pid = current;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_data_available_event);


enum xlink_error xlink_data_consumed_event(struct xlink_handle *handle,
		uint16_t chan, xlink_event data_consumed_event)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;
	char origin = 'K';

	if (chan & (1 << 15))
		origin  = 'U'; // user space call
	chan &= ~(1 << 15); // clear top bit


	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_DATA_CONSUMED_CALLBACK_REQ,
			&link->handle, chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = data_consumed_event;
	event->callback_origin = origin;
	if (!data_consumed_event)
		event->calling_pid = NULL; // disable callbacks on this channel
	else
		event->calling_pid = current;

	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_data_consumed_event);

enum xlink_error xlink_open_channel(struct xlink_handle *handle,
		uint16_t chan, enum xlink_opmode mode, uint32_t data_size,
		uint32_t timeout)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_OPEN_CHANNEL_REQ,
			&link->handle, chan, data_size, timeout);
	if (!event)
		return X_LINK_ERROR;

	event->data = (void *)mode;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_open_channel);

enum xlink_error xlink_close_channel(struct xlink_handle *handle,
		uint16_t chan)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_CLOSE_CHANNEL_REQ,
			&link->handle, chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_close_channel);

enum xlink_error xlink_write_data(struct xlink_handle *handle,
		uint16_t chan, uint8_t const *pmessage, uint32_t size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_DATA_SIZE)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_REQ, &link->handle,
			chan, size, 0);
	if (!event)
		return X_LINK_ERROR;

	if (chan < XLINK_IPC_MAX_CHANNELS
			&& event->interface == IPC_INTERFACE) {
		/* only passing message address across IPC interface */
		event->data = &pmessage;
		rc = xlink_multiplexer_tx(event, &event_queued);
		xlink_destroy_event(event);
	} else {
		event->data = (uint8_t *)pmessage;
		event->paddr = 0;
		rc = xlink_multiplexer_tx(event, &event_queued);
		if (!event_queued)
			xlink_destroy_event(event);
	}
	return rc;
}
EXPORT_SYMBOL(xlink_write_data);

static enum xlink_error xlink_write_data_user(struct xlink_handle *handle,
		uint16_t chan, uint8_t const *pmessage, uint32_t size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;
	dma_addr_t paddr = 0;
	uint32_t addr;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_DATA_SIZE)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_REQ, &link->handle,
			chan, size, 0);
	if (!event)
		return X_LINK_ERROR;
	event->user_data = 1;

	if (chan < XLINK_IPC_MAX_CHANNELS
			&& event->interface == IPC_INTERFACE) {
		/* only passing message address across IPC interface */
		if (get_user(addr, (uint32_t *)pmessage)) {
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		event->data = &addr;
		rc = xlink_multiplexer_tx(event, &event_queued);
		xlink_destroy_event(event);
	} else {
		event->data = xlink_platform_allocate(&xlink->pdev->dev, &paddr,
				size, XLINK_PACKET_ALIGNMENT,
				XLINK_NORMAL_MEMORY);
		if (!event->data) {
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		if (copy_from_user(event->data, (char *)pmessage, size)) {
			xlink_platform_deallocate(&xlink->pdev->dev,
					event->data, paddr,	size,
					XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		event->paddr = paddr;
		rc = xlink_multiplexer_tx(event, &event_queued);
		if (!event_queued) {
			xlink_platform_deallocate(&xlink->pdev->dev,
					event->data, paddr, size,
					XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			xlink_destroy_event(event);
		}
	}
	return rc;
}

enum xlink_error xlink_write_control_data(struct xlink_handle *handle,
		uint16_t chan, uint8_t const *pmessage, uint32_t size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_CONTROL_DATA_SIZE)
		return X_LINK_ERROR; // TODO: XLink Parameter Error

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_CONTROL_REQ,
			&link->handle, chan, size, 0);
	if (!event)
		return X_LINK_ERROR;

	memcpy(event->header.control_data, pmessage, size);
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_write_control_data);

enum xlink_error xlink_write_volatile(struct xlink_handle *handle,
		uint16_t chan, uint8_t const *message, uint32_t size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;
	dma_addr_t paddr;
	int region = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_BUF_SIZE)
		return X_LINK_ERROR; // TODO: XLink Parameter Error

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_VOLATILE_REQ,
			&link->handle, chan, size, 0);
	if (!event)
		return X_LINK_ERROR;

	if (chan < XLINK_IPC_MAX_CHANNELS && event->interface == IPC_INTERFACE)
		region = XLINK_CMA_MEMORY;
	else
		region = XLINK_NORMAL_MEMORY;
	event->data = xlink_platform_allocate(&xlink->pdev->dev, &paddr, size,
			XLINK_PACKET_ALIGNMENT, region);
	if (!event->data) {
		xlink_destroy_event(event);
		return X_LINK_ERROR;
	}
	memcpy(event->data, message, size);
	event->paddr = paddr;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued) {
		xlink_platform_deallocate(&xlink->pdev->dev, event->data, paddr,
			size, XLINK_PACKET_ALIGNMENT, region);
		xlink_destroy_event(event);
	}
	return rc;
}
EXPORT_SYMBOL(xlink_write_volatile);

enum xlink_error xlink_write_data_crc(struct xlink_handle *handle,
		uint16_t chan, const uint8_t *message,
		uint32_t size)
{
	enum xlink_error rc = 0;
	/* To be implemented */
	return rc;
}
EXPORT_SYMBOL(xlink_write_data_crc);

enum xlink_error xlink_read_data(struct xlink_handle *handle,
		uint16_t chan, uint8_t **pmessage, uint32_t *size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_READ_REQ, &link->handle,
			chan, *size, 0);
	if (!event)
		return X_LINK_ERROR;

	event->pdata = (void **)pmessage;
	event->length = size;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_read_data);

enum xlink_error xlink_read_data_to_buffer(struct xlink_handle *handle,
		uint16_t chan, uint8_t * const message, uint32_t *size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_READ_TO_BUFFER_REQ,
			&link->handle, chan, *size, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = message;
	event->length = size;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_read_data_to_buffer);

enum xlink_error xlink_read_data_to_buffer_crc(struct xlink_handle *handle,
		uint16_t chan, uint8_t * const message, uint32_t *size)
{
	enum xlink_error rc = 0;
	/* To be implemented */
	return rc;
}
EXPORT_SYMBOL(xlink_read_data_to_buffer_crc);

enum xlink_error xlink_release_data(struct xlink_handle *handle,
		uint16_t chan, uint8_t * const data_addr)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_RELEASE_REQ, &link->handle,
			chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = data_addr;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL(xlink_release_data);

enum xlink_error xlink_disconnect(struct xlink_handle *handle)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	int interface = NULL_INTERFACE;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	// decrement refcount, if count is 0 lock mutex and disconnect
	if (kref_put_mutex(&link->refcount, release_after_kref_put,
			&xlink->lock)) {
		// stop dispatcher
		interface = get_interface_from_sw_device_id(
				link->handle.sw_device_id);
		if (interface != IPC_INTERFACE) {
			// stop dispatcher
			rc = xlink_dispatcher_stop(link->id);
			if (rc != X_LINK_SUCCESS) {
				pr_err("dispatcher stop failed\n");
				mutex_unlock(&xlink->lock);
				return X_LINK_ERROR;
			}
		}
		// deinitialize multiplexer connection
		rc = xlink_multiplexer_disconnect(link->id);
		if (rc) {
			pr_err("multiplexer disconnect failed\n");
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// TODO: reset device?
		// invalidate link handle reference
		link->handle.sw_device_id = XLINK_INVALID_SW_DEVICE_ID;
		xlink->nmb_connected_links--;
		mutex_unlock(&xlink->lock);
	}
	return rc;
}
EXPORT_SYMBOL(xlink_disconnect);

enum xlink_error xlink_get_device_list(uint32_t *sw_device_id_list,
		uint32_t *num_devices)
{
	enum xlink_error rc = 0;
	uint32_t interface_nmb_devices = 0;
	int i = 0;

	if (!xlink)
		return X_LINK_ERROR;

	if (!sw_device_id_list || !num_devices)
		return X_LINK_ERROR;

	/* loop through each interface and combine the lists */
	for (i = 0; i < NMB_OF_INTERFACES; i++) {
		rc = xlink_platform_get_device_list(i, sw_device_id_list,
				&interface_nmb_devices);
		if (!rc) {
			*num_devices += interface_nmb_devices;
			sw_device_id_list += interface_nmb_devices;
		}
		interface_nmb_devices = 0;
	}
	return X_LINK_SUCCESS;
}
EXPORT_SYMBOL(xlink_get_device_list);

enum xlink_error xlink_get_device_name(struct xlink_handle *handle, char *name,
		size_t name_size)
{
	enum xlink_error rc = 0;
	int interface = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (!name || !name_size)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_get_device_name(interface, handle->sw_device_id,
			name, name_size);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_get_device_name);

enum xlink_error xlink_get_device_status(struct xlink_handle *handle,
		uint32_t *device_status)
{
	enum xlink_error rc = 0;
	uint32_t interface = 0;

	if (!xlink)
		return X_LINK_ERROR;

	if (!device_status)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_get_device_status(interface, handle->sw_device_id,
			device_status);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_get_device_status);

enum xlink_error xlink_boot_device(struct xlink_handle *handle,
		const char *binary_name)
{
	enum xlink_error rc = 0;
	uint32_t interface = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (!binary_name)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_boot_device(interface, handle->sw_device_id,
			binary_name);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_boot_device);

enum xlink_error xlink_reset_device(struct xlink_handle *handle)
{
	enum xlink_error rc = 0;
	uint32_t interface = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_reset_device(interface, handle->sw_device_id);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_reset_device);

enum xlink_error xlink_set_device_mode(struct xlink_handle *handle,
		enum xlink_device_power_mode power_mode)
{
	enum xlink_error rc = 0;
	uint32_t interface = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_set_device_mode(interface, handle->sw_device_id,
			power_mode);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_set_device_mode);

enum xlink_error xlink_get_device_mode(struct xlink_handle *handle,
		enum xlink_device_power_mode *power_mode)
{
	enum xlink_error rc = 0;
	uint32_t interface = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	interface = get_interface_from_sw_device_id(handle->sw_device_id);
	if (interface == NULL_INTERFACE)
		return X_LINK_ERROR;

	rc = xlink_platform_get_device_mode(interface, handle->sw_device_id,
			power_mode);
	if (rc)
		rc = X_LINK_ERROR;
	else
		rc = X_LINK_SUCCESS;
	return rc;
}
EXPORT_SYMBOL(xlink_get_device_mode);

/* Device tree driver match. */
static const struct of_device_id kmb_xlink_of_match[] = {
	{
		.compatible = "intel,keembay-xlink",
	},
	{}
};

/* The xlink driver is a platform device. */
static struct platform_driver kmb_xlink_driver = {
	.probe = kmb_xlink_probe,
	.remove = kmb_xlink_remove,
	.driver = {
			.name = DRV_NAME,
			.of_match_table = kmb_xlink_of_match,
		},
};

/*
 * The remote host system will need to create an xlink platform
 * device for the platform driver to match with
 */
#ifndef CONFIG_XLINK_LOCAL_HOST
static struct platform_device pdev;
void kmb_xlink_release(struct device *dev) { return; }
#endif

static int kmb_xlink_init(void)
{
	int rc = 0;

	rc = platform_driver_register(&kmb_xlink_driver);
#ifndef CONFIG_XLINK_LOCAL_HOST
	pdev.dev.release = kmb_xlink_release;
	pdev.name = DRV_NAME;
	pdev.id = -1;
	if (!rc) {
		rc = platform_device_register(&pdev);
		if (rc)
			platform_driver_unregister(&kmb_xlink_driver);
	}
#endif
	return rc;
}
module_init(kmb_xlink_init);

static void kmb_xlink_exit(void)
{
#ifndef CONFIG_XLINK_LOCAL_HOST
	platform_device_unregister(&pdev);
#endif
	platform_driver_unregister(&kmb_xlink_driver);
}
module_exit(kmb_xlink_exit);

MODULE_DESCRIPTION("KeemBay xlink Kernel Driver");
MODULE_AUTHOR("Seamus Kelly <seamus.kelly@intel.com>");
MODULE_LICENSE("GPL v2");
