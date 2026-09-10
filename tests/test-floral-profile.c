/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/bindings.h"
#include "../src/floral/profile.h"
#include "../src/floral/view.h"

#if !HAVE_STRLCPY
/* The production module gets this compatibility function from utils.c. */
size_t strlcpy(char *dest, const char *src, size_t size)
{
	size_t length = strlen(src);

	if (size) {
		size_t copied = length >= size ? size - 1 : length;

		memcpy(dest, src, copied);
		dest[copied] = '\0';
	}

	return length;
}
#endif

static const char profile_data[] =
	"# OPPO Find X6 Pro\n"
	"version=1\n"
	"brand=OPPO\n"
	"manufacturer=OPPO\n"
	"model=Find X6 Pro\n"
	"device=OP528BL1\n"
	"product=PGEM10\n"
	"board=kalama\n"
	"soc_manufacturer=Qualcomm\n"
	"soc_model=SM8550\n"
	"gpu_vendor=Qualcomm\n"
	"gpu_model=Adreno 740\n"
	"build_id=SKQ1.211006.001\n"
	"build_display=PGEM10_11_A.12\n"
	"version_release=12\n"
	"security_patch=2022-01-05\n"
	"cpu_vendor=Qualcomm\n"
	"cpu_model=ARMv8 Processor rev 1 (v8l)\n"
	"cpu_features=fp,asimd,aes,crc32\n"
	"cpu_feature_view=sm8550\n"
	"cpu_cores=auto\n"
	"kernel_release=5.10.66-android12-9-g123456789abc\n"
	"kernel_version=#1 SMP PREEMPT Mon Feb 7 12:00:00 UTC 2022\n"
	"serial=PGEM10FLORAL0001\n"
	"hardware_revision=EVT1\n"
	"thermal_ambient_celsius=22.0\n"
	"thermal_battery_name=battery\n";

struct expected_directory {
	const char *const *entries;
	size_t count;
	size_t index;
};

static int expect_directory_entry(void *opaque, const char *name)
{
	struct expected_directory *expected = opaque;

	assert(expected->index < expected->count);
	assert(strcmp(name, expected->entries[expected->index]) == 0);
	expected->index++;
	return 0;
}

static void test_parser_and_cpuinfo(void)
{
	struct floral_cpu_profile profile;
	char data[sizeof(profile_data)];
	char output[16384];
	ssize_t length;
	int count;

	memcpy(data, profile_data, sizeof(data));
	assert(floral_profile_parse(data, &profile) == 0);
	assert(floral_profile_has_cpu_identity(&profile));
	assert(strcmp(profile.soc_model, "SM8550") == 0);
	assert(floral_profile_has_kernel_identity(&profile));
	assert(floral_profile_has_dmi_identity(&profile));
	assert(strcmp(profile.dmi.manufacturer, "OPPO") == 0);
	assert(strcmp(profile.dmi.model, "Find X6 Pro") == 0);
	assert(strcmp(profile.dmi.board, "kalama") == 0);
	assert(strcmp(profile.dmi.serial, "PGEM10FLORAL0001") == 0);
	assert(strcmp(profile.dmi.hardware_revision, "EVT1") == 0);
	assert(profile.thermal_ambient_celsius > 21.99f);
	assert(profile.thermal_ambient_celsius < 22.01f);
	assert(strcmp(profile.thermal_battery_name, "battery") == 0);

	count = floral_visible_cpu_count(&profile, "0-79", 8);
	assert(count == 8);
	length = floral_render_cpuinfo(&profile, count, output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strstr(output, "processor\t: 7\n"));
	assert(!strstr(output, "processor\t: 8\n"));
	assert(strstr(output, "CPU part\t: 0xd4e\n"));
	assert(strstr(output, "Hardware\t: SM8550\n"));
	assert(!strstr(output, "Intel"));

	length = floral_render_kernel_identity(&profile, "/proc/version", output,
					       sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output,
		      "Linux version 5.10.66-android12-9-g123456789abc "
		      "#1 SMP PREEMPT Mon Feb 7 12:00:00 UTC 2022\n") == 0);

	length = floral_render_kernel_identity(&profile,
					       "/proc/sys/kernel/osrelease", output,
					       sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "5.10.66-android12-9-g123456789abc\n") == 0);

	assert(floral_visible_cpu_count(&profile, "0-79", 4) == 4);
}

static void test_sysfs_view(void)
{
	static const char *const thermal_entries[] = {
		".", "..", "available_policies", "integral_cutoff", "k_d", "k_i",
		"k_po", "k_pu", "offset", "passive", "policy", "slope",
		"sustainable_power", "temp", "type", "uevent", "power", "subsystem",
	};
	static const char *const power_entries[] = {
		".", "..", "autosuspend_delay_ms", "control", "runtime_active_time",
		"runtime_status", "runtime_suspended_time",
	};
	struct floral_cpu_profile profile;
	struct expected_directory expected;
	char data[sizeof(profile_data)];
	char output[4096];
	ssize_t length;

	memcpy(data, profile_data, sizeof(data));
	assert(floral_profile_parse(data, &profile) == 0);
	assert(floral_sys_node_type(&profile, 8, "/sys/devices/system/cpu/cpu7/cpufreq") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8, "/sys/devices/system/cpu/cpu8") ==
	       FLORAL_SYS_NONE);
	assert(floral_sys_manages_path("/sys/devices/system/cpu/cpu8"));
	assert(floral_sys_manages_path("/sys/devices/system/cpu/vulnerabilities"));
	assert(floral_sys_manages_path("/sys/devices/system/node/node0/meminfo"));
	assert(!floral_sys_manages_path("/sys/devices/system/memory"));
	assert(floral_sys_manages_path("/sys/block"));
	assert(floral_sys_node_type(&profile, 8, "/sys/block") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8, "/sys/block/zram0") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8, "/sys/block/zram0/disksize") ==
	       FLORAL_SYS_FILE);
	assert(floral_sys_node_type(&profile, 8, "/sys/devices/system/node/node0") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_manages_path("/sys/devices/virtual/dmi/id/product_name"));
	assert(!floral_sys_manages_path("/sys/devices/virtual/net"));
	assert(floral_sys_node_type(&profile, 8, "/sys/devices/virtual") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8, "/sys/devices/virtual/dmi/id") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/devices/virtual/dmi/id/product_serial") ==
	       FLORAL_SYS_FILE);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/devices/virtual/dmi/id/product_uuid") ==
	       FLORAL_SYS_NONE);
	assert(floral_sys_node_type(&profile, 8, "/sys/class/thermal") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/class/thermal/thermal_zone0") ==
	       FLORAL_SYS_SYMLINK);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/class/thermal/thermal_zone0/temp") ==
	       FLORAL_SYS_NONE);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/devices/virtual/thermal/thermal_zone0/power") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/devices/virtual/thermal/thermal_zone0/subsystem") ==
	       FLORAL_SYS_SYMLINK);
	assert(floral_sys_node_type(&profile, 8,
				    "/sys/devices/virtual/thermal/thermal_zone0/policy") ==
	       FLORAL_SYS_FILE);
	assert(floral_sys_node_type(&profile, 8, "/sys/class/hwmon") ==
	       FLORAL_SYS_DIRECTORY);
	assert(floral_sys_node_type(&profile, 8, "/sys/class/hwmon/hwmon0") ==
	       FLORAL_SYS_NONE);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/dmi/id/sys_vendor",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "OPPO\n") == 0);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/dmi/id/board_name",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "kalama\n") == 0);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/dmi/id/product_version",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "EVT1\n") == 0);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "3200000\n") == 0);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/system/cpu/cpu0/cache/index3/size",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "8192K\n") == 0);

	length = floral_render_sys_file(&profile, 80,
					"/sys/devices/system/cpu/cpu0/topology/package_cpus",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "0000ffff,ffffffff,ffffffff\n") == 0);

	length = floral_render_sys_file_with_memory(&profile, 4, 4194304, 3145728,
						    "/sys/devices/system/node/node0/meminfo", output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strstr(output, "Node 0 MemTotal:        4194304 kB\n"));
	assert(strstr(output, "Node 0 MemFree:         3145728 kB\n"));
	assert(strstr(output, "Node 0 MemUsed:         1048576 kB\n"));

	length = floral_render_sys_file_with_memory_and_swap(
		&profile, 4, 4194304, 3145728, 2097152, 1024,
		"/sys/block/zram0/mm_stat", output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "1048576 1048576 1048576 0 0 0\n") == 0);

	expected = (struct expected_directory){
		.entries = thermal_entries,
		.count = sizeof(thermal_entries) / sizeof(thermal_entries[0]),
	};
	assert(floral_sys_list_directory(
		       &profile, 8, "/sys/devices/virtual/thermal/thermal_zone0",
		       expect_directory_entry, &expected) == 0);
	assert(expected.index == expected.count);
	expected = (struct expected_directory){
		.entries = power_entries,
		.count = sizeof(power_entries) / sizeof(power_entries[0]),
	};
	assert(floral_sys_list_directory(
		       &profile, 8, "/sys/devices/virtual/thermal/thermal_zone0/power",
		       expect_directory_entry, &expected) == 0);
	assert(expected.index == expected.count);

	length = floral_render_sys_symlink(&profile, 8,
					   "/sys/class/thermal/thermal_zone0",
					   output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "../../devices/virtual/thermal/thermal_zone0") == 0);
	length = floral_render_sys_symlink(
		&profile, 8, "/sys/devices/virtual/thermal/thermal_zone0/subsystem",
		output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "../../../../class/thermal") == 0);

	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/thermal/thermal_zone0/type",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "battery\n") == 0);
	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/thermal/thermal_zone0/temp",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strtol(output, NULL, 10) >= 24800);
	assert(strtol(output, NULL, 10) <= 25200);
	length = floral_render_sys_file(&profile, 8,
					"/sys/devices/virtual/thermal/thermal_zone0/policy",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "step_wise\n") == 0);
	length = floral_render_sys_file(
		&profile, 8, "/sys/devices/virtual/thermal/thermal_zone0/power/control",
		output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "auto\n") == 0);
	assert(floral_render_sys_file(
		       &profile, 8, "/sys/devices/virtual/thermal/thermal_zone0/k_d",
		       output, sizeof(output)) == 0);
}

static void test_container_profile_load(void)
{
	struct floral_cpu_profile profile;
	struct lxcfs_opts opts = {
		.version = 5,
		.floral_mode = true,
	};
	char path[] = "/tmp/floral-lxcfs-profile-XXXXXX";
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(write(fd, profile_data, sizeof(profile_data) - 1) ==
	       (ssize_t)(sizeof(profile_data) - 1));
	assert(close(fd) == 0);
	assert(strlcpy(opts.floral_profile_path, path,
		       sizeof(opts.floral_profile_path)) < sizeof(opts.floral_profile_path));
	assert(floral_profile_load(getpid(), &opts, &profile) == 0);
	assert(strcmp(profile.cpu_feature_view, "sm8550") == 0);
	assert(strcmp(profile.kernel_release, "5.10.66-android12-9-g123456789abc") == 0);
	assert(unlink(path) == 0);
}

static void test_profile_with_required_identity_only(void)
{
	struct floral_cpu_profile profile;
	char data[] =
		"version=1\nbrand=OPPO\nmanufacturer=OPPO\nmodel=PGEM10\n"
		"device=OP528BL1\nproduct=PGEM10\nboard=taro\n"
		"soc_manufacturer=Qualcomm\nsoc_model=SM8450\n"
		"gpu_vendor=Qualcomm\ngpu_model=Adreno 730\n"
		"build_id=SKQ1.211006.001\nbuild_display=PGEM10_11_A.12\n"
		"version_release=12\nsecurity_patch=2022-01-05\n";
	char output[128];
	ssize_t length;

	assert(floral_profile_parse(data, &profile) == 0);
	assert(floral_profile_has_cpu_identity(&profile));
	assert(floral_profile_has_dmi_identity(&profile));
	assert(!profile.dmi.serial[0]);
	assert(!profile.dmi.hardware_revision[0]);
	assert(floral_sys_node_type(&profile, 0,
				    "/sys/devices/virtual/dmi/id/sys_vendor") ==
	       FLORAL_SYS_FILE);
	assert(floral_sys_node_type(&profile, 0,
				    "/sys/devices/virtual/dmi/id/product_serial") ==
	       FLORAL_SYS_NONE);

	length = floral_render_sys_file(&profile, 0,
					"/sys/devices/virtual/dmi/id/product_name",
					output, sizeof(output));
	assert(length > 0);
	output[length] = '\0';
	assert(strcmp(output, "PGEM10\n") == 0);
}

static void test_rejects_invalid_profiles(void)
{
	struct floral_cpu_profile profile;
	char duplicate[] = "version=1\ncpu_model=a\ncpu_model=b\n";
	char invalid_count[] = "version=1\ncpu_cores=0\n";
	char invalid_version[] = "version=2\nsoc_model=SM8550\n";
	char invalid_key[] = "version=1\ncpu_model=a\001b\n";
	char empty_known[] = "version=1\nsoc_model=\n";
	char duplicate_dmi[] = "version=1\nmodel=a\nmodel=b\n";
	char duplicate_unknown[sizeof(profile_data) + 32];
	char partial_sensor[sizeof(profile_data) + 64];
	char invalid_patch[sizeof(profile_data)];
	char invalid_ambient[sizeof(profile_data) + 48];
	char oversized_vendor[sizeof(profile_data) + 128];

	assert(snprintf(duplicate_unknown, sizeof(duplicate_unknown), "%sfoo=a\nfoo=b\n",
			profile_data) > 0);
	assert(snprintf(partial_sensor, sizeof(partial_sensor),
			"%ssensor_pressure_name=Pressure sensor\n", profile_data) > 0);
	assert(snprintf(invalid_patch, sizeof(invalid_patch), "%s", profile_data) > 0);
	char *patch = strstr(invalid_patch, "security_patch=2022-01-05");
	assert(patch);
	memcpy(patch, "security_patch=2022-02-30", strlen("security_patch=2022-02-30"));
	assert(snprintf(invalid_ambient, sizeof(invalid_ambient), "%s", profile_data) > 0);
	char *ambient = strstr(invalid_ambient, "thermal_ambient_celsius=22.0");
	assert(ambient);
	memcpy(ambient, "thermal_ambient_celsius=nan ",
	       strlen("thermal_ambient_celsius=nan "));
	assert(snprintf(oversized_vendor, sizeof(oversized_vendor),
			"%ssensor_pressure_name=Pressure sensor\n"
			"sensor_pressure_vendor=12345678901234567890123456789012"
			"345678901234567890123456789012345\n", profile_data) > 0);

	assert(floral_profile_parse(duplicate, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_count, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_version, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_key, &profile) == -EINVAL);
	assert(floral_profile_parse(empty_known, &profile) == -EINVAL);
	assert(floral_profile_parse(duplicate_dmi, &profile) == -EINVAL);
	assert(floral_profile_parse(duplicate_unknown, &profile) == -EINVAL);
	assert(floral_profile_parse(partial_sensor, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_patch, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_ambient, &profile) == -EINVAL);
	assert(floral_profile_parse(oversized_vendor, &profile) == -EINVAL);
}

int main(void)
{
	test_parser_and_cpuinfo();
	test_sysfs_view();
	test_container_profile_load();
	test_profile_with_required_identity_only();
	test_rejects_invalid_profiles();
	return EXIT_SUCCESS;
}
