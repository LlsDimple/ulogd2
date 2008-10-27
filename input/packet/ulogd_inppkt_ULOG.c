/* ulogd_inppkt_ULOG.c - stackable input plugin for ULOG packets -> ulogd2
 *
 * (C) 2004-2005 by Harald Welte <laforge@gnumonks.org>
 */
#include "config.h"
#include <ulogd/ulogd.h>
#include <ulogd/common.h>
#include <ulogd/plugin.h>
#include <libipulog/libipulog.h>

#include <unistd.h>
#include <stdlib.h>


#ifndef ULOGD_NLGROUP_DEFAULT
#define ULOGD_NLGROUP_DEFAULT	32
#endif

/* Size of the socket recevive memory.  Should be at least the same size as the
 * 'nlbufsiz' module loadtime parameter of ipt_ULOG.o
 * If you have _big_ in-kernel queues, you may have to increase this number.  (
 * --qthreshold 100 * 1500 bytes/packet = 150kB  */
#define ULOGD_RMEM_DEFAULT	131071

/* Size of the receive buffer for the netlink socket.  Should be at least of
 * RMEM_DEFAULT size.  */
#define ULOGD_BUFSIZE_DEFAULT	150000

struct ulog_input {
	struct ipulog_handle *libulog_h;
	unsigned char *libulog_buf;
	struct ulogd_fd ulog_fd;
};

/* configuration entries */

static const struct config_keyset libulog_kset = {
	.num_ces = 3,
	.ces = {
	{
		.key 	 = "bufsize",
		.type 	 = CONFIG_TYPE_INT,
		.options = CONFIG_OPT_NONE,
		.u.value = ULOGD_BUFSIZE_DEFAULT,
	},
	{
		.key	 = "nlgroup",
		.type	 = CONFIG_TYPE_INT,
		.options = CONFIG_OPT_NONE,
		.u.value = ULOGD_NLGROUP_DEFAULT,
	},
	{
		.key	 = "rmem",
		.type	 = CONFIG_TYPE_INT,
		.options = CONFIG_OPT_NONE,
		.u.value = ULOGD_RMEM_DEFAULT,
	},
	}
};

static struct ulogd_key output_keys[] = {
	{ 
		.type = ULOGD_RET_STRING, 
		.flags = ULOGD_RETF_FREE, 
		.name = "raw.mac", 
	},
	{
		.type = ULOGD_RET_RAW,
		.flags = ULOGD_RETF_FREE,
		.name = "raw.pkt",
		.ipfix = {
			.vendor = IPFIX_VENDOR_NETFILTER,
			.field_id = 1,
			},
	},
	{
		.type = ULOGD_RET_UINT32,
		.flags = ULOGD_RETF_NONE,
		.name = "raw.pktlen",
		.ipfix = { 
			.vendor = IPFIX_VENDOR_IETF,
			.field_id = 1
		},
	},
	{
		.type = ULOGD_RET_UINT32,
		.flags = ULOGD_RETF_NONE,
		.name = "raw.pktcount",
		.ipfix = { 
			.vendor = IPFIX_VENDOR_IETF,
			.field_id = 2
		},
	},
	{
		.type = ULOGD_RET_STRING,
		.flags = ULOGD_RETF_NONE, 
		.name = "oob.prefix", 
	},
	{ 	.type = ULOGD_RET_UINT32, 
		.flags = ULOGD_RETF_NONE, 
		.name = "oob.time.sec", 
		.ipfix = { 
			.vendor = IPFIX_VENDOR_IETF, 
			.field_id = 22 
		},
	},
	{
		.type = ULOGD_RET_UINT32,
		.flags = ULOGD_RETF_NONE,
		.name = "oob.time.usec", 
	},
	{
		.type = ULOGD_RET_UINT32,
		.flags = ULOGD_RETF_NONE,
		.name = "oob.mark", 
	},
	{
		.type = ULOGD_RET_STRING,
		.flags = ULOGD_RETF_NONE,
		.name = "oob.in", 
	},
	{
		.type = ULOGD_RET_STRING,
		.flags = ULOGD_RETF_NONE,
		.name = "oob.out", 
	},
};

static int interp_packet(struct ulogd_pluginstance *ip, ulog_packet_msg_t *pkt)
{
	unsigned char *p;
	int i;
	char *buf, *oldbuf = NULL;
	struct ulogd_key *ret = ip->output.keys;

	if (pkt->mac_len) {
		buf = (char *) malloc(3 * pkt->mac_len + 1);
		if (!buf) {
			ulogd_log(ULOGD_ERROR, "OOM!!!\n");
			return -1;
		}
		*buf = '\0';

		p = pkt->mac;
		oldbuf = buf;
		for (i = 0; i < pkt->mac_len; i++, p++)
			sprintf(buf, "%s%02x%c", oldbuf, *p, i==pkt->mac_len-1 ? ' ':':');
		key_set_ptr(&ret[0], buf);
	}

	/* include pointer to raw ipv4 packet */
	key_set_ptr(&ret[1], pkt->payload);
	key_set_u32(&ret[2], pkt->data_len);
	key_set_u32(&ret[3], 1);

	key_set_ptr(&ret[4], pkt->prefix);

	/* god knows why timestamp_usec contains crap if timestamp_sec == 0
	 * if (pkt->timestamp_sec || pkt->timestamp_usec) { */
	if (pkt->timestamp_sec) {
		key_set_u32(&ret[5], pkt->timestamp_sec);
		key_set_u32(&ret[6], pkt->timestamp_usec);
	} else {
		ret[5].flags &= ~ULOGD_RETF_VALID;
		ret[6].flags &= ~ULOGD_RETF_VALID;
	}

	key_set_u32(&ret[7], pkt->mark);
	key_set_ptr(&ret[8], pkt->indev_name);
	key_set_ptr(&ret[9], pkt->outdev_name);
	
	ulogd_propagate_results(ip);
	return 0;
}

static int ulog_read_cb(int fd, unsigned int what, void *param)
{
	struct ulogd_pluginstance *upi = (struct ulogd_pluginstance *)param;
	struct ulog_input *u = upi_priv(upi);
	ulog_packet_msg_t *upkt;
	int len;

	if (!(what & ULOGD_FD_READ))
		return 0;

	while ((len = ipulog_read(u->libulog_h, u->libulog_buf,
				 upi->config_kset->ces[0].u.value, 1))) {
		if (len <= 0) {
			/* this is not supposed to happen */
			ulogd_log(ULOGD_ERROR, "ipulog_read = %d! "
				  "ipulog_errno = %d, errno = %d\n",
				  len, ipulog_errno, errno);
			break;
		}
		while ((upkt = ipulog_get_packet(u->libulog_h,
						 u->libulog_buf, len))) {
			ulogd_log(ULOGD_DEBUG, "==> ulog packet received\n");
			interp_packet(upi, upkt);
		}
	}
	return 0;
}

static int
ulog_configure(struct ulogd_pluginstance *upi)
{
	struct ulog_input *ui = upi_priv(upi);

	ulogd_init_fd(&ui->ulog_fd, -1, ULOGD_FD_READ, ulog_read_cb, upi);

	return 0;
}

static int init(struct ulogd_pluginstance *upi)
{
	struct ulog_input *ui = upi_priv(upi);

	ui->libulog_buf = malloc(upi->config_kset->ces[0].u.value);
	if (!ui->libulog_buf) {
		ulogd_log(ULOGD_ERROR, "Out of memory\n");
		goto out_buf;
	}

	ui->libulog_h = ipulog_create_handle(
				ipulog_group2gmask(upi->config_kset->ces[1].u.value),
				upi->config_kset->ces[2].u.value);
	if (!ui->libulog_h) {
		ulogd_log(ULOGD_ERROR, "Can't create ULOG handle\n");
		goto out_handle;
	}

	ui->ulog_fd.fd = ipulog_get_fd(ui->libulog_h);

	ulogd_register_fd(&ui->ulog_fd);

	return 0;

out_handle:
	free(ui->libulog_buf);
out_buf:
	return -1;
}

static int fini(struct ulogd_pluginstance *pi)
{
	struct ulog_input *ui = upi_priv(pi);

	ulogd_unregister_fd(&ui->ulog_fd);
	free(pi);

	return 0;
}

static struct ulogd_plugin libulog_plugin = {
	.name = "ULOG",
	.input = {
		.type = ULOGD_DTYPE_SOURCE,
		.keys = NULL,
		.num_keys = 0,
	},
	.output = {
		.type = ULOGD_DTYPE_RAW,
		.keys = output_keys,
		.num_keys = sizeof(output_keys)/sizeof(struct ulogd_key),
	},
	.configure = ulog_configure,
	.start = &init,
	.stop = &fini,
	.config_kset = &libulog_kset,
	.rev = ULOGD_PLUGIN_REVISION,
	.priv_size = sizeof(struct ulog_input),
};

void __upi_ctor initializer(void)
{
	ulogd_register_plugin(&libulog_plugin);
}
