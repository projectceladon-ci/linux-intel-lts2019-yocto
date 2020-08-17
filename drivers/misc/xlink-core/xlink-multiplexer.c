// SPDX-License-Identifier: GPL-2.0-only
/*
 * xlink Multiplexer.
 *
 * Copyright (C) 2018-2019 Intel Corporation
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/sched/signal.h>

#ifdef CONFIG_XLINK_LOCAL_HOST
#include <linux/xlink-ipc.h>
#endif

#include "xlink-multiplexer.h"
#include "xlink-dispatcher.h"
#include "xlink-platform.h"

#define THR_UPR 85
#define THR_LWR 80

// timeout used for open channel
#define OPEN_CHANNEL_TIMEOUT_MSEC 5000

struct remote_allocs {
	void *virt;
	dma_addr_t paddr;
	struct list_head list;
};
static struct remote_allocs rmt_queue;

/* Channel mapping table. */
struct xlink_channel_type {
	enum xlink_interface remote_to_local;
	enum xlink_interface local_to_ip;
};

struct xlink_channel_table_entry {
	uint16_t start_range;
	uint16_t stop_range;
	struct xlink_channel_type type;
};

const struct xlink_channel_table_entry default_channel_table[] = {
	{0x0, 0x1, {PCIE_INTERFACE, IPC_INTERFACE}},
	{0x2, 0x9, {USB_CDC_INTERFACE, IPC_INTERFACE}},
	{0xA, 0x3FD, {PCIE_INTERFACE, IPC_INTERFACE}},
	{0x3FE, 0x3FF, {ETH_INTERFACE, IPC_INTERFACE}},
	{0x400, 0xFFE, {PCIE_INTERFACE, NULL_INTERFACE}},
	{0xFFF, 0xFFF, {ETH_INTERFACE, NULL_INTERFACE}},
	{NMB_CHANNELS, NMB_CHANNELS, {NULL_INTERFACE, NULL_INTERFACE}},
};

struct channel {
	struct open_channel *opchan;
	enum xlink_opmode mode;
	enum xlink_channel_status status;
	enum xlink_channel_status ipc_status;
	uint32_t size;
	uint32_t timeout;
};

struct packet {
	uint8_t *data;
	uint32_t length;
	dma_addr_t paddr;
	struct list_head list;
};

struct packet_queue {
	uint32_t count;
	uint32_t capacity;
	struct list_head head;
	struct mutex lock;
};

struct open_channel {
	uint16_t id;
	struct channel *chan;
	struct packet_queue rx_queue;
	struct packet_queue tx_queue;
	int32_t rx_fill_level;
	int32_t tx_fill_level;
	int32_t tx_packet_level;
	int32_t tx_up_limit;
	struct completion opened;
	struct completion pkt_available;
	struct completion pkt_consumed;
	struct completion pkt_released;
	struct task_struct *ready_calling_pid;
	void *ready_callback;
	struct task_struct *consumed_calling_pid;
	void *consumed_callback;
	char callback_origin;
	struct mutex lock;
};

struct xlink_multiplexer {
	struct device *dev;
	struct channel channels[XLINK_MAX_CONNECTIONS][NMB_CHANNELS];
};

static struct xlink_multiplexer *xmux;

/*
 * Multiplexer Internal Functions
 *
 */

int unregister_allocated_buffer(void *buf, dma_addr_t paddr)
{
	struct remote_allocs *rmt = NULL;
	uint8_t alloc_found = 0;

	// find packet in channel rx queue
	list_for_each_entry(rmt, &rmt_queue.list, list) {
		if (rmt->virt == buf && rmt->paddr == paddr) {
			alloc_found = 1;
			break;
		}
	}
	if (!rmt || !alloc_found) {
		//mutex_unlock(&queue->lock);
		return X_LINK_ERROR;
	}
	list_del(&rmt->list);
	kfree(rmt);
	return X_LINK_SUCCESS;
}

static int register_allocated_buffer(void *buf, dma_addr_t paddr)
{
	struct remote_allocs *rmt = NULL;

	rmt = kzalloc(sizeof(*rmt), GFP_KERNEL);
	if (!rmt)
		return X_LINK_ERROR;

	rmt->virt = buf;
	rmt->paddr = paddr;
	list_add_tail(&rmt->list, &rmt_queue.list);
	return X_LINK_SUCCESS;
}

void *find_allocated_buffer(dma_addr_t paddr)
{
	struct remote_allocs *rmt = NULL;
	void *virtaddr = NULL;

	// find allocated buffer given phys addr
	list_for_each_entry(rmt, &rmt_queue.list, list) {
		if (rmt->paddr == paddr) {
			virtaddr = rmt->virt;
			break;
		}
	}
	if (!virtaddr)
		pr_err("%s: alloc NOT found %llx %p\n", __func__,
				paddr, virtaddr);

	return virtaddr;
}

static enum xlink_error run_callback(struct open_channel *opchan,
		void *callback,	struct task_struct *pid)
{
	enum xlink_error rc = X_LINK_SUCCESS;
	int ret;
	void (*func)(int chan);
#if KERNEL_VERSION(4, 20, 0) > LINUX_VERSION_CODE
	struct siginfo info;

	memset(&info, 0, sizeof(struct siginfo));
#else
	struct kernel_siginfo info;

	memset(&info, 0, sizeof(struct kernel_siginfo));
#endif

	if (opchan->callback_origin == 'U') { // user-space origin
		if (pid != NULL) {
			info.si_signo = SIGXLNK;
			info.si_code = SI_QUEUE;
			info.si_errno = opchan->id;
			info.si_ptr = callback;
			ret = send_sig_info(SIGXLNK, &info, pid);
			if (ret < 0) {
				pr_err("Unable to send signal %d\n", ret);
				rc = X_LINK_ERROR;
			}
		} else {
			pr_err("CHAN 0x%x -- calling_pid == NULL\n",
					opchan->id);
			rc = X_LINK_ERROR;
		}
	} else { // kernel origin
		func = callback;
		func(opchan->id);
	}
	return rc;
}

static inline int chan_is_non_blocking_read(struct open_channel *opchan)
{
	if ((opchan->chan->mode == RXN_TXN) ||
			(opchan->chan->mode == RXN_TXB)) {
		return 1;
	}
	return 0;
}
static inline int chan_is_non_blocking_write(struct open_channel *opchan)
{
	if ((opchan->chan->mode == RXN_TXN) ||
			(opchan->chan->mode == RXB_TXN)) {
		return 1;
	}
	return 0;
}


static struct xlink_channel_type const *get_channel_type(uint16_t chan)
{
	int i = 0;
	struct xlink_channel_type const *type = NULL;

	while (default_channel_table[i].start_range < NMB_CHANNELS) {
		if ((chan >= default_channel_table[i].start_range) &&
				(chan <= default_channel_table[i].stop_range)) {
			type = &default_channel_table[i].type;
			break;
		}
		i++;
	}
	return type;
}

static int is_channel_for_device(uint16_t chan, uint32_t sw_device_id,
		enum xlink_dev_type dev_type)
{
	int interface = NULL_INTERFACE;
	struct xlink_channel_type const *chan_type = get_channel_type(chan);

	if (chan_type) {
		interface = get_interface_from_sw_device_id(sw_device_id);
		if (dev_type == VPUIP_DEVICE) {
			if (chan_type->local_to_ip == interface)
				return 1;
		} else {
			if (chan_type->remote_to_local == interface)
				return 1;
		}
	}
	return 0;
}

static int is_enough_space_in_channel(struct open_channel *opchan,
		uint32_t size)
{
	if (opchan->tx_packet_level
			>= ((XLINK_PACKET_QUEUE_CAPACITY/100)*THR_UPR)) {
		pr_info("Packet queue limit reached\n");
		return 0;
	}
	if (opchan->tx_up_limit == 0) {
		if ((opchan->tx_fill_level + size)
				> ((opchan->chan->size / 100) * THR_UPR)) {
			opchan->tx_up_limit = 1;
			return 0;
		}
	}
	if (opchan->tx_up_limit == 1) {
		if ((opchan->tx_fill_level + size)
				< ((opchan->chan->size / 100) * THR_LWR)) {
			opchan->tx_up_limit = 0;
			return 1;
		} else {
			return 0;
		}
	}
	return 1;
}
static bool is_passthru_channel(int chan)
{
	struct xlink_channel_type const *chan_type = get_channel_type(chan);

	if (chan_type->local_to_ip == IPC_INTERFACE)
		return true;
	return false;
}


static int is_control_channel(uint16_t chan)
{
	if ((chan == IP_CONTROL_CHANNEL) || (chan == VPU_CONTROL_CHANNEL))
		return 1;
	else
		return 0;
}

static struct open_channel *get_channel(uint32_t link_id, uint16_t chan)
{
	if (!xmux->channels[link_id][chan].opchan)
		return NULL;
	mutex_lock(&xmux->channels[link_id][chan].opchan->lock);
	return xmux->channels[link_id][chan].opchan;
}

static void release_channel(struct open_channel *opchan)
{
	if (opchan)
		mutex_unlock(&opchan->lock);
}

static int add_packet_to_channel(struct open_channel *opchan,
		struct packet_queue *queue, void *buffer, uint32_t size,
		dma_addr_t paddr)
{
	struct packet *pkt = NULL;

	if (queue->count < queue->capacity) {
		pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
		if (!pkt)
			return X_LINK_ERROR;

		pkt->data = buffer;
		pkt->length = size;
		pkt->paddr = paddr;
		list_add_tail(&pkt->list, &queue->head);
		queue->count++;
		opchan->rx_fill_level += pkt->length;
	}
	return X_LINK_SUCCESS;
}

static struct packet *get_packet_from_channel(struct packet_queue *queue)
{
	struct packet *pkt = NULL;

	// get first packet in queue
	if (!list_empty(&queue->head))
		pkt = list_first_entry(&queue->head, struct packet, list);
	return pkt;
}

static int release_packet_from_channel(struct open_channel *opchan,
		struct packet_queue *queue, uint8_t * const addr,
		uint32_t *size)
{
	uint8_t packet_found = 0;
	struct packet *pkt = NULL;

	if (!addr) {
		// address is null, release first packet in queue
		if (!list_empty(&queue->head)) {
			pkt = list_first_entry(&queue->head, struct packet,
					list);
			packet_found = 1;
		}
	} else {
		// find packet in channel rx queue
		list_for_each_entry(pkt, &queue->head, list) {
			if (pkt->data == addr) {
				packet_found = 1;
				break;
			}
		}
	}
	if (!pkt || !packet_found)
		return X_LINK_ERROR;
	// packet found, deallocate and remove from queue
	xlink_platform_deallocate(xmux->dev, pkt->data, pkt->paddr, pkt->length,
			XLINK_PACKET_ALIGNMENT, XLINK_NORMAL_MEMORY);
	list_del(&pkt->list);
	queue->count--;
	opchan->rx_fill_level -= pkt->length;
	if (size)
		*size = pkt->length;
	kfree(pkt);
	// pr_info("Release of %u on channel 0x%x: rx fill level = %u / %u\n",
	//		pkt->length, opchan->id, opchan->rx_fill_level,
	//		opchan->chan->size);
	return X_LINK_SUCCESS;
}

int core_release_packet_from_channel(uint32_t link_id, uint16_t chan,
		uint8_t * const addr)
{
#ifndef CONFIG_XLINK_LOCAL_HOST
	int rc = 0;

	if (is_passthru_channel(chan)) {
		struct open_channel *opchan = NULL;

		opchan = get_channel(link_id, chan);
		rc = release_packet_from_channel(opchan, &opchan->rx_queue,
				addr, NULL);
		release_channel(opchan);
		return rc;
	} else
		return X_LINK_SUCCESS;
#else
		return X_LINK_SUCCESS;
#endif
}

static int multiplexer_open_channel(uint32_t link_id, uint16_t chan)
{
	struct open_channel *opchan = NULL;

	// channel already open
	if (xmux->channels[link_id][chan].opchan != NULL)
		return X_LINK_SUCCESS;

	// allocate open channel
	opchan = kzalloc(sizeof(*opchan), GFP_KERNEL);
	if (!opchan)
		return X_LINK_ERROR;

	// initialize open channel
	opchan->id = chan;
	opchan->chan = &xmux->channels[link_id][chan];
	// TODO: remove circular dependency
	xmux->channels[link_id][chan].opchan = opchan;
	INIT_LIST_HEAD(&opchan->rx_queue.head);
	opchan->rx_queue.count = 0;
	opchan->rx_queue.capacity = XLINK_PACKET_QUEUE_CAPACITY;
	INIT_LIST_HEAD(&opchan->tx_queue.head);
	opchan->tx_queue.count = 0;
	opchan->tx_queue.capacity = XLINK_PACKET_QUEUE_CAPACITY;
	opchan->rx_fill_level = 0;
	opchan->tx_fill_level = 0;
	opchan->tx_packet_level = 0;
	opchan->tx_up_limit = 0;
	init_completion(&opchan->opened);
	init_completion(&opchan->pkt_available);
	init_completion(&opchan->pkt_consumed);
	init_completion(&opchan->pkt_released);
	mutex_init(&opchan->lock);
	return X_LINK_SUCCESS;
}

static int multiplexer_close_channel(struct open_channel *opchan)
{
	if (!opchan)
		return X_LINK_ERROR;

	// free remaining packets
	while (!list_empty(&opchan->rx_queue.head)) {
		release_packet_from_channel(opchan, &opchan->rx_queue,
				NULL, NULL);
	}

	while (!list_empty(&opchan->tx_queue.head)) {
		release_packet_from_channel(opchan, &opchan->tx_queue,
				NULL, NULL);
	}

	// deallocate data structure and destroy
	opchan->chan->opchan = NULL; // TODO: remove circular dependency
	mutex_destroy(&opchan->rx_queue.lock);
	mutex_destroy(&opchan->tx_queue.lock);
	mutex_unlock(&opchan->lock);
	mutex_destroy(&opchan->lock);
	kfree(opchan);
	return X_LINK_SUCCESS;
}

/*
 * Multiplexer External Functions
 *
 */

enum xlink_error xlink_multiplexer_init(void *dev)
{
	struct platform_device *plat_dev = (struct platform_device *) dev;

	// allocate multiplexer data structure
	xmux = kzalloc(sizeof(*xmux), GFP_KERNEL);
	if (!xmux)
		return X_LINK_ERROR;

	xmux->dev = &plat_dev->dev;
	INIT_LIST_HEAD(&rmt_queue.list);
	return X_LINK_SUCCESS;
}

enum xlink_error xlink_multiplexer_connect(uint32_t link_id)
{
	int rc = 0;

	if (!xmux)
		return X_LINK_ERROR;

	// open ip control channel
	rc = multiplexer_open_channel(link_id, IP_CONTROL_CHANNEL);
	if (rc) {
		goto r_cleanup;
	} else {
		xmux->channels[link_id][IP_CONTROL_CHANNEL].size = CONTROL_CHANNEL_DATASIZE;
		xmux->channels[link_id][IP_CONTROL_CHANNEL].timeout = CONTROL_CHANNEL_TIMEOUT_MS;
		xmux->channels[link_id][IP_CONTROL_CHANNEL].mode = CONTROL_CHANNEL_OPMODE;
		xmux->channels[link_id][IP_CONTROL_CHANNEL].status = CHAN_OPEN;
	}
	// open vpu control channel
	rc = multiplexer_open_channel(link_id, VPU_CONTROL_CHANNEL);
	if (rc) {
		goto r_cleanup;
	} else {
		xmux->channels[link_id][VPU_CONTROL_CHANNEL].size = CONTROL_CHANNEL_DATASIZE;
		xmux->channels[link_id][VPU_CONTROL_CHANNEL].timeout = CONTROL_CHANNEL_TIMEOUT_MS;
		xmux->channels[link_id][VPU_CONTROL_CHANNEL].mode = CONTROL_CHANNEL_OPMODE;
		xmux->channels[link_id][VPU_CONTROL_CHANNEL].status = CHAN_OPEN;
	}
	return X_LINK_SUCCESS;

r_cleanup:
	xlink_multiplexer_disconnect(link_id);
	return X_LINK_ERROR;
}

enum xlink_error xlink_multiplexer_disconnect(uint32_t link_id)
{
	int i = 0;

	if (!xmux)
		return X_LINK_ERROR;

	for (i = 0; i < NMB_CHANNELS; i++) {
		if (xmux->channels[link_id][i].opchan)
			multiplexer_close_channel(xmux->channels[link_id][i].opchan);
	}
	return X_LINK_SUCCESS;
}

enum xlink_error xlink_multiplexer_destroy(void)
{
	int i = 0;

	if (!xmux)
		return X_LINK_ERROR;

	// close all open channels and deallocate remaining packets
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++)
		xlink_multiplexer_disconnect(i);

	// destroy multiplexer
	kfree(xmux);
	xmux = NULL;
	return X_LINK_SUCCESS;
}

enum xlink_error xlink_multiplexer_tx(struct xlink_event *event,
		int *event_queued)
{
	int rc = X_LINK_SUCCESS;
	struct open_channel *opchan = NULL;
	struct xlink_event  *passthru_event = NULL;
	struct packet *pkt = NULL;
	uint32_t size = 0;
	uint32_t link_id = 0;
	uint16_t chan = 0;

	if (!xmux || !event)
		return X_LINK_ERROR;

	link_id = event->link_id;
	chan = event->header.chan;

	// verify channel ID is in range
	if (chan >= NMB_CHANNELS)
		return X_LINK_ERROR;

	// verify communication to device on channel is valid
	if (!is_channel_for_device(chan, event->handle->sw_device_id,
			event->handle->dev_type))
		return X_LINK_ERROR;

	// verify this is not a control channel
	if (is_control_channel(chan))
		return X_LINK_ERROR;

	if (chan < XLINK_IPC_MAX_CHANNELS &&
			event->interface == IPC_INTERFACE) {
		// event should be handled by passthrough
		rc = xlink_passthrough(event);
		// kfree(event);
	} else {
		// event should be handled by dispatcher
		switch (event->header.type) {
		case XLINK_WRITE_REQ:
		case XLINK_WRITE_VOLATILE_REQ:
		case XLINK_WRITE_CONTROL_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan || (opchan->chan->status != CHAN_OPEN)) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				event->header.timeout = opchan->chan->timeout;
				while (!is_enough_space_in_channel(opchan,
						event->header.size)) {
					if ((opchan->chan->mode == RXN_TXB) ||
							(opchan->chan->mode == RXB_TXB)) {
						// channel is blocking, wait for packet to be released
						// TODO: calculate timeout remainder after each loop
						if (opchan->chan->timeout == 0) {
							mutex_unlock(&opchan->lock);
							rc = wait_for_completion_interruptible(
									&opchan->pkt_released);
							mutex_lock(&opchan->lock);
							if (rc < 0) {
								// wait interrupted
								rc = X_LINK_ERROR;
								break;
							}
						} else {
							mutex_unlock(&opchan->lock);
							rc = wait_for_completion_interruptible_timeout(
									&opchan->pkt_released,
									msecs_to_jiffies(opchan->chan->timeout));
							mutex_lock(&opchan->lock);
							if (rc == 0) {
								rc = X_LINK_TIMEOUT;
								break;
							} else if (rc < 0) {
								// wait interrupted
								rc = X_LINK_ERROR;
								break;
							} else if (rc > 0) {
								rc = X_LINK_SUCCESS;
							}
						}
					} else {
						rc = X_LINK_CHAN_FULL;
						break;
					}

				}
				if (rc == X_LINK_SUCCESS) {
					opchan->tx_fill_level += event->header.size;
					opchan->tx_packet_level++;
					if (is_passthru_channel(chan)) {
						/* pass this data on to VPU via IPC */
						switch (event->header.type) {
						case XLINK_WRITE_REQ:
							event->header.type = XLINK_PASSTHRU_WRITE_REQ;
							break;
						case XLINK_WRITE_CONTROL_REQ:
						case XLINK_WRITE_VOLATILE_REQ:
							event->header.type = XLINK_PASSTHRU_VOLATILE_WRITE_REQ;
							break;
						default:
							rc = X_LINK_ERROR;
						}
						xlink_dispatcher_event_add(EVENT_TX, event);
						*event_queued = 1;
					} else {
						xlink_dispatcher_event_add(EVENT_TX, event);
						*event_queued = 1;
						if ((opchan->chan->mode == RXN_TXB) ||
								(opchan->chan->mode == RXB_TXB)) {
							// channel is blocking, wait for packet to be consumed
							// TODO: calculate timeout remainder since last wait
							mutex_unlock(&opchan->lock);
							if (opchan->chan->timeout == 0) {
								rc = wait_for_completion_interruptible(
										&opchan->pkt_consumed);
								// reinit_completion(&opchan->pkt_consumed);
								if (rc < 0) {
									// wait interrupted
									rc = X_LINK_ERROR;
								}
							} else {
								rc = wait_for_completion_interruptible_timeout(
										&opchan->pkt_consumed,
										msecs_to_jiffies(opchan->chan->timeout));
								// reinit_completion(&opchan->pkt_consumed);
								if (rc == 0) {
									rc = X_LINK_TIMEOUT;
								} else if (rc < 0) {
									// wait interrupted
									rc = X_LINK_ERROR;
								} else if (rc > 0) {
									rc = X_LINK_SUCCESS;
								}
							}
							mutex_lock(&opchan->lock);
						}
					}
				}
			}
			release_channel(opchan);
			break;
		case XLINK_READ_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan || (opchan->chan->status != CHAN_OPEN)) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				if (is_passthru_channel(chan)) {
					passthru_event = xlink_create_event(link_id,
							XLINK_PASSTHRU_READ_REQ, event->handle, chan, 0,
							opchan->chan->timeout);
					xlink_dispatcher_event_add(EVENT_TX, passthru_event);
					event->header.type = XLINK_PASSTHRU_READ_REQ;
					*event_queued = 1;
				}
				event->header.timeout = opchan->chan->timeout;
				if ((opchan->chan->mode == RXB_TXN) ||
						(opchan->chan->mode == RXB_TXB)) {
					// channel is blocking, wait for packet to become available
					mutex_unlock(&opchan->lock);
					if (opchan->chan->timeout == 0) {
						rc = wait_for_completion_interruptible(
								&opchan->pkt_available);
					} else {
						rc = wait_for_completion_interruptible_timeout(
								&opchan->pkt_available,
								msecs_to_jiffies(opchan->chan->timeout));
						if (rc == 0) {
							rc = X_LINK_TIMEOUT;
						} else if (rc < 0) {
							// wait interrupted
							rc = X_LINK_ERROR;
						} else if (rc > 0) {
							rc = X_LINK_SUCCESS;
						}
					}
					mutex_lock(&opchan->lock);
				}
				if (rc == X_LINK_SUCCESS) {
					pkt = get_packet_from_channel(&opchan->rx_queue);
					if (pkt) {
						if (event->header.type == XLINK_PASSTHRU_READ_REQ) {
							event->header.type = XLINK_READ_REQ;
							*event->pdata = pkt->data;
							*event->length = pkt->length;
						} else {
							*(uint32_t **)event->pdata = (uint32_t *)pkt->data;
						}
						*event->length = pkt->length;
						xlink_dispatcher_event_add(EVENT_TX, event);
						*event_queued = 1;
					} else {
						rc = X_LINK_ERROR;
					}
				}
			}
			release_channel(opchan);
			break;
		case XLINK_READ_TO_BUFFER_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan || (opchan->chan->status != CHAN_OPEN)) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				if (is_passthru_channel(chan)) {
					passthru_event = xlink_create_event(link_id,
							XLINK_PASSTHRU_READ_TO_BUFFER_REQ, event->handle,
							chan, 0, opchan->chan->timeout);
					xlink_dispatcher_event_add(EVENT_TX, passthru_event);
				}
				event->header.timeout = opchan->chan->timeout;
				if ((opchan->chan->mode == RXB_TXN) ||
						(opchan->chan->mode == RXB_TXB)) {
					// channel is blocking, wait for packet to become available
					mutex_unlock(&opchan->lock);
					if (opchan->chan->timeout == 0) {
						rc = wait_for_completion_interruptible(
								&opchan->pkt_available);
					} else {
						rc = wait_for_completion_interruptible_timeout(
								&opchan->pkt_available,
								msecs_to_jiffies(opchan->chan->timeout));
						if (rc == 0) {
							rc = X_LINK_TIMEOUT;
						} else if (rc > 0) {
							rc = X_LINK_SUCCESS;
						} else if (rc < 0) {
							// wait interrupted
							rc = X_LINK_ERROR;
						}
					}
					mutex_lock(&opchan->lock);
				}
				if (rc == X_LINK_SUCCESS) {
					pkt = get_packet_from_channel(&opchan->rx_queue);
					if (pkt) {
						memcpy(event->data, pkt->data, pkt->length);
						if (is_passthru_channel(chan)) {
							rc = release_packet_from_channel(opchan,
									&opchan->rx_queue, pkt->data, &size);
						}
						*event->length = pkt->length;
						xlink_dispatcher_event_add(EVENT_TX, event);
						*event_queued = 1;
					} else {
						rc = X_LINK_ERROR;
					}
				}
			}
			release_channel(opchan);
			break;
		case XLINK_RELEASE_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				rc = release_packet_from_channel(opchan, &opchan->rx_queue,
						event->data, &size);
				if (rc) {
					rc = X_LINK_ERROR;
				} else {
					event->header.size = size;
					xlink_dispatcher_event_add(EVENT_TX, event);
					*event_queued = 1;
				}
			}
			release_channel(opchan);
			break;
		case XLINK_OPEN_CHANNEL_REQ:
			if (xmux->channels[link_id][chan].status == CHAN_CLOSED) {
				xmux->channels[link_id][chan].size = event->header.size;
				xmux->channels[link_id][chan].timeout = event->header.timeout;
				xmux->channels[link_id][chan].mode = (uintptr_t)event->data;
				rc = multiplexer_open_channel(link_id, chan);
				if (rc) {
					rc = X_LINK_ERROR;
				} else {
					opchan = get_channel(link_id, chan);
					if (!opchan) {
						rc = X_LINK_COMMUNICATION_FAIL;
					} else {
						xlink_dispatcher_event_add(EVENT_TX, event);
						*event_queued = 1;
						mutex_unlock(&opchan->lock);
						rc = wait_for_completion_interruptible_timeout(
								&opchan->opened,
								msecs_to_jiffies(OPEN_CHANNEL_TIMEOUT_MSEC));
						mutex_lock(&opchan->lock);
						if (rc == 0) {
							rc = X_LINK_TIMEOUT;
						} else if (rc > 0) {
							rc = X_LINK_SUCCESS;
						} else if (rc < 0) {
							// wait interrupted
							rc = X_LINK_ERROR;
						}
						if (rc == 0) {
							xmux->channels[link_id][chan].status = CHAN_OPEN;
							release_channel(opchan);
						} else {
							multiplexer_close_channel(opchan);
						}
					}
				}
			} else if (xmux->channels[link_id][chan].status == CHAN_OPEN_PEER) {
				/* channel already open */
				xmux->channels[link_id][chan].status = CHAN_OPEN; // opened locally
				xmux->channels[link_id][chan].size = event->header.size;
				xmux->channels[link_id][chan].timeout = event->header.timeout;
				xmux->channels[link_id][chan].mode = (uintptr_t)event->data;
				rc = multiplexer_open_channel(link_id, chan);
				rc = X_LINK_SUCCESS;
			} else {
				/* channel already open */
				rc = X_LINK_ALREADY_OPEN;
			}
			break;
		case XLINK_DATA_READY_CALLBACK_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				opchan->ready_callback = event->data;
				opchan->ready_calling_pid = event->calling_pid;
				opchan->callback_origin = event->callback_origin;
				pr_info("xlink ready callback process registered - %lx chan %d\n",
						(uintptr_t)event->calling_pid, chan);
			}
			release_channel(opchan);
			break;
		case XLINK_DATA_CONSUMED_CALLBACK_REQ:
			opchan = get_channel(link_id, chan);
			if (!opchan) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				opchan->consumed_callback = event->data;
				opchan->consumed_calling_pid = event->calling_pid;
				opchan->callback_origin = event->callback_origin;
				pr_info("xlink consumed callback process registered - %lx chan %d\n",
						(uintptr_t)event->calling_pid, chan);
			}
			release_channel(opchan);
			break;
		case XLINK_CLOSE_CHANNEL_REQ:
			if (xmux->channels[link_id][chan].status == CHAN_OPEN) {
				opchan = get_channel(link_id, chan);
				rc = multiplexer_close_channel(opchan);
				if (rc)
					rc = X_LINK_ERROR;
				else
					xmux->channels[link_id][chan].status = CHAN_CLOSED;
			} else {
				/* can't close channel not open */
				rc = X_LINK_ERROR;
			}
			break;
		case XLINK_PING_REQ:
			break;
		case XLINK_WRITE_RESP:
		case XLINK_WRITE_VOLATILE_RESP:
		case XLINK_WRITE_CONTROL_RESP:
		case XLINK_READ_RESP:
		case XLINK_READ_TO_BUFFER_RESP:
		case XLINK_RELEASE_RESP:
		case XLINK_OPEN_CHANNEL_RESP:
		case XLINK_CLOSE_CHANNEL_RESP:
		case XLINK_PING_RESP:
		default:
			rc = X_LINK_ERROR;
		}
	}
	return rc;
}

enum xlink_error xlink_multiplexer_rx(struct xlink_event *event)
{
	int rc = X_LINK_SUCCESS;
	struct open_channel *opchan = NULL;
	void *buffer = NULL;
	size_t size = 0;
	uint32_t link_id = 0;
	uint16_t chan = 0;
	uint32_t len = 0;
	uint32_t *addr;
	dma_addr_t paddr = 0;
	struct xlink_event *passthru_event = NULL;

	if (!xmux || !event)
		return X_LINK_ERROR;

	link_id = event->link_id;
	chan = event->header.chan;

	switch (event->header.type) {
	case XLINK_PASSTHRU_READ_REQ:
		event->length = &len;
		event->pdata = (void **)&addr;
		opchan = get_channel(link_id, chan);
		if ((opchan->chan->mode == RXB_TXN) ||
				(opchan->chan->mode == RXB_TXB)) {
			//add to ipc blocking read queue
			rc = xlink_dispatcher_ipc_passthru_event_add(event);
		} else {
			rc = xlink_passthrough(event);
			if (rc == X_LINK_SUCCESS) {
				passthru_event = xlink_create_event(link_id,
						XLINK_WRITE_REQ, event->handle, chan, len,
						event->header.timeout);
				passthru_event->paddr = *(uint32_t *)event->pdata;
				passthru_event->data = find_allocated_buffer(
						passthru_event->paddr);
				if (passthru_event->data == NULL) {
					xlink_destroy_event(event); // event is handled and can now be freed
					rc = X_LINK_ERROR;
				} else {
					xlink_dispatcher_event_add(EVENT_RX, passthru_event);
					unregister_allocated_buffer(passthru_event->data,
							passthru_event->paddr);
				}
			}
		}
		release_channel(opchan);
		break;
	case XLINK_PASSTHRU_READ_TO_BUFFER_REQ:
		opchan = get_channel(link_id, chan);
		event->length = &len;
		if ((opchan->chan->mode == RXB_TXN) ||
				(opchan->chan->mode == RXB_TXB)) {
			//add to ipc blocking read queue
			rc = xlink_dispatcher_ipc_passthru_event_add(event);
		} else {
			rc = xlink_passthrough(event);
			if (rc == X_LINK_SUCCESS) {
				passthru_event = xlink_create_event(link_id,
						XLINK_WRITE_REQ, event->handle, chan, len,
						event->header.timeout);
				passthru_event->data = event->data;
				passthru_event->paddr = event->paddr;
				xlink_dispatcher_event_add(EVENT_RX, passthru_event);
			} else {
				xlink_destroy_event(event); // event is handled and can now be freed
			}
		}
		release_channel(opchan);
		break;
	case XLINK_PASSTHRU_WRITE_REQ:
		opchan = get_channel(link_id, chan);
		if (opchan == NULL) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			buffer = xlink_platform_allocate(xmux->dev, &paddr,
					event->header.size, XLINK_PACKET_ALIGNMENT,
					XLINK_CMA_MEMORY);
			if (buffer) {
				register_allocated_buffer(buffer, paddr);
				size = event->header.size;
				rc = xlink_platform_read(event->interface,
						event->handle->sw_device_id, buffer, &size,
						opchan->chan->timeout, NULL);
				if (rc || event->header.size != size) {
					xlink_platform_deallocate(xmux->dev, buffer, paddr,
							event->header.size, XLINK_PACKET_ALIGNMENT,
							XLINK_CMA_MEMORY);
					rc = X_LINK_ERROR;
					release_channel(opchan);
					break;
				}
				event->paddr = paddr;
				event->data = (void *)&paddr;
			} else {
				rc = X_LINK_ERROR;
			}
			if (rc == X_LINK_SUCCESS)
				rc = xlink_passthrough(event);
			if (rc == 0)
				xlink_destroy_event(event); // event is handled and can now be freed
		}
		release_channel(opchan);
		break;
	case XLINK_PASSTHRU_VOLATILE_WRITE_REQ:
		opchan = get_channel(link_id, chan);
		if (opchan == NULL) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			buffer = xlink_platform_allocate(xmux->dev, &paddr,
					event->header.size, XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			if (buffer) {
				size = event->header.size;
				rc = xlink_platform_read(event->interface,
						event->handle->sw_device_id, buffer, &size,
						opchan->chan->timeout, NULL);
				if (rc || event->header.size != size) {
					xlink_platform_deallocate(xmux->dev, buffer, paddr,
							event->header.size, XLINK_PACKET_ALIGNMENT,
							XLINK_NORMAL_MEMORY);
					rc = X_LINK_ERROR;
					release_channel(opchan);
					break;
				}
				event->data = buffer;
			} else {
				rc = X_LINK_ERROR;
			}
			if (rc == X_LINK_SUCCESS) {
				rc = xlink_passthrough(event);
				xlink_platform_deallocate(xmux->dev, buffer, paddr,
						event->header.size, XLINK_PACKET_ALIGNMENT,
						XLINK_CMA_MEMORY);
			}
			if (rc == 0)
				xlink_destroy_event(event); // event is handled and can now be freed
		}
		release_channel(opchan);
		break;
	case XLINK_WRITE_REQ:
	case XLINK_WRITE_VOLATILE_REQ:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			// printk(KERN_DEBUG "Write of size %lu on channel 0x%x, rx fill level = %u out of %u\n",
			//		event->header.size, chan, opchan->rx_fill_level,
			//		opchan->chan->size);
			buffer = xlink_platform_allocate(xmux->dev, &paddr,
					event->header.size, XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			if (buffer) {
				size = event->header.size;
				rc = xlink_platform_read(event->interface,
						event->handle->sw_device_id, buffer, &size,
						opchan->chan->timeout, NULL);
				if (rc || event->header.size != size) {
					xlink_platform_deallocate(xmux->dev, buffer, paddr,
							event->header.size, XLINK_PACKET_ALIGNMENT,
							XLINK_NORMAL_MEMORY);
					rc = X_LINK_ERROR;
					release_channel(opchan);
					break;
				}
				event->paddr = paddr;
				event->data = buffer;
				if (add_packet_to_channel(opchan, &opchan->rx_queue,
						event->data, event->header.size, paddr)) {
					xlink_platform_deallocate(xmux->dev, buffer, paddr,
							event->header.size, XLINK_PACKET_ALIGNMENT,
							XLINK_NORMAL_MEMORY);
					rc = X_LINK_ERROR;
					release_channel(opchan);
					break;
				}
				event->header.type = XLINK_WRITE_VOLATILE_RESP;
				xlink_dispatcher_event_add(EVENT_RX, event);
				//complete regardless of mode/timeout
				complete(&opchan->pkt_available);
				// run callback
				if (xmux->channels[link_id][chan].status == CHAN_OPEN &&
						chan_is_non_blocking_read(opchan) &&
						opchan->ready_callback != NULL) {
					rc = run_callback(opchan, opchan->ready_callback,
							opchan->ready_calling_pid);
					break;
				}
			} else {
				// failed to allocate buffer
				rc = X_LINK_ERROR;
			}
		}
		release_channel(opchan);
		break;
	case XLINK_WRITE_CONTROL_REQ:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			// printk(KERN_DEBUG "Write of size %u on channel 0x%x, rx fill level = %u out of %u\n",
			//		event->header.size, chan, opchan->rx_fill_level,
			//		opchan->chan->size);
			buffer = xlink_platform_allocate(xmux->dev, &paddr,
					event->header.size, XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			if (buffer) {
				size = event->header.size;
				memcpy(buffer, event->header.control_data, size);
				event->paddr = paddr;
				event->data = buffer;
				if (add_packet_to_channel(opchan, &opchan->rx_queue,
						event->data, event->header.size, paddr)) {
					xlink_platform_deallocate(xmux->dev, buffer, paddr,
							event->header.size, XLINK_PACKET_ALIGNMENT,
							XLINK_NORMAL_MEMORY);
					rc = X_LINK_ERROR;
					release_channel(opchan);
					break;
				}
				event->header.type = XLINK_WRITE_CONTROL_RESP;
				xlink_dispatcher_event_add(EVENT_RX, event);
				// channel blocking, notify waiting threads of available packet
				complete(&opchan->pkt_available);
			} else {
				// failed to allocate buffer
				rc = X_LINK_ERROR;
			}
		}
		release_channel(opchan);
		break;
	case XLINK_READ_REQ:
	case XLINK_READ_TO_BUFFER_REQ:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			event->header.type = XLINK_READ_TO_BUFFER_RESP;
			xlink_dispatcher_event_add(EVENT_RX, event);
			//complete regardless of mode/timeout
			complete(&opchan->pkt_consumed);
		}
		// run callback
		if (xmux->channels[link_id][chan].status == CHAN_OPEN &&
				chan_is_non_blocking_write(opchan) &&
				opchan->consumed_callback != NULL) {
			rc = run_callback(opchan, opchan->consumed_callback,
					opchan->consumed_calling_pid);
		}
		release_channel(opchan);
		break;
	case XLINK_RELEASE_REQ:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			event->header.timeout = opchan->chan->timeout;
			opchan->tx_fill_level -= event->header.size;
			opchan->tx_packet_level--;
			event->header.type = XLINK_RELEASE_RESP;
			xlink_dispatcher_event_add(EVENT_RX, event);
			//complete regardless of mode/timeout
			complete(&opchan->pkt_released);
		}
		release_channel(opchan);
		break;
	case XLINK_OPEN_CHANNEL_REQ:
		if (xmux->channels[link_id][chan].status == CHAN_CLOSED) {
			xmux->channels[link_id][chan].size = event->header.size;
			xmux->channels[link_id][chan].timeout = event->header.timeout;
			//xmux->channels[link_id][chan].mode = *(enum xlink_opmode *)event->data;
			rc = multiplexer_open_channel(link_id, chan);
			if (rc) {
				rc = X_LINK_ERROR;
			} else {
				opchan = get_channel(link_id, chan);
				if (!opchan) {
					rc = X_LINK_COMMUNICATION_FAIL;
				} else {
					xmux->channels[link_id][chan].status = CHAN_OPEN_PEER;
					complete(&opchan->opened);
					passthru_event = xlink_create_event(link_id,
							XLINK_OPEN_CHANNEL_RESP, event->handle, chan, 0,
							opchan->chan->timeout);
					xlink_dispatcher_event_add(EVENT_RX, passthru_event);
				}
				release_channel(opchan);
			}
		} else {
			/* channel already open */
			opchan = get_channel(link_id, chan);
			if (!opchan) {
				rc = X_LINK_COMMUNICATION_FAIL;
			} else {
				passthru_event = xlink_create_event(link_id,
						XLINK_OPEN_CHANNEL_RESP, event->handle, chan, 0, 0);
				xlink_dispatcher_event_add(EVENT_RX, passthru_event);
			}
			release_channel(opchan);
			// printk(KERN_DEBUG "\n RX open chan %x already connected",chan);
		}
		rc = xlink_passthrough(event);
		if (rc == 0)
			xlink_destroy_event(event); // event is handled and can now be freed
		break;
	case XLINK_CLOSE_CHANNEL_REQ:
	case XLINK_PING_REQ:
		break;
	case XLINK_WRITE_RESP:
	case XLINK_WRITE_VOLATILE_RESP:
	case XLINK_WRITE_CONTROL_RESP:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			xlink_destroy_event(event); // event is handled and can now be freed
			// printk(KERN_DEBUG "Write of size %u on channel 0x%x, tx fill level = %u out of %u\n",
			//		event->header.size, chan, opchan->tx_fill_level,
			//		opchan->chan->size);
		}
		release_channel(opchan);
		break;
	case XLINK_READ_RESP:
	case XLINK_READ_TO_BUFFER_RESP:
	case XLINK_RELEASE_RESP:
		xlink_destroy_event(event); // event is handled and can now be freed
		break;
	case XLINK_OPEN_CHANNEL_RESP:
		opchan = get_channel(link_id, chan);
		if (!opchan) {
			rc = X_LINK_COMMUNICATION_FAIL;
		} else {
			xlink_destroy_event(event); // event is handled and can now be freed
			complete(&opchan->opened);
		}
		release_channel(opchan);
		break;
	case XLINK_CLOSE_CHANNEL_RESP:
	case XLINK_PING_RESP:
		xlink_destroy_event(event); // event is handled and can now be freed
		break;
	default:
		rc = X_LINK_ERROR;
	}

	return rc;
}

enum xlink_error xlink_passthrough(struct xlink_event *event)
{
	int rc = 0;
#ifdef CONFIG_XLINK_LOCAL_HOST
	dma_addr_t vpuaddr = 0;
	phys_addr_t physaddr = 0;
	uint32_t timeout = 0;
	uint16_t chan = 0;
	uint32_t link_id = 0;
	struct xlink_ipc_context ipc = {0};
	dma_addr_t paddr = 0;

	if (!xmux || !event)
		return X_LINK_ERROR;

	link_id = event->link_id;
	chan = event->header.chan;
	ipc.chan = chan;

	if (ipc.chan >= XLINK_IPC_MAX_CHANNELS)
		return rc;

	switch (event->header.type) {
	case XLINK_PASSTHRU_WRITE_REQ:
	case XLINK_WRITE_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			/* Translate physical address to VPU address */
			vpuaddr = phys_to_dma(xmux->dev, *(uint32_t *)event->data);
			event->data = &vpuaddr;
			rc = xlink_platform_write(IPC_INTERFACE,
					event->handle->sw_device_id, event->data,
					&event->header.size, 0, &ipc);
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_PASSTHRU_VOLATILE_WRITE_REQ:
	case XLINK_WRITE_VOLATILE_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			ipc.is_volatile = 1;
			rc = xlink_platform_write(IPC_INTERFACE,
					event->handle->sw_device_id, event->data,
					&event->header.size, 0, &ipc);
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_WRITE_CONTROL_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			ipc.is_volatile = 1;
			rc = xlink_platform_write(IPC_INTERFACE,
					event->handle->sw_device_id, event->header.control_data,
					&event->header.size, 0, &ipc);
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_PASSTHRU_READ_REQ:
	case XLINK_READ_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			/* if channel has receive blocking set,
			 * then set timeout to U32_MAX
			 */
			if (xmux->channels[link_id][chan].mode == RXB_TXN ||
					xmux->channels[link_id][chan].mode == RXB_TXB) {
				timeout = U32_MAX;
			} else {
				timeout = xmux->channels[link_id][chan].timeout;
			}
			rc = xlink_platform_read(IPC_INTERFACE,
					event->handle->sw_device_id, &vpuaddr,
					(size_t *)event->length, timeout, &ipc);
			/* Translate VPU address to physical address */
			physaddr = dma_to_phys(xmux->dev, vpuaddr);
			*(phys_addr_t *)event->pdata = physaddr;
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_READ_TO_BUFFER_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			/* if channel has receive blocking set,
			 * then set timeout to U32_MAX
			 */
			if (xmux->channels[link_id][chan].mode == RXB_TXN ||
					xmux->channels[link_id][chan].mode == RXB_TXB) {
				timeout = U32_MAX;
			} else {
				timeout = xmux->channels[link_id][chan].timeout;
			}
			ipc.is_volatile = 1;
			rc = xlink_platform_read(IPC_INTERFACE,
					event->handle->sw_device_id, event->data,
					(size_t *)event->length, timeout, &ipc);
			if (rc || *event->length > XLINK_MAX_BUF_SIZE)
				rc = X_LINK_ERROR;
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_PASSTHRU_READ_TO_BUFFER_REQ:
		if (xmux->channels[link_id][ipc.chan].ipc_status == CHAN_OPEN) {
			/* if channel has receive blocking set,
			 * then set timeout to U32_MAX
			 */
			if (xmux->channels[link_id][ipc.chan].mode == RXB_TXN ||
					xmux->channels[link_id][ipc.chan].mode == RXB_TXB) {
				timeout = U32_MAX;
			} else {
				timeout = xmux->channels[link_id][ipc.chan].timeout;
			}
			ipc.is_volatile = 1;
			event->data = xlink_platform_allocate(xmux->dev, &paddr,
					XLINK_MAX_BUF_SIZE, XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY);
			if (event->data) {
				rc = xlink_platform_read(IPC_INTERFACE,
						event->handle->sw_device_id, event->data,
						(size_t *)event->length, timeout, &ipc);
				if (rc || *event->length > XLINK_MAX_BUF_SIZE) {
					xlink_platform_deallocate(xmux->dev, event->data, paddr,
								event->header.size, XLINK_PACKET_ALIGNMENT,
								XLINK_NORMAL_MEMORY);
					rc = X_LINK_ERROR;
				}
			} else {
				rc = X_LINK_ERROR;
			}
		} else {
			/* channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_RELEASE_REQ:
		break;
	case XLINK_OPEN_CHANNEL_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_CLOSED) {
			xmux->channels[link_id][chan].size = event->header.size;
			xmux->channels[link_id][chan].timeout = event->header.timeout;
			xmux->channels[link_id][chan].mode = (uintptr_t)event->data;
			rc = xlink_platform_open_channel(IPC_INTERFACE,
					event->handle->sw_device_id, chan);
			if (rc)
				rc = X_LINK_ERROR;
			else
				xmux->channels[link_id][chan].ipc_status = CHAN_OPEN;
		} else {
			/* channel already open */
			rc = X_LINK_ALREADY_OPEN;
		}
		break;
	case XLINK_CLOSE_CHANNEL_REQ:
		if (xmux->channels[link_id][chan].ipc_status == CHAN_OPEN) {
			rc = xlink_platform_close_channel(IPC_INTERFACE,
					event->handle->sw_device_id, chan);
			if (rc)
				rc = X_LINK_ERROR;
			else
				xmux->channels[link_id][chan].ipc_status = CHAN_CLOSED;
		} else {
			/* can't close channel not open */
			rc = X_LINK_ERROR;
		}
		break;
	case XLINK_PING_REQ:
	case XLINK_WRITE_RESP:
	case XLINK_WRITE_VOLATILE_RESP:
	case XLINK_WRITE_CONTROL_RESP:
	case XLINK_READ_RESP:
	case XLINK_READ_TO_BUFFER_RESP:
	case XLINK_RELEASE_RESP:
	case XLINK_OPEN_CHANNEL_RESP:
	case XLINK_CLOSE_CHANNEL_RESP:
	case XLINK_PING_RESP:
		break;
	default:
		rc = X_LINK_ERROR;
	}
#else
	rc = 0;
#endif // CONFIG_XLINK_LOCAL_HOST
	return rc;
}
