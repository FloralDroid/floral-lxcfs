/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#define FLORAL_PROFILE_MAX_FIELDS 256

static const char *const required_fields[] = {
	"version",
	"brand",
	"manufacturer",
	"model",
	"device",
	"product",
	"board",
	"soc_manufacturer",
	"soc_model",
	"gpu_vendor",
	"gpu_model",
	"build_id",
	"build_display",
	"version_release",
	"security_patch",
};

static const char *const sensor_prefixes[] = {
	"sensor_accelerometer",
	"sensor_accelerometer_uncalibrated",
	"sensor_gyroscope",
	"sensor_gyroscope_uncalibrated",
	"sensor_magnetic_field",
	"sensor_magnetic_field_uncalibrated",
	"sensor_light",
	"sensor_proximity",
	"sensor_pressure",
	"sensor_ambient_temperature",
	"sensor_gravity",
	"sensor_linear_acceleration",
	"sensor_rotation_vector",
	"sensor_game_rotation_vector",
	"sensor_step_detector",
	"sensor_step_counter",
	"sensor_significant_motion",
	"sensor_virtual_corrected_gyroscope",
	"sensor_virtual_game_rotation_vector",
	"sensor_virtual_gyroscope_bias",
	"sensor_virtual_geomagnetic_rotation_vector",
	"sensor_virtual_gravity",
	"sensor_virtual_linear_acceleration",
	"sensor_virtual_rotation_vector",
	"sensor_virtual_orientation",
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

static bool profile_text_is_valid(const char *value, size_t maximum_length)
{
	return value && value[0] && strlen(value) <= maximum_length &&
	       profile_value_is_safe(value);
}

static bool profile_identifier_is_valid(const char *value)
{
	if (!profile_text_is_valid(value, 64) ||
	    !isalnum((unsigned char)value[0]))
		return false;

	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
			return false;
	}
	return true;
}

static bool security_patch_is_valid(const char *value)
{
	static const int days[] = { 31, 28, 31, 30, 31, 30,
				    31, 31, 30, 31, 30, 31 };
	int year, month, day, maximum_day;

	if (!value || strlen(value) != 10 || value[4] != '-' || value[7] != '-')
		return false;
	for (size_t i = 0; i < 10; i++) {
		if (i != 4 && i != 7 && !isdigit((unsigned char)value[i]))
			return false;
	}
	year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
	       (value[2] - '0') * 10 + value[3] - '0';
	month = (value[5] - '0') * 10 + value[6] - '0';
	day = (value[8] - '0') * 10 + value[9] - '0';
	if (year < 1970 || month < 1 || month > 12)
		return false;
	maximum_day = days[month - 1];
	if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
		maximum_day++;
	return day >= 1 && day <= maximum_day;
}

static bool profile_key_is_valid(const char *key)
{
	size_t length;

	if (!key || key[0] < 'a' || key[0] > 'z')
		return false;
	length = strlen(key);
	if (!length || length > 64)
		return false;
	for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
		if (!islower(*p) && !isdigit(*p) && *p != '_')
			return false;
	}
	return true;
}

static bool key_was_seen(const char *const *seen_keys, size_t seen_count,
			 const char *key)
{
	for (size_t i = 0; i < seen_count; i++) {
		if (strcmp(seen_keys[i], key) == 0)
			return true;
	}
	return false;
}

static const char *profile_value(const char *const *seen_keys,
				 const char *const *seen_values,
				 size_t seen_count, const char *key)
{
	for (size_t i = 0; i < seen_count; i++) {
		if (strcmp(seen_keys[i], key) == 0)
			return seen_values[i];
	}
	return NULL;
}

static bool profile_schema_is_valid(const char *const *seen_keys,
				    const char *const *seen_values,
				    size_t seen_count)
{
	static const char *const identifier_fields[] = {
		"device", "product", "board", "build_id",
	};
	static const char *const identity_fields[] = {
		"brand", "manufacturer", "model", "soc_manufacturer",
		"soc_model", "gpu_vendor", "gpu_model",
	};
	static const char *const optional_identity_fields[] = {
		"build_description", "build_flavor", "build_incremental",
		"build_type", "build_tags", "build_user", "build_host",
		"build_date", "kernel_release", "kernel_version", "memory_type",
		"memory_frequency", "memory_channel", "serial", "hardware_revision",
	};
	static const char *const thermal_name_fields[] = {
		"thermal_cpu_name", "thermal_gpu_name", "thermal_battery_name",
		"thermal_skin_name",
	};

	for (size_t i = 0; i < sizeof(required_fields) / sizeof(required_fields[0]); i++) {
		if (!profile_value(seen_keys, seen_values, seen_count, required_fields[i]))
			return false;
	}
	for (size_t i = 0; i < sizeof(identifier_fields) / sizeof(identifier_fields[0]); i++) {
		if (!profile_identifier_is_valid(profile_value(
			    seen_keys, seen_values, seen_count, identifier_fields[i])))
			return false;
	}
	for (size_t i = 0; i < sizeof(identity_fields) / sizeof(identity_fields[0]); i++) {
		if (!profile_text_is_valid(profile_value(
			    seen_keys, seen_values, seen_count, identity_fields[i]), 64))
			return false;
	}
	if (!profile_text_is_valid(profile_value(seen_keys, seen_values, seen_count,
						    "build_display"), 91) ||
	    !profile_text_is_valid(profile_value(seen_keys, seen_values, seen_count,
						    "version_release"), 64) ||
	    !security_patch_is_valid(profile_value(seen_keys, seen_values, seen_count,
						      "security_patch")))
		return false;
	for (size_t i = 0; i < sizeof(optional_identity_fields) /
					 sizeof(optional_identity_fields[0]); i++) {
		const char *value = profile_value(seen_keys, seen_values, seen_count,
					  optional_identity_fields[i]);
		if (value && !profile_text_is_valid(value, 91))
			return false;
	}
	for (size_t i = 0; i < sizeof(thermal_name_fields) /
					 sizeof(thermal_name_fields[0]); i++) {
		const char *value = profile_value(seen_keys, seen_values, seen_count,
					  thermal_name_fields[i]);
		if (value && !profile_text_is_valid(value, 64))
			return false;
	}
	return true;
}

static bool sensor_pairs_complete(const char *const *seen_keys, size_t seen_count)
{
	char name[96], vendor[96];

	for (size_t i = 0; i < sizeof(sensor_prefixes) / sizeof(sensor_prefixes[0]); i++) {
		int name_length = snprintf(name, sizeof(name), "%s_name", sensor_prefixes[i]);
		int vendor_length = snprintf(vendor, sizeof(vendor), "%s_vendor", sensor_prefixes[i]);

		if (name_length < 0 || (size_t)name_length >= sizeof(name) ||
		    vendor_length < 0 || (size_t)vendor_length >= sizeof(vendor) ||
		    key_was_seen(seen_keys, seen_count, name) !=
			    key_was_seen(seen_keys, seen_count, vendor))
			return false;
	}
	return true;
}

static bool sensor_identities_are_valid(const char *const *seen_keys,
					const char *const *seen_values,
					size_t seen_count)
{
	char name[96], vendor[96];

	for (size_t i = 0; i < sizeof(sensor_prefixes) / sizeof(sensor_prefixes[0]); i++) {
		snprintf(name, sizeof(name), "%s_name", sensor_prefixes[i]);
		snprintf(vendor, sizeof(vendor), "%s_vendor", sensor_prefixes[i]);
		const char *name_value = profile_value(seen_keys, seen_values, seen_count, name);
		const char *vendor_value = profile_value(seen_keys, seen_values, seen_count, vendor);

		if (name_value && (!profile_text_is_valid(name_value, 96) ||
				   !profile_text_is_valid(vendor_value, 64)))
			return false;
	}
	return true;
}

static bool optional_pair_complete(const char *const *seen_keys, size_t seen_count,
				   const char *first, const char *second)
{
	return key_was_seen(seen_keys, seen_count, first) ==
	       key_was_seen(seen_keys, seen_count, second);
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

static int parse_ambient_temperature(const char *value, float *temperature)
{
	char *end = NULL;
	float parsed;

	errno = 0;
	parsed = strtof(value, &end);
	if (errno || !end || *end || !isfinite(parsed) || parsed < -20.0f ||
	    parsed > 50.0f)
		return -EINVAL;
	*temperature = parsed;
	return 0;
}

static bool valid_unsigned_decimal(const char *value)
{
	if (!value[0])
		return false;
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (!isdigit(*p))
			return false;
	}
	return true;
}

void floral_profile_set_default_identity(struct floral_cpu_profile *profile)
{
	if (!profile)
		return;

	memset(&profile->dmi, 0, sizeof(profile->dmi));
	strlcpy(profile->dmi.manufacturer, "FloralDroid",
		sizeof(profile->dmi.manufacturer));
	strlcpy(profile->dmi.model, "Floral F12", sizeof(profile->dmi.model));
	strlcpy(profile->dmi.board, "floral_f12", sizeof(profile->dmi.board));
	/* Serial and revision remain absent unless the per-container profile sets them. */
	profile->thermal_ambient_celsius = 22.0f;
	strlcpy(profile->thermal_battery_name, "battery",
		sizeof(profile->thermal_battery_name));
}

int floral_profile_parse(char *data, struct floral_cpu_profile *profile)
{
	bool version_seen = false;
	char *line;
	const char *seen_keys[FLORAL_PROFILE_MAX_FIELDS];
	const char *seen_values[FLORAL_PROFILE_MAX_FIELDS];
	size_t seen_count = 0;
	struct floral_profile_field fields[] = {
		{ "cpu_vendor", profile->cpu_vendor, sizeof(profile->cpu_vendor), false },
		{ "cpu_model", profile->cpu_model, sizeof(profile->cpu_model), false },
		{ "cpu_features", profile->cpu_features, sizeof(profile->cpu_features), false },
		{ "cpu_feature_view", profile->cpu_feature_view, sizeof(profile->cpu_feature_view), false },
		{ "cpu_cores", profile->cpu_cores, sizeof(profile->cpu_cores), false },
		{ "soc_model", profile->soc_model, sizeof(profile->soc_model), false },
		{ "kernel_release", profile->kernel_release, sizeof(profile->kernel_release), false },
		{ "kernel_version", profile->kernel_version, sizeof(profile->kernel_version), false },
		{ "thermal_battery_name", profile->thermal_battery_name,
		  sizeof(profile->thermal_battery_name), false },
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
	floral_profile_set_default_identity(profile);

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
		if (!profile_key_is_valid(key) || !value[0] || strlen(value) > 256 ||
		    !profile_value_is_safe(value) ||
		    seen_count == FLORAL_PROFILE_MAX_FIELDS ||
		    key_was_seen(seen_keys, seen_count, key))
			return -EINVAL;
		seen_keys[seen_count] = key;
		seen_values[seen_count++] = value;

		if (strcmp(key, "version") == 0) {
			if (parse_profile_version(value, &profile->version))
				return -EINVAL;
			version_seen = true;
			continue;
		}
		if (strcmp(key, "thermal_ambient_celsius") == 0) {
			if (parse_ambient_temperature(value,
						      &profile->thermal_ambient_celsius))
				return -EINVAL;
			continue;
		}
		if (strcmp(key, "build_date_utc") == 0 &&
		    !valid_unsigned_decimal(value))
			return -EINVAL;

		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			if (strcmp(key, fields[i].name) != 0)
				continue;
			if (fields[i].seen ||
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

	if (!version_seen || !profile_schema_is_valid(seen_keys, seen_values, seen_count) ||
	    !sensor_pairs_complete(seen_keys, seen_count) ||
	    !sensor_identities_are_valid(seen_keys, seen_values, seen_count) ||
	    !optional_pair_complete(seen_keys, seen_count,
				    "build_date", "build_date_utc") ||
	    !valid_cpu_count(profile->cpu_cores))
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
