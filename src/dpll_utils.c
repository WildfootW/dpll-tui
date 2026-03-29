/*
 * SPDX-License-Identifier: MIT
 *
 * DPLL netlink utility functions (YNL wrappers).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#include <linux/dpll.h>
#include <ynl/ynl.h>
#include "../hdr/dpll_utils.h"
#include "../hdr/log.h"

extern const struct ynl_family ynl_dpll_family;

struct ynl_sock *ys;

/* ---- internal: find a pin by package_label via dump ---- */
static int find_pin_by_package_label(struct ynl_sock *sock,
				     const char *package_label,
				     struct dpll_pin_get_rsp *out)
{
	struct dpll_pin_get_req_dump req = {0};
	struct dpll_pin_get_list *list = dpll_pin_get_dump(sock, &req);
	if (!list) return -1;

	for (struct dpll_pin_get_list *it = list; it; it = it->next) {
		if (it->obj.package_label &&
		    strncmp(it->obj.package_label, package_label,
			    strlen(package_label)) == 0) {
			memcpy(out, &it->obj, sizeof(*out));
			return 0;
		}
	}
	return -1;
}

/* ---- public API ---- */

int init_dpll(void)
{
	struct ynl_error yerr;
	ys = ynl_sock_create(&ynl_dpll_family, &yerr);
	if (!ys) {
		LOG_ERROR("Cannot create netlink socket: %s\n", yerr.msg);
		return 1;
	}
	return 0;
}

int dpll_find_device_id_by_type(struct ynl_sock *sock, enum dpll_type device_type)
{
	if (!sock) return -1;

	struct dpll_device_get_list *list = dpll_device_get_dump(sock);
	if (!list) return -1;

	int device_id = -1;
	for (struct dpll_device_get_list *it = list; it; it = it->next) {
		if (it->obj._present.type && it->obj.type == device_type &&
		    it->obj._present.id) {
			device_id = it->obj.id;
			break;
		}
	}
	dpll_device_get_list_free(list);
	return device_id;
}

__s64 dpll_pin_set_phase_adjust(struct ynl_sock *sock, char *package_label,
				__s64 phase_adjust)
{
	if (phase_adjust > INT32_MAX || phase_adjust < INT32_MIN) {
		LOG_ERROR("phase_adjust %" PRId64 " out of __s32 range\n", (int64_t)phase_adjust);
		return -1;
	}

	struct dpll_pin_get_rsp pin = {0};
	if (find_pin_by_package_label(sock, package_label, &pin))
		return -1;

	__s64 old = pin._present.phase_adjust ? pin.phase_adjust : -1;

	struct dpll_pin_set_req req = {0};
	req._present.id = 1;
	req.id = pin.id;
	req._present.phase_adjust = 1;
	req.phase_adjust = (__s32)phase_adjust;

	if (dpll_pin_set(sock, &req))
		LOG_ERROR("dpll_pin_set phase_adjust failed\n");

	return old;
}

int dpll_pin_set_state(struct ynl_sock *sock, __u32 device_id,
		       char *package_label, int state)
{
	struct dpll_pin_get_rsp pin = {0};
	if (find_pin_by_package_label(sock, package_label, &pin)) {
		LOG_ERROR("Pin %s not found\n", package_label);
		return -1;
	}

	int old_state = -1;
	if (pin.parent_device) {
		for (unsigned i = 0; i < pin._count.parent_device; i++) {
			if (pin.parent_device[i].parent_id == device_id) {
				old_state = pin.parent_device[i].state;
				break;
			}
		}
	}

	struct dpll_pin_set_req req = {0};
	req._present.id = 1;
	req.id = pin.id;
	req.parent_device = calloc(1, sizeof(*req.parent_device));
	if (!req.parent_device) return -1;
	req._count.parent_device = 1;
	req.parent_device->_present.state = 1;
	req.parent_device->state = state;
	req.parent_device->_present.parent_id = 1;
	req.parent_device->parent_id = device_id;

	int ret = dpll_pin_set(sock, &req);
	free(req.parent_device);
	if (ret) { LOG_ERROR("dpll_pin_set state failed\n"); return -1; }
	return old_state;
}

int dpll_pin_set_priority(struct ynl_sock *sock, __u32 device_id,
			  char *package_label, int prio)
{
	if (!sock) return -1;

	struct dpll_pin_get_rsp pin = {0};
	if (find_pin_by_package_label(sock, package_label, &pin)) {
		LOG_ERROR("Pin %s not found\n", package_label);
		return -1;
	}

	int old_prio = -1;
	if (pin.parent_device) {
		for (unsigned i = 0; i < pin._count.parent_device; i++) {
			if (pin.parent_device[i].parent_id == device_id) {
				old_prio = pin.parent_device[i].prio;
				break;
			}
		}
	}

	struct dpll_pin_set_req req = {0};
	req._present.id = 1;
	req.id = pin.id;
	req.parent_device = calloc(1, sizeof(*req.parent_device));
	if (!req.parent_device) return -1;
	req._count.parent_device = 1;
	req.parent_device->_present.prio = 1;
	req.parent_device->prio = prio;
	req.parent_device->_present.parent_id = 1;
	req.parent_device->parent_id = device_id;

	int ret = dpll_pin_set(sock, &req);
	free(req.parent_device);
	if (ret) { LOG_ERROR("dpll_pin_set priority failed\n"); return -1; }
	return old_prio;
}

int dpll_pin_get_priority(struct ynl_sock *sock, __u32 device_id,
			  char *package_label)
{
	struct dpll_pin_get_rsp pin = {0};
	if (find_pin_by_package_label(sock, package_label, &pin))
		return -1;

	if (pin.parent_device) {
		for (unsigned i = 0; i < pin._count.parent_device; i++) {
			if (pin.parent_device[i].parent_id == device_id)
				return pin.parent_device[i].prio;
		}
	}
	return -1;
}
