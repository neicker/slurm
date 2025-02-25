/*****************************************************************************\
 *  data_t parser plugin interface
 ******************************************************************************
 *  Copyright (C) 2022 SchedMD LLC.
 *  Written by Nathan Rini <nate@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include "src/common/data.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/timers.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/data_parser.h"
#include "src/interfaces/serializer.h"

#define PARSE_MAJOR_TYPE "data_parser"
#define PARSE_MAGIC 0x0ea0b1be

struct data_parser_s {
	int magic;
	int plugin_offset;
	/* arg returned by plugin init() */
	void *arg;
	const char *plugin_type; /* ptr to plugin plugin_type - do not xfree */
	char *params; /* parameters from _new - must xfree */
	char *plugin_string; /* plugin_type+params - must xfree */
};

typedef struct {
	int (*parse)(void *arg, data_parser_type_t type, void *dst,
		     ssize_t dst_bytes, data_t *src, data_t *parent_path);
	int (*dump)(void *arg, data_parser_type_t type, void *src,
		    ssize_t src_bytes, data_t *dst);
	/* ptr returned to be handed to commands as arg */
	void *(*new)(data_parser_on_error_t on_parse_error,
		     data_parser_on_error_t on_dump_error,
		     data_parser_on_error_t on_query_error, void *error_arg,
		     data_parser_on_warn_t on_parse_warn,
		     data_parser_on_warn_t on_dump_warn,
		     data_parser_on_warn_t on_query_warn, void *warn,
		     char *params);
	void (*free)(void *arg);
	int (*assign)(void *arg, data_parser_attr_type_t type, void *obj);
	int (*specify)(void *arg, data_t *dst);
} parse_funcs_t;

typedef struct {
	char *plugin_type;
	char *params;
} plugin_param_t;

/*
 * Must be synchronized with parse_funcs_t above.
 */
static const char *parse_syms[] = {
	"data_parser_p_parse",
	"data_parser_p_dump",
	"data_parser_p_new",
	"data_parser_p_free",
	"data_parser_p_assign",
	"data_parser_p_specify",
};

static plugins_t *plugins = NULL;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_parsers = 0;

static const char *_get_plugin_version(const char *plugin_type);

extern int data_parser_g_parse(data_parser_t *parser, data_parser_type_t type,
			       void *dst, ssize_t dst_bytes, data_t *src,
			       data_t *parent_path)
{
	const parse_funcs_t *funcs;
	DEF_TIMERS;
	int rc;

	if (!parser)
		return ESLURM_DATA_INVALID_PARSER;

	funcs = plugins->functions[parser->plugin_offset];

	if (!src || (data_get_type(src) == DATA_TYPE_NONE))
		return ESLURM_DATA_PARSE_NOTHING;

	xassert(type > DATA_PARSER_TYPE_INVALID);
	xassert(type < DATA_PARSER_TYPE_MAX);
	xassert(parser->magic == PARSE_MAGIC);
	xassert(data_get_type(parent_path) == DATA_TYPE_LIST);
	xassert(plugins && (plugins->magic == PLUGINS_MAGIC));
	xassert(parser->plugin_offset < plugins->count);
	xassert(plugins->functions[parser->plugin_offset]);

	START_TIMER;
	rc = funcs->parse(parser->arg, type, dst, dst_bytes, src, parent_path);
	END_TIMER2(__func__);

	return rc;
}

extern int data_parser_g_dump(data_parser_t *parser, data_parser_type_t type,
			      void *src, ssize_t src_bytes, data_t *dst)
{
	DEF_TIMERS;
	int rc;
	const parse_funcs_t *funcs;

	if (!parser)
		return ESLURM_DATA_INVALID_PARSER;

	funcs = plugins->functions[parser->plugin_offset];

	xassert(data_get_type(dst));
	xassert(type > DATA_PARSER_TYPE_INVALID);
	xassert(type < DATA_PARSER_TYPE_MAX);
	xassert(parser->magic == PARSE_MAGIC);
	xassert(plugins && (plugins->magic == PLUGINS_MAGIC));
	xassert(parser->plugin_offset < plugins->count);
	xassert(plugins->functions[parser->plugin_offset]);

	START_TIMER;
	rc = funcs->dump(parser->arg, type, src, src_bytes, dst);
	END_TIMER2(__func__);

	return rc;
}

/* takes ownership of params */
static data_parser_t *_new_parser(data_parser_on_error_t on_parse_error,
				  data_parser_on_error_t on_dump_error,
				  data_parser_on_error_t on_query_error,
				  void *error_arg,
				  data_parser_on_warn_t on_parse_warn,
				  data_parser_on_warn_t on_dump_warn,
				  data_parser_on_warn_t on_query_warn,
				  void *warn_arg, int plugin_index,
				  char *params)
{
	DEF_TIMERS;
	const parse_funcs_t *funcs;
	data_parser_t *parser = xmalloc(sizeof(*parser));

	parser->magic = PARSE_MAGIC;

	parser->plugin_offset = plugin_index;
	parser->plugin_type = plugins->types[plugin_index];
	parser->params = params;

	START_TIMER;
	funcs = plugins->functions[plugin_index];
	parser->arg = funcs->new(on_parse_error, on_dump_error, on_query_error,
				 error_arg, on_parse_warn, on_dump_warn,
				 on_query_warn, warn_arg, params);
	END_TIMER2(__func__);

	slurm_mutex_lock(&init_mutex);
	xassert(active_parsers >= 0);
	active_parsers++;
	slurm_mutex_unlock(&init_mutex);

	return parser;
}

static plugin_param_t *_parse_plugin_type(const char *plugin_type)
{
	char *type, *last = NULL, *pl;
	plugin_param_t *pparams = NULL;
	int count = 0;

	if (!plugin_type)
		return NULL;

	pl = xstrdup(plugin_type);
	type = strtok_r(pl, ",", &last);
	while (type) {
		char *pl;
		plugin_param_t *p;

		xrecalloc(pparams, (count + 2), sizeof(*pparams));

		p = &pparams[count];

		if ((pl = xstrstr(type,
				  SLURM_DATA_PARSER_PLUGIN_PARAMS_CHAR))) {
			p->plugin_type = xstrndup(type, (pl - type));
			p->params = xstrdup(pl);
		} else {
			p->plugin_type = xstrdup(type);
		}

		log_flag(DATA, "%s: plugin=%s params=%s",
		       __func__, p->plugin_type, p->params);

		count++;

		type = strtok_r(NULL, ",", &last);
	}

	xfree(pl);
	return pparams;
}

static int _load_plugins(plugin_param_t *pparams, plugrack_foreach_t listf,
			 bool skip_loading)
{
	int rc = SLURM_SUCCESS;

	if (skip_loading)
		return rc;

	slurm_mutex_lock(&init_mutex);

	xassert(sizeof(parse_funcs_t) ==
		(sizeof(void *) * ARRAY_SIZE(parse_syms)));

	if (!pparams) {
		rc = load_plugins(&plugins, PARSE_MAJOR_TYPE, NULL, listf,
				  parse_syms, ARRAY_SIZE(parse_syms));
	} else {
		for (int i = 0; !rc && pparams[i].plugin_type; i++)
			rc = load_plugins(&plugins, PARSE_MAJOR_TYPE,
					  pparams[i].plugin_type, listf,
					  parse_syms, ARRAY_SIZE(parse_syms));
	}

	xassert(rc || plugins);

	slurm_mutex_unlock(&init_mutex);

	return rc;
}

static int _find_plugin_by_type(const char *plugin_type)
{
	if (!plugin_type)
		return -1;

	/* quick match by pointer address */
	for (int i = 0; i < plugins->count; i++) {
		if (plugin_type == plugins->types[i])
			return i;
	}

	/* match by full string */
	for (int i = 0; i < plugins->count; i++) {
		if (!xstrcasecmp(plugin_type, plugins->types[i]))
			return i;
	}

	/* match by string without "data_parser/" */
	for (int i = 0; i < plugins->count; i++) {
		if (!xstrcasecmp(plugin_type,
				 _get_plugin_version(plugins->types[i])))
			return i;
	}

	return -1;
}

extern data_parser_t *data_parser_g_new(data_parser_on_error_t on_parse_error,
					data_parser_on_error_t on_dump_error,
					data_parser_on_error_t on_query_error,
					void *error_arg,
					data_parser_on_warn_t on_parse_warn,
					data_parser_on_warn_t on_dump_warn,
					data_parser_on_warn_t on_query_warn,
					void *warn_arg, const char *plugin_type,
					plugrack_foreach_t listf,
					bool skip_loading)
{
	int rc, index;
	char *params = NULL;
	data_parser_t *parser = NULL;
	plugin_param_t *pparams;

	if (!xstrcasecmp(plugin_type, "list")) {
		xassert(listf);
		load_plugins(&plugins, PARSE_MAJOR_TYPE, plugin_type, listf,
			     parse_syms, ARRAY_SIZE(parse_syms));
		return NULL;
	}

	pparams = _parse_plugin_type(plugin_type);

	if (!pparams || !pparams[0].plugin_type) {
		error("%s: invalid plugin %s", __func__, plugin_type);
		goto cleanup;
	}

	if (pparams[1].plugin_type) {
		error("%s: rejecting ambiguous plugin %s",
		      __func__, plugin_type);
		goto cleanup;
	}

	if ((rc = _load_plugins(pparams, listf, skip_loading))) {
		error("%s: failure loading plugins: %s",
		      __func__, slurm_strerror(rc));
		goto cleanup;
	}

	if ((index = _find_plugin_by_type(pparams[0].plugin_type)) < 0) {
		error("%s: unable to find plugin %s", __func__,
		      pparams[0].plugin_type);
		goto cleanup;
	}

	SWAP(params, pparams[0].params);

	parser = _new_parser(on_parse_error, on_dump_error, on_query_error,
			     error_arg, on_parse_warn, on_dump_warn,
			     on_query_warn, warn_arg, index, params);
cleanup:
	if (pparams) {
		for (int i = 0; pparams[i].plugin_type; i++) {
			xfree(pparams[i].plugin_type);
			xfree(pparams[i].params);
		}
		xfree(pparams);
	}

	return parser;
}

extern data_parser_t **data_parser_g_new_array(
	data_parser_on_error_t on_parse_error,
	data_parser_on_error_t on_dump_error,
	data_parser_on_error_t on_query_error,
	void *error_arg,
	data_parser_on_warn_t on_parse_warn,
	data_parser_on_warn_t on_dump_warn,
	data_parser_on_warn_t on_query_warn,
	void *warn_arg,
	const char *plugin_type,
	plugrack_foreach_t listf,
	bool skip_loading)
{
	int rc, i = 0;
	data_parser_t **parsers = NULL;
	plugin_param_t *pparams;

	if (!xstrcasecmp(plugin_type, "list")) {
		xassert(listf);
		load_plugins(&plugins, PARSE_MAJOR_TYPE, plugin_type, listf,
			     parse_syms, ARRAY_SIZE(parse_syms));
		return NULL;
	}

	pparams = _parse_plugin_type(plugin_type);

	if ((rc = _load_plugins(pparams, listf, skip_loading))) {
		error("%s: failure loading plugins: %s",
		      __func__, slurm_strerror(rc));
		goto cleanup;
	}

	/* always allocate for all possible plugins */
	parsers = xcalloc((plugins->count + 1), sizeof(*parsers));

	if (pparams) {
		for (; pparams[i].plugin_type; i++) {
			int index =
				_find_plugin_by_type(pparams[i].plugin_type);

			if (index < 0) {
				error("%s: unable to find plugin %s",
				      __func__, pparams[i].plugin_type);
				goto cleanup;
			}

			parsers[i] = _new_parser(on_parse_error, on_dump_error,
						 on_query_error, error_arg,
						 on_parse_warn, on_dump_warn,
						 on_query_warn, warn_arg, index,
						 pparams[i].params);

			pparams[i].params = NULL;
			xfree(pparams[i].plugin_type);
		}
	} else {
		for (; i < plugins->count; i++) {
			parsers[i] = _new_parser(on_parse_error, on_dump_error,
						 on_query_error, error_arg,
						 on_parse_warn, on_dump_warn,
						 on_query_warn, warn_arg, i,
						 NULL);
		}
	}

	xfree(pparams);

	return parsers;
cleanup:
	if (pparams) {
		for (; pparams[i].plugin_type; i++) {
			xfree(pparams[i].plugin_type);
			xfree(pparams[i].params);
		}
		xfree(pparams);
	}

	if (plugins)
		for (int j = 0; j < plugins->count; j++)
			FREE_NULL_DATA_PARSER(parsers[j]);
	xfree(parsers);

	return NULL;
}

extern const char *data_parser_get_plugin(data_parser_t *parser)
{
	if (!parser)
		return NULL;

	xassert(parser->magic == PARSE_MAGIC);

	/*
	 * Generate string as requested using full plugin type where the
	 * original request may not having included data_parser/
	 */
	if (!parser->plugin_string)
		xstrfmtcat(parser->plugin_string, "%s%s", parser->plugin_type,
			   (parser->params ? parser->params : ""));

	return parser->plugin_string;
}

static const char *_get_plugin_version(const char *plugin_type)
{
	static const char prefix[] = PARSE_MAJOR_TYPE "/";

#ifndef NDEBUG
	/* catch if the prefix ever changes in an unexpected way */
	const char *match;
	xassert(plugin_type);
	match = xstrstr(plugin_type, prefix);
	xassert(match);
#endif

	return plugin_type + strlen(prefix);
}

extern const char *data_parser_get_plugin_version(data_parser_t *parser)
{
	xassert(!parser || parser->magic == PARSE_MAGIC);

	if (!parser)
		return NULL;

	return _get_plugin_version(parser->plugin_type);
}

extern const char *data_parser_get_plugin_params(data_parser_t *parser)
{
	xassert(!parser || parser->magic == PARSE_MAGIC);

	if (!parser)
		return NULL;

	return parser->params;
}

extern void data_parser_g_free(data_parser_t *parser, bool skip_unloading)
{
	DEF_TIMERS;
	const parse_funcs_t *funcs;

	if (!parser)
		return;

	funcs = plugins->functions[parser->plugin_offset];

	if (plugins) {
		xassert(plugins->magic == PLUGINS_MAGIC);
		xassert(plugins->functions[parser->plugin_offset]);
		xassert(parser->magic == PARSE_MAGIC);
		xassert(parser->plugin_offset < plugins->count);
	}

	START_TIMER;
	if (plugins)
		funcs->free(parser->arg);
	END_TIMER2(__func__);

	xfree(parser->params);
	xfree(parser->plugin_string);
	parser->arg = NULL;
	parser->plugin_offset = -1;
	parser->magic = ~PARSE_MAGIC;
	xfree(parser);

	slurm_mutex_lock(&init_mutex);
	xassert(active_parsers >= 0);
	active_parsers--;
	xassert(active_parsers >= 0);

	if (!skip_unloading && !active_parsers)
		FREE_NULL_PLUGINS(plugins);
	slurm_mutex_unlock(&init_mutex);
}

extern void data_parser_g_array_free(data_parser_t **ptr, bool skip_unloading)
{
	if (!ptr)
		return;

	for (int i = 0; ptr[i]; i++)
		data_parser_g_free(ptr[i], skip_unloading);

	xfree(ptr);
}

extern int data_parser_g_assign(data_parser_t *parser,
				data_parser_attr_type_t type, void *obj)
{
	int rc;
	DEF_TIMERS;
	const parse_funcs_t *funcs;

	if (!parser)
		return ESLURM_DATA_INVALID_PARSER;

	funcs = plugins->functions[parser->plugin_offset];

	xassert(parser);
	xassert(plugins);
	xassert(parser->magic == PARSE_MAGIC);
	xassert(parser->plugin_offset < plugins->count);
	xassert(type > DATA_PARSER_ATTR_INVALID);
	xassert(type < DATA_PARSER_ATTR_MAX);

	START_TIMER;
	rc = funcs->assign(parser->arg, type, obj);
	END_TIMER2(__func__);

	return rc;
}

static bool _dump_cli_stdout_on_error(void *arg, data_parser_type_t type,
				      int error_code, const char *source,
				      const char *why, ...)
{
	va_list ap;
	data_t *errors = arg;
	data_t *e = data_set_dict(data_list_append(errors));

	if (why) {
		va_start(ap, why);
		data_set_string_own(data_key_set(e, "description"),
				    vxstrfmt(why, ap));
		va_end(ap);
	}

	if (error_code) {
		data_set_int(data_key_set(e, "error_number"), error_code);
		data_set_string(data_key_set(e, "error"),
				slurm_strerror(error_code));
	}

	if (source)
		data_set_string(data_key_set(e, "source"), source);

	data_set_string_fmt(data_key_set(e, "data_type"), "0x%x", type);

	return false;
}

static void _dump_cli_stdout_on_warn(void *arg, data_parser_type_t type,
				     const char *source, const char *why, ...)
{
	va_list ap;
	data_t *warns = arg;
	data_t *w = data_set_dict(data_list_append(warns));

	if (why) {
		va_start(ap, why);
		data_set_string_own(data_key_set(w, "description"),
				    vxstrfmt(why, ap));
		va_end(ap);
	}

	if (source)
		data_set_string(data_key_set(w, "source"), source);

	data_set_string_fmt(data_key_set(w, "data_type"), "0x%x", type);
}

static void _populate_cli_response_meta(data_t *meta, int argc, char **argv,
					data_parser_t *parser)
{
	data_t *cmd, *slurm, *slurmv, *plugin;

	plugin = data_set_dict(data_key_set(meta, "plugins"));
	cmd = data_set_list(data_key_set(meta, "command"));
	slurm = data_set_dict(data_key_set(meta, "Slurm"));
	slurmv = data_set_dict(data_key_set(slurm, "version"));

	data_set_string(data_key_set(plugin, "data_parser"),
			data_parser_get_plugin(parser));
	data_set_string(data_key_set(plugin, "accounting_storage"),
			slurm_conf.accounting_storage_type);

	data_set_string(data_key_set(slurm, "release"), SLURM_VERSION_STRING);
	(void) data_convert_type(data_set_string(data_key_set(slurmv, "major"),
						 SLURM_MAJOR),
				 DATA_TYPE_INT_64);
	(void) data_convert_type(data_set_string(data_key_set(slurmv, "micro"),
						 SLURM_MICRO),
				 DATA_TYPE_INT_64);
	(void) data_convert_type(data_set_string(data_key_set(slurmv, "minor"),
						 SLURM_MINOR),
				 DATA_TYPE_INT_64);

	for (int i = 0; i < argc; i++)
		data_set_string(data_list_append(cmd), argv[i]);
}

static void _plugrack_foreach_list(const char *full_type, const char *fq_path,
				   const plugin_handle_t id, void *arg)
{
	info("%s", full_type);
}

extern int data_parser_dump_cli_stdout(data_parser_type_t type, void *obj,
				       int obj_bytes, const char *key, int argc,
				       char **argv, void *acct_db_conn,
				       const char *mime_type,
				       const char *data_parser)
{
	int rc = SLURM_SUCCESS;
	data_t *resp, *meta, *dout;
	char *out = NULL;
	data_parser_t *parser;

	if (!xstrcasecmp(data_parser, "list")) {
		info("Possible data_parser plugins:");
		data_parser_g_new(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
				  NULL, "list", _plugrack_foreach_list, false);
		return SLURM_SUCCESS;
	}

	resp = data_set_dict(data_new());
	meta = data_set_dict(data_key_set(resp, "meta"));
	dout = data_key_set(resp, key);

	parser = data_parser_g_new(
		_dump_cli_stdout_on_error,
		_dump_cli_stdout_on_error,
		_dump_cli_stdout_on_error,
		data_set_list(data_key_set(resp, "errors")),
		_dump_cli_stdout_on_warn,
		_dump_cli_stdout_on_warn,
		_dump_cli_stdout_on_warn,
		data_set_list(data_key_set(resp, "warnings")),
		(data_parser ? data_parser : SLURM_DATA_PARSER_VERSION),
		NULL,
		false);

	if (!parser) {
		rc = ESLURM_DATA_INVALID_PARSER;
		goto cleanup;
	}

	_populate_cli_response_meta(meta, argc, argv, parser);

	data_parser_g_assign(parser, DATA_PARSER_ATTR_DBCONN_PTR, acct_db_conn);
	data_parser_g_dump(parser, type, obj, obj_bytes, dout);
	serialize_g_data_to_string(&out, NULL, resp, mime_type,
				   SER_FLAGS_PRETTY);

	printf("%s\n", out);

cleanup:
	xfree(out);
	FREE_NULL_DATA_PARSER(parser);

	return rc;
}

extern int data_parser_g_specify(data_parser_t *parser, data_t *dst)
{
	int rc;
	DEF_TIMERS;
	const parse_funcs_t *funcs;

	if (!parser)
		return ESLURM_DATA_INVALID_PARSER;

	funcs = plugins->functions[parser->plugin_offset];

	xassert(parser);
	xassert(plugins);
	xassert(parser->magic == PARSE_MAGIC);
	xassert(parser->plugin_offset < plugins->count);

	START_TIMER;
	rc = funcs->specify(parser->arg, dst);
	END_TIMER2(__func__);

	return rc;
}
