#include "patch.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace patch {

static std::shared_ptr<Patch> patches;
static std::shared_ptr<Patch> patches_last;

int load(const char* patch_file) {
	std::shared_ptr<Patch> patch;
	xmlNode* node;
	xmlNode* root;
	xmlDoc* doc;
	int errors;

	doc = xmlReadFile(patch_file, NULL, 0);
	if (!doc) {
		std::cerr << "[PATCH] failed to parse " << patch_file << std::endl;
		return -EINVAL;
	}

	root = xmlDocGetRootElement(doc);
	for (node = root->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;

		if (xmlStrcmp(node->name, (xmlChar*)"patch")) {
			std::cerr << "[PATCH] unrecognized tag \"" << node->name
					  << "\", ignoring" << std::endl;
			continue;
		}

		errors = 0;

		patch = std::make_shared<Patch>();

		patch->sector_size =
			attr_as_unsigned(node, "SECTOR_SIZE_IN_BYTES", &errors);
		patch->byte_offset = attr_as_unsigned(node, "byte_offset", &errors);
		patch->filename = attr_as_string(node, "filename", &errors);
		patch->partition =
			attr_as_unsigned(node, "physical_partition_number", &errors);
		patch->size_in_bytes = attr_as_unsigned(node, "size_in_bytes", &errors);
		patch->start_sector = attr_as_string(node, "start_sector", &errors);
		patch->value = attr_as_string(node, "value", &errors);
		patch->what = attr_as_string(node, "what", &errors);

		if (errors) {
			std::cerr << "[PATCH] errors while parsing patch" << std::endl;
			continue;
		}

		if (patches) {
			patches_last->next = patch;
			patches_last = patch;
		} else {
			patches = patch;
			patches_last = patch;
		}
	}

	xmlFreeDoc(doc);

	return 0;
}
int execute(std::shared_ptr<patch_apply>& p) {
	return execute(p.get());
}

int execute(patch_apply* dev) {
	std::shared_ptr<Patch> patch;
	int ret;

	for (patch = patches; patch.get(); patch = patch->next) {
		if (strcmp(patch->filename, "DISK"))
			continue;

		ret = dev->apply_patch(patch);
		if (ret)
			return ret;
	}

	return 0;
}

}  // namespace patch
