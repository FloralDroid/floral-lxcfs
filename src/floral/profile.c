/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "profile.h"

#include "../bindings.h"
#include "../memory_utils.h"
#include "../utils.h"

struct floral_profile_field {
	const char *name;
	char *value;
	size_t size;
	bool seen;
};

static char *trim_profile_value(char *value)
{
	char *end;

	while (isspace((unsigned char)*value))
		value++;

	end = value + strlen(value);
	while (end > value && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';

	return value;
}

static bool profile_value_is_safe(const char *value)
{
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (iscntrl(*p))
			return false;
	}

	return true;
}

static int parse_profile_version(const char *value, unsigned int *version)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !end || *end || parsed != 1)
		return -EINVAL;

	*version = (unsigned int)parsed;
	return 0;
}

static bool valid_cpu_count(const char *value)
{
	char *end = NULL;
	unsigned long count;

	if (!value[0] || strcmp(value, "auto") == 0)
		return true;

	errno = 0;
	count = strtoul(value, &end, 10);
	return !errno && end && !*end && count >= 1 && count <= 256;
}

void floral_profile_set_default_dmi_identity(struct floral_cpu_profile *profile)
{
	if (!profile)
		return;

	memset(&profile->dmi, 0, sizeof(profile->dmi));
	strlcpy(profile->dmi.manufacturer, "FloralDroid",
		sizeof(profile->dmi.manufacturer));
	strlcpy(profile->dmi.model, "Floral F12", sizeof(profile->dmi.model));
	strlcpy(profile->dmi.board, "floral_f12", sizeof(profile->dmi.board));
	/* Serial and revision remain absent unless the per-container profile sets them. */
}

int floral_profile_parse(char *data, struct floral_cpu_profile *profile)
{
	bool version_seen = false;
	char *line;
	struct floral_profile_field fields[] = {
		{ "cpu_vendor", profile->cpu_vendor, sizeof(profile->cpu_vendor), false },
		{ "cpu_model", profile->cpu_model, sizeof(profile->cpu_model), false },
		{ "cpu_features", profile->cpu_features, sizeof(profile->cpu_features), false },
		{ "cpu_feature_view", profile->cpu_feature_view, sizeof(profile->cpu_feature_view), false },
		{ "cpu_cores", profile->cpu_cores, sizeof(profile->cpu_cores), false },
		{ "soc_model", profile->soc_model, sizeof(profile->soc_model), false },
		{ "kernel_release", profile->kernel_release, sizeof(profile->kernel_release), false },
		{ "kernel_version", profile->kernel_version, sizeof(profile->kernel_version), false },
		{ "manufacturer", profile->dmi.manufacturer, sizeof(profile->dmi.manufacturer), false },
		{ "model", profile->dmi.model, sizeof(profile->dmi.model), false },
		{ "board", profile->dmi.board, sizeof(profile->dmi.board), false },
		{ "serial", profile->dmi.serial, sizeof(profile->dmi.serial), false },
		{ "hardware_revision", profile->dmi.hardware_revision,
		  sizeof(profile->dmi.hardware_revision), false },
	};

	if (!data || !profile)
		return -EINVAL;

	memset(profile, 0, sizeof(*profile));
	/* DMI defaults mask host identity even when optional keys are omitted. */
	floral_profile_set_default_dmi_identity(profile);

	while ((line = strsep(&data, "\n"))) {
		char *equals, *key, *value;
		bool known = false;

		line = trim_profile_value(line);
		if (!line[0] || line[0] == '#')
			continue;

		equals = strchr(line, '=');
		if (!equals)
			return -EINVAL;
		*equals = '\0';
		key = trim_profile_value(line);
		value = trim_profile_value(equals + 1);
		if (!key[0] || !profile_value_is_safe(key) || !profile_value_is_safe(value))
			return -EINVAL;

		if (strcmp(key, "version") == 0) {
			if (version_seen || parse_profile_version(value, &profile->version))
				return -EINVAL;
			version_seen = true;
			continue;
		}

		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			if (strcmp(key, fields[i].name) != 0)
				continue;
			if (!value[0] || fields[i].seen ||
			    strlcpy(fields[i].value, value, fields[i].size) >= fields[i].size)
				return -EINVAL;
			fields[i].seen = true;
			known = true;
			break;
		}

		/* The same file is consumed by AOSP, so unrelated device fields are expected. */
		if (!known)
			continue;
	}

	if (!version_seen || !valid_cpu_count(profile->cpu_cores))
		return -EINVAL;

	return 0;
}

static int open_profile_beneath_root(int rootfd, const char *path)
{
	__do_close int current = -EBADF;
	const char *cursor;

	if (!path || path[0] != '/' || strstr(path, "//"))
		return -EINVAL;

	current = fcntl(rootfd, F_DUPFD_CLOEXEC, 3);
	if (current < 0)
		return -errno;

	cursor = path + 1;
	while (*cursor) {
		__do_close int next = -EBADF;
		char component[NAME_MAX + 1];
		const char *slash = strchr(cursor, '/');
		size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
		bool last = !slash;

		if (!length || length > NAME_MAX)
			return -EINVAL;
		memcpy(component, cursor, length);
		component[length] = '\0';
		if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0)
			return -EINVAL;

		if (last)
			return openat(current, component, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

		next = openat(current, component,
			      O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0)
			return -errno;
		close_prot_errno_disarm(current);
		current = move_fd(next);
		cursor = slash + 1;
	}

	return -EINVAL;
}

int floral_profile_load(pid_t initpid, const struct lxcfs_opts *opts,
			struct floral_cpu_profile *profile)
{
	__do_close int rootfd = -EBADF, fd = -EBADF;
	char proc_root[64];
	char data[FLORAL_PROFILE_MAX_SIZE + 1];
	struct stat st;
	ssize_t total = 0;
	int ret;

	if (!profile || !lxcfs_has_floral_profile(opts) || initpid <= 0)
		return -ENOENT;

	ret = snprintf(proc_root, sizeof(proc_root), "/proc/%d/root", initpid);
	if (ret < 0 || (size_t)ret >= sizeof(proc_root))
		return -EINVAL;

	rootfd = open(proc_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (rootfd < 0)
		return -errno;

	fd = open_profile_beneath_root(rootfd, opts->floral_profile_path);
	if (fd < 0)
		return fd;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > FLORAL_PROFILE_MAX_SIZE)
		return -EINVAL;

	while ((size_t)total < FLORAL_PROFILE_MAX_SIZE) {
		ssize_t count = read(fd, data + total, FLORAL_PROFILE_MAX_SIZE - total);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!count)
			break;
		total += count;
	}
	if ((size_t)total == FLORAL_PROFILE_MAX_SIZE) {
		char extra;
		ssize_t count;

		do {
			count = read(fd, &extra, 1);
		} while (count < 0 && errno == EINTR);
		if (count != 0)
			return -E2BIG;
	}
	if (memchr(data, '\0', total))
		return -EINVAL;
	data[total] = '\0';

	return floral_profile_parse(data, profile);
}

bool floral_profile_has_cpu_identity(const struct floral_cpu_profile *profile)
{
	if (!profile || profile->version != 1)
		return false;

	return profile->cpu_vendor[0] || profile->cpu_model[0] ||
	       profile->cpu_features[0] || profile->cpu_feature_view[0] ||
	       profile->soc_model[0];
}

bool floral_profile_has_kernel_identity(const struct floral_cpu_profile *profile)
{
	if (!profile || profile->version != 1)
		return false;

	return profile->kernel_release[0] || profile->kernel_version[0];
}

bool floral_profile_has_dmi_identity(const struct floral_cpu_profile *profile)
{
	if (!profile || profile->version != 1)
		return false;

	return profile->dmi.manufacturer[0] && profile->dmi.model[0] &&
	       profile->dmi.board[0];
}
