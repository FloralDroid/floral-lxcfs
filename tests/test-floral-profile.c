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

static const char profile_data[] =
	"# OPPO Find X6 Pro\n"
	"version=1\n"
	"brand=OPPO\n"
	"model=Find X6 Pro\n"
	"soc_model=SM8550\n"
	"cpu_vendor=Qualcomm\n"
	"cpu_model=ARMv8 Processor rev 1 (v8l)\n"
	"cpu_features=fp,asimd,aes,crc32\n"
	"cpu_feature_view=sm8550\n"
	"cpu_cores=auto\n"
	"kernel_release=5.10.66-android12-9-g123456789abc\n"
	"kernel_version=#1 SMP PREEMPT Mon Feb 7 12:00:00 UTC 2022\n";

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
	struct floral_cpu_profile profile;
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

static void test_rejects_invalid_profiles(void)
{
	struct floral_cpu_profile profile;
	char duplicate[] = "version=1\ncpu_model=a\ncpu_model=b\n";
	char invalid_count[] = "version=1\ncpu_cores=0\n";
	char invalid_version[] = "version=2\nsoc_model=SM8550\n";
	char invalid_key[] = "version=1\ncpu_model=a\001b\n";
	char empty_known[] = "version=1\nsoc_model=\n";

	assert(floral_profile_parse(duplicate, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_count, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_version, &profile) == -EINVAL);
	assert(floral_profile_parse(invalid_key, &profile) == -EINVAL);
	assert(floral_profile_parse(empty_known, &profile) == -EINVAL);
}

int main(void)
{
	test_parser_and_cpuinfo();
	test_sysfs_view();
	test_container_profile_load();
	test_rejects_invalid_profiles();
	return EXIT_SUCCESS;
}
