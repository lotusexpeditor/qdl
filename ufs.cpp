/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "ufs.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdbool>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "patch.h"
#include "qdl.h"

namespace ufs {

std::shared_ptr<Common> ufs_common_p;
std::shared_ptr<Epilogue> ufs_epilogue_p;
std::shared_ptr<Body> ufs_body_p;
std::shared_ptr<Body> ufs_body_last;

static const char notice_bconfigdescrlock[] =
	"\n"
	"Please pay attention that UFS provisioning is irreversible (OTP) "
	"operation unless parameter bConfigDescrLock = 0.\n"
	"In order to prevent unintentional device locking the tool has the "
	"following safety:\n\n"
	"	if you REALLY intend to perform OTP, please ensure that your XML "
	"includes property\n"
	"	bConfigDescrLock = 1 AND provide command line parameter "
	"--finalize-provisioning.\n\n"
	"	Unless you intend to lock your device, please set bConfigDescrLock = 0 "
	"in your XML\n"
	"	and don't use command line parameter --finalize-provisioning.\n\n"
	"In case of mismatch between CL and XML provisioning is not performed.\n\n";

bool need_provisioning(void) {
	return !!ufs_epilogue_p;
}

std::shared_ptr<Common> parse_common_params(xmlNode* node,
											bool finalize_provisioning) {
	std::shared_ptr<Common> result(new Common);
	int errors;

	errors = 0;

	result->bNumberLU = attr_as_unsigned(node, "bNumberLU", &errors);
	result->bBootEnable = !!attr_as_unsigned(node, "bBootEnable", &errors);
	result->bDescrAccessEn =
		!!attr_as_unsigned(node, "bDescrAccessEn", &errors);
	result->bInitPowerMode = attr_as_unsigned(node, "bInitPowerMode", &errors);
	result->bHighPriorityLUN =
		attr_as_unsigned(node, "bHighPriorityLUN", &errors);
	result->bSecureRemovalType =
		attr_as_unsigned(node, "bSecureRemovalType", &errors);
	result->bInitActiveICCLevel =
		attr_as_unsigned(node, "bInitActiveICCLevel", &errors);
	result->wPeriodicRTCUpdate =
		attr_as_unsigned(node, "wPeriodicRTCUpdate", &errors);
	result->bConfigDescrLock =
		!!attr_as_unsigned(node, "bConfigDescrLock", &errors);

	if (errors) {
		std::cerr << "[UFS] errors while parsing common" << std::endl;

		return {NULL};
	}

	return result;
}

std::shared_ptr<Body> parse_body(xmlNode* node) {
	std::shared_ptr<Body> result(new Body);
	int errors;

	errors = 0;

	result->LUNum = attr_as_unsigned(node, "LUNum", &errors);
	result->bLUEnable = !!attr_as_unsigned(node, "bLUEnable", &errors);
	result->bBootLunID = attr_as_unsigned(node, "bBootLunID", &errors);
	result->size_in_kb = attr_as_unsigned(node, "size_in_kb", &errors);
	result->bDataReliability =
		attr_as_unsigned(node, "bDataReliability", &errors);
	result->bLUWriteProtect =
		attr_as_unsigned(node, "bLUWriteProtect", &errors);
	result->bMemoryType = attr_as_unsigned(node, "bMemoryType", &errors);
	result->bLogicalBlockSize =
		attr_as_unsigned(node, "bLogicalBlockSize", &errors);
	result->bProvisioningType =
		attr_as_unsigned(node, "bProvisioningType", &errors);
	result->wContextCapabilities =
		attr_as_unsigned(node, "wContextCapabilities", &errors);
	result->desc = attr_as_string(node, "desc", &errors);

	if (errors) {
		std::cerr << "[UFS] errors while parsing body" << std::endl;
		return {NULL};
	}
	return result;
}

std::shared_ptr<Epilogue> parse_epilogue(xmlNode* node) {
	std::shared_ptr<Epilogue> result(new Epilogue);
	int errors = 0;

	result->LUNtoGrow = attr_as_unsigned(node, "LUNtoGrow", &errors);

	if (errors) {
		std::cerr << "[UFS] errors while parsing epilogue" << std::endl;
		return {NULL};
	}
	return result;
}

int load(const char* ufs_file, bool finalize_provisioning) {
	xmlNode* node;
	xmlNode* root;
	xmlDoc* doc;
	int retval = 0;
	std::shared_ptr<Body> ufs_body_tmp;

	if (ufs_common_p) {
		std::cerr << "Only one UFS provisioning XML allowed, " << ufs_file
				  << " ignored" << std::endl;
		return -EEXIST;
	}

	doc = xmlReadFile(ufs_file, NULL, 0);
	if (!doc) {
		std::cerr << "[UFS] failed to parse " << ufs_file << std::endl;
		return -EINVAL;
	}

	root = xmlDocGetRootElement(doc);

	for (node = root->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;

		if (xmlStrcmp(node->name, (xmlChar*)"ufs")) {
			std::cerr << "[UFS] unrecognized tag \"" << node->name
					  << "\", ignoring" << std::endl;
			continue;
		}

		if (xmlGetProp(node, (xmlChar*)"bNumberLU")) {
			if (!ufs_common_p) {
				ufs_common_p = parse_common_params(node, finalize_provisioning);
			} else {
				std::cerr << "[UFS] Only one common tag is allowed" << std::endl
						  << "[UFS] provisioning aborted" << std::endl;
				retval = -EINVAL;
				break;
			}

			if (!ufs_common_p) {
				std::cerr << "[UFS] Common tag corrupted" << std::endl
						  << "[UFS] provisioning aborted" << std::endl;
				retval = -EINVAL;
				break;
			}
		} else if (xmlGetProp(node, (xmlChar*)"LUNum")) {
			ufs_body_tmp = parse_body(node);
			if (ufs_body_tmp) {
				if (ufs_body_p) {
					ufs_body_last->next = ufs_body_tmp;
					ufs_body_last = ufs_body_tmp;
				} else {
					ufs_body_p = ufs_body_tmp;
					ufs_body_last = ufs_body_tmp;
				}
			} else {
				std::cerr << "[UFS] LU tag corrupted" << std::endl
						  << "[UFS] provisioning aborted" << std::endl;
				retval = -EINVAL;
				break;
			}
		} else if (xmlGetProp(node, (xmlChar*)"commit")) {
			if (!ufs_epilogue_p) {
				ufs_epilogue_p = parse_epilogue(node);
				if (ufs_epilogue_p)
					continue;
			} else {
				std::cerr << "[UFS] Only one finalizing tag is allowed"
						  << std::endl
						  << "[UFS] provisioning aborted" << std::endl;
				retval = -EINVAL;
				break;
			}

			if (!ufs_epilogue_p) {
				std::cerr << "[UFS] Finalizing tag corrupted" << std::endl
						  << "[UFS] provisioning aborted" << std::endl;
				retval = -EINVAL;
				break;
			}

		} else {
			std::cerr << "[UFS] Unknown tag or " << ufs_file << " corrupted"
					  << std::endl
					  << "[UFS] provisioning aborted" << std::endl;

			retval = -EINVAL;
			break;
		}
	}

	xmlFreeDoc(doc);

	if (!retval && (!ufs_common_p || !ufs_body_p || !ufs_epilogue_p)) {
		std::cerr << "[UFS] " << ufs_file << " seems to be incomplete"
				  << std::endl
				  << "[UFS] provisioning aborted" << std::endl;
		retval = -EINVAL;
	}

	if (retval) {
		if (ufs_common_p) {
			ufs_common_p.reset();
		}
		if (ufs_body_p) {
			ufs_body_p.reset();
		}
		if (ufs_epilogue_p) {
			ufs_epilogue_p.reset();
		}
		std::cerr << "[UFS] " << ufs_file << " seems to be corrupted, ignore"
				  << std::endl;
		return retval;
	}
	if (!finalize_provisioning != !ufs_common_p->bConfigDescrLock) {
		std::cerr << "[UFS] Value bConfigDescrLock "
				  << ufs_common_p->bConfigDescrLock << " in file " << ufs_file
				  << " don't match "
					 "command line parameter --finalize-provisioning "
				  << finalize_provisioning << std::endl
				  << "[UFS] provisioning aborted" << std::endl;
		std::cerr << notice_bconfigdescrlock;
		return -EINVAL;
	}
	return 0;
}

int provisioning_execute(ufs_apply* prov) {
	int ret;
	std::shared_ptr<Body> body;

	if (ufs_common_p->bConfigDescrLock) {
		int i;
		std::cout << "Attention!" << std::endl
				  << "Irreversible provisioning will start in 5 s" << std::endl;
		for (i = 5; i > 0; i--) {
			std::cout << ".\a";
			sleep(1);
		}
		std::cout << std::endl;
	}

	// Just ask a target to check the XML w/o real provisioning
	ret = prov->apply_ufs_common(ufs_common_p);
	if (ret)
		return ret;
	for (body = ufs_body_p; body; body = body->next) {
		ret = prov->apply_ufs_body(body);
		if (ret)
			return ret;
	}
	ret = prov->apply_ufs_epilogue(ufs_epilogue_p, false);
	if (ret) {
		std::cerr
			<< "UFS provisioning impossible, provisioning XML may be corrupted"
			<< std::endl;
		return ret;
	}

	// Real provisioning -- target didn't refuse a given XML
	ret = prov->apply_ufs_common(ufs_common_p);
	if (ret)
		return ret;
	for (body = ufs_body_p; body; body = body->next) {
		ret = prov->apply_ufs_body(body);
		if (ret)
			return ret;
	}
	return prov->apply_ufs_epilogue(ufs_epilogue_p, true);
}
}  // namespace ufs
