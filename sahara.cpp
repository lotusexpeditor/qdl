#include "sahara.h"

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdbool>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

#include "scope_exit.h"

void Sahara::hello(Sahara::Pkt& pkt) {
	Pkt resp;

	assert(pkt.length == 0x30);

	std::cout << std::hex << "HELLO version: 0x" << pkt.hello_req.version
			  << " compatible: 0x" << pkt.hello_req.compatible
			  << " max_len: " << std::dec << pkt.hello_req.max_len
			  << " mode: " << pkt.hello_req.mode << std::endl;

	resp.cmd = 2;
	resp.length = 0x30;
	resp.hello_resp.version = 2;
	resp.hello_resp.compatible = 1;
	resp.hello_resp.status = 0;
	resp.hello_resp.mode = pkt.hello_req.mode;

	Qdl::write(&resp, resp.length, true);
}

int Sahara::read_common(const char* mbn, off_t offset, size_t len) {
	int progfd = -1;
	ssize_t n;
	std::shared_ptr<char[]> buf;

	progfd = open(mbn, O_RDONLY);
	if (progfd < 0)
		return -errno;

	buf = std::shared_ptr<char[]>(new char[len]);
	if (!buf)
		return -ENOMEM;

	lseek(progfd, offset, SEEK_SET);
	n = ::read(progfd, buf.get(), len);
	if (n != len)
		return -errno;

	n = Qdl::write(buf.get(), n, true);
	if (n != len)
		err(1, "failed to write %zu bytes to sahara", len);

	close(progfd);

	return 0;
}

void Sahara::read(Sahara::Pkt& pkt, const char* mbn) {
	int ret;

	assert(pkt.length == 0x14);

	std::cout << "READ image: " << pkt.read_req.image << " offset: 0x"
			  << std::hex << pkt.read_req.offset << " length: 0x"
			  << pkt.read_req.length << std::endl;

	ret = Sahara::read_common(mbn, pkt.read_req.offset, pkt.read_req.length);
	if (ret < 0)
		errx(1, "failed to read image chunk to sahara");
}

void Sahara::read64(Sahara::Pkt& pkt, const char* mbn) {
	int ret;

	assert(pkt.length == 0x20);

	std::cout << "READ64 image: " << pkt.read64_req.image << std::hex
			  << " offset: 0x%" << pkt.read64_req.offset << " length: 0x%"
			  << pkt.read64_req.length << std::endl;

	ret =
		Sahara::read_common(mbn, pkt.read64_req.offset, pkt.read64_req.length);
	if (ret < 0)
		errx(1, "failed to read image chunk to sahara");
}

void Sahara::eoi(Sahara::Pkt& pkt) {
	Pkt done;

	assert(pkt.length == 0x10);

	std::cout << "END OF IMAGE image: " << pkt.eoi.image
			  << " status: " << pkt.eoi.status << std::endl;

	if (pkt.eoi.status != 0) {
		std::cout << "received non-successful result" << std::endl;
		return;
	}

	done.cmd = 5;
	done.length = 0x8;
	Qdl::write(&done, done.length, true);
}

int Sahara::done(Sahara::Pkt& pkt) {
	assert(pkt.length == 0xc);

	std::cout << "DONE status: " << pkt.done_resp.status << std::endl;

	return pkt.done_resp.status;
}

int Sahara::run(char* prog_mbn) {
	Pkt* pkt;
	char buf[4096];
	char tmp[32];
	bool done = false;
	int n;

	while (!done) {
		n = Qdl::read(buf, sizeof(buf), 1000);
		if (n < 0)
			break;

		pkt = (Sahara::Pkt*)buf;
		if (n != pkt->length) {
			std::cerr << "length not matching";
			return -EINVAL;
		}

		switch (pkt->cmd) {
			case 1:
				Sahara::hello(*pkt);
				break;
			case 3:
				Sahara::read(*pkt, prog_mbn);
				break;
			case 4:
				Sahara::eoi(*pkt);
				break;
			case 6:
				Sahara::done(*pkt);
				done = true;
				break;
			case 0x12:
				Sahara::read64(*pkt, prog_mbn);
				break;
			default:
				std::stringstream ss;
				ss << "CMD" << std::hex << pkt->cmd;
				strcpy(tmp, ss.str().c_str());
				print_hex_dump(tmp, buf, n);
				break;
		}
	}

	return done ? 0 : -1;
}
