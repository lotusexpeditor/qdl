#include "firehose.h"

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <poll.h>
#include <sys/stat.h>
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
#include <ctime>
#include <iostream>

#include "ufs.h"

static void xml_setpropf(xmlNode* node,
						 const char* attr,
						 const char* fmt,
						 ...) {
	xmlChar buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf((char*)buf, sizeof(buf), fmt, ap);
	xmlSetProp(node, (xmlChar*)attr, buf);
	va_end(ap);
}

xmlNode* Firehose::response_parse(const char* buf, size_t len, int* error) {
	xmlNode* node;
	xmlNode* root;
	xmlDoc* doc;

	doc = xmlReadMemory(buf, len, NULL, NULL, 0);
	if (!doc) {
		std::cerr << "failed to parse firehose packet" << std::endl;
		*error = -EINVAL;
		return NULL;
	}

	root = xmlDocGetRootElement(doc);
	for (node = root; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;
		if (xmlStrcmp(node->name, (xmlChar*)"data") == 0)
			break;
	}

	if (!node) {
		std::cerr << "firehose packet without data tag" << std::endl;
		*error = -EINVAL;
		xmlFreeDoc(doc);
		return NULL;
	}

	node = node->children;
	while (node && node->type != XML_ELEMENT_NODE)
		node = node->next;

	return node;
}

void Firehose::response_log(xmlNode* node) {
	xmlChar* value;

	value = xmlGetProp(node, (xmlChar*)"value");
	std::cout << "LOG: " << value << std::endl;
}

int Firehose::read(int wait, std::function<int(xmlNode*)> response_parser) {
	char buf[4096];
	xmlNode* nodes;
	xmlNode* node;
	int error;
	char* msg;
	char* end;
	bool done = false;
	int ret = -ENXIO;
	int n;
	int timeout = 1000;

	if (wait > 0)
		timeout = wait;

	for (;;) {
		n = Qdl::read(buf, sizeof(buf), timeout);
		if (n < 0) {
			if (done)
				break;

			warn("failed to read");
			return -ETIMEDOUT;
		}
		buf[n] = '\0';

		if (qdl_debug)
			std::cerr << "FIREHOSE READ: " << buf << std::endl;

		for (msg = buf; msg[0]; msg = end) {
			end = strstr(msg, "</data>");
			if (!end) {
				std::cerr << "firehose response truncated" << std::endl;
				exit(1);
			}

			end += strlen("</data>");

			nodes = Firehose::response_parse(msg, end - msg, &error);
			if (!nodes) {
				std::cerr << "unable to parse response" << std::endl;
				return error;
			}

			for (node = nodes; node; node = node->next) {
				if (xmlStrcmp(node->name, (xmlChar*)"log") == 0) {
					Firehose::response_log(node);
				} else if (xmlStrcmp(node->name, (xmlChar*)"response") == 0) {
					ret = response_parser(node);
					done = true;
					timeout = 1;
				}
			}

			xmlFreeDoc(nodes->doc);
		}

		if (wait > 0)
			timeout = 100;
	}

	return ret;
}

int Firehose::write(xmlDoc* doc) {
	int saved_errno;
	xmlChar* s;
	int len;
	int ret;

	xmlDocDumpMemory(doc, &s, &len);

	if (qdl_debug)
		std::cerr << "FIREHOSE WRITE: " << s << std::endl;

	ret = Qdl::write(s, len, true);
	saved_errno = errno;
	xmlFree(s);
	return ret < 0 ? -saved_errno : 0;
}

static size_t max_payload_size = 1048576;

static int firehose_configure_response_parser(xmlNode* node) {
	xmlChar* payload;
	xmlChar* value;
	size_t max_size;

	value = xmlGetProp(node, (xmlChar*)"value");
	payload = xmlGetProp(node, (xmlChar*)"MaxPayloadSizeToTargetInBytes");
	if (!value || !payload)
		return -EINVAL;

	max_size = strtoul((char*)payload, NULL, 10);

	/*
	 * When receiving an ACK the remote may indicate that we should attempt
	 * a larger payload size
	 */
	if (!xmlStrcmp(value, (xmlChar*)"ACK")) {
		payload = xmlGetProp(
			node, (xmlChar*)"MaxPayloadSizeToTargetInBytesSupported");
		if (!payload)
			return -EINVAL;

		max_size = strtoul((char*)payload, NULL, 10);
	}

	return max_size;
}

int Firehose::send_configure(size_t payload_size,
							 bool skip_storage_init,
							 const char* storage) {
	xmlNode* root;
	xmlNode* node;
	xmlDoc* doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"configure", NULL);
	xml_setpropf(node, "MemoryName", storage);
	xml_setpropf(node, "MaxPayloadSizeToTargetInBytes", "%d", payload_size);
	xml_setpropf(node, "verbose", "%d", 0);
	xml_setpropf(node, "ZLPAwareHost", "%d", 1);
	xml_setpropf(node, "SkipStorageInit", "%d", skip_storage_init);

	ret = Firehose::write(doc);
	xmlFreeDoc(doc);
	if (ret < 0)
		return ret;

	return Firehose::read(-1, firehose_configure_response_parser);
}

int Firehose::configure(bool skip_storage_init, const char* storage) {
	int ret;

	ret =
		Firehose::send_configure(max_payload_size, skip_storage_init, storage);
	if (ret < 0)
		return ret;

	/* Retry if remote proposed different size */
	if (ret != max_payload_size) {
		ret = Firehose::send_configure(ret, skip_storage_init, storage);
		if (ret < 0)
			return ret;

		max_payload_size = ret;
	}

	if (qdl_debug) {
		std::cerr << "[CONFIGURE] max payload size: " << max_payload_size
				  << std::endl;
	}

	return 0;
}

static int firehose_nop_parser(xmlNode* node) {
	xmlChar* value;

	value = xmlGetProp(node, (xmlChar*)"value");
	return !!xmlStrcmp(value, (xmlChar*)"ACK");
}

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define ROUND_UP(x, a) (((x) + (a)-1) & ~((a)-1))

int Firehose::apply_program(std::shared_ptr<program::Program>& program,
							int fd) {
	unsigned num_sectors;
	struct stat sb;
	size_t chunk_size;
	xmlNode* root;
	xmlNode* node;
	xmlDoc* doc;
	std::shared_ptr<char[]> buf;
	time_t t0;
	time_t t;
	int left;
	int ret;
	int n;

	if (fw_only) {
		if (!strcmp(program->label, "system") ||
			!strcmp(program->label, "cust") ||
			!strcmp(program->label, "userdata") ||
			!strcmp(program->label, "keystore") ||
			!strcmp(program->label, "boot") ||
			!strcmp(program->label, "recovery") ||
			!strcmp(program->label, "sec")) {
			std::cout << "[FIREHOSE]: skipping " << program->label << std::endl;
			return 0;
		}
	}

	num_sectors = program->num_sectors;

	ret = fstat(fd, &sb);
	if (ret < 0)
		err(1, "failed to stat \"%s\"\n", program->filename);

	num_sectors =
		(sb.st_size + program->sector_size - 1) / program->sector_size;

	if (program->num_sectors && num_sectors > program->num_sectors) {
		fprintf(stderr, "[PROGRAM] %s truncated to %d\n", program->label,
				program->num_sectors * program->sector_size);
		num_sectors = program->num_sectors;
	}

	buf = std::shared_ptr<char[]>(new char[max_payload_size]);
	if (!buf)
		err(1, "failed to allocate sector buffer");

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"program", NULL);
	xml_setpropf(node, "SECTOR_SIZE_IN_BYTES", "%d", program->sector_size);
	xml_setpropf(node, "num_partition_sectors", "%d", num_sectors);
	xml_setpropf(node, "physical_partition_number", "%d", program->partition);
	xml_setpropf(node, "start_sector", "%s", program->start_sector);
	if (program->filename)
		xml_setpropf(node, "filename", "%s", program->filename);

	ret = Firehose::write(doc);
	if (ret < 0) {
		std::cerr << "[PROGRAM] failed to write program command" << std::endl;
		goto out;
	}

	ret = Firehose::read(-1, firehose_nop_parser);
	if (ret) {
		std::cerr << "[PROGRAM] failed to setup programming" << std::endl;
		goto out;
	}

	t0 = time(NULL);

	lseek(fd, program->file_offset * program->sector_size, SEEK_SET);
	left = num_sectors;
	while (left > 0) {
		chunk_size = MIN(max_payload_size / program->sector_size, left);

		n = ::read(fd, buf.get(), chunk_size * program->sector_size);
		if (n < 0)
			err(1, "failed to read");

		if (n < max_payload_size)
			std::memset(buf.get() + n, 0, max_payload_size - n);

		n = Qdl::write(buf.get(), chunk_size * program->sector_size, true);
		if (n < 0)
			err(1, "failed to write");

		if (n != chunk_size * program->sector_size)
			err(1, "failed to write full sector");

		left -= chunk_size;
	}

	t = time(NULL) - t0;

	ret = Firehose::read(-1, firehose_nop_parser);
	if (ret) {
		std::cerr << "[PROGRAM] failed" << std::endl;
	} else if (t) {
		std::cerr << "[PROGRAM] flashed \"" << program->label
				  << "\" successfully at "
				  << (program->sector_size * num_sectors / t / 1024) << "kB/s"
				  << std::endl;
	} else {
		std::cerr << "[PROGRAM] flashed \"" << program->label
				  << "\" successfully" << std::endl;
	}

out:
	xmlFreeDoc(doc);
	return ret;
}

int Firehose::apply_patch(std::shared_ptr<patch::Patch>& patch) {
	xmlNode* root;
	xmlNode* node;
	xmlDoc* doc;
	int ret;

	printf("%s\n", patch->what);

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"patch", NULL);
	xml_setpropf(node, "SECTOR_SIZE_IN_BYTES", "%d", patch->sector_size);
	xml_setpropf(node, "byte_offset", "%d", patch->byte_offset);
	xml_setpropf(node, "filename", "%s", patch->filename);
	xml_setpropf(node, "physical_partition_number", "%d", patch->partition);
	xml_setpropf(node, "size_in_bytes", "%d", patch->size_in_bytes);
	xml_setpropf(node, "start_sector", "%s", patch->start_sector);
	xml_setpropf(node, "value", "%s", patch->value);

	ret = Firehose::write(doc);
	if (ret < 0)
		goto out;

	ret = Firehose::read(-1, firehose_nop_parser);
	if (ret)
		std::cerr << "[APPLY PATCH] " << ret << std::endl;

out:
	xmlFreeDoc(doc);
	return ret;
}

int Firehose::send_single_tag(xmlNode* node) {
	xmlNode* root;
	xmlDoc* doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);
	xmlAddChild(root, node);

	ret = Firehose::write(doc);
	if (ret < 0)
		goto out;

	ret = Firehose::read(-1, firehose_nop_parser);
	if (ret) {
		std::cerr << "[UFS] " << __func__ << " err " << ret << std::endl;
		ret = -EINVAL;
	}

out:
	xmlFreeDoc(doc);
	return ret;
}

int Firehose::apply_ufs_common(std::shared_ptr<ufs::Common>& ufs) {
	xmlNode* node_to_send;
	int ret;

	node_to_send = xmlNewNode(NULL, (xmlChar*)"ufs");

	xml_setpropf(node_to_send, "bNumberLU", "%d", ufs->bNumberLU);
	xml_setpropf(node_to_send, "bBootEnable", "%d", ufs->bBootEnable);
	xml_setpropf(node_to_send, "bDescrAccessEn", "%d", ufs->bDescrAccessEn);
	xml_setpropf(node_to_send, "bInitPowerMode", "%d", ufs->bInitPowerMode);
	xml_setpropf(node_to_send, "bHighPriorityLUN", "%d", ufs->bHighPriorityLUN);
	xml_setpropf(node_to_send, "bSecureRemovalType", "%d",
				 ufs->bSecureRemovalType);
	xml_setpropf(node_to_send, "bInitActiveICCLevel", "%d",
				 ufs->bInitActiveICCLevel);
	xml_setpropf(node_to_send, "wPeriodicRTCUpdate", "%d",
				 ufs->wPeriodicRTCUpdate);
	xml_setpropf(node_to_send, "bConfigDescrLock", "%d",
				 0 /*ufs->bConfigDescrLock*/);	// Safety, remove before fly

	ret = Firehose::send_single_tag(node_to_send);
	if (ret)
		std::cerr << "[APPLY UFS common] " << ret << std::endl;

	return ret;
}

int Firehose::apply_ufs_body(std::shared_ptr<ufs::Body>& ufs) {
	xmlNode* node_to_send;
	int ret;

	node_to_send = xmlNewNode(NULL, (xmlChar*)"ufs");

	xml_setpropf(node_to_send, "LUNum", "%d", ufs->LUNum);
	xml_setpropf(node_to_send, "bLUEnable", "%d", ufs->bLUEnable);
	xml_setpropf(node_to_send, "bBootLunID", "%d", ufs->bBootLunID);
	xml_setpropf(node_to_send, "size_in_kb", "%d", ufs->size_in_kb);
	xml_setpropf(node_to_send, "bDataReliability", "%d", ufs->bDataReliability);
	xml_setpropf(node_to_send, "bLUWriteProtect", "%d", ufs->bLUWriteProtect);
	xml_setpropf(node_to_send, "bMemoryType", "%d", ufs->bMemoryType);
	xml_setpropf(node_to_send, "bLogicalBlockSize", "%d",
				 ufs->bLogicalBlockSize);
	xml_setpropf(node_to_send, "bProvisioningType", "%d",
				 ufs->bProvisioningType);
	xml_setpropf(node_to_send, "wContextCapabilities", "%d",
				 ufs->wContextCapabilities);
	if (ufs->desc)
		xml_setpropf(node_to_send, "desc", "%s", ufs->desc);

	ret = Firehose::send_single_tag(node_to_send);
	if (ret)
		std::cerr << "[APPLY UFS body] " << ret << std::endl;

	return ret;
}

int Firehose::apply_ufs_epilogue(std::shared_ptr<ufs::Epilogue>& ufs,
								 bool commit) {
	xmlNode* node_to_send;
	int ret;

	node_to_send = xmlNewNode(NULL, (xmlChar*)"ufs");

	xml_setpropf(node_to_send, "LUNtoGrow", "%d", ufs->LUNtoGrow);
	xml_setpropf(node_to_send, "commit", "%d", commit);

	ret = Firehose::send_single_tag(node_to_send);
	if (ret)
		std::cerr << "[APPLY UFS epilogue] " << ret << std::endl;

	return ret;
}

int Firehose::set_bootable(int part) {
	xmlNode* root;
	xmlNode* node;
	xmlDoc* doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"setbootablestoragedrive", NULL);
	xml_setpropf(node, "value", "%d", part);

	ret = Firehose::write(doc);
	xmlFreeDoc(doc);
	if (ret < 0)
		return ret;

	ret = Firehose::read(-1, firehose_nop_parser);
	if (ret) {
		std::cerr << "failed to mark partition " << part << " as bootable"
				  << std::endl;
		return -1;
	}

	std::cout << "partition " << part << " is now bootable" << std::endl;
	return 0;
}

int Firehose::reset() {
	xmlNode* root;
	xmlNode* node;
	xmlDoc* doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"power", NULL);
	xml_setpropf(node, "value", "reset");

	ret = Firehose::write(doc);
	xmlFreeDoc(doc);
	if (ret < 0)
		return ret;

	return Firehose::read(-1, firehose_nop_parser);
}

int Firehose::run(const char* incdir, const char* storage) {
	int bootable;
	int ret;

	/* Wait for the firehose payload to boot */
	sleep(3);

	Firehose::read(1000, NULL);

	if (ufs::need_provisioning()) {
		ret = Firehose::configure(true, storage);
		if (ret)
			return ret;
		ret = ufs::provisioning_execute(this);
		if (!ret)
			std::cout << "UFS provisioning succeeded" << std::endl;
		else
			std::cout << "UFS provisioning failed" << std::endl;
		return ret;
	}

	ret = Firehose::configure(false, storage);
	if (ret)
		return ret;

	ret = program::execute(this, incdir);
	if (ret)
		return ret;

	ret = patch::execute(this);
	if (ret)
		return ret;

	bootable = program::find_bootable_partition();
	if (bootable < 0)
		std::cerr << "no boot partition found" << std::endl;
	else
		Firehose::set_bootable(bootable);

	Firehose::reset();

	return 0;
}
