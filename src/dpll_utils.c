/*
 * SPDX-License-Identifier: MIT
 *
 * DPLL netlink utility functions (YNL wrappers).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/dpll.h>
#include <ynl/ynl.h>
#include "../hdr/dpll_utils.h"
#include "../hdr/log.h"

extern const struct ynl_family ynl_dpll_family;

struct ynl_sock *ys;

/*
 * Workaround for a bug in some libynl builds (e.g. kernel-tools-libs
 * 6.12.0-213.el10) where ynl_attr_put_str() sets nla_len without
 * accounting for the NUL terminator.  Kernels that enforce strict
 * NLA_NUL_STRING policy reject the CTRL_CMD_GETFAMILY request with
 * -EINVAL, making ynl_sock_create() fail.
 *
 * This function replicates ynl_sock_create() but constructs the
 * GETFAMILY message correctly.  It is only called when the standard
 * ynl_sock_create() fails, so fixed libynl versions take the fast path.
 */
static struct ynl_sock *
ynl_sock_create_compat(const struct ynl_family *yf, struct ynl_error *yse)
{
	struct ynl_sock *s = NULL;
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	socklen_t addrlen = sizeof(addr);
	int fd = -1;

	s = calloc(1, sizeof(*s) + 2 * YNL_SOCKET_BUFFER_SIZE);
	if (!s) {
		if (yse) snprintf(yse->msg, sizeof(yse->msg), "calloc failed");
		return NULL;
	}

	s->family = yf;
	s->tx_buf = s->raw_buf;
	s->rx_buf = s->raw_buf + YNL_SOCKET_BUFFER_SIZE;
	s->ntf_last_next = &s->ntf_first;
	s->seq = random();

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		if (yse) snprintf(yse->msg, sizeof(yse->msg),
				  "socket: %s", strerror(errno));
		goto err;
	}
	s->socket = fd;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (yse) snprintf(yse->msg, sizeof(yse->msg),
				  "bind: %s", strerror(errno));
		goto err;
	}
	getsockname(fd, (struct sockaddr *)&addr, &addrlen);
	s->portid = addr.nl_pid;

	/* --- resolve generic-netlink family (NUL-terminated name) --- */
	{
		unsigned char reqbuf[256];
		memset(reqbuf, 0, sizeof(reqbuf));

		size_t name_len = strlen(yf->name) + 1; /* include NUL */

		struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
		nlh->nlmsg_type  = GENL_ID_CTRL;
		nlh->nlmsg_flags = NLM_F_REQUEST;
		nlh->nlmsg_seq   = ++s->seq;
		nlh->nlmsg_len   = NLMSG_HDRLEN;

		struct genlmsghdr *genl =
			(struct genlmsghdr *)((char *)nlh + NLMSG_HDRLEN);
		genl->cmd     = CTRL_CMD_GETFAMILY;
		genl->version = 1;
		nlh->nlmsg_len += GENL_HDRLEN;

		struct nlattr *nla =
			(struct nlattr *)((char *)nlh + nlh->nlmsg_len);
		nla->nla_type = CTRL_ATTR_FAMILY_NAME;
		nla->nla_len  = NLA_HDRLEN + name_len;
		memcpy((char *)nla + NLA_HDRLEN, yf->name, name_len);
		nlh->nlmsg_len += NLA_ALIGN(nla->nla_len);

		if (send(fd, reqbuf, nlh->nlmsg_len, 0) < 0) {
			if (yse) snprintf(yse->msg, sizeof(yse->msg),
					  "send GETFAMILY: %s",
					  strerror(errno));
			goto err;
		}

		unsigned char rspbuf[4096];
		ssize_t n = recv(fd, rspbuf, sizeof(rspbuf), 0);
		if (n < (ssize_t)NLMSG_HDRLEN) {
			if (yse) snprintf(yse->msg, sizeof(yse->msg),
					  "recv GETFAMILY: %s",
					  n < 0 ? strerror(errno) : "short");
			goto err;
		}

		struct nlmsghdr *resp = (struct nlmsghdr *)rspbuf;
		if (resp->nlmsg_type == NLMSG_ERROR) {
			int err_code =
				((struct nlmsgerr *)NLMSG_DATA(resp))->error;
			if (yse) snprintf(yse->msg, sizeof(yse->msg),
					  "GETFAMILY: %s",
					  strerror(-err_code));
			goto err;
		}

		/* walk response attributes */
		int rem = (int)resp->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
		struct nlattr *a = (struct nlattr *)
			((char *)resp + NLMSG_HDRLEN + GENL_HDRLEN);

		while (rem >= (int)NLA_HDRLEN && rem >= a->nla_len &&
		       a->nla_len >= NLA_HDRLEN) {
			unsigned int atype = a->nla_type & NLA_TYPE_MASK;

			if (atype == CTRL_ATTR_FAMILY_ID)
				s->family_id = *(__u16 *)((char *)a + NLA_HDRLEN);

			if (atype == CTRL_ATTR_MCAST_GROUPS) {
				/* count groups */
				unsigned int ng = 0;
				int gr = a->nla_len - NLA_HDRLEN;
				struct nlattr *g =
					(struct nlattr *)((char *)a + NLA_HDRLEN);
				while (gr >= (int)NLA_HDRLEN &&
				       gr >= g->nla_len &&
				       g->nla_len >= NLA_HDRLEN) {
					ng++;
					int step = NLA_ALIGN(g->nla_len);
					gr -= step;
					g = (struct nlattr *)((char *)g + step);
				}
				if (!ng) goto next;

				s->mcast_groups = calloc(ng,
							 sizeof(*s->mcast_groups));
				s->n_mcast_groups = ng;

				gr = a->nla_len - NLA_HDRLEN;
				g = (struct nlattr *)((char *)a + NLA_HDRLEN);
				unsigned int gi = 0;
				while (gr >= (int)NLA_HDRLEN &&
				       gr >= g->nla_len &&
				       g->nla_len >= NLA_HDRLEN &&
				       gi < ng) {
					int ir = g->nla_len - NLA_HDRLEN;
					struct nlattr *ia = (struct nlattr *)
						((char *)g + NLA_HDRLEN);
					while (ir >= (int)NLA_HDRLEN &&
					       ir >= ia->nla_len &&
					       ia->nla_len >= NLA_HDRLEN) {
						unsigned int it =
							ia->nla_type & NLA_TYPE_MASK;
						if (it == CTRL_ATTR_MCAST_GRP_ID)
							s->mcast_groups[gi].id =
								*(__u32 *)((char *)ia + NLA_HDRLEN);
						else if (it == CTRL_ATTR_MCAST_GRP_NAME)
							strncpy(s->mcast_groups[gi].name,
								(char *)ia + NLA_HDRLEN,
								GENL_NAMSIZ - 1);
						int is = NLA_ALIGN(ia->nla_len);
						ir -= is;
						ia = (struct nlattr *)((char *)ia + is);
					}
					gi++;
					int step = NLA_ALIGN(g->nla_len);
					gr -= step;
					g = (struct nlattr *)((char *)g + step);
				}
			}
next:;
			int step = NLA_ALIGN(a->nla_len);
			rem -= step;
			a = (struct nlattr *)((char *)a + step);
		}

		if (!s->family_id) {
			if (yse) snprintf(yse->msg, sizeof(yse->msg),
					  "family '%s' not found", yf->name);
			goto err;
		}
	}

	return s;
err:
	if (fd >= 0) close(fd);
	if (s) { free(s->mcast_groups); free(s); }
	return NULL;
}

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
	if (ys)
		return 0;

	LOG_INFO("ynl_sock_create failed (%s), trying compat workaround\n",
		 yerr.msg);
	ys = ynl_sock_create_compat(&ynl_dpll_family, &yerr);
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
