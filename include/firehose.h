#pragma once

#include <functional>

#include "patch.h"
#include "program.h"
#include "qdl.h"
#include "ufs.h"

struct Firehose : Qdl,
				  virtual ufs::ufs_apply,
				  virtual patch::patch_apply,
				  virtual program::program_apply {
	int apply_ufs_common(std::shared_ptr<ufs::Common>& common);
	int apply_ufs_body(std::shared_ptr<ufs::Body>&);
	int apply_ufs_epilogue(std::shared_ptr<ufs::Epilogue>&, bool commit);

	int apply_patch(std::shared_ptr<patch::Patch>&);

	int apply_program(std::shared_ptr<program::Program>& program, int fd);

	int run(const char* incdir, const char* storage);
	int reset();
	int set_bootable(int part);
	int send_single_tag(xmlNode* node);
	int configure(bool skip_storage_init, const char* storage);
	int send_configure(size_t payload_size,
					   bool skip_storage_init,
					   const char* storage);
	int write(xmlDoc* doc);
	int read(int wait, std::function<int(xmlNode*)>);

	static void response_log(xmlNode* node);
	static xmlNode* response_parse(const char* buf, size_t len, int* error);
};
