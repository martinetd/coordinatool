/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <ctype.h>

#include "client_common.h"

static int getenv_str(const char *name, const char **val) {
	const char *env = getenv(name);
	if (!env)
		return 0;
	*val = env;
	return 1;
}

static long long str_suffix_to_u32(const char *str, const char *error_hint) {
	char *endptr;

	long long val = strtoll(str, &endptr, 0);
	long multiplier = 1;

	if (!endptr)
		abort();
	switch (*endptr) {
	case 0:
		
		break;
	case 'G':
		multiplier *= 1024;
		// fallthrough
	case 'M':
		multiplier *= 1024;
		// fallthrough
	case 'K':
		multiplier *= 1024;
		if (endptr[1] != 0) {
			LOG_ERROR(-EINVAL, "trailing data after size prefix: %s, continuing anyway",
				  endptr + 1);
		}
		break;
	default:
		LOG_ERROR(-EINVAL, "%s was set to %s, which has trailing %s -- ignoring",
			  error_hint, str, endptr);
		return -1;
	}

	/* allow -1 as max */
	if (val == -1)
		return UINT32_MAX;

	if (val > UINT32_MAX / multiplier || val < 0) {
		LOG_ERROR(-EINVAL, "%s was set to %s, which would overflow -- ignoring",
			  error_hint, str);
		return -1;
	}
	return val * multiplier;
}

static void getenv_u32(const char *name, uint32_t *val) {
	const char *env = getenv(name);
	if (!env)
		return;
	
	long long envval = str_suffix_to_u32(env, name);
	if (envval >= 0)
		*val = envval;
}

static int str_to_verbose(const char *str) {
	if (!strcasecmp(str, "off")) {
		return LLAPI_MSG_OFF;
	}
	if (!strcasecmp(str, "fatal")) {
		return LLAPI_MSG_FATAL;
	}
	if (!strcasecmp(str, "error")) {
		return LLAPI_MSG_ERROR;
	}
	if (!strcasecmp(str, "warn")) {
		return LLAPI_MSG_WARN;
	}
	if (!strcasecmp(str, "normal")) {
		return LLAPI_MSG_NORMAL;
	}
	if (!strcasecmp(str, "info")) {
		return LLAPI_MSG_INFO;
	}
	if (!strcasecmp(str, "debug")) {
		return LLAPI_MSG_DEBUG;
	}
	LOG_ERROR(-EINVAL, "invalid debug level: %s", str);
	return -1;
}

static void getenv_verbose(const char *name, int *val) {
	const char *env = getenv(name);
	if (!env)
		return;

	int verbose = str_to_verbose(env);
	if (verbose >= 0)
		*val = verbose;
}


static int config_parse(const char *confpath, struct ct_state_config *config, int fail_enoent) {
	int rc = 0;
	FILE *conffile = fopen(confpath, "r");
	if (!conffile) {
		if (errno == ENOENT && !fail_enoent) {
			LOG_INFO("Config file %s not found, skipping",
				 confpath);
			return 0;
		}
		rc = -errno;
		LOG_ERROR(rc, "Could not open config file %s, aborting",
			  confpath);
		return rc;
	}

	char *line = NULL;
	size_t line_size = 0;
	ssize_t n, linenum = 0;
	while ((n = getline(&line, &line_size, conffile)) >= 0) {
		linenum++;
		LOG_DEBUG("Read line %zd: %s", linenum, line);
		char *key = line, *val;
		while (n > 0 && isspace(*key)) {
			key++; n--;
		}
		if (n == 0) /* blank line */
			continue;
		if (*key == '#') /* comment */
			continue;

		/* trailing spaces */
		while (n > 0 && isspace(key[n-1])) {
			key[n-1] = 0;
			n--;
		}
		if (n == 0) // should never happen
			abort();

		ssize_t i = 0;
		while (i < n - 1 && !isspace(key[i])) {
			i++;
		}
		if (i >= n - 1) {
			rc = -EINVAL;
			LOG_ERROR(rc, "%s in %s (line %zd) not in 'key value' format",
				  line, confpath, linenum);
			goto out;
		}
		key[i] = 0;
		i++;
		val = key + i;
		n -= i;
		while (n > 0 && isspace(*val)) {
			val++; n--;
		}
		if (n == 0) {
			rc = -EINVAL;
			LOG_ERROR(rc, "%s in %s (line %zd) not in 'key value' format",
				  line, confpath, linenum);
			goto out;
		}

		if (!strcasecmp(key, "host")) {
			config->host = strdup(val);
			continue;
		}
		if (!strcasecmp(key, "port")) {
			config->port = strdup(val);
			continue;
		}
		if (!strcasecmp(key, "max_restore")) {
			long long intval = str_suffix_to_u32(val, "max_restore");
			if (intval >= 0)
				config->max_restore = intval;
			continue;
		}
		if (!strcasecmp(key, "max_archive")) {
			long long intval = str_suffix_to_u32(val, "max_archive");
			if (intval >= 0)
				config->max_archive = intval;
			continue;
		}
		if (!strcasecmp(key, "max_remove")) {
			long long intval = str_suffix_to_u32(val, "max_remove");
			if (intval >= 0)
				config->max_remove = intval;
			continue;
		}
		if (!strcasecmp(key, "hal_size")) {
			long long intval = str_suffix_to_u32(val, "hal_size");
			if (intval >= 0)
				config->hsm_action_list_size = intval;
			continue;
		}
		if (!strcasecmp(key, "archive_id")) {
			long long intval = str_suffix_to_u32(val, "archive_id");
			if (intval >= 0)
				config->archive_id = intval;
			continue;
		}
		if (!strcasecmp(key, "verbose")) {
			int intval = str_to_verbose(val);
			if (intval >= 0)
				config->verbose = intval;
			continue;
		}

		rc = -EINVAL;
		LOG_ERROR(rc, "unknown key %s in %s (line %zd)",
			  key, confpath, linenum);
		goto out;

	}
	// XXX check error

out:
	free(line);
	(void)fclose(conffile);
	return rc;

}


int ct_config_init(struct ct_state_config *config) {
	const char *confpath = "/etc/coordinatool.conf";
	int rc;
	
	/* first set defaults */
	config->host = "coordinatool";
	config->port = "5123";
	config->max_restore = -1;
	config->max_archive = -1;
	config->max_remove = -1;
	config->hsm_action_list_size = 1024 * 1024;
	config->archive_id = 0;
	config->verbose = LLAPI_MSG_INFO;
	/* verbose from env once first to debug config.. */
	getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);

	/* then parse config */
	int fail_enoent = getenv_str("COORDINATOOL_CONF", &confpath);
	rc = config_parse(confpath, config, fail_enoent);
	if (rc)
		return rc;

	/* then overwrite with env */
	getenv_str("COORDINATOOL_HOST", &config->host);
	getenv_str("COORDINATOOL_PORT", &config->port);
	getenv_u32("COORDINATOOL_MAX_RESTORE", &config->max_restore);
	getenv_u32("COORDINATOOL_MAX_ARCHIVE", &config->max_archive);
	getenv_u32("COORDINATOOL_MAX_REMOVE", &config->max_remove);
	getenv_u32("COORDINATOOL_HAL_SIZE", &config->hsm_action_list_size);
	getenv_u32("COORDINATOOL_ARCHIVE_ID", &config->archive_id);
	getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);

	return 0;
}
