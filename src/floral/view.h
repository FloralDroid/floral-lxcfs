/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __LXCFS_FLORAL_VIEW_H
#define __LXCFS_FLORAL_VIEW_H

#include "config.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "profile.h"

int floral_visible_cpu_count(const struct floral_cpu_profile *profile,
			     const char *cpuset, int cfs_count);
ssize_t floral_render_cpuinfo(const struct floral_cpu_profile *profile,
			      int cpu_count, char *buffer, size_t size);
ssize_t floral_render_kernel_identity(const struct floral_cpu_profile *profile,
				      const char *path, char *buffer, size_t size);
bool floral_sys_manages_path(const char *path);

enum floral_sys_node_type {
	FLORAL_SYS_NONE,
	FLORAL_SYS_DIRECTORY,
	FLORAL_SYS_FILE,
	FLORAL_SYS_SYMLINK,
};

typedef int (*floral_sys_emit_t)(void *context, const char *name);

enum floral_sys_node_type floral_sys_node_type(const struct floral_cpu_profile *profile,
					       int cpu_count, const char *path);
int floral_sys_list_directory(const struct floral_cpu_profile *profile,
			      int cpu_count, const char *path,
			      floral_sys_emit_t emit, void *context);
ssize_t floral_render_sys_file(const struct floral_cpu_profile *profile,
			       int cpu_count, const char *path,
			       char *buffer, size_t size);
ssize_t floral_render_sys_file_with_memory(const struct floral_cpu_profile *profile,
					   int cpu_count, uint64_t memory_total_kb,
					   uint64_t memory_free_kb, const char *path,
					   char *buffer, size_t size);
ssize_t floral_render_sys_file_with_memory_and_swap(
					   const struct floral_cpu_profile *profile,
					   int cpu_count, uint64_t memory_total_kb,
					   uint64_t memory_free_kb, uint64_t swap_total_kb,
					   uint64_t swap_used_kb, const char *path,
					   char *buffer, size_t size);
ssize_t floral_render_sys_symlink(const struct floral_cpu_profile *profile,
				  int cpu_count, const char *path,
				  char *buffer, size_t size);

#endif /* __LXCFS_FLORAL_VIEW_H */
