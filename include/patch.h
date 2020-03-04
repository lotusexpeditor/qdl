#ifndef __PATCH_H__
#define __PATCH_H__

#include "qdl.h"
namespace patch {
struct Patch {
	unsigned sector_size;
	unsigned byte_offset;
	const char* filename;
	unsigned partition;
	unsigned size_in_bytes;
	const char* start_sector;
	const char* value;
	const char* what;

	std::shared_ptr<Patch> next;
};

struct patch_apply {
	virtual int apply_patch(std::shared_ptr<Patch>&) = 0;
};

int load(const char* patch_file);
int execute(std::shared_ptr<patch_apply>&);
int execute(patch_apply*);
}  // namespace patch
#endif
