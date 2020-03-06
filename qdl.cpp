#include "qdl.h"

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <libudev.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdbool>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include "firehose.h"
#include "patch.h"
#include "program.h"
#include "sahara.h"
#include "ufs.h"

bool qdl_debug;
bool fw_only;

enum class qdl_file {
	unknown,
	patch,
	program,
	ufs,
	contents,
};

static qdl_file detect_type(const char* xml_file) {
	xmlNode* root;
	xmlDoc* doc;
	xmlNode* node;
	qdl_file type = qdl_file::unknown;

	doc = xmlReadFile(xml_file, NULL, 0);
	if (!doc) {
		std::cerr << "[PATCH] failed to parse " << xml_file << std::endl;
		return qdl_file::unknown;
	}

	root = xmlDocGetRootElement(doc);
	if (!xmlStrcmp(root->name, (xmlChar*)"patches")) {
		type = qdl_file::patch;
	} else if (!xmlStrcmp(root->name, (xmlChar*)"data")) {
		for (node = root->children; node; node = node->next) {
			if (node->type != XML_ELEMENT_NODE)
				continue;
			if (!xmlStrcmp(node->name, (xmlChar*)"program")) {
				type = qdl_file::program;
				break;
			}
			if (!xmlStrcmp(node->name, (xmlChar*)"ufs")) {
				type = qdl_file::ufs;
				break;
			}
		}
	} else if (!xmlStrcmp(root->name, (xmlChar*)"contents")) {
		type = qdl_file::contents;
	}

	xmlFreeDoc(doc);

	return type;
}

int Qdl::parse_usb_desc(int fd, int* intf) {
	const struct usb_interface_descriptor* ifc;
	const struct usb_endpoint_descriptor* ept;
	const struct usb_device_descriptor* dev;
	const struct usb_config_descriptor* cfg;
	const struct usb_descriptor_header* hdr;
	unsigned type;
	unsigned out;
	unsigned in;
	unsigned k;
	unsigned l;
	ssize_t n;
	size_t out_size;
	size_t in_size;
	void* ptr;
	void* end;
	char desc[1024];

	n = ::read(fd, desc, sizeof(desc));
	if (n < 0)
		return n;

	ptr = (void*)desc;
	end = ptr + n;

	dev = (usb_device_descriptor*)ptr;

	/* Consider only devices with vid 0x0506 and product id 0x9008 */
	if (dev->idVendor != 0x05c6 || dev->idProduct != 0x9008)
		return -EINVAL;

	ptr += dev->bLength;
	if (ptr >= end || dev->bDescriptorType != USB_DT_DEVICE)
		return -EINVAL;

	cfg = (usb_config_descriptor*)ptr;
	ptr += cfg->bLength;
	if (ptr >= end || cfg->bDescriptorType != USB_DT_CONFIG)
		return -EINVAL;

	for (k = 0; k < cfg->bNumInterfaces; k++) {
		if (ptr >= end)
			return -EINVAL;

		do {
			ifc = (usb_interface_descriptor*)ptr;
			if (ifc->bLength < USB_DT_INTERFACE_SIZE)
				return -EINVAL;

			ptr += ifc->bLength;
		} while (ptr < end && ifc->bDescriptorType != USB_DT_INTERFACE);

		in = -1;
		out = -1;
		in_size = 0;
		out_size = 0;

		for (l = 0; l < ifc->bNumEndpoints; l++) {
			if (ptr >= end)
				return -EINVAL;

			do {
				ept = (usb_endpoint_descriptor*)ptr;
				if (ept->bLength < USB_DT_ENDPOINT_SIZE)
					return -EINVAL;

				ptr += ept->bLength;
			} while (ptr < end && ept->bDescriptorType != USB_DT_ENDPOINT);

			type = ept->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
			if (type != USB_ENDPOINT_XFER_BULK)
				continue;

			if (ept->bEndpointAddress & USB_DIR_IN) {
				in = ept->bEndpointAddress;
				in_size = ept->wMaxPacketSize;
			} else {
				out = ept->bEndpointAddress;
				out_size = ept->wMaxPacketSize;
			}

			if (ptr >= end)
				break;

			hdr = (usb_descriptor_header*)ptr;
			if (hdr->bDescriptorType == USB_DT_SS_ENDPOINT_COMP)
				ptr += USB_DT_SS_EP_COMP_SIZE;
		}

		if (ifc->bInterfaceClass != 0xff)
			continue;

		if (ifc->bInterfaceSubClass != 0xff)
			continue;

		/* bInterfaceProtocol of 0xff and 0x10 has been seen */
		if (ifc->bInterfaceProtocol != 0xff && ifc->bInterfaceProtocol != 16)
			continue;

		this->fd = fd;
		this->in_ep = in;
		this->out_ep = out;
		this->in_maxpktsize = in_size;
		this->out_maxpktsize = out_size;

		*intf = ifc->bInterfaceNumber;

		return 0;
	}

	return -ENOENT;
}

int Qdl::usb_open() {
	struct udev_enumerate* enumerate;
	struct udev_list_entry* devices;
	struct udev_list_entry* dev_list_entry;
	struct udev_monitor* mon;
	struct udev_device* dev;
	const char* dev_node;
	struct udev* udev;
	const char* path;
	usbdevfs_ioctl cmd;
	int mon_fd;
	int intf = -1;
	int ret;
	int fd;

	udev = udev_new();
	if (!udev)
		err(1, "failed to initialize udev");

	mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", NULL);
	udev_monitor_enable_receiving(mon);
	mon_fd = udev_monitor_get_fd(mon);

	enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(dev_list_entry, devices) {
		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);
		dev_node = udev_device_get_devnode(dev);

		if (!dev_node)
			continue;

		fd = open(dev_node, O_RDWR);
		if (fd < 0)
			continue;

		ret = Qdl::parse_usb_desc(fd, &intf);
		if (!ret)
			goto found;

		close(fd);
	}

	std::cerr << "Waiting for EDL device" << std::endl;

	for (;;) {
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(mon_fd, &rfds);

		ret = select(mon_fd + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0)
			return -1;

		if (!FD_ISSET(mon_fd, &rfds))
			continue;

		dev = udev_monitor_receive_device(mon);
		dev_node = udev_device_get_devnode(dev);

		if (!dev_node)
			continue;

		std::cout << dev_node << std::endl;

		fd = open(dev_node, O_RDWR);
		if (fd < 0)
			continue;

		ret = Qdl::parse_usb_desc(fd, &intf);
		if (!ret)
			goto found;

		close(fd);
	}

	udev_enumerate_unref(enumerate);
	udev_monitor_unref(mon);
	udev_unref(udev);

	return -ENOENT;

found:
	udev_enumerate_unref(enumerate);
	udev_monitor_unref(mon);
	udev_unref(udev);

	cmd.ifno = intf;
	cmd.ioctl_code = USBDEVFS_DISCONNECT;
	cmd.data = NULL;

	ret = ioctl(this->fd, USBDEVFS_IOCTL, &cmd);
	if (ret && errno != ENODATA)
		err(1, "failed to disconnect kernel driver");

	ret = ioctl(this->fd, USBDEVFS_CLAIMINTERFACE, &intf);
	if (ret < 0)
		err(1, "failed to claim USB interface");

	return 0;
}

int Qdl::read(void* buf, size_t len, unsigned int timeout) {
	struct usbdevfs_bulktransfer bulk;

	bulk.ep = this->in_ep;
	bulk.len = len;
	bulk.data = buf;
	bulk.timeout = timeout;

	return ioctl(this->fd, USBDEVFS_BULK, &bulk);
}

int Qdl::write(const void* buf, size_t len, bool eot) {
	unsigned char* data = (unsigned char*)buf;
	struct usbdevfs_bulktransfer bulk;
	unsigned count = 0;
	size_t len_orig = len;
	int n;

	if (len == 0) {
		bulk.ep = this->out_ep;
		bulk.len = 0;
		bulk.data = data;
		bulk.timeout = 1000;

		n = ioctl(this->fd, USBDEVFS_BULK, &bulk);
		if (n != 0) {
			std::cerr << "ERROR: n = " << n << ", errno = " << errno << " ("
					  << strerror(errno) << ")" << std::endl;
			return -1;
		}
		return 0;
	}

	while (len > 0) {
		int xfer;
		xfer = (len > this->out_maxpktsize) ? this->out_maxpktsize : len;

		bulk.ep = this->out_ep;
		bulk.len = xfer;
		bulk.data = data;
		bulk.timeout = 1000;

		n = ioctl(this->fd, USBDEVFS_BULK, &bulk);
		if (n != xfer) {
			std::cerr << "ERROR: n = " << n << ", errno = " << errno << " ("
					  << strerror(errno) << ")" << std::endl;
			return -1;
		}
		count += xfer;
		len -= xfer;
		data += xfer;
	}

	if (eot && (len_orig % this->out_maxpktsize) == 0) {
		bulk.ep = this->out_ep;
		bulk.len = 0;
		bulk.data = NULL;
		bulk.timeout = 1000;

		n = ioctl(this->fd, USBDEVFS_BULK, &bulk);
		if (n < 0)
			return n;
	}

	return count;
}

static void print_usage() {
	extern const char* __progname;
	std::cerr << __progname
			  << " [--debug] [--firmware] [--storage <emmc|ufs>] "
				 "[--finalize-provisioning] "
				 "[--include <PATH>] <prog.mbn> [<program> <patch> ...]"
			  << std::endl;
}

int main(int argc, char** argv) {
	char *prog_mbn, *storage = "ufs";
	char* incdir = NULL;
	qdl_file type;
	int ret;
	int opt;
	bool qdl_finalize_provisioning = false;
	std::shared_ptr<Sahara> qdl(new Sahara);

	static struct option options[] = {
		{"debug", no_argument, 0, 'd'},
		{"include", required_argument, 0, 'i'},
		{"finalize-provisioning", no_argument, 0, 'l'},
		{"storage", required_argument, 0, 's'},
		{"help", no_argument, 0, 'h'},
		{"firmware", no_argument, 0, 'f'},
		{0, 0, 0, 0}};

	while ((opt = getopt_long(argc, argv, "fdi:", options, NULL)) != -1) {
		switch (opt) {
			case 'd':
				qdl_debug = true;
				break;
			case 'i':
				incdir = optarg;
				break;
			case 'l':
				qdl_finalize_provisioning = true;
				break;
			case 's':
				storage = optarg;
				break;
			case 'f':
				fw_only = true;
				break;
			case 'h':
				print_usage();
				return 0;
			default:
				print_usage();
				return 1;
		}
	}

	/* at least 2 non optional args required */
	if ((optind + 2) > argc) {
		print_usage();
		return 1;
	}

	prog_mbn = argv[optind++];

	do {
		type = detect_type(argv[optind]);
		if (type == qdl_file::unknown)
			errx(1, "failed to detect file type of %s\n", argv[optind]);

		switch (type) {
			case qdl_file::patch:
				ret = patch::load(argv[optind]);
				if (ret < 0)
					errx(1, "patch_load %s failed", argv[optind]);
				break;
			case qdl_file::program:
				ret = program::load(argv[optind]);
				if (ret < 0)
					errx(1, "program_load %s failed", argv[optind]);
				break;
			case qdl_file::ufs:
				ret = ufs::load(argv[optind], qdl_finalize_provisioning);
				if (ret < 0)
					errx(1, "ufs_load %s failed", argv[optind]);
				break;
			default:
				errx(1, "%s type not yet supported", argv[optind]);
				break;
		}
	} while (++optind < argc);

	ret = std::dynamic_pointer_cast<Qdl>(qdl)->usb_open();
	if (ret)
		return 1;

	ret = std::dynamic_pointer_cast<Sahara>(qdl)->run(prog_mbn);
	if (ret < 0)
		return 1;

	ret = std::dynamic_pointer_cast<Firehose>(qdl)->run(incdir, storage);
	if (ret < 0)
		return 1;

	return 0;
}
