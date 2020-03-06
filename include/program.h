#ifndef __PROGRAM_H__
#define __PROGRAM_H__

#include <cstdbool>
#include <memory>

#include "qdl.h"

namespace program {

struct Program {
	unsigned sector_size;
	unsigned file_offset;
	const char* filename;
	const char* label;
	unsigned num_sectors;
	unsigned partition;
	const char* start_sector;

	std::shared_ptr<Program> next;
};

struct program_apply {
	virtual int apply_program(std::shared_ptr<Program>&, int) = 0;
};

int load(const char* program_file);
int execute(program_apply*, const char* incdir);
int find_bootable_partition();

}  // namespace program
#endif
