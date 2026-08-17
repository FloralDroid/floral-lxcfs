/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __LXCFS_FLORAL_PROFILE_H
#define __LXCFS_FLORAL_PROFILE_H

#include "config.h"

#include <linux/limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct lxcfs_opts;

#define FLORAL_PROFILE_MAX_SIZE 8192
#define FLORAL_PROFILE_VALUE_MAX 512

struct floral_dmi_profile {
	char manufacturer[FLORAL_PROFILE_VALUE_MAX];
	char model[FLORAL_PROFILE_VALUE_MAX];
	char board[FLORAL_PROFILE_VALUE_MAX];
	char serial[FLORAL_PROFILE_VALUE_MAX];
	char hardware_revision[FLORAL_PROFILE_VALUE_MAX];
};

struct floral_cpu_profile {
	unsigned int version;
	char cpu_vendor[FLORAL_PROFILE_VALUE_MAX];
	char cpu_model[FLORAL_PROFILE_VALUE_MAX];
	char cpu_features[FLORAL_PROFILE_VALUE_MAX];
	char cpu_feature_view[FLORAL_PROFILE_VALUE_MAX];
	char cpu_cores[32];
	char soc_model[FLORAL_PROFILE_VALUE_MAX];
	char kernel_release[FLORAL_PROFILE_VALUE_MAX];
	char kernel_version[FLORAL_PROFILE_VALUE_MAX];
	struct floral_dmi_profile dmi;
};

int floral_profile_parse(char *data, struct floral_cpu_profile *profile);
int floral_profile_load(pid_t initpid, const struct lxcfs_opts *opts,
			struct floral_cpu_profile *profile);
void floral_profile_set_default_dmi_identity(struct floral_cpu_profile *profile);
bool floral_profile_has_cpu_identity(const struct floral_cpu_profile *profile);
bool floral_profile_has_kernel_identity(const struct floral_cpu_profile *profile);
bool floral_profile_has_dmi_identity(const struct floral_cpu_profile *profile);

#endif /* __LXCFS_FLORAL_PROFILE_H */
