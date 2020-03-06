/*
 * Copyright (c) 2016-2017, Linaro Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "program.h"

#include <fcntl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

#include "qdl.h"

namespace program {

static std::shared_ptr<Program> programes;
static std::shared_ptr<Program> programes_last;

int load(const char* program_file) {
	std::shared_ptr<Program> program;
	xmlNode* node;
	xmlNode* root;
	xmlDoc* doc;
	int errors;

	doc = xmlReadFile(program_file, NULL, 0);
	if (!doc) {
		std::cerr << "[PROGRAM] failed to parse " << program_file << std::endl;
		return -EINVAL;
	}

	root = xmlDocGetRootElement(doc);
	for (node = root->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;

		if (xmlStrcmp(node->name, (xmlChar*)"program")) {
			std::cerr << "[PROGRAM] unrecognized tag \"" << node->name
					  << "\", ignoring" << std::endl;
			continue;
		}

		errors = 0;

		program = std::make_shared<Program>();

		program->sector_size =
			attr_as_unsigned(node, "SECTOR_SIZE_IN_BYTES", &errors);
		program->file_offset =
			attr_as_unsigned(node, "file_sector_offset", &errors);
		program->filename = attr_as_string(node, "filename", &errors);
		program->label = attr_as_string(node, "label", &errors);
		program->num_sectors =
			attr_as_unsigned(node, "num_partition_sectors", &errors);
		program->partition =
			attr_as_unsigned(node, "physical_partition_number", &errors);
		program->start_sector = attr_as_string(node, "start_sector", &errors);

		if (errors) {
			std::cerr << "[PROGRAM] errors while parsing program" << std::endl;
			continue;
		}

		if (programes) {
			programes_last->next = program;
			programes_last = program;
		} else {
			programes = program;
			programes_last = program;
		}
	}

	xmlFreeDoc(doc);

	return 0;
}

int execute(program_apply* ptr, const char* incdir) {
	std::shared_ptr<Program> program;
	const char* filename;
	char tmp[PATH_MAX];
	int ret;
	int fd;

	for (program = programes; program; program = program->next) {
		if (!program->filename)
			continue;

		filename = program->filename;
		if (incdir) {
			std::stringstream ss;
			ss << incdir << "/" << filename;
			strncpy(tmp, ss.str().c_str(), PATH_MAX);
			if (access(tmp, F_OK) != -1)
				filename = tmp;
		}

		fd = open(filename, O_RDONLY);

		if (fd < 0) {
			std::cout << "Unable to open " << program->filename << "...ignoring"
					  << std::endl;
			continue;
		}

		ret = ptr->apply_program(program, fd);

		close(fd);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * program_find_bootable_partition() - find one bootable partition
 *
 * Returns partition number, or negative errno on failure.
 *
 * Scan program tags for a partition with the label "sbl1", "xbl" or "xbl_a"
 * and return the partition number for this. If more than one line matches
 * we're assuming our logic is flawed and return an error.
 */
int find_bootable_partition(void) {
	std::shared_ptr<Program> program;
	const char* label;
	int part = -ENOENT;

	for (program = programes; program; program = program->next) {
		label = program->label;

		if (!strcmp(label, "xbl") || !strcmp(label, "xbl_a") ||
			!strcmp(label, "sbl1")) {
			if (part != -ENOENT)
				return -EINVAL;

			part = program->partition;
		}
	}

	return part;
}
}  // namespace program
