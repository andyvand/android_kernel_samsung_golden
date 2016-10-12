/**
 * SAMSUNG MODEM IPC version 4
 *
 * Copyright (C) 2010 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

//#define DEBUG

#if defined(DEBUG)
//#define NOISY_DEBUG
#endif

#include "pdp.h"
#include "sipc.h"
#include "sipc4.h"
#include "main.h"

#include <linux/circ_buf.h>
#include <linux/workqueue.h>
#include <asm/errno.h>

#include <net/sock.h>
#include <linux/if_ether.h>
#include <linux/phonet.h>
#include <net/phonet/phonet.h>
#include <net/phonet/pep.h>

#ifndef CONFIG_ARCH_U8500
#include <linux/onedram.h>
#endif

#include <linux/modem/shrm/shrm_svnet.h>

#ifdef CONFIG_ARCH_U8500
static DEFINE_SPINLOCK(shm_raw);
static u8 wr_ipcctrl_msg[10*1024];
static u8 wr_isi_msg[8*1024];
#endif

#if defined(CONFIG_KERNEL_DEBUG_SEC)
#include <linux/kernel_sec_common.h>
#define ERRMSG "Unknown CP Crash"
static char cp_errmsg[65];
static void _go_dump(struct sipc *si);
#else
#define _go_dump(si) do { } while(0)
#endif

#if defined(NOISY_DEBUG)
static struct device *_dev;
#  define _dbg(format, arg...) \
	dev_dbg(_dev, format, ## arg)
#else
#  define _dbg(format, arg...) \
	do { } while (0)
#endif

const char *sipc_version = "4.1";

static const char hdlc_start[1] = { HDLC_START };
static const char hdlc_end[1] = { HDLC_END };

struct mailbox_data {
	u16 mask_send;
	u16 mask_req_ack;
	u16 mask_res_ack;
};

static struct mailbox_data mb_data[IPCIDX_MAX] = {
	{
		.mask_send = MBD_SEND_FMT,
		.mask_req_ack = MBD_REQ_ACK_FMT,
		.mask_res_ack = MBD_RES_ACK_FMT,
	},
	{
		.mask_send = MBD_SEND_RAW,
		.mask_req_ack = MBD_REQ_ACK_RAW,
		.mask_res_ack = MBD_RES_ACK_RAW,
	},
	{
		.mask_send = MBD_SEND_RFS,
		.mask_req_ack = MBD_REQ_ACK_RFS,
		.mask_res_ack = MBD_RES_ACK_RFS,
	},
};

/* semaphore latency */
unsigned long long time_max_semlat;
//static volatile unsigned long *TCNT = (unsigned long *)0xF520000C;

struct sipc;
struct ringbuf;

struct ringbuf_info {
	unsigned int out_off;
	unsigned int in_off;
	unsigned int size;
	int (*read)(struct sipc *si, int inbuf, struct ringbuf *rb);
};

struct ringbuf {
	unsigned char *out_base;
	unsigned char *in_base;
	struct ringbuf_cont *cont;
	struct ringbuf_info *info;
};
#define rb_size info->size
#define rb_read info->read
#define rb_out_head cont->out_head
#define rb_out_tail cont->out_tail
#define rb_in_head cont->in_head
#define rb_in_tail cont->in_tail


static int _read_fmt(struct sipc *si, int inbuf, struct ringbuf *rb);
static int _read_raw(struct sipc *si, int inbuf, struct ringbuf *rb);
static int _read_rfs(struct sipc *si, int inbuf, struct ringbuf *rb);
static int _read_fmt_from_shrm(struct sipc *si, u8 *buf, u16 len);
static int _read_isi_from_shrm(struct sipc *si, u8 *buf, u16 len);
static int _read_raw_from_shrm(struct sipc *si, u8 *buf, u16 len);

static struct ringbuf_info rb_info[IPCIDX_MAX] = {
	{
		.out_off = FMT_OUT,
		.in_off = FMT_IN,
		.size = FMT_SZ,
		.read = _read_fmt,
	},
	{
		.out_off = RAW_OUT,
		.in_off = RAW_IN,
		.size = RAW_SZ,
		.read = _read_raw,
	},
	{
		.out_off = RFS_OUT,
		.in_off = RFS_IN,
		.size = RFS_SZ,
		.read = _read_rfs,
	},
};

#define FRAG_BLOCK_MAX (PAGE_SIZE - sizeof(struct list_head) \
		- sizeof(u32) - sizeof(char *))
struct frag_block {
	struct list_head list;
	u32 len;
	char *ptr;
	char buf[FRAG_BLOCK_MAX];
};

struct frag_list {
	struct list_head list;
	u8 msg_id;
	u32 len;
	// timeout??
	struct list_head block_head;
};

struct frag_head {
	struct list_head head;
	unsigned long bitmap[FMT_ID_SIZE/BITS_PER_LONG];
};

struct frag_info {
	struct sk_buff *skb;
	unsigned int offset;
	u8 msg_id;
};

struct sipc {
	struct sipc_mapped *map;
	struct ringbuf rb[IPCIDX_MAX];

	struct resource *res;

	void (*queue)(u32, void *);
	void *queue_data;

	/* for fragmentation */
	u8 msg_id;
	char *frag_buf;
	struct frag_info frag;

	/* for merging */
	struct frag_head frag_map;

	int od_rel; /* onedram authority release */

	struct net_device *svndev;

	const struct attribute_group *group;

	struct sk_buff_head rfs_rx;
};

/* sizeof(struct phonethdr) + NET_SKB_PAD > SMP_CACHE_BYTES */
//#define RFS_MTU (PAGE_SIZE - sizeof(struct phonethdr) - NET_SKB_PAD)
/* SMP_CACHE_BYTES > sizeof(struct phonethdr) + NET_SKB_PAD */
#define RFS_MTU (PAGE_SIZE - SMP_CACHE_BYTES)
#define RFS_TX_RATE 4

/* TODO: move PDP related codes to other source file */
static DEFINE_MUTEX(pdp_mutex);
static struct net_device *pdp_devs[PDP_MAX];
static int pdp_cnt;
unsigned long pdp_bitmap[PDP_MAX/BITS_PER_LONG];

static void clear_pdp_wq(struct work_struct *work);
static DECLARE_WORK(pdp_work, clear_pdp_wq);

static ssize_t show_act(struct device *d,
		struct device_attribute *attr, char *buf);
static ssize_t show_deact(struct device *d,
		struct device_attribute *attr, char *buf);
static ssize_t store_act(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count);
static ssize_t store_deact(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t show_suspend(struct device *d,
		struct device_attribute *attr, char *buf);
static ssize_t store_suspend(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count);
static ssize_t store_resume(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count);

static DEVICE_ATTR(activate, S_IRUGO | S_IWUSR | S_IWGRP, show_act, store_act);
static DEVICE_ATTR(deactivate, S_IRUGO | S_IWUSR | S_IWGRP, show_deact, store_deact);
static DEVICE_ATTR(suspend, S_IRUGO | S_IWUSR | S_IWGRP, show_suspend, store_suspend);
static DEVICE_ATTR(resume, S_IRUGO | S_IWUSR | S_IWGRP, NULL, store_resume);

static struct attribute *pdp_attributes[] = {
	&dev_attr_activate.attr,
	&dev_attr_deactivate.attr,
	&dev_attr_suspend.attr,
	&dev_attr_resume.attr,
	NULL
};

static const struct attribute_group pdp_group = {
	.name = "pdp",
	.attrs = pdp_attributes,
};


#if defined(NOISY_DEBUG)
#define DUMP_LIMIT 32
static char dump_buf[64];
void _dbg_dump(u8 *buf, int size)
{
	int i;
	int len = 0;

	if (!buf)
		return;

	if (size > DUMP_LIMIT)
		size = DUMP_LIMIT;

	for (i=0;i<32 && i<size;i++) {
		len += sprintf(&dump_buf[len], "%02x ", buf[i]);
		if ((i & 0xf) == 0xf) {
			dump_buf[len] = '\0';
			printk(KERN_INFO "dump %04x [ %s]\n", (i>>4), dump_buf);
			len = 0;
		}
	}
	if (len) {
		dump_buf[len] = '\0';
		printk(KERN_INFO "dump %04x [ %s]\n", i, dump_buf);
	}
}
#else
#  define _dbg_dump(buf, size) do { } while(0)
#endif

static u8 _get_msg_id(struct sipc *si)
{
	if (!si)
		return 0;

	si->msg_id = (si->msg_id + 1) & FMT_ID_MASK;

	return si->msg_id;
}

static int _get_auth(void)
{
	int r;
	unsigned long long t, d;

	t = cpu_clock(smp_processor_id());
#ifdef CONFIG_ARCH_U8500
	r = 0;
#else
	r = onedram_get_auth(MB_CMD(MBC_REQ_SEM)); // wait for completion
#endif //CONFIG_ARCH_U8500
	d = cpu_clock(smp_processor_id()) - t;
	if (d > time_max_semlat)
		time_max_semlat = d;

	return r;
}

static void _put_auth(struct sipc *si)
{
	if (!si)
		return;
#ifdef CONFIG_ARCH_U8500
#else
	onedram_put_auth(0);

	if (si->od_rel && !onedram_rel_sem()) {
		onedram_write_mailbox(MB_CMD(MBC_RES_SEM));
		si->od_rel = 0;
	}
#endif
}

static inline void _req_rel_auth(struct sipc *si)
{
	si->od_rel = 1;
}

static int _get_auth_try(void)
{
#ifdef CONFIG_ARCH_U8500
	return 0;
#else
	return onedram_get_auth(0);
#endif
}

static void _check_buffer(struct sipc *si)
{
#ifdef CONFIG_ARCH_U8500
#else
	int i;
	u32 mailbox;

	i = onedram_read_sem();
	if (i != 0x1)
		return;

	i = _get_auth_try();
	if (i)
		return;

	mailbox = 0;

	for (i=0;i<IPCIDX_MAX;i++) {
		int inbuf;
		struct ringbuf *rb;

		rb = &si->rb[i];
		inbuf = CIRC_CNT(rb->rb_in_head, rb->rb_in_tail, rb->rb_size);
		if (!inbuf)
			continue;

		mailbox |= mb_data[i].mask_send;
	}
	_put_auth(si);

	if (mailbox)
		si->queue(MB_DATA(mailbox), si->queue_data);
#endif //CONFIG_ARCH_U8500
}

static void _do_command(struct sipc *si, u32 mailbox)
{
#ifndef CONFIG_ARCH_U8500
	int r;
#endif

	u32 cmd = (mailbox & MBC_MASK) & ~(MB_CMD(0));

//	dev_dbg(&si->svndev->dev, "Command: %x\n", cmd);

	switch(cmd) {
	case MBC_REQ_SEM:
#ifdef CONFIG_ARCH_U8500
#else
		r = onedram_rel_sem();
		if (r) {
			dev_dbg(&si->svndev->dev, "onedram in use, "
					"defer releasing semaphore\n");
			_req_rel_auth(si);
		}
		else
			onedram_write_mailbox(MB_CMD(MBC_RES_SEM));
#endif
		break;
	case MBC_RES_SEM:
		/* do nothing */
		break;
	case MBC_PHONE_START:
#ifdef CONFIG_ARCH_U8500
#else

		onedram_write_mailbox(MB_CMD(MBC_INIT_END) | CP_BOOT_AIRPLANE
				| AP_OS_ANDROID);
#endif
		break;
	case MBC_RESET:
		printk(KERN_INFO "svnet reset mailbox msg : 0x%08x\n", mailbox);
		si->queue(SIPC_RESET_MB, si->queue_data);
		break;
	case MBC_ERR_DISPLAY:
		printk(KERN_INFO "svnet error display mailbox msg : 0x%08x\n",
				mailbox);
		si->queue(SIPC_EXIT_MB, si->queue_data);
		break;
	/* TODO : impletment other commands... */
	default:
		/* do nothing */

		break;
	}
}

#ifdef CONFIG_ARCH_U8500

void sipc_msr_crash_handler(void *data)
{
	struct sipc *si = (struct sipc *)data;

	printk(KERN_INFO "Modem crash detected on SIPC side\n");

	if (netif_running(si->svndev))
		netif_stop_queue(si->svndev);

	/* Notify SEC RIL of the modem crash */
	si->queue(SIPC_RESET_MB, si->queue_data);
}

void sipc_msr_reinit_handler(void *data)
{
	struct sipc *si = (struct sipc *)data;

	printk(KERN_INFO "Modem reinit detected on SIPC side\n");
	netif_start_queue(si->svndev);
}

void sipc_handler(u16 len, u8 *buf, void *data)
{
	struct sipc *si = (struct sipc *)data;

	if (!si || !si->queue)
		return;

	_fmt_wakelock_timeout();

	_read_fmt_from_shrm(si, buf, len);

}

void sisi_handler(u16 len, u8 *buf, void *data)
{
	struct sipc *si = (struct sipc *)data;

        if (!si || !si->queue)
                return;

        _read_isi_from_shrm(si, buf, len);
}

void sraw_handler(u16 len, u8 *buf, void *data)
{
	struct sipc *si = (struct sipc *)data;

	if (!si || !si->queue)
		return;

	_non_fmt_wakelock_timeout();

	_read_raw_from_shrm(si, buf, len);
}

#else
void sipc_handler(u32 mailbox, void *data)
{
	struct sipc *si = (struct sipc *)data;

	if (!si || !si->queue)
		return;

	dev_dbg(&si->svndev->dev, "recv mailbox %x\n", mailbox);

#if defined(CONFIG_KERNEL_DEBUG_SEC)
	if (mailbox == KERNEL_SEC_DUMP_AP_DEAD_ACK) {
		// deal ack for ap crash that indicated to cp
		kernel_sec_set_cp_ack();
	}
#endif

	if ((mailbox & MB_VALID) == 0) {
		dev_err(&si->svndev->dev, "Invalid mailbox message: %x\n", mailbox);
		return;
	}

	if (mailbox & MB_COMMAND) {
		_check_buffer(si);  // check buffer for missing interrupt
		_do_command(si, mailbox);
		return;
	}

	si->queue(mailbox, si->queue_data);
}
#endif
static inline void _init_data(struct sipc *si, unsigned char *base)
{
	int i;

	si->map = (struct sipc_mapped *)base;
	si->map->magic = 0x0;
	si->map->access = 0x0;

	for (i=0;i<IPCIDX_MAX;i++) {
		struct ringbuf *r = &si->rb[i];
		struct ringbuf_info *info = &rb_info[i];
		struct ringbuf_cont *cont = &si->map->rbcont[i];

		r->out_base = base + info->out_off;
		r->in_base = base + info->in_off;
		r->info = info;
		r->cont = cont;

		cont->out_head = 0;
		cont->out_tail = 0;
		cont->in_head = 0;
		cont->in_tail = 0;
	}
}

static void _init_proc(struct sipc *si)
{
#ifdef CONFIG_ARCH_U8500
#else
	u32 mailbox;
	int r;

	r = onedram_read_mailbox(&mailbox);
	if (r)
		return;

	sipc_handler(mailbox, si);
#endif
}

struct sipc* sipc_open(void (*queue)(u32, void*), struct net_device *ndev)
{
	struct sipc *si;
	int r;
#ifndef CONFIG_ARCH_U8500
	struct resource *res;
	void * onedram_vbase;
#endif

	if (!queue || !ndev)
		return ERR_PTR(-EINVAL);

	si = kzalloc(sizeof(struct sipc), GFP_KERNEL);
	if (!si)
		return ERR_PTR(-ENOMEM);

	/* If FMT_SZ grown up, MUST be changed!! */
	si->frag_buf = kmalloc(FMT_SZ, GFP_KERNEL);
	if (!si->frag_buf) {
		sipc_close(&si);
		return ERR_PTR(-ENOMEM);
	}
	INIT_LIST_HEAD(&si->frag_map.head);

#ifdef CONFIG_ARCH_U8500
	r = shrm_register_msr_handlers(sipc_msr_crash_handler,
			sipc_msr_reinit_handler, (void *)si);

	r = shrm_register_handler((void *)si, IPCCTRL, sipc_handler);
	if (r) {
                sipc_close(&si);
                return ERR_PTR(r);
        }

	r = shrm_register_handler((void *)si, ISI_MESSAGING, sisi_handler);
	if (r) {
                sipc_close(&si);
                return ERR_PTR(r);
        }

	r = shrm_register_handler((void *)si, IPCDATA, sraw_handler);
	if (r) {
		sipc_close(&si);
		return ERR_PTR(r);
	}

#else

	res = onedram_request_region(0, SIPC_MAP_SIZE, SIPC_NAME);
	if (!res) {
		sipc_close(&si);
		return ERR_PTR(-EBUSY);
	}
	si->res = res;

	r = onedram_register_handler(sipc_handler, si);
	if (r) {
		sipc_close(&si);
		return ERR_PTR(r);
	}
#endif
	si->queue = queue;
	si->queue_data = ndev;
	si->svndev = ndev;

	r = sysfs_create_group(&si->svndev->dev.kobj, &pdp_group);
	if (r) {
		sipc_close(&si);
		return ERR_PTR(r);
	}
	si->group = &pdp_group;

#if defined(NOISY_DEBUG)
	_dev = &si->svndev->dev;
#endif

#ifdef CONFIG_ARCH_U8500
#else
	onedram_get_vbase(&onedram_vbase);

	// use io remapped address rather than request_region
	if (onedram_vbase) {
		_init_data(si, (unsigned char *)onedram_vbase);
	}else {
		_init_data(si, (unsigned char *)res->start);
	}
#endif
	skb_queue_head_init(&si->rfs_rx);

	/* process init message */
	_init_proc(si);

	return si;
}

static void clear_pdp_wq(struct work_struct *work)
{
	int i;
	mutex_lock(&pdp_mutex);

	for (i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
		if (pdp_devs[i]) {
			destroy_pdp(&pdp_devs[i]);
			clear_bit(i, pdp_bitmap);
		}
	}
	pdp_cnt = 0;

	mutex_unlock(&pdp_mutex);
}

void sipc_exit(void)
{
	if (work_pending(&pdp_work))
		flush_work(&pdp_work);
	else
		clear_pdp_wq(NULL);
}

void sipc_close(struct sipc **psi)
{
	struct sipc *si;

	if (!psi || !*psi)
		return;

	si = *psi;

#ifdef CONFIG_ARCH_U8500
	shrm_unregister_handler();
#endif

	if (si->group && si->svndev) {
		int i;
		sysfs_remove_group(&si->svndev->dev.kobj, si->group);

		mutex_lock(&pdp_mutex);
		for (i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
			if (pdp_devs[i])
				netif_stop_queue(pdp_devs[i]);
		}
		mutex_unlock(&pdp_mutex);
	}

	if (si->frag_buf)
		kfree(si->frag_buf);

#ifdef CONFIG_ARCH_U8500
#else
	if (si->queue)
		onedram_unregister_handler(sipc_handler);

	if (si->res)
		onedram_release_region(0, SIPC_MAP_SIZE);
#endif
	kfree(si);
	*psi = NULL;
}

static inline void _wake_queue(int idx)
{
	mutex_lock(&pdp_mutex);

	if (pdp_devs[idx] && !test_bit(idx, pdp_bitmap))
		netif_wake_queue(pdp_devs[idx]);

	mutex_unlock(&pdp_mutex);
}

static int __write(struct ringbuf *rb, u8 *buf, unsigned int size)
{
	int c;
	int len = 0;

	// no check space
	_dbg("%s b: size %u head %u tail %u\n", __func__,
			size, rb->rb_out_head, rb->rb_out_tail);
	_dbg_dump(buf, size);


	while(1) {
		c = CIRC_SPACE_TO_END(rb->rb_out_head, rb->rb_out_tail, rb->rb_size);
		if(size < c)
			c = size;
		if(c <= 0)
			break;
		memcpy(rb->out_base + rb->rb_out_head, buf, c);
		rb->rb_out_head = (rb->rb_out_head + c) & (rb->rb_size - 1);
		buf += c;
		size -= c;
		len += c;
	}

	_dbg("%s a: size %u head %u tail %u\n", __func__,
			len, rb->rb_out_head, rb->rb_out_tail);

	return len;
}

static inline void _set_raw_hdr(struct raw_hdr *h, int res,
		unsigned int len, int control)
{
	h->len = len;
	h->channel = CHID(res);
	h->control = 0;
}

#ifndef CONFIG_ARCH_U8500
static int _write_raw_buf(struct ringbuf *rb, int res, struct sk_buff *skb)
{
	int len;
	struct raw_hdr h;

	_dbg("%s: packet %p res 0x%02x\n", __func__, skb, res);

	len = skb->len + sizeof(h);

	_set_raw_hdr(&h, res, len, 0);

	len  = __write(rb, (u8 *)hdlc_start, sizeof(hdlc_start));
	len += __write(rb, (u8 *)&h, sizeof(h));
	len += __write(rb, skb->data, skb->len);
	len += __write(rb, (u8 *)hdlc_end, sizeof(hdlc_end));

	return len;
}

static int _write_raw_skb(struct ringbuf *rb, int res, struct sk_buff *skb)
{
	char *b;

	_dbg("%s: packet %p res 0x%02x\n", __func__, skb, res);

	b = skb_put(skb, sizeof(hdlc_end));
	memcpy(b, hdlc_end, sizeof(hdlc_end));

	b = skb_push(skb, sizeof(struct raw_hdr) + sizeof(hdlc_start));
	memcpy(b, hdlc_start, sizeof(hdlc_start));

	b += sizeof(hdlc_start);

	_set_raw_hdr((struct raw_hdr *)b, res,
			skb->len - sizeof(hdlc_start) - sizeof(hdlc_end), 0);

	return __write(rb, skb->data, skb->len);
}

static int _write_raw(struct ringbuf *rb, struct sk_buff *skb, int res)
{
	int len;
	int space;

	space = CIRC_SPACE(rb->rb_out_head, rb->rb_out_tail, rb->rb_size);
	if(space < skb->len + sizeof(struct raw_hdr)
			+ sizeof(hdlc_start) + sizeof(hdlc_end))
		return -ENOSPC;

	if(skb_headroom(skb) > (sizeof(struct raw_hdr) + sizeof(hdlc_start))
			&& skb_tailroom(skb) > sizeof(hdlc_end)) {
		len = _write_raw_skb(rb, res, skb);
	} else {
		len = _write_raw_buf(rb, res, skb);
	}

	if (res >= PN_PDP_START && res <= PN_PDP_END)
		_wake_queue(PDP_ID(res));
	else
		netif_wake_queue(skb->dev);
	return len;
}

static int _write_rfs_buf(struct ringbuf *rb, struct sk_buff *skb)
{
	int len;

	_dbg("%s: packet %p\n", __func__, skb);
	len  = __write(rb, (u8 *)hdlc_start, sizeof(hdlc_start));
	len += __write(rb, skb->data, skb->len);
	len += __write(rb, (u8 *)hdlc_end, sizeof(hdlc_end));

	return len;
}

static int _write_rfs_skb(struct ringbuf *rb, struct sk_buff *skb)
{
	char *b;

	_dbg("%s: packet %p\n", __func__, skb);
	b = skb_put(skb, sizeof(hdlc_end));
	memcpy(b, hdlc_end, sizeof(hdlc_end));

	b = skb_push(skb, sizeof(hdlc_start));
	memcpy(b, hdlc_start, sizeof(hdlc_start));

	return __write(rb, skb->data, skb->len);
}

static int _write_rfs(struct ringbuf *rb, struct sk_buff *skb)
{
	int len;
	int space;

	space = CIRC_SPACE(rb->rb_out_head, rb->rb_out_tail, rb->rb_size);
	if(space < skb->len + sizeof(hdlc_start) + sizeof(hdlc_end))
		return -ENOSPC;

	if(skb_headroom(skb) > sizeof(hdlc_start)
			&& skb_tailroom(skb) > sizeof(hdlc_end)) {
		len = _write_rfs_skb(rb, skb);
	} else {
		len = _write_rfs_buf(rb, skb);
	}

	netif_wake_queue(skb->dev);
	return len;
}

static int _write_fmt_buf(char *frag_buf, struct ringbuf *rb,
		struct sk_buff *skb, struct frag_info *fi, int wlen,
		u8 control)
{
	char *buf = frag_buf;
	struct fmt_hdr *h;

	memcpy(buf, hdlc_start, sizeof(hdlc_start));
	buf += sizeof(hdlc_start);

	h = (struct fmt_hdr *)buf;
	h->len = sizeof(struct fmt_hdr) + wlen;
	h->control = control;
	buf += sizeof(struct fmt_hdr);

	memcpy(buf, skb->data + fi->offset, wlen);
	buf += wlen;

	memcpy(buf, hdlc_end, sizeof(hdlc_end));
	buf += sizeof(hdlc_end);

	return __write(rb, frag_buf, buf - frag_buf);
}
#endif

#ifdef CONFIG_ARCH_U8500
static int _write_raw_to_shrm(int res, struct sk_buff *skb)
{

	u8 *buf;
	int err = 0;
	int orig_len;
	int newheadroom, newtailroom;
	struct sk_buff *nskb;

#ifdef DEBUG
	 printk(KERN_DEBUG, "%s: len = %d++\n", __func__, skb->len);
	// printk(KERN_DEBUG, "skb->tail = %x\n", skb->tail);
	// printk(KERN_DEBUG, "skb->end = %x\n", skb->end);
	// printk(KERN_DEBUG, "skb->len = %x\n", skb->len);
#endif

	orig_len = skb->len;

	if(skb_headroom(skb) > (sizeof(struct raw_hdr) + sizeof(hdlc_start))
			&& skb_tailroom(skb) > sizeof(hdlc_end)) {
#ifdef DEBUG
		 printk(KERN_DEBUG, "---------Writing through skbuff------------\n");
#endif
		nskb = skb;
	} else {
#ifdef DEBUG
		printk(KERN_DEBUG, "=================Writing through increased skbuff=============-\n");
#endif
		newheadroom = sizeof(struct raw_hdr) + sizeof(hdlc_start);
		newtailroom = sizeof(hdlc_end);

		nskb = skb_copy_expand(skb, newheadroom, newtailroom, GFP_ATOMIC);
		if (!nskb) {
			printk(KERN_INFO "%s: skb_alloc failed\n", __func__);
			return -ENOMEM;
		}
	}

	skb_put(nskb, sizeof(hdlc_end));
	skb_copy_to_linear_data_offset(nskb, orig_len, hdlc_end, sizeof(hdlc_end));

	buf = skb_push(nskb, sizeof(hdlc_start) + sizeof(struct raw_hdr));
	skb_copy_to_linear_data(nskb, hdlc_start, sizeof(hdlc_start));
	buf += sizeof(hdlc_start);

	/* copy the raw header */
	_set_raw_hdr((struct raw_hdr *)buf, res,
			nskb->len - sizeof(hdlc_start) - sizeof(hdlc_end), 0);


	/*send message to modem*/
#ifdef DEBUG
	int i = 0;
	for (; i<nskb->len; i++)
		printk(KERN_INFO "%02x\t", nskb->data[i]);
	printk("\n");
#endif
	err = shrm_host_modem_msg_send(IPCDATA, nskb->data,
			nskb->len);

	if (skb != nskb)
		kfree_skb(nskb);
#ifdef DEBUG
	printk(KERN_DEBUG, "-%s--: skbuff write err = %d\n",__func__, err);
#endif
	return err;
}

static int _write_rfs_to_shrm(struct sk_buff *skb)
{
	printk(KERN_INFO "+%s\n", __func__);
	printk(KERN_INFO "-%s\n", __func__);
	return 0;
}
#define swap_en(x) ((x & 0xff00)>>8)|((x & 0xff)<<8)
static int _write_fmt_to_shrm(struct sipc *si, struct sk_buff *skb)
{
        struct fmt_hdr *h;
        u8 *buf = wr_ipcctrl_msg;
	int err;

#ifdef DEBUG
	printk(KERN_DEBUG, "%s: ++\n", __func__);
#endif
        /*copy hdlc start*/
        memcpy(buf, hdlc_start, sizeof(hdlc_start));
        buf += sizeof(hdlc_start);

        h = (struct fmt_hdr *)buf;
        //h->len = swap_en((sizeof(struct fmt_hdr) + skb->len));
        h->len = sizeof(struct fmt_hdr) + skb->len;
        h->control = 0x0;
        buf += sizeof(struct fmt_hdr);

        memcpy((void *)buf, skb->data, skb->len);
        buf += skb->len;

        memcpy(buf, hdlc_end, sizeof(hdlc_end));
        buf += sizeof(hdlc_end);

        /*send message to modem*/
        err = shrm_host_modem_msg_send(IPCCTRL, (void *)wr_ipcctrl_msg,
                (sizeof(hdlc_start) +
                sizeof(struct fmt_hdr) +
                skb->len +
                sizeof(hdlc_end)));

#ifdef DEBUG
	printk(KERN_INFO "KERN_DEBUG, %s: --\n", __func__);
#endif
	return err;
}

#endif

#ifndef CONFIG_ARCH_U8500
static int _write_fmt(struct sipc *si, struct ringbuf *rb, struct sk_buff *skb)
{
	int len;
	int space;
	int remain;
	struct frag_info *fi = &si->frag;

	if (skb != fi->skb) {
		/* new packet */
		fi->skb = skb;
//		fi->msg_id = _get_msg_id(si);
		fi->offset = 0;
	}

	len = 0;
	remain = skb->len - fi->offset;

	_dbg("%s: packet %p length %d sent %d\n", __func__, skb, skb->len, fi->offset);

	while (remain > 0) {
		int wlen;
		u8 control;

		space = CIRC_SPACE(rb->rb_out_head, rb->rb_out_tail, rb->rb_size);
		space -= sizeof(struct fmt_hdr)
			+ sizeof(hdlc_start) + sizeof(hdlc_end);
		if (space < FMT_TX_MIN)
			return -ENOSPC;

		if (remain > space) {
			/* multiple frame */
			wlen = space;
			control = 0x1 | FMT_MB_MASK;
		} else {
			wlen = remain;
			if (fi->offset == 0) {
				/* single frame */
				control = 0x0;
			} else {
				/* last frmae */
				control = 0x1;
			}
		}

		wlen = _write_fmt_buf(si->frag_buf, rb, skb, fi, wlen, control);
		if (wlen < 0)
			return wlen;

		len += wlen;

		wlen -= sizeof(hdlc_start) + sizeof(struct fmt_hdr)
			+ sizeof(hdlc_end);

		fi->offset += wlen;
		remain -= wlen;
	}

	wlen = remain; /*write whole*/
	control = 0x0; /*no multiframe*/
	wlen = _write_fmt_buf(si->frag_buf, rb, skb, fi, wlen, control);
	if (wlen < 0)
			return wlen;

			len += wlen;


	if (len > 0) {
		fi->skb = NULL;
		fi->offset = 0;
	}

	netif_wake_queue(skb->dev);
	return len; /* total write bytes */
}
#endif

#ifdef CONFIG_ARCH_U8500
static int _write_isi(struct sipc *si, struct sk_buff *skb)
{
	u8 *buf = wr_isi_msg;
	int err;
	int tail;

	dev_dbg(&si->svndev->dev,"IN %d %s\n",err, __func__);

	if (skb->data[RESOURCE_ID_INDEX] == PN_PIPE) {
		if ((skb->data[MSG_ID_INDEX] == PNS_PIPE_CREATED_IND) ||
			(skb->data[MSG_ID_INDEX] == PNS_PIPE_ENABLED_IND) ||
			(skb->data[MSG_ID_INDEX] == PNS_PIPE_DISABLED_IND))
			skb->data[SRC_OBJ_INDEX] = skb->data[PIPE_HDL_INDEX];
	}
	/*isi adaptations*/
	if (skb->data[2] == 0xa0) {
		/*isi servers do not know 0xa0 device address.
		Hence PN_DEV_HOST is used as send device id */
		skb->data[2] = PN_DEV_HOST;
	}
	skb->data[0] = 0x26;

        for(tail=0;tail<skb->len;tail++)
                dev_dbg(&si->svndev->dev,"tx[%d]=%x\n",tail,skb->data[tail]);

	if ((void *)wr_isi_msg !=
			memcpy((void *)buf, skb->data, skb->len)) {
		dev_err(&si->svndev->dev, "memcpy failed\n");
		dev_kfree_skb(skb);
		return -EFAULT;
	}

        /*send message to modem*/
        err = shrm_host_modem_msg_send(ISI_MESSAGING, (void *)buf,
			skb->len);


	dev_dbg(&si->svndev->dev,"OUT %d %s\n",err, __func__);
        return err;
}
#endif

static int _write(struct sipc *si, int res, struct sk_buff *skb, u32 *mailbox)
{
	int r;
	int rid;

	rid = res_to_ridx(res);
	if(rid < 0 || rid >= IPCIDX_MAX)
		return -EINVAL;

	switch(rid) {
	case IPCIDX_FMT:
#ifdef CONFIG_ARCH_U8500
		r = _write_fmt_to_shrm(si, skb);
#else
		r = _write_fmt(si, &si->rb[rid], skb);
#endif
		break;
	case IPCIDX_RAW:
#ifdef CONFIG_ARCH_U8500
		r = _write_raw_to_shrm(res, skb);
#else
		r = _write_raw(&si->rb[rid], skb, res);
#endif
		break;
	case IPCIDX_RFS:
#ifdef CONFIG_ARCH_U8500
		r = _write_rfs_to_shrm(skb);
#else
		r = _write_rfs(&si->rb[rid], skb);
#endif
		break;
	default:
		/* do nothing */
		r = 0;
		break;
	}
#ifndef CONFIG_ARCH_U8500
	if(r > 0)
		*mailbox |= mb_data[rid].mask_send;
#endif
	_dbg("%s: return %d\n", __func__, r);
	return r;
}

static inline void _update_stat(struct net_device *ndev, unsigned int len)
{
	if(!ndev)
		return;

	ndev->stats.tx_bytes += len;
	ndev->stats.tx_packets++;
}

static inline int _write_pn(struct sipc *si, struct sk_buff *skb, u32 *mb)
{
	int r;
	struct phonethdr *ph;

	ph = pn_hdr(skb);

#ifdef CONFIG_ARCH_U8500
	/*
	Use 0x60 or 0x64 as device id of modem.
	*/
	if ((ph->pn_rdev == 0x60) || (ph->pn_rdev == 0x64)
		|| ((ph->pn_rdev == 0x00) && (ph->pn_res == 0xD9))
		|| ((ph->pn_rdev == 0x00) && (ph->pn_res == 0x10)
			&& (skb->len == 12)))
		return _write_isi(si, skb);
#endif
	skb_pull(skb, sizeof(struct phonethdr) + 1); // 1 is addr len

	r = _write(si, ph->pn_res, skb, mb);
	if (r < 0)
		skb_push(skb, sizeof(struct phonethdr) + 1);

	return r;
}

int sipc_write(struct sipc *si, struct sk_buff_head *sbh)
{
	int r;
	u32 mailbox;
	struct sk_buff *skb;

	if (!sbh)
		return -EINVAL;

	if (!si) {
		skb_queue_purge(sbh);
		return -ENXIO;
	}
	r = _get_auth();
	if (r)
		return r;

	r = mailbox = 0;

	skb = skb_dequeue(sbh);
	while (skb) {
		struct net_device *ndev = skb->dev;
		int len = skb->len;

		dev_dbg(&si->svndev->dev, "write packet %p\n", skb);

		if (skb->protocol != __constant_htons(ETH_P_PHONET)) {
			struct pdp_priv *priv;
			priv = netdev_priv(ndev);
			r = _write(si, PN_PDP(priv->channel), skb, &mailbox);
		} else
			r = _write_pn(si, skb, &mailbox);

		if (r < 0)
			break;

		_update_stat(ndev, len);
		dev_kfree_skb_any(skb);

		skb = skb_dequeue(sbh);
	}

	_req_rel_auth(si);
	_put_auth(si);
#ifndef CONFIG_ARCH_U8500
	if(mailbox)
		onedram_write_mailbox(MB_DATA(mailbox));
#endif

	if (r < 0) {
		if (r == -ENOSPC) {
			dev_err(&si->svndev->dev,
					"write nospc queue %p\n", skb);
			skb_queue_head(sbh, skb);
			netif_stop_queue(skb->dev);
		} else {
			dev_err(&si->svndev->dev,
					"write err %d, drop %p\n", r, skb);
			dev_kfree_skb_any(skb);
		}
	}

	return r;
}

extern int __read(struct ringbuf *rb, unsigned char *buf, unsigned int size)
{
	int c;
	int len = 0;
	unsigned char *p = buf;

	_dbg("%s b: size %u head %u tail %u\n", __func__,
			size, rb->rb_in_head, rb->rb_in_tail);

	while(1) {
		c = CIRC_CNT_TO_END(rb->rb_in_head, rb->rb_in_tail, rb->rb_size);
		if(size < c)
			c = size;
		if(c <= 0)
			break;
		if (p) {
			memcpy(p, rb->in_base + rb->rb_in_tail, c);
			p += c;
		}
		rb->rb_in_tail = (rb->rb_in_tail + c) & (rb->rb_size - 1);
		size -= c;
		len += c;
	}

	_dbg("%s a: size %u head %u tail %u\n", __func__,
			len, rb->rb_in_head, rb->rb_in_tail);
	_dbg_dump(buf, len);

	return len;
}

static inline void _get_raw_hdr(struct raw_hdr *h, int *res,
		unsigned int *len, int *control)
{
	if(res)
		*res = PN_RAW(h->channel);
	if(len)
		*len = h->len;
	if(control)
		*control = h->control;
}

static inline void _phonet_rx(struct net_device *ndev,
		struct sk_buff *skb, int res)
{
	int r;
	struct phonethdr *ph;

	skb->protocol = __constant_htons(ETH_P_PHONET);

	ph = (struct phonethdr *)skb_push(skb, sizeof(struct phonethdr));
	ph->pn_rdev = ndev->dev_addr[0];
	ph->pn_sdev = 0;
	ph->pn_res = res;
	ph->pn_length = __cpu_to_be16(skb->len + 2 - sizeof(*ph));
	ph->pn_robj = 0;
	ph->pn_sobj = 0;

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += skb->len;

	skb_reset_mac_header(skb);

	r = netif_rx_ni(skb);
	if (r != NET_RX_SUCCESS)
		dev_err(&ndev->dev, "phonet rx error: %d\n", r);

	_dbg("%s: res 0x%02x packet %p len %d\n", __func__, res, skb, skb->len);
}

static int _read_pn(struct net_device *ndev, struct ringbuf *rb, int len,
		int res)
{
	int r;
	struct sk_buff *skb;
	char *p;
	int read_len = len + sizeof(hdlc_end);

	_dbg("%s: res 0x%02x data %d\n", __func__, res, len);

	skb = netdev_alloc_skb(ndev, read_len + sizeof(struct phonethdr));
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, sizeof(struct phonethdr));

	p = skb_put(skb, len);
	r = __read(rb, p, read_len);
	if (r != read_len) {
		kfree_skb(skb);
		return -EBADMSG;
	}

	_phonet_rx(ndev, skb, res);

	return r;
}

static inline struct sk_buff* _alloc_phskb(struct net_device *ndev, int len)
{
	struct sk_buff *skb;

	skb = netdev_alloc_skb(ndev, len + sizeof(struct phonethdr));
	if (likely(skb))
		skb_reserve(skb, sizeof(struct phonethdr));

	return skb;
}

static inline int _alloc_rfs(struct net_device *ndev,
		struct sk_buff_head *list, int len)
{
	int r = 0;
	struct sk_buff *skb;

	__skb_queue_head_init(list);

	while (len > 0) {
		skb = _alloc_phskb(ndev, RFS_MTU);
		if (unlikely(!skb)) {
			r = -ENOMEM;
			break;
		}
		__skb_queue_tail(list, skb);
		len -= RFS_MTU;
	}

	return r;
}
static void _free_rfs(struct sk_buff_head *list)
{
	struct sk_buff *skb;

	skb = __skb_dequeue(list);
	while (skb) {
		__kfree_skb(skb);
		skb = __skb_dequeue(list);
	}
}

static inline int _read_rfs_rb(struct ringbuf *rb, int len,
		struct sk_buff_head *list)
{
	int r;
	int read_len;
	struct sk_buff *skb;
	char *p;

	read_len = 0;
	skb = list->next;
	while (skb != (struct sk_buff *)list) {
		int rd = RFS_MTU;

		if (skb == list->next) /* first sk has header */
			rd -= sizeof(struct rfs_hdr);

		if (len < rd)
			rd = len;

		p = skb_put(skb, rd);
		r = __read(rb, p, rd);
		if (r != rd)
			return -EBADMSG;

		len -= r;
		read_len += r;
		skb = skb->next;
	}

	return read_len;
}

static int _read_rfs_data(struct sipc *si, struct ringbuf *rb, int len,
		struct rfs_hdr *h)
{
	int r;
	struct sk_buff_head list;
	struct sk_buff *skb;
	char *p;
	int read_len;
	struct net_device *ndev = si->svndev;

	_dbg("%s: %d bytes\n", __func__, len);

	/* alloc sk_buffs */
	r = _alloc_rfs(ndev, &list, len + sizeof(struct rfs_hdr));
	if (r)
		goto free_skb;

	skb = list.next;
	p = skb_put(skb, sizeof(struct rfs_hdr));
	memcpy(p, h, sizeof(struct rfs_hdr));

	/* read data all */
	r = _read_rfs_rb(rb, len, &list);
	if (r < 0)
		goto free_skb;

	read_len = r;

	/* move to rfs_rx queue */
	skb = __skb_dequeue(&list);
	while (skb) {
//		_phonet_rx(ndev, skb, PN_RFS);
		skb_queue_tail(&si->rfs_rx, skb);
		skb = __skb_dequeue(&list);
	}

	/* remove hdlc_end */
	read_len += __read(rb, NULL, sizeof(hdlc_end));

	return read_len;

free_skb:
	_free_rfs(&list);
	return r;
}

static int _read_pdp(struct ringbuf *rb, int len,
		int res)
{
	int r;
	struct sk_buff *skb;
	char *p;
	int read_len = len + sizeof(hdlc_end);
	struct net_device *ndev;

	_dbg("%s: res 0x%02x data %d\n", __func__, res, len);

	mutex_lock(&pdp_mutex);

	ndev = pdp_devs[PDP_ID(res)];
	if (!ndev) {
		// drop data
		r = __read(rb, NULL, read_len);
		mutex_unlock(&pdp_mutex);
		return r;
	}

	skb = netdev_alloc_skb(ndev, read_len);
	if (unlikely(!skb)) {
		mutex_unlock(&pdp_mutex);
		return -ENOMEM;
	}

	p = skb_put(skb, len);
	r = __read(rb, p, read_len);
	if (r != read_len) {
		mutex_unlock(&pdp_mutex);
		kfree_skb(skb);
		return -EBADMSG;
	}
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += skb->len;

	mutex_unlock(&pdp_mutex);

	read_len = r;

	skb->protocol = __constant_htons(ETH_P_IP);

	skb_reset_mac_header(skb);

	_dbg("%s: pdp packet %p len %d\n", __func__, skb, skb->len);

	r = netif_rx_ni(skb);
	if (r != NET_RX_SUCCESS)
		dev_err(&ndev->dev, "pdp rx error: %d\n", r);

	return read_len;
}

static int _read_raw(struct sipc *si, int inbuf, struct ringbuf *rb)
{
	int r;
	char buf[sizeof(struct raw_hdr) + sizeof(hdlc_start)];
	int res, data_len;
	u32 tail;

	while (inbuf > 0) {
		tail = rb->rb_in_tail;

		r = __read(rb, buf, sizeof(buf));
		if (r < sizeof(buf) ||
				strncmp(buf, hdlc_start, sizeof(hdlc_start))) {
			dev_err(&si->svndev->dev, "Bad message: %c %d\n", buf[0], r);
			return -EBADMSG;
		}
		inbuf -= r;

		_get_raw_hdr((struct raw_hdr *)&buf[sizeof(hdlc_start)],
				&res, &data_len, NULL);

		data_len -= sizeof(struct raw_hdr);

		if (res >= PN_PDP_START && res <= PN_PDP_END) {
			r = _read_pdp(rb, data_len, res);
		} else {
			r = _read_pn(si->svndev, rb, data_len, res);
		}

		if (r < 0) {
			if (r == -ENOMEM)
				rb->rb_in_tail = tail;

			return r;
		}

		inbuf -= r;
	}

	return 0;
}

static int _read_rfs(struct sipc *si, int inbuf, struct ringbuf *rb)
{
	int r;
	char buf[sizeof(struct rfs_hdr) + sizeof(hdlc_start)];
	int data_len;
	u32 tail;
	struct rfs_hdr *h;

	h = (struct rfs_hdr *)&buf[sizeof(hdlc_start)];
	while (inbuf > 0) {
		tail = rb->rb_in_tail;

		r = __read(rb, buf, sizeof(buf));
		if (r < sizeof(buf) ||
				strncmp(buf, hdlc_start, sizeof(hdlc_start))) {
			dev_err(&si->svndev->dev, "Bad message: %c %d\n", buf[0], r);
			return -EBADMSG;
		}
		inbuf -= r;

		data_len = h->len - sizeof(struct rfs_hdr);

		r = _read_rfs_data(si, rb, data_len, h);
		if (r < 0) {
			if (r == -ENOMEM)
				rb->rb_in_tail = tail;

			return r;
		}

		inbuf -= r;
	}

	return 0;
}


static struct frag_list* _find_frag_list(u8 control, struct frag_head *fh)
{
	struct frag_list *fl;
	u8 msg_id = control & FMT_ID_MASK;

	if (!test_bit(msg_id, fh->bitmap))
		return NULL;

	list_for_each_entry(fl, &fh->head, list) {
		if (fl->msg_id == msg_id)
			break;
	}

	return fl;
}

static int _fill_skb(struct sk_buff *skb, struct frag_list *fl)
{
	struct frag_block *fb, *n;
	int offset = 0;
	char *p;

	list_for_each_entry_safe(fb, n, &fl->block_head, list) {
		p = skb_put(skb, fb->len);
		memcpy(p, fb->buf, fb->len);
		offset += fb->len;
		list_del(&fb->list);
		kfree(fb);
	}

	return offset;
}

static void _destroy_frag_list(struct frag_list *fl, struct frag_head *fh)
{
	struct frag_block *fb, *n;

	if (!fl || !fh)
		return;

	list_for_each_entry_safe(fb, n, &fl->block_head, list) {
		kfree(fb);
	}

	clear_bit(fl->msg_id, fh->bitmap);
	list_del(&fl->list);
	kfree(fl);
}

static struct frag_list* _create_frag_list(u8 control, struct frag_head *fh)
{
	struct frag_list *fl;
	u8 msg_id = control & FMT_ID_MASK;

	if (test_bit(msg_id, fh->bitmap)) {
		fl = _find_frag_list(control, fh);
		_destroy_frag_list(fl, fh);
	}

	fl = kmalloc(sizeof(struct frag_list), GFP_KERNEL);
	if (!fl)
		return NULL;

	INIT_LIST_HEAD(&fl->block_head);
	fl->msg_id = msg_id;
	fl->len = 0;
	list_add(&fl->list, &fh->head);
	set_bit(msg_id, fh->bitmap);

	return fl;
}

static inline struct frag_block* _create_frag_block(struct frag_list *fl)
{
	struct frag_block *fb;

	fb = kmalloc(sizeof(struct frag_block), GFP_KERNEL);
	if (!fb)
		return NULL;

	fb->len = 0;
	fb->ptr = fb->buf;
	list_add_tail(&fb->list, &fl->block_head);

	return fb;
}

static struct frag_block* _prepare_frag_block(struct frag_list *fl, int size)
{
	struct frag_block *fb;

	if (size > FRAG_BLOCK_MAX)
		BUG();

	if (list_empty(&fl->block_head)) {
		fb = _create_frag_block(fl);
	} else {
		fb = list_entry(fl->block_head.prev, struct frag_block, list);
		if (size > FRAG_BLOCK_MAX - fb->len)
			fb = _create_frag_block(fl);
	}

	return fb;
}

static int _read_fmt_frag(struct frag_head *fh, struct fmt_hdr *h,
		struct ringbuf *rb)
{
	int r;
	int data_len;
	int read_len;
	struct frag_list *fl;
	struct frag_block *fb;

	data_len = h->len - sizeof(struct fmt_hdr);
	read_len = data_len + sizeof(hdlc_end);

	_dbg("%s: data %d\n", __func__, data_len);

	fl = _find_frag_list(h->control, fh);
	if (!fl)
		fl = _create_frag_list(h->control, fh);

	if (!fl)
		return -ENOMEM;

	fb = _prepare_frag_block(fl, read_len);
	if (!fb) {
		if (fl->len == 0)
			_destroy_frag_list(fl, fh);

		return -ENOMEM;
	}

	r = __read(rb, fb->ptr, read_len);
	if (r != read_len) {
		_destroy_frag_list(fl, fh);
		return -EBADMSG;
	}

	fb->ptr += data_len;
	fb->len += data_len;
	fl->len += data_len;

	_dbg("%s: fl %p len %d fb %p ptr %p len %d\n", __func__,
			fl, fl->len, fb, fb->ptr, fb->len);

	return r;
}

static int _read_fmt_last(struct frag_head *fh, struct fmt_hdr *h,
		struct ringbuf *rb, struct net_device *ndev)
{
	int r;
	int data_len;
	int read_len;
	int total_len;
	struct sk_buff *skb;
	struct frag_list *fl;
	char *p;

	total_len = data_len = h->len - sizeof(struct fmt_hdr);
	read_len = data_len + sizeof(hdlc_end);

	fl = _find_frag_list(h->control & FMT_ID_MASK, fh);
	if (fl)
		total_len += fl->len;

	_dbg("%s: total %d data %d\n", __func__, total_len, data_len);

	skb = netdev_alloc_skb(ndev, total_len
			+ sizeof(struct phonethdr) + sizeof(hdlc_end));
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, sizeof(struct phonethdr));

	if (fl)
		_fill_skb(skb, fl);

	_destroy_frag_list(fl, fh);

	p = skb_put(skb, data_len);
	r = __read(rb, p, read_len);
	if (r != read_len) {
		kfree_skb(skb);
		return -EBADMSG;
	}

	_phonet_rx(ndev, skb, PN_FMT);

	return r;
}

#ifdef CONFIG_ARCH_U8500
static int _read_fmt_from_shrm(struct sipc *si, u8 *buf, u16 len)
{
	struct fmt_hdr *h;
	u32 tail;
	struct net_device *ndev = si->svndev;
	struct sk_buff *skb;
	int data_len;
	int read_len;
	int total_len;
	char *p;

	dev_dbg(&ndev->dev,"IN %s\n",__func__);

	for(tail=0;tail<len;tail++)
		dev_dbg(&ndev->dev,"rx[%d]=%x\n",tail,buf[tail]);

	h = (struct fmt_hdr *)&buf[sizeof(hdlc_start)];

	total_len = data_len = h->len - sizeof(struct fmt_hdr);
	read_len = data_len + sizeof(hdlc_end);
	dev_dbg(&ndev->dev,"Received pkt of len %x\n",h->len);

	skb = netdev_alloc_skb(ndev, total_len +
				sizeof (struct phonethdr) + sizeof(hdlc_end));
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, sizeof(struct phonethdr));
	p = skb_put(skb, data_len);
	memcpy(p, &buf[sizeof(hdlc_start) + sizeof(struct fmt_hdr)],read_len);
	_phonet_rx(ndev, skb, PN_FMT);

	dev_dbg(&ndev->dev,"OUT %s\n",__func__);
	return 0;
}

static int _read_isi_from_shrm(struct sipc *si, u8 *buf, u16 len)
{
        u32 tail;
        struct net_device *ndev = si->svndev;
        struct sk_buff *skb;

        dev_dbg(&ndev->dev,"IN %s\n",__func__);

        for(tail=0;tail<len;tail++)
                dev_dbg(&ndev->dev,"rx[%d]=%x\n",tail,buf[tail]);

	skb = netdev_alloc_skb(ndev, len);
	if (unlikely(!skb))
		return -ENOMEM;

	for(tail=0;tail<len;tail++)
               dev_dbg(&ndev->dev,"rx[%d]=%x\n",tail,buf[tail]);

	skb_copy_to_linear_data(skb, buf, len);
	skb_put(skb, len);
	skb_reset_mac_header(skb);
	__skb_pull(skb, ndev->hard_header_len);

	skb->dev = ndev;
	skb->protocol = htons(ETH_P_PHONET);
	skb->priority = 0;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

    /* restore svnet network address */
	if ((skb->data[0] == 0x26) || skb->data[0] == 0x00)
		skb->data[0] = 0xa0;

	if (likely(netif_rx_ni(skb) == NET_RX_SUCCESS)) {
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
	} else
		ndev->stats.rx_dropped++;

        dev_dbg(&ndev->dev,"OUT %s\n",__func__);
	return 0;
}

static int _read_pn_from_shrm(struct net_device *ndev, u8 *buf, int len,
		int res)
{
	int r = 0;
	struct sk_buff *skb;
	char *p;
	int read_len = len + sizeof(hdlc_end);

#ifdef DEBUG
	printk(KERN_DEBUG, "IN %s\n",__func__);
#endif
	skb = netdev_alloc_skb(ndev, read_len + sizeof(struct phonethdr));
	if(unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, sizeof(struct phonethdr));
	p = skb_put(skb, len);
	memcpy(p, &buf[sizeof(hdlc_start) + sizeof(struct raw_hdr)], read_len);
	_phonet_rx(ndev, skb, res);
#ifdef DEBUG
	printk(KERN_DEBUG, "OUT %s\n",__func__);
#endif
	return r;
}

static int _read_pdp_from_shrm(u8 *buf, int len, int res)
{
	int r;
	struct sk_buff *skb;
	char *p;
	int read_len = len + sizeof(hdlc_end);
	struct net_device *ndev;
	u8 buf_ver;
	const u8 *ip_version;

#ifdef DEBUG
	printk(KERN_DEBUG, "\n%s:++\n", __func__);
#endif

	spin_lock(&shm_raw);
//	mutex_lock(&pdp_mutex);

	ndev = pdp_devs[PDP_ID(res)];
	if (!ndev) {
		// drop data
		spin_unlock(&shm_raw);
//		mutex_unlock(&pdp_mutex);
		return -ENODEV;
	}

	skb = netdev_alloc_skb(ndev, read_len);
	if (unlikely(!skb)) {
		spin_unlock(&shm_raw);
//		mutex_unlock(&pdp_mutex);
		return -ENOMEM;
	}

	p = skb_put(skb, len);
	memcpy(p, &buf[sizeof(hdlc_start) + sizeof(struct raw_hdr)], read_len);


	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += skb->len;

	spin_unlock(&shm_raw);
//	mutex_unlock(&pdp_mutex);

	/* check the version of IP */
	ip_version = skb_header_pointer(skb, 0, 1, &buf_ver);
	if (!ip_version) {
		kfree_skb(skb);
		return -EINVAL;
	}
	switch (*ip_version >> 4) {
		case 4:
			skb->protocol = htons(ETH_P_IP);
			break;
		case 6:
			skb->protocol = htons(ETH_P_IPV6);
			break;
		default:
			kfree_skb(skb);
			return -EINVAL;
	}


	skb_reset_mac_header(skb);

	r = netif_rx_ni(skb);
	if (r != NET_RX_SUCCESS)
		dev_err(&ndev->dev, "pdp rx error: %d\n", r);

#ifdef DEBUG
	int i = 0;
	for (; i< len; i++)
		printk(KERN_INFO "%02x\t", skb->data[i]);
	printk(KERN_DEBUG, "\n%s: --\n", __func__);
	printk(KERN_DEBUG, "OUT %s\n",__func__);
#endif
	return read_len;

}

static int _read_raw_from_shrm(struct sipc *si, u8 *buf, u16 len)
{
	int res, data_len;
	struct net_device *ndev = si->svndev;
	int r = 0;

#ifdef DEBUG
	printk(KERN_DEBUG, "IN %s\n",__func__);
	int i;
	for (i=0; i< len; i++)
		printk(KERN_INFO "%02x\t", buf[i]);
	printk(KERN_DEBUG, "\n----------------------------------------\n\n");
#endif
/*
	dev_dbg(&ndev->dev, "IN %s\n", __func__);
*/
	_get_raw_hdr((struct raw_hdr *)&buf[sizeof(hdlc_start)],
			&res, &data_len, NULL);
#if 0
	if (res == 0x00)
		res = 0x2a;
#endif
	data_len -= sizeof(struct raw_hdr);

	if (res >= PN_PDP_START && res <= PN_PDP_END) {
		r = _read_pdp_from_shrm(buf, data_len, res);
	} else {
		r = _read_pn_from_shrm(si->svndev, buf, data_len, res);
	}

/*
	dev_dbg(&ndev->dev, "OUT %s\n",__func__);
*/
	return r;
}


#endif

static int _read_fmt(struct sipc *si, int inbuf, struct ringbuf *rb)
{
	int r;
	char buf[sizeof(struct fmt_hdr) + sizeof(hdlc_start)];
	struct fmt_hdr *h;
	u32 tail;
	struct net_device *ndev = si->svndev;

	h = (struct fmt_hdr *)&buf[sizeof(hdlc_start)];
	while (inbuf > 0) {
		tail = rb->rb_in_tail;

		r = __read(rb, buf, sizeof(buf));
		if (r < sizeof(buf) ||
				strncmp(buf, hdlc_start, sizeof(hdlc_start))) {
			dev_err(&ndev->dev, "Bad message: %c %d\n", buf[0], r);
			return -EBADMSG;
		}
		inbuf -= r;

		if (is_fmt_last(h->control))
			r = _read_fmt_last(&si->frag_map, h, rb, ndev);
		else
			r = _read_fmt_frag(&si->frag_map, h, rb);

		if (r < 0) {
			if (r == -ENOMEM)
				rb->rb_in_tail = tail;

			return r;
		}

		inbuf -= r;
	}

	return 0;
}

static inline int check_mailbox(u32 mailbox, int idx)
{
	return mailbox & mb_data[idx].mask_send;
}

static inline void purge_buffer(struct ringbuf *rb)
{
	rb->rb_in_tail = rb->rb_in_head;
}

int sipc_read(struct sipc *si, u32 mailbox, int *cond)
{
	int r = 0;
	int i;
	u32 res = 0;

	if (!si)
		return -EINVAL;

	r = _get_auth();
	if (r)
		return r;

	for (i=0;i<IPCIDX_MAX;i++) {
		int inbuf;
		struct ringbuf *rb;

//		if (!check_mailbox(mailbox, i))
//			continue;

		rb = &si->rb[i];
		inbuf = CIRC_CNT(rb->rb_in_head, rb->rb_in_tail, rb->rb_size);
		if (!inbuf)
			continue;

		if (i == IPCIDX_FMT)
			_fmt_wakelock_timeout();
		else
			_non_fmt_wakelock_timeout();

		_dbg("%s: %d bytes in %d\n", __func__, inbuf, i);

		r = rb->rb_read(si, inbuf, rb);
		if (r < 0) {
			if (r == -EBADMSG)
				purge_buffer(rb);

			dev_err(&si->svndev->dev, "read err %d\n", r);
			break;
		}

		if (mailbox & mb_data[i].mask_req_ack)
			res = mb_data[i].mask_res_ack;
	}

	_req_rel_auth(si);
	_put_auth(si);
#ifndef CONFIG_ARCH_U8500
	if (res)
		onedram_write_mailbox(MB_DATA(res));
#endif

	*cond =	skb_queue_len(&si->rfs_rx);

	return r;
}

int sipc_rx(struct sipc *si)
{
	int tx_cnt;
	struct sk_buff *skb;

	if (!si)
		return -EINVAL;

	if (skb_queue_len(&si->rfs_rx) == 0)
		return 0;

	tx_cnt = 0;
	skb = skb_dequeue(&si->rfs_rx);
	while (skb) {
		_phonet_rx(si->svndev, skb, PN_RFS);
		tx_cnt++;
		if (tx_cnt > RFS_TX_RATE)
			break;
		skb = skb_dequeue(&si->rfs_rx);
	}

	return skb_queue_len(&si->rfs_rx);
}

static inline ssize_t _debug_show_buf(struct sipc *si, char *buf)
{
	int i;
	int r;
	int inbuf, outbuf;
	char *p = buf;

	r = _get_auth();
	if (r) {
		p += sprintf(p, "\nGet authority: timed out!\n");
		return p - buf;
	}

	p += sprintf(p, "\nHeader info ---------\n");

	for (i=0;i<IPCIDX_MAX;i++) {
		struct ringbuf *rb = &si->rb[i];
		inbuf = CIRC_CNT(rb->rb_in_head, rb->rb_in_tail, rb->rb_size);
		outbuf = CIRC_CNT(rb->rb_out_head, rb->rb_out_tail, rb->rb_size);
		p += sprintf(p, "%d\tSize\t%8u\n\tIn\t%8u\t%8u\t%8u\n\tOut\t%8u\t%8u\t%8u\n",
				i, rb->rb_size,
				rb->rb_in_head, rb->rb_in_tail, inbuf,
				rb->rb_out_head, rb->rb_out_tail, outbuf);
	}
	_put_auth(si);

	return p - buf;
}

static inline ssize_t _debug_show_pdp(struct sipc *si, char *buf)
{
	int i;
	char *p = buf;

	p += sprintf(p, "\nPDP count: %d\n", pdp_cnt);

	mutex_lock(&pdp_mutex);
	for (i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
		if (pdp_devs[i])
			p += sprintf(p, "pdp%d: %d", i,
					netif_queue_stopped(pdp_devs[i]));
	}
	mutex_unlock(&pdp_mutex);

	return p - buf;
}

ssize_t sipc_debug_show(struct sipc *si, char *buf)
{
	char *p = buf;

	if (!si || !buf)
		return 0;

	p += _debug_show_buf(si, p);

	p += _debug_show_pdp(si, p);

	p += sprintf(p, "\nDebug command -----------\n");
	p += sprintf(p, "R0\tcopy FMT out to in\n");
	p += sprintf(p, "R1\tcopy RAW out to in\n");
	p += sprintf(p, "R2\tcopy RFS out to in\n");

	return p - buf;
}

static void test_copy_buf(struct sipc *si, int idx)
{
	struct ringbuf *rb;

	if(idx >= IPCIDX_MAX)
		return;

	rb = (struct ringbuf *)&si->rb[idx];

	memcpy(rb->in_base, rb->out_base, rb->info->size);
	rb->rb_in_head = rb->rb_out_head;
	rb->rb_in_tail = rb->rb_out_tail;

	rb->rb_out_tail = rb->rb_out_head;

	if (si->queue)
		si->queue(MB_DATA(mb_data[idx].mask_send), si->queue_data);
}

int sipc_debug(struct sipc *si, const char *buf)
{
	int r;

	if (!si || !buf)
		return -EINVAL;

	r = _get_auth();
	if (r)
		return r;

	switch (buf[0]) {
	case 'R':
		test_copy_buf(si, buf[1]-'0');
		break;
	default:
		/* do nothing */
		break;
	}
	_put_auth(si);

	return 0;
}

#ifndef CONFIG_ARCH_U8500
int sipc_whitelist(struct sipc *si, const char *buf, size_t count)
{
	int r;
	struct ringbuf *rb;

	if (!si || !buf)
		return -EINVAL;

	r = _get_auth();
	if (r)
		return r;

	rb = (struct ringbuf *)&si->rb[IPCIDX_FMT];

	//write direct full-established-packet to buf
	r =  __write(rb,(u8 *) buf, (unsigned int )count);

	_req_rel_auth(si);
	_put_auth(si);

	onedram_write_mailbox(MB_DATA(mb_data[IPCIDX_FMT].mask_send));
	return r;
}
#else // To adjust white list
int sipc_whitelist(struct sipc *si, const char *buf, size_t count)
{

	int err, ret = 0;
	/*send message to modem*/
	err = shrm_host_modem_msg_send(IPCCTRL, (void *)buf, count);
	if (!err)
		ret = count;
	else
		ret = 0;

	pr_info("[Message To CP] Ret: %d\n", ret);
	return ret;
}
#endif

int sipc_check_skb(struct sipc *si, struct sk_buff *skb)
{
	struct phonethdr *ph;

	ph = pn_hdr(skb);

	if (ph->pn_res == PN_CMD)
		return 1;

	return 0;
}

int sipc_do_cmd(struct sipc *si, struct sk_buff *skb)
{
	if (!si)
		return -EINVAL;

	skb_pull(skb, sizeof(struct phonethdr) + 1);

	// TODO:
	if (!strncmp("PHONE_ON", skb->data, sizeof("PHONE_ON"))) {

		return 0;
	}

	return 0;
}

static int pdp_activate(struct net_device *svndev, int channel)
{
	int idx;
	struct net_device *ndev;

	if (!svndev || channel < 1 || channel > PDP_MAX)
		return -EINVAL;

	idx = channel - 1; /* start from 0 */

	mutex_lock(&pdp_mutex);

	if (pdp_devs[idx]) {
		mutex_unlock(&pdp_mutex);
		return -EBUSY;
	}

	ndev = create_pdp(channel, svndev);
	if (IS_ERR(ndev)) {
		mutex_unlock(&pdp_mutex);
		return PTR_ERR(ndev);
	}

	pdp_devs[idx] = ndev;
	pdp_cnt++;

	mutex_unlock(&pdp_mutex);

	return 0;
}

static int pdp_deactivate(int channel)
{
	int idx;

	if (channel < 1 || channel > PDP_MAX)
		return -EINVAL;

	idx = channel - 1; /* start from 0 */

	mutex_lock(&pdp_mutex);

	if (!pdp_devs[idx]) {
		mutex_unlock(&pdp_mutex);
		return -EBUSY;
	}

	destroy_pdp(&pdp_devs[idx]);
	clear_bit(idx, pdp_bitmap);
	pdp_cnt--;

	mutex_unlock(&pdp_mutex);

	return 0;
}

static ssize_t show_act(struct device *d,
		struct device_attribute *attr, char *buf)
{
	int i;
	char *p = buf;

	mutex_lock(&pdp_mutex);

	for (i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
		if (pdp_devs[i])
			p += sprintf(p, "%d\n", (i+1));
	}

	mutex_unlock(&pdp_mutex);

	return p - buf;
}

static ssize_t show_deact(struct device *d,
		struct device_attribute *attr, char *buf)
{
	int i;
	char *p = buf;

	mutex_lock(&pdp_mutex);

	for(i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
		if (!pdp_devs[i])
			p += sprintf(p, "%d\n", (i+1));
	}

	mutex_unlock(&pdp_mutex);

	return p - buf;
}

static ssize_t store_act(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int r;
	unsigned long chan;
	struct net_device *ndev = to_net_dev(d);

	if (!ndev)
		return count;

	r = strict_strtoul(buf, 10, &chan);
	if (!r)
		r = pdp_activate(ndev, chan);

	if (r)
		dev_err(&ndev->dev, "Failed to activate pdp "
				" channel %lu: %d\n", chan, r);

	return count;
}

static ssize_t store_deact(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int r;
	unsigned long chan;
	struct net_device *ndev = to_net_dev(d);

	if (!ndev)
		return count;

	r = strict_strtoul(buf, 10, &chan);
	if (!r)
		r = pdp_deactivate(chan);

	if (r)
		dev_err(&ndev->dev, "Failed to deactivate pdp"
				" channel %lu: %d\n", chan, r);

	return count;
}

static ssize_t show_suspend(struct device *d,
		struct device_attribute *attr, char *buf)
{
	int i;
	char *p = buf;

	mutex_lock(&pdp_mutex);

	for (i=0;i<sizeof(pdp_devs)/sizeof(pdp_devs[0]);i++) {
		if (test_bit(i, pdp_bitmap))
			p += sprintf(p, "%d\n", (i+1));
	}

	mutex_unlock(&pdp_mutex);

	return p - buf;
}

static ssize_t store_suspend(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int r;
	unsigned long chan;
	int id;

	r = strict_strtoul(buf, 10, &chan);
	if (r)
		return count;

	if (chan < 1 || chan > PDP_MAX)
		return count;

	id = chan - 1;

	mutex_lock(&pdp_mutex);

	set_bit(id, pdp_bitmap);

	if (pdp_devs[id])
		netif_stop_queue(pdp_devs[id]);

	mutex_unlock(&pdp_mutex);

	return count;
}

static ssize_t store_resume(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int r;
	unsigned long chan;
	int id;

	r = strict_strtoul(buf, 10, &chan);
	if (r)
		return count;

	if (chan < 1 || chan > PDP_MAX)
		return count;

	id = chan - 1;

	mutex_lock(&pdp_mutex);

	clear_bit(id, pdp_bitmap);

	if (pdp_devs[id])
		netif_wake_queue(pdp_devs[id]);

	mutex_unlock(&pdp_mutex);

	return count;
}

void createPDP(struct net_device *svn_netdev)
{
	struct net_device *pdp_ndev;

	if(svn_netdev == NULL)
		pr_err("[%s] svnet_dev is NULL\n", __func__);
	else {
		int idx;
		for(idx = 0; idx < 3; idx++) {

			mutex_lock(&pdp_mutex);

			if (pdp_devs[idx]) {
				mutex_unlock(&pdp_mutex);
				pr_err("[%s] There is pdp_devs[%d]\n", __func__,
					idx);
			}

			pdp_ndev = create_pdp(idx + 1, svn_netdev);
			if (IS_ERR(pdp_ndev)) {
				mutex_unlock(&pdp_mutex);
				pr_err("[%s] ndev err[%ld]\n", __func__,
					PTR_ERR(pdp_ndev));
			}

			pdp_devs[idx] = pdp_ndev;
			pdp_cnt++;

			mutex_unlock(&pdp_mutex);
		}
		pr_info("[%s] Done\n", __func__);
	}
}


void sipc_ramdump(struct sipc *si)
{
#if defined(CONFIG_KERNEL_DEBUG_SEC)
	/* silent reset at debug level low */
	if ( kernel_sec_get_debug_level() == KERNEL_SEC_DEBUG_LEVEL_LOW )
		return;
#endif
	_go_dump(si);
}

#if defined(CONFIG_KERNEL_DEBUG_SEC)
static void _go_dump(struct sipc *si)
{
	int r;
	t_kernel_sec_mmu_info mmu_info;

	memset(cp_errmsg, 0, sizeof(cp_errmsg));

	r = _get_auth();
	if (r)
		strcpy(cp_errmsg, ERRMSG);
	else {
		char *p;
		p = (char *)si->map + FATAL_DISP;
		memcpy(cp_errmsg, p, sizeof(cp_errmsg));
	}

	printk(KERN_INFO "CP Dump Cause - %s\n", cp_errmsg);

//	kernel_sec_set_cause_strptr(cp_errmsg, sizeof(cp_errmsg));
	kernel_sec_set_upload_magic_number();
	kernel_sec_get_mmu_reg_dump(&mmu_info);
	kernel_sec_set_upload_cause(UPLOAD_CAUSE_CP_ERROR_FATAL);
	kernel_sec_hw_reset(false);

	// Never Return!!!
}
#endif

