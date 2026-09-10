/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "view.h"

#include "../cpuset_parse.h"

struct floral_cpu_core {
	unsigned int part;
	unsigned int max_frequency_khz;
};

static const struct floral_cpu_core sm8550_cores[] = {
	{ 0xd46, 2000000 },
	{ 0xd46, 2000000 },
	{ 0xd46, 2000000 },
	{ 0xd47, 2800000 },
	{ 0xd47, 2800000 },
	{ 0xd4d, 2800000 },
	{ 0xd4d, 2800000 },
	{ 0xd4e, 3200000 },
};

#define FLORAL_CPU_SYS_PREFIX "/sys/devices/system/cpu"
#define FLORAL_NODE_SYS_PREFIX "/sys/devices/system/node"
#define FLORAL_BLOCK_SYS_PREFIX "/sys/block"
#define FLORAL_VIRTUAL_SYS_PREFIX "/sys/devices/virtual"
#define FLORAL_DMI_SYS_PREFIX FLORAL_VIRTUAL_SYS_PREFIX "/dmi"
#define FLORAL_DMI_ID_SYS_PREFIX FLORAL_DMI_SYS_PREFIX "/id"
#define FLORAL_THERMAL_SYS_PREFIX FLORAL_VIRTUAL_SYS_PREFIX "/thermal"
#define FLORAL_CLASS_THERMAL_SYS_PREFIX "/sys/class/thermal"
#define FLORAL_CLASS_HWMON_SYS_PREFIX "/sys/class/hwmon"

static const char *const cpu_root_files[] = {
	"online",
	"present",
	"possible",
	"offline",
	"isolated",
	"kernel_max",
	"uevent",
};

static const char *const cpu_files[] = {
	"online",
	"uevent",
	"cpu_capacity",
};

static const char *const topology_files[] = {
	"cluster_id",
	"core_id",
	"core_cpus",
	"core_cpus_list",
	"core_siblings",
	"core_siblings_list",
	"die_id",
	"package_cpus",
	"package_cpus_list",
	"physical_package_id",
	"thread_siblings",
	"thread_siblings_list",
};

static const char *const cpufreq_files[] = {
	"affected_cpus",
	"cpuinfo_cur_freq",
	"cpuinfo_max_freq",
	"cpuinfo_min_freq",
	"related_cpus",
	"scaling_available_governors",
	"scaling_cur_freq",
	"scaling_driver",
	"scaling_governor",
	"scaling_max_freq",
	"scaling_min_freq",
};

static const char *const cache_files[] = {
	"allocation_policy",
	"coherency_line_size",
	"id",
	"level",
	"number_of_sets",
	"physical_line_partition",
	"shared_cpu_list",
	"shared_cpu_map",
	"size",
	"type",
	"ways_of_associativity",
	"write_policy",
};

static const char *const node_root_files[] = {
	"has_cpu",
	"has_memory",
	"online",
	"possible",
};

static const char *const node_files[] = {
	"cpulist",
	"cpumap",
	"distance",
	"meminfo",
	"numastat",
	"uevent",
};

static const char *const block_files[] = {
	"dev",
	"disksize",
	"size",
	"ro",
	"removable",
	"initstate",
	"mem_used_total",
	"orig_data_size",
	"compr_data_size",
	"mm_stat",
	"stat",
	"uevent",
	"comp_algorithm",
	"writeback",
};

static const char *const dmi_files[] = {
	"sys_vendor",
	"product_name",
	"product_version",
	"product_serial",
	"board_vendor",
	"board_name",
	"board_version",
	"board_serial",
};

static const char *const thermal_files[] = {
	"available_policies",
	"integral_cutoff",
	"k_d",
	"k_i",
	"k_po",
	"k_pu",
	"offset",
	"passive",
	"policy",
	"slope",
	"sustainable_power",
	"temp",
	"type",
	"uevent",
};

static const char *const thermal_power_files[] = {
	"autosuspend_delay_ms",
	"control",
	"runtime_active_time",
	"runtime_status",
	"runtime_suspended_time",
};

static bool profile_is_sm8550(const struct floral_cpu_profile *profile)
{
	return strcasecmp(profile->cpu_feature_view, "sm8550") == 0 ||
	       strcasecmp(profile->soc_model, "sm8550") == 0;
}

static const struct floral_cpu_core *profile_core(const struct floral_cpu_profile *profile,
						  int cpu)
{
	if (profile_is_sm8550(profile))
		return &sm8550_cores[cpu % (int)(sizeof(sm8550_cores) / sizeof(sm8550_cores[0]))];

	return NULL;
}

static int cpu_cluster(int cpu)
{
	if (cpu <= 2)
		return 0;
	if (cpu <= 4)
		return 1;
	if (cpu <= 6)
		return 2;
	return 3;
}

static void cluster_cpu_range(int cpu, int cpu_count, int *first, int *last)
{
	static const int cluster_first[] = { 0, 3, 5, 7 };
	static const int cluster_last[] = { 2, 4, 6, 7 };
	int cluster = cpu_cluster(cpu);

	*first = cluster_first[cluster];
	*last = cluster_last[cluster];
	if (*last >= cpu_count)
		*last = cpu_count - 1;
	if (*first > *last)
		*first = *last;
}

static int profile_cpu_limit(const struct floral_cpu_profile *profile)
{
	char *end = NULL;
	long parsed;

	if (!profile->cpu_cores[0] || strcmp(profile->cpu_cores, "auto") == 0)
		return 0;

	errno = 0;
	parsed = strtol(profile->cpu_cores, &end, 10);
	if (errno || !end || *end || parsed < 1 || parsed > 256)
		return 0;

	return (int)parsed;
}

int floral_visible_cpu_count(const struct floral_cpu_profile *profile,
			     const char *cpuset, int cfs_count)
{
	int available = cpuset ? cpu_number_in_cpuset(cpuset) : 0;
	int configured = profile_cpu_limit(profile);
	int template_limit = profile_is_sm8550(profile) ?
					   (int)(sizeof(sm8550_cores) / sizeof(sm8550_cores[0])) :
					   0;
	int visible;

	if (available <= 0)
		available = cfs_count;
	if (cfs_count > 0 && (available <= 0 || cfs_count < available))
		available = cfs_count;

	visible = available > 0 ? available : configured;
	if (configured > 0 && (visible <= 0 || configured < visible))
		visible = configured;
	if (template_limit > 0 && (visible <= 0 || template_limit < visible))
		visible = template_limit;

	return visible > 0 ? visible : 1;
}

static int append_format(char **cursor, size_t *remaining, const char *format, ...)
{
	va_list arguments;
	int count;

	va_start(arguments, format);
	count = vsnprintf(*cursor, *remaining, format, arguments);
	va_end(arguments);
	if (count < 0 || (size_t)count >= *remaining)
		return -ENOSPC;

	*cursor += count;
	*remaining -= count;
	return count;
}

static int append_features(char **cursor, size_t *remaining, const char *features)
{
	const char *source = features[0] ? features :
						 "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp "
					   "cpuid asimdrdm lrcpc dcpop asimddp";
	bool separator = false;

	for (; *source; source++) {
		char output = *source;

		if (output == ',' || output == ' ' || output == '\t') {
			if (separator)
				continue;
			output = ' ';
			separator = true;
		} else {
			separator = false;
		}
		if (*remaining <= 1)
			return -ENOSPC;
		*(*cursor)++ = output;
		(*remaining)--;
	}
	if (*remaining <= 1)
		return -ENOSPC;
	*(*cursor)++ = '\n';
	(*remaining)--;
	**cursor = '\0';
	return 0;
}

ssize_t floral_render_cpuinfo(const struct floral_cpu_profile *profile,
			      int cpu_count, char *buffer, size_t size)
{
	const char *model = profile->cpu_model[0] ? profile->cpu_model :
							  "ARMv8 Processor rev 1 (v8l)";
	const char *hardware = profile->soc_model[0] ? profile->soc_model :
							     (profile->cpu_vendor[0] ? profile->cpu_vendor : "Generic ARM64");
	char *cursor = buffer;
	size_t remaining = size;

	if (!profile || !buffer || !size || cpu_count < 1)
		return -EINVAL;

	for (int cpu = 0; cpu < cpu_count; cpu++) {
		const struct floral_cpu_core *core = profile_core(profile, cpu);
		unsigned int part = core ? core->part : 0xd0f;

		if (append_format(&cursor, &remaining,
				  "processor\t: %d\n"
				  "model name\t: %s\n"
				  "BogoMIPS\t: 38.40\n"
				  "Features\t: ",
				  cpu, model) < 0)
			return -ENOSPC;
		if (append_features(&cursor, &remaining, profile->cpu_features) < 0)
			return -ENOSPC;
		if (append_format(&cursor, &remaining,
				  "CPU implementer\t: 0x41\n"
				  "CPU architecture: 8\n"
				  "CPU variant\t: 0x0\n"
				  "CPU part\t: 0x%03x\n"
				  "CPU revision\t: 1\n\n",
				  part) < 0)
			return -ENOSPC;
	}

	if (append_format(&cursor, &remaining, "Hardware\t: %s\n", hardware) < 0)
		return -ENOSPC;

	return cursor - buffer;
}

ssize_t floral_render_kernel_identity(const struct floral_cpu_profile *profile,
				      const char *path, char *buffer, size_t size)
{
	const char *release = profile && profile->kernel_release[0] ?
					    profile->kernel_release :
					    NULL;
	const char *version = profile && profile->kernel_version[0] ?
					    profile->kernel_version :
					    NULL;

	if (!profile || !buffer || !size || !floral_profile_has_kernel_identity(profile))
		return -ENOENT;
	if (strcmp(path, "/proc/sys/kernel/osrelease") == 0 && release)
		return snprintf(buffer, size, "%s\n", release);
	if (strcmp(path, "/proc/version") == 0 && release) {
		if (version)
			return snprintf(buffer, size, "Linux version %s %s\n", release, version);
		return snprintf(buffer, size, "Linux version %s\n", release);
	}
	return -ENOENT;
}

static bool string_in_array(const char *value, const char *const *array, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(value, array[i]) == 0)
			return true;
	}

	return false;
}

bool floral_sys_manages_path(const char *path)
{
	if (!path)
		return false;

	return strcmp(path, FLORAL_CPU_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_CPU_SYS_PREFIX "/",
		       strlen(FLORAL_CPU_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_NODE_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_NODE_SYS_PREFIX "/",
		       strlen(FLORAL_NODE_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_BLOCK_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_BLOCK_SYS_PREFIX "/",
		       strlen(FLORAL_BLOCK_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_VIRTUAL_SYS_PREFIX) == 0 ||
	       strcmp(path, FLORAL_DMI_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_DMI_SYS_PREFIX "/",
		       strlen(FLORAL_DMI_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_THERMAL_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_THERMAL_SYS_PREFIX "/",
		       strlen(FLORAL_THERMAL_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX "/",
		       strlen(FLORAL_CLASS_THERMAL_SYS_PREFIX "/")) == 0 ||
	       strcmp(path, FLORAL_CLASS_HWMON_SYS_PREFIX) == 0 ||
	       strncmp(path, FLORAL_CLASS_HWMON_SYS_PREFIX "/",
		       strlen(FLORAL_CLASS_HWMON_SYS_PREFIX "/")) == 0;
}

static enum floral_sys_node_type thermal_device_node_type(const char *path)
{
	const char *suffix;

	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(path, FLORAL_THERMAL_SYS_PREFIX "/",
		    strlen(FLORAL_THERMAL_SYS_PREFIX "/")) != 0)
		return FLORAL_SYS_NONE;
	suffix = path + strlen(FLORAL_THERMAL_SYS_PREFIX "/");
	if (strcmp(suffix, "thermal_zone0") == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(suffix, "thermal_zone0/", strlen("thermal_zone0/")) != 0)
		return FLORAL_SYS_NONE;

	suffix += strlen("thermal_zone0/");
	if (strcmp(suffix, "power") == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strcmp(suffix, "subsystem") == 0)
		return FLORAL_SYS_SYMLINK;
	if (string_in_array(suffix, thermal_files,
			    sizeof(thermal_files) / sizeof(thermal_files[0])))
		return FLORAL_SYS_FILE;
	if (strncmp(suffix, "power/", strlen("power/")) == 0 &&
	    string_in_array(suffix + strlen("power/"), thermal_power_files,
			    sizeof(thermal_power_files) /
			    sizeof(thermal_power_files[0])))
		return FLORAL_SYS_FILE;
	return FLORAL_SYS_NONE;
}

static enum floral_sys_node_type thermal_class_node_type(const char *path)
{
	if (strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX "/thermal_zone0") == 0)
		return FLORAL_SYS_SYMLINK;
	return FLORAL_SYS_NONE;
}

static const char *dmi_file_value(const struct floral_cpu_profile *profile,
				  const char *file)
{
	if (strcmp(file, "sys_vendor") == 0 || strcmp(file, "board_vendor") == 0)
		return profile->dmi.manufacturer;
	if (strcmp(file, "product_name") == 0)
		return profile->dmi.model;
	if (strcmp(file, "board_name") == 0)
		return profile->dmi.board;
	if (strcmp(file, "product_serial") == 0 || strcmp(file, "board_serial") == 0)
		return profile->dmi.serial[0] ? profile->dmi.serial : NULL;
	if (strcmp(file, "product_version") == 0 || strcmp(file, "board_version") == 0)
		return profile->dmi.hardware_revision[0] ?
		       profile->dmi.hardware_revision : NULL;

	return NULL;
}

static enum floral_sys_node_type dmi_sys_node_type(
					const struct floral_cpu_profile *profile,
					const char *path)
{
	const char *file;

	if (!floral_profile_has_dmi_identity(profile))
		return FLORAL_SYS_NONE;
	if (strcmp(path, FLORAL_VIRTUAL_SYS_PREFIX) == 0 ||
	    strcmp(path, FLORAL_DMI_SYS_PREFIX) == 0 ||
	    strcmp(path, FLORAL_DMI_ID_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(path, FLORAL_DMI_ID_SYS_PREFIX "/",
		    strlen(FLORAL_DMI_ID_SYS_PREFIX "/")) != 0)
		return FLORAL_SYS_NONE;

	file = path + strlen(FLORAL_DMI_ID_SYS_PREFIX "/");
	return dmi_file_value(profile, file) ? FLORAL_SYS_FILE : FLORAL_SYS_NONE;
}

static enum floral_sys_node_type block_sys_node_type(const char *path)
{
	const char *suffix;

	if (strcmp(path, FLORAL_BLOCK_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(path, FLORAL_BLOCK_SYS_PREFIX "/",
		    strlen(FLORAL_BLOCK_SYS_PREFIX "/")) != 0)
		return FLORAL_SYS_NONE;

	suffix = path + strlen(FLORAL_BLOCK_SYS_PREFIX "/");
	if (strcmp(suffix, "zram0") == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(suffix, "zram0/", strlen("zram0/")) == 0 &&
	    string_in_array(suffix + strlen("zram0/"), block_files,
			    sizeof(block_files) / sizeof(block_files[0])))
		return FLORAL_SYS_FILE;
	return FLORAL_SYS_NONE;
}

static enum floral_sys_node_type node_sys_node_type(const char *path)
{
	const char *suffix;

	if (strcmp(path, FLORAL_NODE_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(path, FLORAL_NODE_SYS_PREFIX "/",
		    strlen(FLORAL_NODE_SYS_PREFIX "/")) != 0)
		return FLORAL_SYS_NONE;

	suffix = path + strlen(FLORAL_NODE_SYS_PREFIX "/");
	if (string_in_array(suffix, node_root_files,
			    sizeof(node_root_files) / sizeof(node_root_files[0])))
		return FLORAL_SYS_FILE;
	if (strcmp(suffix, "node0") == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(suffix, "node0/", strlen("node0/")) == 0 &&
	    string_in_array(suffix + strlen("node0/"), node_files,
			    sizeof(node_files) / sizeof(node_files[0])))
		return FLORAL_SYS_FILE;
	return FLORAL_SYS_NONE;
}

static int parse_cpu_path(const char *path, int cpu_count, int *cpu, const char **suffix)
{
	const char *number;
	char *end = NULL;
	long parsed;

	if (strncmp(path, FLORAL_CPU_SYS_PREFIX "/cpu",
		    strlen(FLORAL_CPU_SYS_PREFIX "/cpu")) != 0)
		return -EINVAL;

	number = path + strlen(FLORAL_CPU_SYS_PREFIX "/cpu");
	if (!isdigit((unsigned char)*number))
		return -EINVAL;

	errno = 0;
	parsed = strtol(number, &end, 10);
	if (errno || !end || parsed < 0 || parsed >= cpu_count || (*end && *end != '/'))
		return -EINVAL;

	*cpu = (int)parsed;
	*suffix = end;
	return 0;
}

static bool parse_cache_path(const char *suffix, int *index, const char **file)
{
	char *end = NULL;
	long parsed;
	const char *number;

	if (strncmp(suffix, "/cache/index", strlen("/cache/index")) != 0)
		return false;
	number = suffix + strlen("/cache/index");
	if (!isdigit((unsigned char)*number))
		return false;

	errno = 0;
	parsed = strtol(number, &end, 10);
	if (errno || !end || parsed < 0 || parsed > 3 || (*end && *end != '/'))
		return false;

	*index = (int)parsed;
	*file = *end == '/' ? end + 1 : end;
	return true;
}

enum floral_sys_node_type floral_sys_node_type(const struct floral_cpu_profile *profile,
					       int cpu_count, const char *path)
{
	const char *suffix;
	int cpu, index;

	if (!profile || !floral_sys_manages_path(path))
		return FLORAL_SYS_NONE;
	if (strcmp(path, FLORAL_CLASS_HWMON_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX) == 0 ||
	    strncmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX "/",
		    strlen(FLORAL_CLASS_THERMAL_SYS_PREFIX "/")) == 0)
		return thermal_class_node_type(path);
	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX) == 0 ||
	    strncmp(path, FLORAL_THERMAL_SYS_PREFIX "/",
		    strlen(FLORAL_THERMAL_SYS_PREFIX "/")) == 0)
		return thermal_device_node_type(path);
	if (strcmp(path, FLORAL_VIRTUAL_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strcmp(path, FLORAL_DMI_SYS_PREFIX) == 0 ||
	    strncmp(path, FLORAL_DMI_SYS_PREFIX "/",
		    strlen(FLORAL_DMI_SYS_PREFIX "/")) == 0)
		return dmi_sys_node_type(profile, path);
	if (cpu_count < 1 || !floral_profile_has_cpu_identity(profile))
		return FLORAL_SYS_NONE;
	if (strncmp(path, FLORAL_BLOCK_SYS_PREFIX,
		    strlen(FLORAL_BLOCK_SYS_PREFIX)) == 0)
		return block_sys_node_type(path);
	if (strcmp(path, FLORAL_NODE_SYS_PREFIX) == 0 ||
	    strncmp(path, FLORAL_NODE_SYS_PREFIX "/",
		    strlen(FLORAL_NODE_SYS_PREFIX "/")) == 0)
		return node_sys_node_type(path);

	if (strcmp(path, FLORAL_CPU_SYS_PREFIX) == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(path, FLORAL_CPU_SYS_PREFIX "/", strlen(FLORAL_CPU_SYS_PREFIX "/")) != 0)
		return FLORAL_SYS_NONE;

	suffix = path + strlen(FLORAL_CPU_SYS_PREFIX "/");
	if (string_in_array(suffix, cpu_root_files,
			    sizeof(cpu_root_files) / sizeof(cpu_root_files[0])))
		return FLORAL_SYS_FILE;

	if (parse_cpu_path(path, cpu_count, &cpu, &suffix))
		return FLORAL_SYS_NONE;
	if (!suffix[0])
		return FLORAL_SYS_DIRECTORY;
	if (suffix[0] != '/')
		return FLORAL_SYS_NONE;
	suffix++;

	if (string_in_array(suffix, cpu_files, sizeof(cpu_files) / sizeof(cpu_files[0])))
		return FLORAL_SYS_FILE;
	if (strcmp(suffix, "topology") == 0 || strcmp(suffix, "cpufreq") == 0 ||
	    strcmp(suffix, "cache") == 0)
		return FLORAL_SYS_DIRECTORY;
	if (strncmp(suffix, "topology/", strlen("topology/")) == 0 &&
	    string_in_array(suffix + strlen("topology/"), topology_files,
			    sizeof(topology_files) / sizeof(topology_files[0])))
		return FLORAL_SYS_FILE;
	if (strncmp(suffix, "cpufreq/", strlen("cpufreq/")) == 0 &&
	    string_in_array(suffix + strlen("cpufreq/"), cpufreq_files,
			    sizeof(cpufreq_files) / sizeof(cpufreq_files[0])))
		return FLORAL_SYS_FILE;
	if (parse_cache_path(suffix - 1, &index, &suffix)) {
		if (!suffix[0])
			return FLORAL_SYS_DIRECTORY;
		if (string_in_array(suffix, cache_files,
				    sizeof(cache_files) / sizeof(cache_files[0])))
			return FLORAL_SYS_FILE;
	}

	return FLORAL_SYS_NONE;
}

static int emit_array(floral_sys_emit_t emit, void *context,
		      const char *const *entries, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (emit(context, entries[i]))
			return -ENOENT;
	}

	return 0;
}

int floral_sys_list_directory(const struct floral_cpu_profile *profile,
			      int cpu_count, const char *path,
			      floral_sys_emit_t emit, void *context)
{
	const char *suffix;
	char name[32];
	int cpu, index;

	if (!emit || floral_sys_node_type(profile, cpu_count, path) != FLORAL_SYS_DIRECTORY)
		return -ENOENT;

	if (emit(context, ".") || emit(context, ".."))
		return -ENOENT;
	if (strcmp(path, FLORAL_VIRTUAL_SYS_PREFIX) == 0) {
		if (emit(context, "dmi"))
			return -ENOENT;
		return emit(context, "thermal");
	}
	if (strcmp(path, FLORAL_CLASS_HWMON_SYS_PREFIX) == 0)
		return 0;
	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX) == 0 ||
	    strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX) == 0)
		return emit(context, "thermal_zone0");
	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX "/thermal_zone0") == 0) {
		if (emit_array(emit, context, thermal_files,
			       sizeof(thermal_files) / sizeof(thermal_files[0])) ||
		    emit(context, "power"))
			return -ENOENT;
		return emit(context, "subsystem");
	}
	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX "/thermal_zone0/power") == 0)
		return emit_array(emit, context, thermal_power_files,
				  sizeof(thermal_power_files) /
				  sizeof(thermal_power_files[0]));
	if (strcmp(path, FLORAL_DMI_SYS_PREFIX) == 0)
		return emit(context, "id");
	if (strcmp(path, FLORAL_DMI_ID_SYS_PREFIX) == 0) {
		for (size_t i = 0; i < sizeof(dmi_files) / sizeof(dmi_files[0]); i++) {
			if (dmi_file_value(profile, dmi_files[i]) &&
			    emit(context, dmi_files[i]))
				return -ENOENT;
		}
		return 0;
	}
	if (strcmp(path, FLORAL_NODE_SYS_PREFIX) == 0) {
		if (emit(context, "node0"))
			return -ENOENT;
		return emit_array(emit, context, node_root_files,
				  sizeof(node_root_files) / sizeof(node_root_files[0]));
	}
	if (strcmp(path, FLORAL_NODE_SYS_PREFIX "/node0") == 0)
		return emit_array(emit, context, node_files,
				  sizeof(node_files) / sizeof(node_files[0]));
	if (strcmp(path, FLORAL_BLOCK_SYS_PREFIX) == 0)
		return emit(context, "zram0");
	if (strcmp(path, FLORAL_BLOCK_SYS_PREFIX "/zram0") == 0)
		return emit_array(emit, context, block_files,
				  sizeof(block_files) / sizeof(block_files[0]));
	if (strcmp(path, FLORAL_CPU_SYS_PREFIX) == 0) {
		for (int i = 0; i < cpu_count; i++) {
			int length = snprintf(name, sizeof(name), "cpu%d", i);
			if (length < 0 || (size_t)length >= sizeof(name) || emit(context, name))
				return -ENOENT;
		}
		return emit_array(emit, context, cpu_root_files,
				  sizeof(cpu_root_files) / sizeof(cpu_root_files[0]));
	}

	if (parse_cpu_path(path, cpu_count, &cpu, &suffix))
		return -ENOENT;
	if (!suffix[0]) {
		static const char *const directories[] = { "topology", "cpufreq", "cache" };
		if (emit_array(emit, context, directories,
			       sizeof(directories) / sizeof(directories[0])))
			return -ENOENT;
		return emit_array(emit, context, cpu_files,
				  sizeof(cpu_files) / sizeof(cpu_files[0]));
	}
	if (strcmp(suffix, "/topology") == 0)
		return emit_array(emit, context, topology_files,
				  sizeof(topology_files) / sizeof(topology_files[0]));
	if (strcmp(suffix, "/cpufreq") == 0)
		return emit_array(emit, context, cpufreq_files,
				  sizeof(cpufreq_files) / sizeof(cpufreq_files[0]));
	if (strcmp(suffix, "/cache") == 0) {
		for (int i = 0; i < 4; i++) {
			int length = snprintf(name, sizeof(name), "index%d", i);
			if (length < 0 || (size_t)length >= sizeof(name) || emit(context, name))
				return -ENOENT;
		}
		return 0;
	}
	if (parse_cache_path(suffix, &index, &suffix) && !suffix[0])
		return emit_array(emit, context, cache_files,
				  sizeof(cache_files) / sizeof(cache_files[0]));

	return -ENOENT;
}

static int append_cpu_list(char **cursor, size_t *remaining, int first, int last)
{
	if (first == last)
		return append_format(cursor, remaining, "%d\n", first);
	return append_format(cursor, remaining, "%d-%d\n", first, last);
}

static int append_cpu_map(char **cursor, size_t *remaining, int first, int last)
{
	int highest_group;

	if (first < 0 || last < first)
		return -EINVAL;

	highest_group = last / 32;
	for (int group = highest_group; group >= 0; group--) {
		uint32_t mask = 0;
		int group_first = group * 32;
		int group_last = group_first + 31;

		for (int cpu = first; cpu <= last; cpu++) {
			if (cpu >= group_first && cpu <= group_last)
				mask |= UINT32_C(1) << (cpu - group_first);
		}
		if (append_format(cursor, remaining, "%s%08x",
				  group == highest_group ? "" : ",", mask) < 0)
			return -ENOSPC;
	}

	return append_format(cursor, remaining, "\n");
}

static ssize_t render_root_file(int cpu_count, const char *file,
				char *buffer, size_t size)
{
	char *cursor = buffer;
	size_t remaining = size;

	if (strcmp(file, "online") == 0 || strcmp(file, "present") == 0 ||
	    strcmp(file, "possible") == 0)
		append_cpu_list(&cursor, &remaining, 0, cpu_count - 1);
	else if (strcmp(file, "kernel_max") == 0)
		append_format(&cursor, &remaining, "%d\n", cpu_count - 1);
	else if (strcmp(file, "offline") == 0 || strcmp(file, "isolated") == 0 ||
		 strcmp(file, "uevent") == 0)
		append_format(&cursor, &remaining, "\n");
	else
		return -ENOENT;

	return cursor - buffer;
}

static ssize_t render_topology_file(int cpu, int cpu_count, const char *file,
				    char *buffer, size_t size)
{
	char *cursor = buffer;
	size_t remaining = size;

	if (strcmp(file, "cluster_id") == 0)
		append_format(&cursor, &remaining, "%d\n", cpu_cluster(cpu));
	else if (strcmp(file, "core_id") == 0)
		append_format(&cursor, &remaining, "%d\n", cpu);
	else if (strcmp(file, "die_id") == 0 || strcmp(file, "physical_package_id") == 0)
		append_format(&cursor, &remaining, "0\n");
	else if (strcmp(file, "thread_siblings_list") == 0 ||
		 strcmp(file, "core_cpus_list") == 0)
		append_cpu_list(&cursor, &remaining, cpu, cpu);
	else if (strcmp(file, "thread_siblings") == 0 || strcmp(file, "core_cpus") == 0)
		append_cpu_map(&cursor, &remaining, cpu, cpu);
	else if (strcmp(file, "core_siblings_list") == 0 ||
		 strcmp(file, "package_cpus_list") == 0)
		append_cpu_list(&cursor, &remaining, 0, cpu_count - 1);
	else if (strcmp(file, "core_siblings") == 0 || strcmp(file, "package_cpus") == 0)
		append_cpu_map(&cursor, &remaining, 0, cpu_count - 1);
	else
		return -ENOENT;

	return cursor - buffer;
}

static ssize_t render_cpufreq_file(const struct floral_cpu_profile *profile,
				   int cpu, int cpu_count, const char *file,
				   char *buffer, size_t size)
{
	const struct floral_cpu_core *core = profile_core(profile, cpu);
	unsigned int maximum = core ? core->max_frequency_khz : 2500000;
	char *cursor = buffer;
	size_t remaining = size;
	int first, last;

	cluster_cpu_range(cpu, cpu_count, &first, &last);
	if (strcmp(file, "affected_cpus") == 0 || strcmp(file, "related_cpus") == 0)
		append_cpu_list(&cursor, &remaining, first, last);
	else if (strcmp(file, "cpuinfo_min_freq") == 0 || strcmp(file, "scaling_min_freq") == 0)
		append_format(&cursor, &remaining, "300000\n");
	else if (strcmp(file, "cpuinfo_cur_freq") == 0 || strcmp(file, "scaling_cur_freq") == 0 ||
		 strcmp(file, "cpuinfo_max_freq") == 0 || strcmp(file, "scaling_max_freq") == 0)
		append_format(&cursor, &remaining, "%u\n", maximum);
	else if (strcmp(file, "scaling_available_governors") == 0)
		append_format(&cursor, &remaining, "performance schedutil\n");
	else if (strcmp(file, "scaling_driver") == 0)
		append_format(&cursor, &remaining, "qcom-cpufreq-hw\n");
	else if (strcmp(file, "scaling_governor") == 0)
		append_format(&cursor, &remaining, "schedutil\n");
	else
		return -ENOENT;

	return cursor - buffer;
}

static ssize_t render_cache_file(int cpu, int cpu_count, int index, const char *file,
				 char *buffer, size_t size)
{
	static const char *const types[] = { "Data", "Instruction", "Unified", "Unified" };
	static const unsigned int sizes_kb[] = { 64, 64, 1024, 8192 };
	static const unsigned int ways[] = { 4, 4, 8, 16 };
	char *cursor = buffer;
	size_t remaining = size;
	int first = cpu, last = cpu;
	unsigned int cache_size = sizes_kb[index];

	if (index == 2 && cpu <= 2)
		cache_size = 512;
	if (index == 3) {
		first = 0;
		last = cpu_count - 1;
	}

	if (strcmp(file, "allocation_policy") == 0)
		append_format(&cursor, &remaining, "ReadWriteAllocate\n");
	else if (strcmp(file, "coherency_line_size") == 0)
		append_format(&cursor, &remaining, "64\n");
	else if (strcmp(file, "id") == 0)
		append_format(&cursor, &remaining, "%d\n", index == 3 ? 0 : cpu);
	else if (strcmp(file, "level") == 0)
		append_format(&cursor, &remaining, "%d\n", index < 2 ? 1 : index);
	else if (strcmp(file, "number_of_sets") == 0)
		append_format(&cursor, &remaining, "%u\n", cache_size * 1024 / 64 / ways[index]);
	else if (strcmp(file, "physical_line_partition") == 0)
		append_format(&cursor, &remaining, "1\n");
	else if (strcmp(file, "shared_cpu_list") == 0)
		append_cpu_list(&cursor, &remaining, first, last);
	else if (strcmp(file, "shared_cpu_map") == 0)
		append_cpu_map(&cursor, &remaining, first, last);
	else if (strcmp(file, "size") == 0)
		append_format(&cursor, &remaining, "%uK\n", cache_size);
	else if (strcmp(file, "type") == 0)
		append_format(&cursor, &remaining, "%s\n", types[index]);
	else if (strcmp(file, "ways_of_associativity") == 0)
		append_format(&cursor, &remaining, "%u\n", ways[index]);
	else if (strcmp(file, "write_policy") == 0)
		append_format(&cursor, &remaining, "WriteBack\n");
	else
		return -ENOENT;

	return cursor - buffer;
}

static ssize_t render_node_file(int cpu_count, uint64_t memory_total_kb,
				uint64_t memory_free_kb, const char *path,
				char *buffer, size_t size)
{
	const char *file;
	char *cursor = buffer;
	size_t remaining = size;
	uint64_t total_pages = memory_total_kb / 4;
	uint64_t used_kb = memory_total_kb > memory_free_kb ?
					 memory_total_kb - memory_free_kb :
					 0;

	if (strncmp(path, FLORAL_NODE_SYS_PREFIX "/node0/",
		    strlen(FLORAL_NODE_SYS_PREFIX "/node0/")) != 0) {
		file = path + strlen(FLORAL_NODE_SYS_PREFIX "/");
		if (string_in_array(file, node_root_files,
				    sizeof(node_root_files) / sizeof(node_root_files[0])))
			return snprintf(buffer, size, "0\n");
		return -ENOENT;
	}

	file = path + strlen(FLORAL_NODE_SYS_PREFIX "/node0/");
	if (strcmp(file, "cpulist") == 0)
		append_cpu_list(&cursor, &remaining, 0, cpu_count - 1);
	else if (strcmp(file, "cpumap") == 0)
		append_cpu_map(&cursor, &remaining, 0, cpu_count - 1);
	else if (strcmp(file, "distance") == 0)
		append_format(&cursor, &remaining, "10\n");
	else if (strcmp(file, "meminfo") == 0)
		append_format(&cursor, &remaining,
			      "Node 0 MemTotal:       %8" PRIu64 " kB\n"
			      "Node 0 MemFree:        %8" PRIu64 " kB\n"
			      "Node 0 MemUsed:        %8" PRIu64 " kB\n",
			      memory_total_kb, memory_free_kb, used_kb);
	else if (strcmp(file, "numastat") == 0)
		append_format(&cursor, &remaining,
			      "numa_hit %" PRIu64 "\n"
			      "numa_miss 0\n"
			      "numa_foreign 0\n"
			      "interleave_hit 0\n"
			      "local_node %" PRIu64 "\n"
			      "other_node 0\n",
			      total_pages, total_pages);
	else if (strcmp(file, "uevent") == 0)
		append_format(&cursor, &remaining, "\n");
	else
		return -ENOENT;

	return cursor - buffer;
}

static ssize_t render_block_file(uint64_t swap_total_kb, uint64_t swap_used_kb,
				 const char *path, char *buffer, size_t size)
{
	const char *file = path + strlen(FLORAL_BLOCK_SYS_PREFIX "/zram0/");
	uint64_t swap_total_bytes = swap_total_kb * UINT64_C(1024);
	uint64_t swap_used_bytes = swap_used_kb * UINT64_C(1024);

	if (strcmp(file, "dev") == 0)
		return snprintf(buffer, size, "252:0\n");
	if (strcmp(file, "disksize") == 0)
		return snprintf(buffer, size, "%" PRIu64 "\n", swap_total_bytes);
	if (strcmp(file, "size") == 0)
		return snprintf(buffer, size, "%" PRIu64 "\n", swap_total_bytes / 512);
	if (strcmp(file, "ro") == 0 || strcmp(file, "removable") == 0)
		return snprintf(buffer, size, "0\n");
	if (strcmp(file, "initstate") == 0)
		return snprintf(buffer, size, "1\n");
	if (strcmp(file, "mem_used_total") == 0)
		return snprintf(buffer, size, "%" PRIu64 "\n", swap_used_bytes);
	if (strcmp(file, "orig_data_size") == 0 ||
	    strcmp(file, "compr_data_size") == 0)
		return snprintf(buffer, size, "%" PRIu64 "\n", swap_used_bytes);
	if (strcmp(file, "mm_stat") == 0)
		return snprintf(buffer, size, "%" PRIu64 " %" PRIu64 " %" PRIu64
				" 0 0 0\n", swap_used_bytes, swap_used_bytes,
				swap_used_bytes);
	if (strcmp(file, "stat") == 0)
		return snprintf(buffer, size, "0 0 0 0 0 0 0 0 0 0 0\n");
	if (strcmp(file, "uevent") == 0)
		return snprintf(buffer, size,
				"MAJOR=252\nMINOR=0\nDEVNAME=zram0\nDEVTYPE=disk\n");
	if (strcmp(file, "comp_algorithm") == 0)
		return snprintf(buffer, size, "none\n");
	if (strcmp(file, "writeback") == 0)
		return snprintf(buffer, size, "0\n");
	return -ENOENT;
}

static ssize_t render_thermal_file(const struct floral_cpu_profile *profile,
				   const char *path, char *buffer, size_t size)
{
	const char *file = strrchr(path, '/');

	if (!file)
		return -ENOENT;
	file++;
	if (strcmp(file, "type") == 0)
		return snprintf(buffer, size, "%s\n", profile->thermal_battery_name);
	if (strcmp(file, "available_policies") == 0)
		return snprintf(buffer, size, "user_space step_wise bang_bang \n");
	if (strcmp(file, "passive") == 0 ||
	    strcmp(file, "runtime_active_time") == 0 ||
	    strcmp(file, "runtime_suspended_time") == 0)
		return snprintf(buffer, size, "0\n");
	if (strcmp(file, "policy") == 0)
		return snprintf(buffer, size, "step_wise\n");
	if (strcmp(file, "control") == 0)
		return snprintf(buffer, size, "auto\n");
	if (strcmp(file, "runtime_status") == 0)
		return snprintf(buffer, size, "unsupported\n");
	if (strcmp(file, "temp") == 0) {
		/* A slow triangular drift avoids exposing a frozen magic value. */
		time_t now = time(NULL);
		int phase = now < 0 ? 0 : (int)(now % 120);
		int drift = phase <= 60 ? phase * 400 / 60 - 200 :
						(120 - phase) * 400 / 60 - 200;
		int temperature =
			(int)(profile->thermal_ambient_celsius * 1000.0f) + 3000 + drift;
		return snprintf(buffer, size, "%d\n", temperature);
	}
	if (strcmp(file, "integral_cutoff") == 0 || strcmp(file, "k_d") == 0 ||
	    strcmp(file, "k_i") == 0 || strcmp(file, "k_po") == 0 ||
	    strcmp(file, "k_pu") == 0 || strcmp(file, "offset") == 0 ||
	    strcmp(file, "slope") == 0 || strcmp(file, "sustainable_power") == 0 ||
	    strcmp(file, "autosuspend_delay_ms") == 0 || strcmp(file, "uevent") == 0)
		return 0;
	return -ENOENT;
}

ssize_t floral_render_sys_symlink(const struct floral_cpu_profile *profile,
				  int cpu_count, const char *path,
				  char *buffer, size_t size)
{
	if (!buffer || !size ||
	    floral_sys_node_type(profile, cpu_count, path) != FLORAL_SYS_SYMLINK)
		return -ENOENT;
	if (strcmp(path, FLORAL_CLASS_THERMAL_SYS_PREFIX "/thermal_zone0") == 0)
		return snprintf(buffer, size,
				"../../devices/virtual/thermal/thermal_zone0");
	if (strcmp(path, FLORAL_THERMAL_SYS_PREFIX "/thermal_zone0/subsystem") == 0)
		return snprintf(buffer, size, "../../../../class/thermal");
	return -ENOENT;
}

ssize_t floral_render_sys_file(const struct floral_cpu_profile *profile,
			       int cpu_count, const char *path,
			       char *buffer, size_t size)
{
	return floral_render_sys_file_with_memory(profile, cpu_count, 0, 0,
						  path, buffer, size);
}

ssize_t floral_render_sys_file_with_memory(const struct floral_cpu_profile *profile,
					   int cpu_count, uint64_t memory_total_kb,
					   uint64_t memory_free_kb, const char *path,
					   char *buffer, size_t size)
{
	return floral_render_sys_file_with_memory_and_swap(profile, cpu_count,
						   memory_total_kb, memory_free_kb,
						   0, 0, path, buffer, size);
}

ssize_t floral_render_sys_file_with_memory_and_swap(
					   const struct floral_cpu_profile *profile,
					   int cpu_count, uint64_t memory_total_kb,
					   uint64_t memory_free_kb, uint64_t swap_total_kb,
					   uint64_t swap_used_kb, const char *path,
					   char *buffer, size_t size)
{
	const char *suffix, *file;
	int cpu, index;

	if (!buffer || !size ||
	    floral_sys_node_type(profile, cpu_count, path) != FLORAL_SYS_FILE)
		return -ENOENT;
	if (strncmp(path, FLORAL_DMI_ID_SYS_PREFIX "/",
		    strlen(FLORAL_DMI_ID_SYS_PREFIX "/")) == 0) {
		const char *value = dmi_file_value(
			profile, path + strlen(FLORAL_DMI_ID_SYS_PREFIX "/"));

		return value ? snprintf(buffer, size, "%s\n", value) : -ENOENT;
	}
	if (strncmp(path, FLORAL_THERMAL_SYS_PREFIX "/thermal_zone0/",
		    strlen(FLORAL_THERMAL_SYS_PREFIX "/thermal_zone0/")) == 0)
		return render_thermal_file(profile, path, buffer, size);
	if (strcmp(path, FLORAL_NODE_SYS_PREFIX) == 0 ||
	    strncmp(path, FLORAL_NODE_SYS_PREFIX "/",
		    strlen(FLORAL_NODE_SYS_PREFIX "/")) == 0)
		return render_node_file(cpu_count, memory_total_kb, memory_free_kb,
					path, buffer, size);
	if (strncmp(path, FLORAL_BLOCK_SYS_PREFIX "/zram0/",
		    strlen(FLORAL_BLOCK_SYS_PREFIX "/zram0/")) == 0)
		return render_block_file(swap_total_kb, swap_used_kb, path, buffer, size);

	if (strncmp(path, FLORAL_CPU_SYS_PREFIX "/cpu",
		    strlen(FLORAL_CPU_SYS_PREFIX "/cpu")) != 0)
		return render_root_file(cpu_count, path + strlen(FLORAL_CPU_SYS_PREFIX "/"),
					buffer, size);

	if (parse_cpu_path(path, cpu_count, &cpu, &suffix))
		return -ENOENT;
	if (strcmp(suffix, "/online") == 0)
		return snprintf(buffer, size, "1\n");
	if (strcmp(suffix, "/uevent") == 0)
		return snprintf(buffer, size, "\n");
	if (strcmp(suffix, "/cpu_capacity") == 0) {
		unsigned int capacity = cpu <= 2 ? 512 : (cpu <= 6 ? 768 : 1024);
		return snprintf(buffer, size, "%u\n", capacity);
	}
	if (strncmp(suffix, "/topology/", strlen("/topology/")) == 0)
		return render_topology_file(cpu, cpu_count, suffix + strlen("/topology/"),
					    buffer, size);
	if (strncmp(suffix, "/cpufreq/", strlen("/cpufreq/")) == 0)
		return render_cpufreq_file(profile, cpu, cpu_count,
					   suffix + strlen("/cpufreq/"), buffer, size);
	if (parse_cache_path(suffix, &index, &file) && file[0])
		return render_cache_file(cpu, cpu_count, index, file, buffer, size);

	return -ENOENT;
}
