/*	$NetBSD: npf.c,v 1.42 2016/12/27 20:32:58 rmind Exp $	*/

/*-
 * Copyright (c) 2010-2015 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: npf.c,v 1.42 2016/12/27 20:32:58 rmind Exp $");

#include <sys/types.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <net/if.h>
#include <prop/proplib.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <err.h>

#define	_NPF_PRIVATE
#include "npf.h"

struct nl_rule {
	prop_dictionary_t	nrl_dict;
};

struct nl_rproc {
	prop_dictionary_t	nrp_dict;
};

struct nl_table {
	prop_dictionary_t	ntl_dict;
};

struct nl_alg {
	prop_dictionary_t	nal_dict;
};

struct nl_ext {
	const char *		nxt_name;
	prop_dictionary_t	nxt_dict;
};

struct nl_config {
	/* Rules, translations, procedures, tables, connections. */
	prop_dictionary_t	ncf_dict;
	prop_array_t		ncf_alg_list;
	prop_array_t		ncf_rules_list;
	prop_array_t		ncf_rproc_list;
	prop_array_t		ncf_table_list;
	prop_array_t		ncf_nat_list;
	prop_array_t		ncf_conn_list;

	/* Iterators. */
	prop_object_iterator_t	ncf_rule_iter;
	unsigned		ncf_reduce[16];
	unsigned		ncf_nlevel;
	unsigned		ncf_counter;
	nl_rule_t		ncf_cur_rule;

	prop_object_iterator_t	ncf_table_iter;
	nl_table_t		ncf_cur_table;

	prop_object_iterator_t	ncf_rproc_iter;
	nl_rproc_t		ncf_cur_rproc;

	/* Error report and debug information. */
	prop_dictionary_t	ncf_err;
	prop_dictionary_t	ncf_debug;

	bool			ncf_flush;
};

static prop_array_t	_npf_ruleset_transform(prop_array_t);

static bool
_npf_add_addr(prop_dictionary_t dict, const char *name, int af,
    const npf_addr_t *addr)
{
	size_t sz;

	if (af == AF_INET) {
		sz = sizeof(struct in_addr);
	} else if (af == AF_INET6) {
		sz = sizeof(struct in6_addr);
	} else {
		return false;
	}
	prop_data_t addrdat = prop_data_create_data(addr, sz);
	if (addrdat == NULL) {
		return false;
	}
	prop_dictionary_set(dict, name, addrdat);
	prop_object_release(addrdat);
	return true;
}

static unsigned
_npf_get_addr(prop_dictionary_t dict, const char *name, npf_addr_t *addr)
{
	prop_object_t obj = prop_dictionary_get(dict, name);
	const void *d = prop_data_data_nocopy(obj);

	if (d == NULL)
		return false;

	size_t sz = prop_data_size(obj);
	switch (sz) {
	case sizeof(struct in_addr):
	case sizeof(struct in6_addr):
		memcpy(addr, d, sz);
		return (unsigned)sz;
	default:
		return 0;
	}
}

/*
 * CONFIGURATION INTERFACE.
 */

nl_config_t *
npf_config_create(void)
{
	nl_config_t *ncf;

	ncf = calloc(1, sizeof(*ncf));
	if (ncf == NULL) {
		return NULL;
	}
	ncf->ncf_alg_list = prop_array_create();
	ncf->ncf_rules_list = prop_array_create();
	ncf->ncf_rproc_list = prop_array_create();
	ncf->ncf_table_list = prop_array_create();
	ncf->ncf_nat_list = prop_array_create();
	ncf->ncf_flush = false;
	return ncf;
}

static prop_dictionary_t
_npf_build_config(nl_config_t *ncf)
{
	prop_dictionary_t npf_dict;
	prop_array_t rlset;

	npf_dict = prop_dictionary_create();
	if (npf_dict == NULL) {
		return NULL;
	}
	prop_dictionary_set_uint32(npf_dict, "version", NPF_VERSION);

	rlset = _npf_ruleset_transform(ncf->ncf_rules_list);
	if (rlset == NULL) {
		prop_object_release(npf_dict);
		return NULL;
	}
	prop_object_release(ncf->ncf_rules_list);
	ncf->ncf_rules_list = rlset;

	prop_dictionary_set(npf_dict, "rules", ncf->ncf_rules_list);
	prop_dictionary_set(npf_dict, "algs", ncf->ncf_alg_list);
	prop_dictionary_set(npf_dict, "rprocs", ncf->ncf_rproc_list);
	prop_dictionary_set(npf_dict, "tables", ncf->ncf_table_list);
	prop_dictionary_set(npf_dict, "nat", ncf->ncf_nat_list);
	if (ncf->ncf_conn_list) {
		prop_dictionary_set(npf_dict, "conn-list",
		    ncf->ncf_conn_list);
	}
	prop_dictionary_set_bool(npf_dict, "flush", ncf->ncf_flush);
	if (ncf->ncf_debug) {
		prop_dictionary_set(npf_dict, "debug", ncf->ncf_debug);
	}
	return npf_dict;
}

int
npf_config_submit(nl_config_t *ncf, int fd, npf_error_t *errinfo)
{
#if !defined(_NPF_STANDALONE)
	prop_dictionary_t npf_dict;
	int error = 0;

	npf_dict = _npf_build_config(ncf);
	if (!npf_dict) {
		return ENOMEM;
	}
	error = prop_dictionary_sendrecv_ioctl(npf_dict, fd,
	    IOC_NPF_LOAD, &ncf->ncf_err);
	if (error) {
		prop_object_release(npf_dict);
		assert(ncf->ncf_err == NULL);
		return error;
	}
	prop_dictionary_get_int32(ncf->ncf_err, "errno", &error);
	if (error) {
		memset(errinfo, 0, sizeof(*errinfo));

		prop_dictionary_get_int64(ncf->ncf_err, "id",
		    &errinfo->id);
		prop_dictionary_get_cstring(ncf->ncf_err,
		    "source-file", &errinfo->source_file);
		prop_dictionary_get_uint32(ncf->ncf_err,
		    "source-line", &errinfo->source_line);
	}
	prop_object_release(npf_dict);
	return error;
#else
	(void)ncf; (void)fd;
	return ENOTSUP;
#endif
}

static nl_config_t *
_npf_config_consdict(prop_dictionary_t npf_dict)
{
	nl_config_t *ncf;

	ncf = calloc(1, sizeof(*ncf));
	if (ncf == NULL) {
		return NULL;
	}
	ncf->ncf_dict = npf_dict;
	ncf->ncf_alg_list = prop_dictionary_get(npf_dict, "algs");
	ncf->ncf_rules_list = prop_dictionary_get(npf_dict, "rules");
	ncf->ncf_rproc_list = prop_dictionary_get(npf_dict, "rprocs");
	ncf->ncf_table_list = prop_dictionary_get(npf_dict, "tables");
	ncf->ncf_nat_list = prop_dictionary_get(npf_dict, "nat");
	ncf->ncf_conn_list = prop_dictionary_get(npf_dict, "conn-list");
	return ncf;
}

nl_config_t *
npf_config_retrieve(int fd)
{
	prop_dictionary_t npf_dict = NULL;
	nl_config_t *ncf;
	int error;

#ifdef _NPF_STANDALONE
	error = ENOTSUP;
#else
	error = prop_dictionary_recv_ioctl(fd, IOC_NPF_SAVE, &npf_dict);
#endif
	if (error) {
		return NULL;
	}
	ncf = _npf_config_consdict(npf_dict);
	if (ncf == NULL) {
		prop_object_release(npf_dict);
		return NULL;
	}
	return ncf;
}

void *
npf_config_export(nl_config_t *ncf, size_t *length)
{
	prop_dictionary_t npf_dict = ncf->ncf_dict;
	void *blob;

	if (!npf_dict && (npf_dict = _npf_build_config(ncf)) == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	if ((blob = prop_dictionary_externalize(npf_dict)) == NULL) {
		prop_object_release(npf_dict);
		return NULL;
	}
	prop_object_release(npf_dict);
	*length = strlen(blob);
	return blob;
}

nl_config_t *
npf_config_import(const void *blob, size_t len __unused)
{
	prop_dictionary_t npf_dict;
	nl_config_t *ncf;

	npf_dict = prop_dictionary_internalize(blob);
	if (!npf_dict) {
		return NULL;
	}
	ncf = _npf_config_consdict(npf_dict);
	if (!ncf) {
		prop_object_release(npf_dict);
		return NULL;
	}
	return ncf;
}

int
npf_config_flush(int fd)
{
	nl_config_t *ncf;
	npf_error_t errinfo;
	int error;

	ncf = npf_config_create();
	if (ncf == NULL) {
		return ENOMEM;
	}
	ncf->ncf_flush = true;
	error = npf_config_submit(ncf, fd, &errinfo);
	npf_config_destroy(ncf);
	return error;
}

bool
npf_config_active_p(nl_config_t *ncf)
{
	bool active = false;
	prop_dictionary_get_bool(ncf->ncf_dict, "active", &active);
	return active;
}

bool
npf_config_loaded_p(nl_config_t *ncf)
{
	return ncf->ncf_rules_list != NULL;
}

void *
npf_config_build(nl_config_t *ncf)
{
	if (!ncf->ncf_dict && !(ncf->ncf_dict = _npf_build_config(ncf))) {
		errno = ENOMEM;
		return NULL;
	}
	return (void *)ncf->ncf_dict;
}

void
npf_config_destroy(nl_config_t *ncf)
{
	if (!ncf->ncf_dict) {
		prop_object_release(ncf->ncf_alg_list);
		prop_object_release(ncf->ncf_rules_list);
		prop_object_release(ncf->ncf_rproc_list);
		prop_object_release(ncf->ncf_table_list);
		prop_object_release(ncf->ncf_nat_list);
	}
	if (ncf->ncf_err) {
		prop_object_release(ncf->ncf_err);
	}
	if (ncf->ncf_debug) {
		prop_object_release(ncf->ncf_debug);
	}
	free(ncf);
}

static bool
_npf_prop_array_lookup(prop_array_t array, const char *key, const char *name)
{
	prop_dictionary_t dict;
	prop_object_iterator_t it;

	it = prop_array_iterator(array);
	while ((dict = prop_object_iterator_next(it)) != NULL) {
		const char *lname;
		prop_dictionary_get_cstring_nocopy(dict, key, &lname);
		if (strcmp(name, lname) == 0)
			break;
	}
	prop_object_iterator_release(it);
	return dict ? true : false;
}

/*
 * DYNAMIC RULESET INTERFACE.
 */

int
npf_ruleset_add(int fd, const char *rname, nl_rule_t *rl, uint64_t *id)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_dictionary_t ret = NULL;
	int error;

	prop_dictionary_set_cstring(rldict, "ruleset-name", rname);
	prop_dictionary_set_uint32(rldict, "command", NPF_CMD_RULE_ADD);
#ifdef _NPF_STANDALONE
	error = ENOTSUP;
#else
	error = prop_dictionary_sendrecv_ioctl(rldict, fd, IOC_NPF_RULE, &ret);
#endif
	if (!error) {
		prop_dictionary_get_uint64(ret, "id", id);
	}
	return error;
}

int
npf_ruleset_remove(int fd, const char *rname, uint64_t id)
{
	prop_dictionary_t rldict;

	rldict = prop_dictionary_create();
	if (rldict == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set_cstring(rldict, "ruleset-name", rname);
	prop_dictionary_set_uint32(rldict, "command", NPF_CMD_RULE_REMOVE);
	prop_dictionary_set_uint64(rldict, "id", id);
#ifdef _NPF_STANDALONE
	return ENOTSUP;
#else
	return prop_dictionary_send_ioctl(rldict, fd, IOC_NPF_RULE);
#endif
}

int
npf_ruleset_remkey(int fd, const char *rname, const void *key, size_t len)
{
	prop_dictionary_t rldict;
	prop_data_t keyobj;

	rldict = prop_dictionary_create();
	if (rldict == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set_cstring(rldict, "ruleset-name", rname);
	prop_dictionary_set_uint32(rldict, "command", NPF_CMD_RULE_REMKEY);

	keyobj = prop_data_create_data(key, len);
	if (keyobj == NULL) {
		prop_object_release(rldict);
		return ENOMEM;
	}
	prop_dictionary_set(rldict, "key", keyobj);
	prop_object_release(keyobj);

#ifdef _NPF_STANDALONE
	return ENOTSUP;
#else
	return prop_dictionary_send_ioctl(rldict, fd, IOC_NPF_RULE);
#endif
}

int
npf_ruleset_flush(int fd, const char *rname)
{
	prop_dictionary_t rldict;

	rldict = prop_dictionary_create();
	if (rldict == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set_cstring(rldict, "ruleset-name", rname);
	prop_dictionary_set_uint32(rldict, "command", NPF_CMD_RULE_FLUSH);
#ifdef _NPF_STANDALONE
	return ENOTSUP;
#else
	return prop_dictionary_send_ioctl(rldict, fd, IOC_NPF_RULE);
#endif
}

/*
 * _npf_ruleset_transform: transform the ruleset representing nested
 * rules with lists into an array.
 */

static void
_npf_ruleset_transform1(prop_array_t rlset, prop_array_t rules)
{
	prop_object_iterator_t it;
	prop_dictionary_t rldict;
	prop_array_t subrlset;

	it = prop_array_iterator(rules);
	while ((rldict = prop_object_iterator_next(it)) != NULL) {
		unsigned idx;

		/* Add rules to the array (reference is retained). */
		prop_array_add(rlset, rldict);

		subrlset = prop_dictionary_get(rldict, "subrules");
		if (subrlset) {
			/* Process subrules recursively. */
			_npf_ruleset_transform1(rlset, subrlset);
			/* Add the skip-to position. */
			idx = prop_array_count(rlset);
			prop_dictionary_set_uint32(rldict, "skip-to", idx);
			prop_dictionary_remove(rldict, "subrules");
		}
	}
	prop_object_iterator_release(it);
}

static prop_array_t
_npf_ruleset_transform(prop_array_t rlset)
{
	prop_array_t nrlset;

	nrlset = prop_array_create();
	_npf_ruleset_transform1(nrlset, rlset);
	return nrlset;
}

/*
 * NPF EXTENSION INTERFACE.
 */

nl_ext_t *
npf_ext_construct(const char *name)
{
	nl_ext_t *ext;

	ext = malloc(sizeof(*ext));
	if (ext == NULL) {
		return NULL;
	}
	ext->nxt_name = strdup(name);
	if (ext->nxt_name == NULL) {
		free(ext);
		return NULL;
	}
	ext->nxt_dict = prop_dictionary_create();

	return ext;
}

void
npf_ext_param_u32(nl_ext_t *ext, const char *key, uint32_t val)
{
	prop_dictionary_t extdict = ext->nxt_dict;
	prop_dictionary_set_uint32(extdict, key, val);
}

void
npf_ext_param_bool(nl_ext_t *ext, const char *key, bool val)
{
	prop_dictionary_t extdict = ext->nxt_dict;
	prop_dictionary_set_bool(extdict, key, val);
}

void
npf_ext_param_string(nl_ext_t *ext, const char *key, const char *val)
{
	prop_dictionary_t extdict = ext->nxt_dict;
	prop_dictionary_set_cstring(extdict, key, val);
}

/*
 * RULE INTERFACE.
 */

nl_rule_t *
npf_rule_create(const char *name, uint32_t attr, const char *ifname)
{
	prop_dictionary_t rldict;
	nl_rule_t *rl;

	rl = malloc(sizeof(*rl));
	if (rl == NULL) {
		return NULL;
	}
	rldict = prop_dictionary_create();
	if (rldict == NULL) {
		free(rl);
		return NULL;
	}
	if (name) {
		prop_dictionary_set_cstring(rldict, "name", name);
	}
	prop_dictionary_set_uint32(rldict, "attr", attr);

	if (ifname) {
		prop_dictionary_set_cstring(rldict, "ifname", ifname);
	}
	rl->nrl_dict = rldict;
	return rl;
}

int
npf_rule_setcode(nl_rule_t *rl, int type, const void *code, size_t len)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_data_t cdata;

	switch (type) {
	case NPF_CODE_NC:
	case NPF_CODE_BPF:
		break;
	default:
		return ENOTSUP;
	}
	prop_dictionary_set_uint32(rldict, "code-type", (uint32_t)type);
	if ((cdata = prop_data_create_data(code, len)) == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set(rldict, "code", cdata);
	prop_object_release(cdata);
	return 0;
}

int
npf_rule_setkey(nl_rule_t *rl, const void *key, size_t len)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_data_t kdata;

	if ((kdata = prop_data_create_data(key, len)) == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set(rldict, "key", kdata);
	prop_object_release(kdata);
	return 0;
}

int
npf_rule_setinfo(nl_rule_t *rl, const void *info, size_t len)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_data_t idata;

	if ((idata = prop_data_create_data(info, len)) == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set(rldict, "info", idata);
	prop_object_release(idata);
	return 0;
}

int
npf_rule_setprio(nl_rule_t *rl, int pri)
{
	prop_dictionary_t rldict = rl->nrl_dict;

	prop_dictionary_set_int32(rldict, "prio", pri);
	return 0;
}

int
npf_rule_setproc(nl_rule_t *rl, const char *name)
{
	prop_dictionary_t rldict = rl->nrl_dict;

	prop_dictionary_set_cstring(rldict, "rproc", name);
	return 0;
}

void *
npf_rule_export(nl_rule_t *rl, size_t *length)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	void *xml;

	if ((xml = prop_dictionary_externalize(rldict)) == NULL) {
		return NULL;
	}
	*length = strlen(xml);
	return xml;
}

bool
npf_rule_exists_p(nl_config_t *ncf, const char *name)
{
	return _npf_prop_array_lookup(ncf->ncf_rules_list, "name", name);
}

int
npf_rule_insert(nl_config_t *ncf, nl_rule_t *parent, nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_array_t rlset;

	if (parent) {
		prop_dictionary_t pdict = parent->nrl_dict;
		rlset = prop_dictionary_get(pdict, "subrules");
		if (rlset == NULL) {
			rlset = prop_array_create();
			prop_dictionary_set(pdict, "subrules", rlset);
			prop_object_release(rlset);
		}
	} else {
		rlset = ncf->ncf_rules_list;
	}
	prop_array_add(rlset, rldict);
	prop_object_release(rldict);
	return 0;
}

static nl_rule_t *
_npf_rule_iterate1(nl_config_t *ncf, prop_array_t rlist, unsigned *level)
{
	prop_dictionary_t rldict;
	uint32_t skipto = 0;

	if (!ncf->ncf_rule_iter) {
		/* Initialise the iterator. */
		ncf->ncf_rule_iter = prop_array_iterator(rlist);
		ncf->ncf_nlevel = 0;
		ncf->ncf_reduce[0] = 0;
		ncf->ncf_counter = 0;
	}

	rldict = prop_object_iterator_next(ncf->ncf_rule_iter);
	if ((ncf->ncf_cur_rule.nrl_dict = rldict) == NULL) {
		prop_object_iterator_release(ncf->ncf_rule_iter);
		ncf->ncf_rule_iter = NULL;
		return NULL;
	}
	*level = ncf->ncf_nlevel;

	prop_dictionary_get_uint32(rldict, "skip-to", &skipto);
	if (skipto) {
		ncf->ncf_nlevel++;
		ncf->ncf_reduce[ncf->ncf_nlevel] = skipto;
	}
	if (ncf->ncf_reduce[ncf->ncf_nlevel] == ++ncf->ncf_counter) {
		assert(ncf->ncf_nlevel > 0);
		ncf->ncf_nlevel--;
	}
	return &ncf->ncf_cur_rule;
}

nl_rule_t *
npf_rule_iterate(nl_config_t *ncf, unsigned *level)
{
	return _npf_rule_iterate1(ncf, ncf->ncf_rules_list, level);
}

const char *
npf_rule_getname(nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	const char *rname = NULL;

	prop_dictionary_get_cstring_nocopy(rldict, "name", &rname);
	return rname;
}

uint32_t
npf_rule_getattr(nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	uint32_t attr = 0;

	prop_dictionary_get_uint32(rldict, "attr", &attr);
	return attr;
}

const char *
npf_rule_getinterface(nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	const char *ifname = NULL;

	prop_dictionary_get_cstring_nocopy(rldict, "ifname", &ifname);
	return ifname;
}

const void *
npf_rule_getinfo(nl_rule_t *rl, size_t *len)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_object_t obj = prop_dictionary_get(rldict, "info");

	*len = prop_data_size(obj);
	return prop_data_data_nocopy(obj);
}

const char *
npf_rule_getproc(nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	const char *rpname = NULL;

	prop_dictionary_get_cstring_nocopy(rldict, "rproc", &rpname);
	return rpname;
}

uint64_t
npf_rule_getid(nl_rule_t *rl)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	uint64_t id = 0;

	(void)prop_dictionary_get_uint64(rldict, "id", &id);
	return (unsigned)id;
}

const void *
npf_rule_getcode(nl_rule_t *rl, int *type, size_t *len)
{
	prop_dictionary_t rldict = rl->nrl_dict;
	prop_object_t obj = prop_dictionary_get(rldict, "code");

	prop_dictionary_get_uint32(rldict, "code-type", (uint32_t *)type);
	*len = prop_data_size(obj);
	return prop_data_data_nocopy(obj);
}

int
_npf_ruleset_list(int fd, const char *rname, nl_config_t *ncf)
{
	prop_dictionary_t rldict, ret = NULL;
	int error;

	rldict = prop_dictionary_create();
	if (rldict == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set_cstring(rldict, "ruleset-name", rname);
	prop_dictionary_set_uint32(rldict, "command", NPF_CMD_RULE_LIST);
#ifdef _NPF_STANDALONE
	error = ENOTSUP;
#else
	error = prop_dictionary_sendrecv_ioctl(rldict, fd, IOC_NPF_RULE, &ret);
#endif
	if (!error) {
		prop_array_t rules;

		rules = prop_dictionary_get(ret, "rules");
		if (rules == NULL) {
			return EINVAL;
		}
		prop_object_release(ncf->ncf_rules_list);
		ncf->ncf_rules_list = rules;
	}
	return error;
}

void
npf_rule_destroy(nl_rule_t *rl)
{

	prop_object_release(rl->nrl_dict);
	free(rl);
}

/*
 * RULE PROCEDURE INTERFACE.
 */

nl_rproc_t *
npf_rproc_create(const char *name)
{
	prop_dictionary_t rpdict;
	prop_array_t extcalls;
	nl_rproc_t *nrp;

	nrp = malloc(sizeof(nl_rproc_t));
	if (nrp == NULL) {
		return NULL;
	}
	rpdict = prop_dictionary_create();
	if (rpdict == NULL) {
		free(nrp);
		return NULL;
	}
	prop_dictionary_set_cstring(rpdict, "name", name);

	extcalls = prop_array_create();
	if (extcalls == NULL) {
		prop_object_release(rpdict);
		free(nrp);
		return NULL;
	}
	prop_dictionary_set(rpdict, "extcalls", extcalls);
	prop_object_release(extcalls);

	nrp->nrp_dict = rpdict;
	return nrp;
}

int
npf_rproc_extcall(nl_rproc_t *rp, nl_ext_t *ext)
{
	prop_dictionary_t rpdict = rp->nrp_dict;
	prop_dictionary_t extdict = ext->nxt_dict;
	prop_array_t extcalls;

	extcalls = prop_dictionary_get(rpdict, "extcalls");
	if (_npf_prop_array_lookup(extcalls, "name", ext->nxt_name)) {
		return EEXIST;
	}
	prop_dictionary_set_cstring(extdict, "name", ext->nxt_name);
	prop_array_add(extcalls, extdict);
	prop_object_release(extdict);
	return 0;
}

bool
npf_rproc_exists_p(nl_config_t *ncf, const char *name)
{
	return _npf_prop_array_lookup(ncf->ncf_rproc_list, "name", name);
}

int
npf_rproc_insert(nl_config_t *ncf, nl_rproc_t *rp)
{
	prop_dictionary_t rpdict = rp->nrp_dict;
	const char *name;

	if (!prop_dictionary_get_cstring_nocopy(rpdict, "name", &name)) {
		return EINVAL;
	}
	if (npf_rproc_exists_p(ncf, name)) {
		return EEXIST;
	}
	prop_array_add(ncf->ncf_rproc_list, rpdict);
	prop_object_release(rpdict);
	return 0;
}

nl_rproc_t *
npf_rproc_iterate(nl_config_t *ncf)
{
	prop_dictionary_t rpdict;

	if (!ncf->ncf_rproc_iter) {
		/* Initialise the iterator. */
		ncf->ncf_rproc_iter = prop_array_iterator(ncf->ncf_rproc_list);
	}
	rpdict = prop_object_iterator_next(ncf->ncf_rproc_iter);
	if ((ncf->ncf_cur_rproc.nrp_dict = rpdict) == NULL) {
		prop_object_iterator_release(ncf->ncf_rproc_iter);
		ncf->ncf_rproc_iter = NULL;
		return NULL;
	}
	return &ncf->ncf_cur_rproc;
}

const char *
npf_rproc_getname_int(nl_rproc_t *rp)
{
	prop_dictionary_t rpdict = rp->nrp_dict;
	const char *rpname = NULL;

	prop_dictionary_get_cstring_nocopy(rpdict, "name", &rpname);
	return rpname;
}

/*
 * NAT INTERFACE.
 */

nl_nat_t *
npf_nat_create(int type, u_int flags, const char *ifname,
    int af, npf_addr_t *addr, npf_netmask_t mask, in_port_t port)
{
	nl_rule_t *rl;
	prop_dictionary_t rldict;
	uint32_t attr;

	attr = NPF_RULE_PASS | NPF_RULE_FINAL |
	    (type == NPF_NATOUT ? NPF_RULE_OUT : NPF_RULE_IN);

	/* Create a rule for NAT policy.  Next, will add NAT data. */
	rl = npf_rule_create(NULL, attr, ifname);
	if (rl == NULL) {
		return NULL;
	}
	rldict = rl->nrl_dict;

	/* Translation type and flags. */
	prop_dictionary_set_int32(rldict, "type", type);
	prop_dictionary_set_uint32(rldict, "flags", flags);

	/* Translation IP and mask. */
	if (!_npf_add_addr(rldict, "nat-ip", af, addr)) {
		npf_rule_destroy(rl);
		return NULL;
	}
	prop_dictionary_set_uint32(rldict, "nat-mask", (uint32_t)mask);

	/* Translation port (for redirect case). */
	prop_dictionary_set_uint16(rldict, "nat-port", port);

	return (nl_nat_t *)rl;
}

int
npf_nat_insert(nl_config_t *ncf, nl_nat_t *nt, int pri __unused)
{
	prop_dictionary_t rldict = nt->nrl_dict;

	prop_dictionary_set_int32(rldict, "prio", NPF_PRI_LAST);
	prop_array_add(ncf->ncf_nat_list, rldict);
	prop_object_release(rldict);
	return 0;
}

nl_nat_t *
npf_nat_iterate(nl_config_t *ncf)
{
	u_int level;
	return _npf_rule_iterate1(ncf, ncf->ncf_nat_list, &level);
}

int
npf_nat_setalgo(nl_nat_t *nt, u_int algo)
{
	prop_dictionary_t rldict = nt->nrl_dict;
	prop_dictionary_set_uint32(rldict, "nat-algo", algo);
	return 0;
}

int
npf_nat_setnpt66(nl_nat_t *nt, uint16_t adj)
{
	prop_dictionary_t rldict = nt->nrl_dict;
	int error;

	if ((error = npf_nat_setalgo(nt, NPF_ALGO_NPT66)) != 0) {
		return error;
	}
	prop_dictionary_set_uint16(rldict, "npt66-adj", adj);
	return 0;
}

int
npf_nat_gettype(nl_nat_t *nt)
{
	prop_dictionary_t rldict = nt->nrl_dict;
	int type = 0;

	prop_dictionary_get_int32(rldict, "type", &type);
	return type;
}

u_int
npf_nat_getflags(nl_nat_t *nt)
{
	prop_dictionary_t rldict = nt->nrl_dict;
	unsigned flags = 0;

	prop_dictionary_get_uint32(rldict, "flags", &flags);
	return flags;
}

void
npf_nat_getmap(nl_nat_t *nt, npf_addr_t *addr, size_t *alen, in_port_t *port)
{
	prop_dictionary_t rldict = nt->nrl_dict;
	prop_object_t obj = prop_dictionary_get(rldict, "nat-ip");

	*alen = prop_data_size(obj);
	memcpy(addr, prop_data_data_nocopy(obj), *alen);

	*port = 0;
	prop_dictionary_get_uint16(rldict, "nat-port", port);
}

/*
 * TABLE INTERFACE.
 */

nl_table_t *
npf_table_create(const char *name, u_int id, int type)
{
	prop_dictionary_t tldict;
	prop_array_t tblents;
	nl_table_t *tl;

	tl = malloc(sizeof(*tl));
	if (tl == NULL) {
		return NULL;
	}
	tldict = prop_dictionary_create();
	if (tldict == NULL) {
		free(tl);
		return NULL;
	}
	prop_dictionary_set_cstring(tldict, "name", name);
	prop_dictionary_set_uint64(tldict, "id", (uint64_t)id);
	prop_dictionary_set_int32(tldict, "type", type);

	tblents = prop_array_create();
	if (tblents == NULL) {
		prop_object_release(tldict);
		free(tl);
		return NULL;
	}
	prop_dictionary_set(tldict, "entries", tblents);
	prop_object_release(tblents);

	tl->ntl_dict = tldict;
	return tl;
}

int
npf_table_add_entry(nl_table_t *tl, int af, const npf_addr_t *addr,
    const npf_netmask_t mask)
{
	prop_dictionary_t tldict = tl->ntl_dict, entdict;
	prop_array_t tblents;

	/* Create the table entry. */
	entdict = prop_dictionary_create();
	if (entdict == NULL) {
		return ENOMEM;
	}

	if (!_npf_add_addr(entdict, "addr", af, addr)) {
		return EINVAL;
	}
	prop_dictionary_set_uint8(entdict, "mask", mask);

	tblents = prop_dictionary_get(tldict, "entries");
	prop_array_add(tblents, entdict);
	prop_object_release(entdict);
	return 0;
}

int
npf_table_setdata(nl_table_t *tl, const void *blob, size_t len)
{
	prop_dictionary_t tldict = tl->ntl_dict;
	prop_data_t bobj;

	if ((bobj = prop_data_create_data(blob, len)) == NULL) {
		return ENOMEM;
	}
	prop_dictionary_set(tldict, "data", bobj);
	prop_object_release(bobj);
	return 0;
}

static bool
_npf_table_exists_p(nl_config_t *ncf, const char *name)
{
	prop_dictionary_t tldict;
	prop_object_iterator_t it;

	it = prop_array_iterator(ncf->ncf_table_list);
	while ((tldict = prop_object_iterator_next(it)) != NULL) {
		const char *tname = NULL;

		if (prop_dictionary_get_cstring_nocopy(tldict, "name", &tname)
		    && strcmp(tname, name) == 0)
			break;
	}
	prop_object_iterator_release(it);
	return tldict ? true : false;
}

int
npf_table_insert(nl_config_t *ncf, nl_table_t *tl)
{
	prop_dictionary_t tldict = tl->ntl_dict;
	const char *name = NULL;

	if (!prop_dictionary_get_cstring_nocopy(tldict, "name", &name)) {
		return EINVAL;
	}
	if (_npf_table_exists_p(ncf, name)) {
		return EEXIST;
	}
	prop_array_add(ncf->ncf_table_list, tldict);
	prop_object_release(tldict);
	return 0;
}

nl_table_t *
npf_table_iterate(nl_config_t *ncf)
{
	prop_dictionary_t tldict;

	if (!ncf->ncf_table_iter) {
		/* Initialise the iterator. */
		ncf->ncf_table_iter = prop_array_iterator(ncf->ncf_table_list);
	}
	tldict = prop_object_iterator_next(ncf->ncf_table_iter);
	if ((ncf->ncf_cur_table.ntl_dict = tldict) == NULL) {
		prop_object_iterator_release(ncf->ncf_table_iter);
		ncf->ncf_table_iter = NULL;
		return NULL;
	}
	return &ncf->ncf_cur_table;
}

unsigned
npf_table_getid(nl_table_t *tl)
{
	prop_dictionary_t tldict = tl->ntl_dict;
	uint64_t id = (uint64_t)-1;

	prop_dictionary_get_uint64(tldict, "id", &id);
	return (unsigned)id;
}

const char *
npf_table_getname(nl_table_t *tl)
{
	prop_dictionary_t tldict = tl->ntl_dict;
	const char *tname = NULL;

	prop_dictionary_get_cstring_nocopy(tldict, "name", &tname);
	return tname;
}

int
npf_table_gettype(nl_table_t *tl)
{
	prop_dictionary_t tldict = tl->ntl_dict;
	int type = 0;

	prop_dictionary_get_int32(tldict, "type", &type);
	return type;
}

void
npf_table_destroy(nl_table_t *tl)
{
	prop_object_release(tl->ntl_dict);
	free(tl);
}

/*
 * ALG INTERFACE.
 */

int
_npf_alg_load(nl_config_t *ncf, const char *name)
{
	prop_dictionary_t al_dict;

	if (_npf_prop_array_lookup(ncf->ncf_alg_list, "name", name))
		return EEXIST;

	al_dict = prop_dictionary_create();
	prop_dictionary_set_cstring(al_dict, "name", name);
	prop_array_add(ncf->ncf_alg_list, al_dict);
	prop_object_release(al_dict);
	return 0;
}

int
_npf_alg_unload(nl_config_t *ncf, const char *name)
{
	if (!_npf_prop_array_lookup(ncf->ncf_alg_list, "name", name))
		return ENOENT;

	// Not yet: prop_array_add(ncf->ncf_alg_list, al_dict);
	return ENOTSUP;
}

/*
 * MISC.
 */

static prop_dictionary_t
_npf_debug_initonce(nl_config_t *ncf)
{
	if (!ncf->ncf_debug) {
		prop_array_t iflist = prop_array_create();
		ncf->ncf_debug = prop_dictionary_create();
		prop_dictionary_set(ncf->ncf_debug, "interfaces", iflist);
		prop_object_release(iflist);
	}
	return ncf->ncf_debug;
}

void
_npf_debug_addif(nl_config_t *ncf, const char *ifname)
{
	prop_dictionary_t ifdict, dbg = _npf_debug_initonce(ncf);
	prop_array_t iflist = prop_dictionary_get(dbg, "interfaces");
	u_int if_idx = if_nametoindex(ifname);

	if (_npf_prop_array_lookup(iflist, "name", ifname)) {
		return;
	}
	ifdict = prop_dictionary_create();
	prop_dictionary_set_cstring(ifdict, "name", ifname);
	prop_dictionary_set_uint32(ifdict, "index", if_idx);
	prop_array_add(iflist, ifdict);
	prop_object_release(ifdict);
}

int
npf_nat_lookup(int fd, int af, npf_addr_t *addr[2], in_port_t port[2],
    int proto, int dir)
{
	prop_dictionary_t conn_dict, conn_res = NULL;
	int error = EINVAL;

	conn_dict = prop_dictionary_create();
	if (conn_dict == NULL)
		return ENOMEM;

	if (!prop_dictionary_set_uint16(conn_dict, "direction", dir))
		goto out;

	conn_res = prop_dictionary_create();
	if (conn_res == NULL)
		goto out;

	if (!_npf_add_addr(conn_res, "saddr", af, addr[0]))
		goto out;
	if (!_npf_add_addr(conn_res, "daddr", af, addr[1]))
		goto out;
	if (!prop_dictionary_set_uint16(conn_res, "sport", port[0]))
		goto out;
	if (!prop_dictionary_set_uint16(conn_res, "dport", port[1]))
		goto out;
	if (!prop_dictionary_set_uint16(conn_res, "proto", proto))
		goto out;
	if (!prop_dictionary_set(conn_dict, "key", conn_res))
		goto out;

	prop_object_release(conn_res);

#if !defined(_NPF_STANDALONE)
	error = prop_dictionary_sendrecv_ioctl(conn_dict, fd,
	    IOC_NPF_CONN_LOOKUP, &conn_res);
#else
	error = ENOTSUP;
#endif
	if (error != 0)
		goto out;

	prop_dictionary_t nat = prop_dictionary_get(conn_res, "nat");
	if (nat == NULL) {
		errno = ENOENT;
		goto out;
	}

	if (!_npf_get_addr(nat, "oaddr", addr[0])) {
		error = EINVAL;
		goto out;
	}

	prop_dictionary_get_uint16(nat, "oport", &port[0]);
	prop_dictionary_get_uint16(nat, "tport", &port[1]);
out:
	if (conn_res)
		prop_object_release(conn_res);
	prop_object_release(conn_dict);
	return error;
}

struct npf_endpoint {
	npf_addr_t addr[2];
	in_port_t port[2];
	uint16_t alen;
	uint16_t proto;
};

static bool
npf_endpoint_load(prop_dictionary_t cdict, const char *name,
    struct npf_endpoint *ep)
{
	prop_dictionary_t ed = prop_dictionary_get(cdict, name);
	if (ed == NULL)
		return false;
	if (!(ep->alen = _npf_get_addr(ed, "saddr", &ep->addr[0])))
		return false;
	if (ep->alen != _npf_get_addr(ed, "daddr", &ep->addr[1]))
		return false;
	if (!prop_dictionary_get_uint16(ed, "sport", &ep->port[0]))
		return false;
	if (!prop_dictionary_get_uint16(ed, "dport", &ep->port[1]))
		return false;
	if (!prop_dictionary_get_uint16(ed, "proto", &ep->proto))
		return false;
	return true;
}

static void
npf_conn_handle(prop_dictionary_t cdict, npf_conn_func_t fun, void *v)
{
	prop_dictionary_t nat;
	struct npf_endpoint ep;
	uint16_t tport;
	const char *ifname;

	if (!prop_dictionary_get_cstring_nocopy(cdict, "ifname", &ifname))
		goto err;

	if ((nat = prop_dictionary_get(cdict, "nat")) != NULL &&
	    prop_object_type(nat) == PROP_TYPE_DICTIONARY) {
		if (!prop_dictionary_get_uint16(nat, "tport", &tport))
			goto err;
	} else {
		tport = 0;
	}
	if (!npf_endpoint_load(cdict, "forw-key", &ep))
		goto err;

	in_port_t p[] = {
	    ntohs(ep.port[0]),
	    ntohs(ep.port[1]),
	    ntohs(tport)
	};
	(*fun)((unsigned)ep.alen, ep.addr, p, ifname, v);
err:
	return;
}

int
npf_conn_list(int fd, npf_conn_func_t fun, void *v)
{
	nl_config_t *ncf;

	ncf = npf_config_retrieve(fd);
	if (ncf == NULL) {
		return errno;
	}

	/* Connection list - array */ 
	if (prop_object_type(ncf->ncf_conn_list) != PROP_TYPE_ARRAY) {
		return EINVAL;
	}

	prop_object_iterator_t it = prop_array_iterator(ncf->ncf_conn_list);
	prop_dictionary_t condict;
	while ((condict = prop_object_iterator_next(it)) != NULL) {
		if (prop_object_type(condict) != PROP_TYPE_DICTIONARY) {
			return EINVAL;
		}
		npf_conn_handle(condict, fun, v);
	}
	return 0;
}
