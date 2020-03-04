#pragma once

#include <cstdint>

#include "firehose.h"

struct Sahara : Firehose {
	struct Pkt {
		uint32_t cmd;
		uint32_t length;

		union {
			struct {
				uint32_t version;
				uint32_t compatible;
				uint32_t max_len;
				uint32_t mode;
			} hello_req;
			struct {
				uint32_t version;
				uint32_t compatible;
				uint32_t status;
				uint32_t mode;
			} hello_resp;
			struct {
				uint32_t image;
				uint32_t offset;
				uint32_t length;
			} read_req;
			struct {
				uint32_t image;
				uint32_t status;
			} eoi;
			struct {
			} done_req;
			struct {
				uint32_t status;
			} done_resp;
			struct {
				uint64_t image;
				uint64_t offset;
				uint64_t length;
			} read64_req;
		};
	};
	int run(char* prog_mbn);

   private:
	void hello(Pkt&);
	int read_common(const char* mbn, off_t offset, size_t len);
	void read(Pkt& pkt, const char* mbn);
	void read64(Pkt& pkt, const char* mbn);
	void eoi(Pkt& pkt);
	int done(Pkt& pkt);
};
