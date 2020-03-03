#pragma once

#ifndef __QDL_H__
#define __QDL_H__

#include <libxml/tree.h>

#include <cstdbool>

struct Qdl {
	int read(void* buf, size_t len, unsigned int timeout);
	int write(const void* buf, size_t len, bool eot);

   protected:
	int parse_usb_desc(int fd, int* intf);
	int usb_open();
	int fd;

	int in_ep;
	int out_ep;

	size_t in_maxpktsize;
	size_t out_maxpktsize;

	friend int main(int, char**);
};

void print_hex_dump(const char* prefix, const void* buf, size_t len);
unsigned attr_as_unsigned(xmlNode* node, const char* attr, int* errors);
const char* attr_as_string(xmlNode* node, const char* attr, int* errors);

extern bool qdl_debug;

#endif
