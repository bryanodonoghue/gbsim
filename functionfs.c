/*
 * Greybus Simulator
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Provided under the three clause BSD license found in the LICENSE file.
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/usb/functionfs.h>

#include "gbsim.h"
#include "config.h"

#define FFS_PREFIX	"/dev/ffs-gbsim/"
#define FFS_GBEMU_EP0	FFS_PREFIX"ep0"
#define FFS_GBEMU_SVC	FFS_PREFIX"ep1"
#define FFS_GBEMU_IN	FFS_PREFIX"ep2"
#define FFS_GBEMU_OUT	FFS_PREFIX"ep3"

#define STR_INTERFACE	"gbsim"

#define NEVENT		5

int control = -ENXIO;
int to_ap = -ENXIO;
int from_ap = -ENXIO;

static pthread_t recv_pthread;

#define GBSIM_LEGACY_DESCRIPTORS

/* 
 * Descriptors:
 *
 * EP0 [control]	- Ch9 and SVC inbound messages
 * EP1 [interrupt in]	- SVC outbound events/messages
 * EP2 [bulk in]	- CPort outbound messages
 * EP3 [bulk out]	- CPort inbound messages
 */
static const struct {
	struct {
		__le32 magic;
		__le32 length;
#ifndef GBSIM_LEGACY_DESCRIPTORS
		__le32 flags;
#endif
		__le32 fs_count;
		__le32 hs_count;
	} __attribute__((packed)) header;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio svc_in;
		struct usb_endpoint_descriptor_no_audio to_ap;
		struct usb_endpoint_descriptor_no_audio from_ap;
	} __attribute__((packed)) fs_descs, hs_descs;
} __attribute__((packed)) descriptors = {
	.header = {
#ifdef GBSIM_LEGACY_DESCRIPTORS
		.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC),
#else
		.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.flags = htole32(FUNCTIONFS_HAS_FS_DESC |
				     FUNCTIONFS_HAS_HS_DESC),
#endif
		.length = htole32(sizeof descriptors),
		.fs_count = htole32(4),
		.hs_count = htole32(4),
	},
	.fs_descs = {
		.intf = {
			.bLength = sizeof descriptors.fs_descs.intf,
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 3,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.svc_in = {
			.bLength = sizeof descriptors.fs_descs.svc_in,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.bInterval = 10,
			.wMaxPacketSize = 64
		},
		.to_ap = {
			.bLength = sizeof descriptors.fs_descs.to_ap,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = 64
		},
		.from_ap = {
			.bLength = sizeof descriptors.fs_descs.from_ap,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 3 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = 64
		},
	},
	.hs_descs = {
		.intf = {
			.bLength = sizeof descriptors.hs_descs.intf,
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 3,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.svc_in = {
			.bLength = sizeof descriptors.hs_descs.svc_in,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.bInterval = 10,
			.wMaxPacketSize = 512,
		},
		.to_ap = {
			.bLength = sizeof descriptors.hs_descs.to_ap,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = 512,
		},
		.from_ap = {
			.bLength = sizeof descriptors.hs_descs.from_ap,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 3 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = 512,
		},
	},
};

static const struct {
	struct usb_functionfs_strings_head header;
	struct {
		__le16 code;
		const char str1[sizeof STR_INTERFACE];
	} __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
	.header = {
		.magic = htole32(FUNCTIONFS_STRINGS_MAGIC),
		.length = htole32(sizeof strings),
		.str_count = htole32(1),
		.lang_count = htole32(1),
	},
	.lang0 = {
		htole16(0x0409), /* en-us */
		STR_INTERFACE,
	},
};

/*
 * Endpoint handling
 */

void cleanup_endpoint(int ep_fd, char *ep_name)
{
	int ret;

	if (ep_fd < 0)
		return;

	ret = ioctl(ep_fd, FUNCTIONFS_FIFO_STATUS);
	if (ret < 0) {
		/* ENODEV reported after disconnect */
		if(errno != ENODEV)
			gbsim_error("get fifo status(%s): %s \n", ep_name, strerror(errno));
	} else if(ret) {
			gbsim_error("%s: unclaimed = %d \n", ep_name, ret);
		if(ioctl(ep_fd, FUNCTIONFS_FIFO_FLUSH) < 0)
			gbsim_error("%s: fifo flush \n", ep_name);
	}

	if(close(ep_fd) < 0)
		gbsim_error("%s: close \n", ep_name);
}

static int enable_endpoints(void)
{
	int ret;

	/* Start SVC/CPort endpoints here */
	gbsim_debug("Start SVC/CPort endpoints\n");

	to_ap = open(FFS_GBEMU_IN, O_RDWR);
	if (to_ap < 0)
		return to_ap;

	from_ap = open(FFS_GBEMU_OUT, O_RDWR);
	if (from_ap < 0)
		return from_ap;

	ret = pthread_create(&recv_pthread, NULL, recv_thread, NULL);
	if (ret < 0) {
		perror("can't create cport thread");
		return ret;
	}

	/*
	 * Start communication with the AP in following sequence:
	 * - Send a svc protocol version request
	 * - For a valid response, send the 'hello' message.
	 */
	ret = svc_request_send(GB_REQUEST_TYPE_PROTOCOL_VERSION, AP_INTF_ID);
	if (ret) {
		gbsim_error("Failed to send svc version request (%d)\n", ret);
		return ret;
	}

	return 0;
}

static void disable_endpoints(void)
{
	gbsim_debug("Disable SVC/CPort endpoints\n");

	if (to_ap < 0 || from_ap < 0)
		return;

	pthread_cancel(recv_pthread);
	pthread_join(recv_pthread, NULL);

	close(from_ap);
	from_ap = -EINVAL;
	close(to_ap);
	to_ap = -EINVAL;
}

static int read_control(void)
{
	struct usb_functionfs_event event[NEVENT];
	int i, nevent, ret;

	static const char *const names[] = {
		[FUNCTIONFS_BIND] = "BIND",
		[FUNCTIONFS_UNBIND] = "UNBIND",
		[FUNCTIONFS_ENABLE] = "ENABLE",
		[FUNCTIONFS_DISABLE] = "DISABLE",
		[FUNCTIONFS_SETUP] = "SETUP",
		[FUNCTIONFS_SUSPEND] = "SUSPEND",
		[FUNCTIONFS_RESUME] = "RESUME",
	};

	ret = read(control, &event, sizeof(event));
	if (ret < 0) {
		if (errno == EAGAIN) {
			sleep(1);
			return ret;
		}
		perror("ep0 read after poll");
		return ret;
	}
	nevent = ret/ sizeof event[0];

	for (i = 0; i < nevent; i++) {
		gbsim_debug("USB %s\n", names[event->type]);

		switch (event[i].type) {
		case FUNCTIONFS_BIND:
			break;
		case FUNCTIONFS_UNBIND:
			break;
		case FUNCTIONFS_ENABLE:
			enable_endpoints();
			break;
		case FUNCTIONFS_DISABLE:
			disable_endpoints();
			break;
		case FUNCTIONFS_SETUP:
			break;
		case FUNCTIONFS_SUSPEND:
			break;
		case FUNCTIONFS_RESUME:
			break;
		default:
			gbsim_error("unknown event %d\n", event[i].type);
		}
	}

	return ret;
}

static void functionfs_init_gb(void)
{
	int ret;

	control = open(FFS_GBEMU_EP0, O_RDWR);
	if (control < 0) {
		perror(FFS_GBEMU_EP0);
		control = -errno;
		return;
	}

	ret = write(control, &descriptors, sizeof(descriptors));
	if (ret < 0) {
		perror("write dev descriptors");
		close(control);
		control = -errno;
		return;
	}

	ret = write(control, &strings, sizeof(strings));
	if (ret < 0) {
		perror("write dev strings");
		close(control);
		control = -errno;
		return;
	}

	return;
}

int functionfs_loop(void)
{
	struct pollfd ep_poll[1];
	int ret;

	do {
		/* Always listen on control */
		ep_poll[0].fd = control;
		ep_poll[0].events = POLLIN | POLLHUP;

		ret = poll(ep_poll, 1, -1);
		if (ret < 0) {
			perror("poll");
			break;
		}

		/* TODO: What to do with HUP? */
		if (ep_poll[0].revents & POLLIN) {
			ret = read_control();
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;
				goto done;
			}
		}
	} while (1);

	return 0;

done:
	return ret;
}

int functionfs_init(void)
{
	/* Mount functionfs */
	mkdir(FFS_PREFIX, S_IRWXU|S_IRWXG|S_IRWXO);
	mount("gbsim", FFS_PREFIX, "functionfs", 0, NULL);

	/* Configure the Greybus emulator */
	functionfs_init_gb();

	return 0;
}

int functionfs_cleanup(void)
{
	recv_thread_cleanup(NULL);

	return 0;
}
