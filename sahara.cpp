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

#include "scope_exit.h"

template <typename T, typename Tp>
constexpr std::shared_ptr<T>& p(std::shared_ptr<Tp>& pt) {
	return std::dynamic_pointer_cast<T>(pt);
}

void Sahara::hello(Sahara_pkt& pkt) {
	Sahara_pkt resp;

	assert(pkt.length == 0x30);

	printf("HELLO version: 0x%x compatible: 0x%x max_len: %d mode: %d\n",
		   pkt.hello_req.version, pkt.hello_req.compatible,
		   pkt.hello_req.max_len, pkt.hello_req.mode);

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

	scope_exit progfd_close([progfd]() { close(progfd); });

	buf = std::shared_ptr<char[]>(new char[len]);
	if (!buf.get())
		return -ENOMEM;

	lseek(progfd, offset, SEEK_SET);
	n = ::read(progfd, buf.get(), len);
	if (n != len) {
		return -errno;
	}

	n = Qdl::write(buf.get(), n, true);
	if (n != len)
		err(1, "failed to write %zu bytes to sahara", len);

	return 0;
}

void Sahara::read(Sahara_pkt& pkt, const char* mbn) {
	int ret;

	assert(pkt.length == 0x14);

	printf("READ image: %d offset: 0x%x length: 0x%x\n", pkt.read_req.image,
		   pkt.read_req.offset, pkt.read_req.length);

	ret = Sahara::read_common(mbn, pkt.read_req.offset, pkt.read_req.length);
	if (ret < 0)
		errx(1, "failed to read image chunk to sahara");
}

void Sahara::read64(Sahara_pkt& pkt, const char* mbn) {
	int ret;

	assert(pkt.length == 0x20);

	printf("READ64 image: %" PRId64 " offset: 0x%" PRIx64 " length: 0x%" PRIx64
		   "\n",
		   pkt.read64_req.image, pkt.read64_req.offset, pkt.read64_req.length);

	ret =
		Sahara::read_common(mbn, pkt.read64_req.offset, pkt.read64_req.length);
	if (ret < 0)
		errx(1, "failed to read image chunk to sahara");
}

void Sahara::eoi(Sahara_pkt& pkt) {
	Sahara_pkt done;

	assert(pkt.length == 0x10);

	printf("END OF IMAGE image: %d status: %d\n", pkt.eoi.image,
		   pkt.eoi.status);

	if (pkt.eoi.status != 0) {
		printf("received non-successful result\n");
		return;
	}

	done.cmd = 5;
	done.length = 0x8;
	Qdl::write(&done, done.length, true);
}

int Sahara::done(Sahara_pkt& pkt) {
	assert(pkt.length == 0xc);

	printf("DONE status: %d\n", pkt.done_resp.status);

	return pkt.done_resp.status;
}

int Sahara::run(char* prog_mbn) {
	Sahara_pkt* pkt;
	char buf[4096];
	char tmp[32];
	bool done = false;
	int n;

	while (!done) {
		n = Qdl::read(buf, sizeof(buf), 1000);
		if (n < 0)
			break;

		pkt = (Sahara_pkt*)buf;
		if (n != pkt->length) {
			fprintf(stderr, "length not matching");
			return -EINVAL;
		}

		switch (pkt->cmd) {
			case 1:
				this->hello(*pkt);
				break;
			case 3:
				this->read(*pkt, prog_mbn);
				break;
			case 4:
				this->eoi(*pkt);
				break;
			case 6:
				this->done(*pkt);
				done = true;
				break;
			case 0x12:
				this->read64(*pkt, prog_mbn);
				break;
			default:
				sprintf(tmp, "CMD%x", pkt->cmd);
				print_hex_dump(tmp, buf, n);
				break;
		}
	}

	return done ? 0 : -1;
}
