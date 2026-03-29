/*
 * SPDX-License-Identifier: MIT
 */

#ifndef _DPLL_UTILS_H
#define _DPLL_UTILS_H

#include <linux/types.h>
#include <linux/dpll.h>

#if defined __has_include && !__has_include(<ynl/dpll-user.h>)
#error "Missing <ynl/dpll-user.h>: install kernel-tools-libs-devel or equivalent."
#endif
#include <ynl/dpll-user.h>
#include "log.h"

/* Global YNL socket (created by init_dpll) */
extern struct ynl_sock *ys;

int  init_dpll(void);
int  dpll_find_device_id_by_type(struct ynl_sock *ys, enum dpll_type device_type);

int  dpll_pin_set_state(struct ynl_sock *ys, __u32 device_id,
                        char *package_label, int state);
int  dpll_pin_set_priority(struct ynl_sock *ys, __u32 device_id,
                           char *package_label, int prio);
int  dpll_pin_get_priority(struct ynl_sock *ys, __u32 device_id,
                           char *package_label);
__s64 dpll_pin_set_phase_adjust(struct ynl_sock *ys, char *package_label,
                                __s64 phase_adjust);

#endif /* _DPLL_UTILS_H */
