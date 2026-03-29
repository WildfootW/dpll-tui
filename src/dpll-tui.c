/*
 * SPDX-License-Identifier: MIT
 *
 * dpll-tui - ncurses TUI to inspect and control DPLL devices via YNL (netlink).
 *
 * Keys:
 *   q / Esc    quit
 *   Tab        switch DPLL device (PPS / EEC)
 *   Up / Down  move pin selection
 *   r          refresh now
 *   s          set pin state   (text menu)
 *   p          set pin priority (numeric input)
 *   a          set pin phase_adjust (fs, int64 input)
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/dpll.h>
#include <ynl/ynl.h>

#include <curses.h>

#include "../hdr/dpll_utils.h"
#include "../hdr/log.h"

extern const struct ynl_family ynl_dpll_family;

FILE    *g_log_file;
LogLevel g_log_level;

/* ---- pin state name table ---- */
typedef struct {
	int value;
	const char *name;
} StateEntry;

static const StateEntry pin_states[] = {
	{ DPLL_PIN_STATE_CONNECTED,    "connected"    },
	{ DPLL_PIN_STATE_DISCONNECTED, "disconnected" },
	{ DPLL_PIN_STATE_SELECTABLE,   "selectable"   },
};
#define N_PIN_STATES (int)(sizeof(pin_states) / sizeof(pin_states[0]))

static const char *state_name(int st)
{
	for (int i = 0; i < N_PIN_STATES; i++)
		if (pin_states[i].value == st)
			return pin_states[i].name;
	return "?";
}

/* ---- per-pin row ---- */
typedef struct {
	const char *label;
	const char *package_label;
	uint32_t pin_id;
	int state;
	int prio;
	bool has_phase_offset;
	int64_t phase_offset;
} PinRow;

/* ---- device status ---- */
typedef struct {
	uint32_t device_id;
	enum dpll_type type;
	const char *type_name;
	enum dpll_lock_status lock_status;
	enum dpll_mode mode;
} DeviceStatus;

static const char *type_str(enum dpll_type t)
{
	switch (t) {
	case DPLL_TYPE_EEC: return "EEC";
	case DPLL_TYPE_PPS: return "PPS";
	default: return "OTHER";
	}
}

/* ---- ncurses helpers ---- */
static void status_msg(const char *fmt, ...)
{
	move(LINES - 1, 0);
	clrtoeol();
	va_list ap;
	va_start(ap, fmt);
	char buf[256];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	attron(A_BOLD);
	mvprintw(LINES - 1, 0, "%s", buf);
	attroff(A_BOLD);
	refresh();
}

static void prompt_str(char *buf, size_t buflen, const char *msg)
{
	move(LINES - 2, 0);
	clrtoeol();
	mvprintw(LINES - 2, 0, "%s", msg);
	move(LINES - 1, 0);
	clrtoeol();
	echo();
	curs_set(1);
	getnstr(buf, (int)buflen - 1);
	noecho();
	curs_set(0);
}

static bool parse_i64(const char *s, int64_t *out)
{
	char *end = NULL;
	errno = 0;
	long long v = strtoll(s, &end, 0);
	if (errno || end == s || (end && *end != '\0'))
		return false;
	*out = (int64_t)v;
	return true;
}

static bool parse_i32(const char *s, int *out)
{
	int64_t v;
	if (!parse_i64(s, &v))
		return false;
	if (v < INT32_MIN || v > INT32_MAX)
		return false;
	*out = (int)v;
	return true;
}

/* ---- state menu (drawn inline, returns selected enum or -1) ---- */
static int state_menu(void)
{
	int sel = 0;
	int row_base = LINES - 2 - N_PIN_STATES;
	if (row_base < 4) row_base = 4;

	timeout(-1); /* block until keypress */
	while (1) {
		for (int i = 0; i < N_PIN_STATES; i++) {
			move(row_base + i, 0);
			clrtoeol();
			if (i == sel) attron(A_REVERSE);
			mvprintw(row_base + i, 2, "[%d] %s", pin_states[i].value, pin_states[i].name);
			if (i == sel) attroff(A_REVERSE);
		}
		move(row_base + N_PIN_STATES, 0);
		clrtoeol();
		mvprintw(row_base + N_PIN_STATES, 0, "Up/Down to pick, Enter to confirm, q to cancel");
		refresh();

		int ch = getch();
		if (ch == 'q' || ch == 'Q' || ch == 27) { /* Esc */
			timeout(250);
			return -1;
		}
		if (ch == KEY_UP && sel > 0) sel--;
		if (ch == KEY_DOWN && sel < N_PIN_STATES - 1) sel++;
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			timeout(250);
			return pin_states[sel].value;
		}
	}
}

/* ---- data loading ---- */
static int load_device_status(struct ynl_sock *sock, uint32_t dev_id, DeviceStatus *out)
{
	struct dpll_device_get_req req = {0};
	req._present.id = 1;
	req.id = dev_id;

	struct dpll_device_get_rsp *rsp = dpll_device_get(sock, &req);
	if (!rsp) return -1;

	out->device_id = dev_id;
	out->lock_status = rsp->_present.lock_status ? rsp->lock_status : DPLL_LOCK_STATUS_UNLOCKED;
	out->mode = rsp->_present.mode ? rsp->mode : (enum dpll_mode)0;
	dpll_device_get_rsp_free(rsp);
	return 0;
}

static size_t build_pin_rows(struct ynl_sock *sock, uint32_t dev_id, PinRow **out)
{
	struct dpll_pin_get_req_dump dreq = {0};
	struct dpll_pin_get_list *list = dpll_pin_get_dump(sock, &dreq);
	size_t cap = 64, n = 0;
	PinRow *rows = calloc(cap, sizeof(*rows));
	if (!rows) { if (list) dpll_pin_get_list_free(list); return 0; }

	if (list) {
		ynl_dump_foreach(list, pin) {
			if (!pin->_present.id) continue;

			struct dpll_pin_parent_device *pd = NULL;
			if (pin->parent_device) {
				for (unsigned i = 0; i < pin->_count.parent_device; i++) {
					if (pin->parent_device[i]._present.parent_id &&
					    pin->parent_device[i].parent_id == dev_id) {
						pd = &pin->parent_device[i];
						break;
					}
				}
			}
			if (!pd) continue;

			if (n == cap) {
				cap *= 2;
				PinRow *nr = realloc(rows, cap * sizeof(*rows));
				if (!nr) break;
				rows = nr;
			}
			rows[n].pin_id = pin->id;
			rows[n].package_label = pin->package_label ? pin->package_label : NULL;
			rows[n].label = rows[n].package_label;
			rows[n].state = pd->_present.state ? (int)pd->state : -1;
			rows[n].prio = pd->_present.prio ? (int)pd->prio : -1;
			rows[n].has_phase_offset = pd->_present.phase_offset ? true : false;
			rows[n].phase_offset = pd->_present.phase_offset ? (int64_t)pd->phase_offset : 0;
			n++;
		}
		dpll_pin_get_list_free(list);
	}
	*out = rows;
	return n;
}

/* ---- drawing ---- */
static void draw_header(const DeviceStatus *ds, int sel_dev, int ndev)
{
	erase();
	attron(A_BOLD);
	mvprintw(0, 0, " dpll-tui  |  device %d/%d: %s  id=%" PRIu32
	         "  |  lock=%s  mode=%s",
	         sel_dev + 1, ndev, ds->type_name, ds->device_id,
	         dpll_lock_status_str(ds->lock_status),
	         dpll_mode_str(ds->mode));
	attroff(A_BOLD);
	mvprintw(1, 0, " q quit | Tab device | Up/Down select | s state | p prio | a phase_adj | r refresh");
	mvhline(2, 0, ACS_HLINE, COLS);
}

static void draw_pins(const PinRow *rows, size_t n, size_t sel, int top)
{
	int hdr_row = 3, data_start = 4;
	attron(A_UNDERLINE);
	mvprintw(hdr_row, 0, " %-4s %-6s %-14s %-6s %-14s %-s",
	         "#", "pin_id", "state", "prio", "phase_off(fs)", "label");
	attroff(A_UNDERLINE);

	int visible = LINES - data_start - 2;
	for (int i = 0; i < visible; i++) {
		int idx = top + i;
		if (idx < 0 || (size_t)idx >= n) break;

		const PinRow *r = &rows[idx];
		char ph[24];
		if (r->has_phase_offset)
			snprintf(ph, sizeof(ph), "%" PRId64, r->phase_offset);
		else
			snprintf(ph, sizeof(ph), "-");

		if ((size_t)idx == sel) attron(A_REVERSE);
		mvprintw(data_start + i, 0, " %-4d %-6" PRIu32 " %-14s %-6d %-14s %-.*s",
		         idx, r->pin_id, state_name(r->state), r->prio, ph,
		         COLS - 50, r->label ? r->label : "(null)");
		if ((size_t)idx == sel) attroff(A_REVERSE);
	}
}

/* ---- main ---- */
int main(void)
{
	g_log_level = LOG_LEVEL_ERROR;

	if (init_dpll() != 0) {
		fprintf(stderr, "init_dpll failed (cannot create YNL netlink socket)\n");
		return 1;
	}

	uint32_t dev_ids[8];
	enum dpll_type dev_types[8];
	int dev_count = 0;

	int id;
	id = dpll_find_device_id_by_type(ys, DPLL_TYPE_PPS);
	if (id >= 0) { dev_ids[dev_count] = (uint32_t)id; dev_types[dev_count] = DPLL_TYPE_PPS; dev_count++; }
	id = dpll_find_device_id_by_type(ys, DPLL_TYPE_EEC);
	if (id >= 0 && dev_count < 8) { dev_ids[dev_count] = (uint32_t)id; dev_types[dev_count] = DPLL_TYPE_EEC; dev_count++; }

	if (!dev_count) {
		fprintf(stderr, "No DPLL devices found via netlink\n");
		return 1;
	}

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(250);

	int sel_dev = 0;
	size_t sel_pin = 0;
	int top = 0;
	PinRow *rows = NULL;
	size_t nrows = 0;
	time_t last_refresh = 0;

	while (1) {
		time_t now = time(NULL);
		if (now != (time_t)-1 && (now - last_refresh) >= 1) {
			free(rows); rows = NULL;
			nrows = build_pin_rows(ys, dev_ids[sel_dev], &rows);
			if (sel_pin >= nrows) sel_pin = nrows ? nrows - 1 : 0;
			if (top > (int)sel_pin) top = (int)sel_pin;
			last_refresh = now;
		}

		DeviceStatus ds = {
			.device_id = dev_ids[sel_dev],
			.type = dev_types[sel_dev],
			.type_name = type_str(dev_types[sel_dev]),
			.lock_status = DPLL_LOCK_STATUS_UNLOCKED,
			.mode = (enum dpll_mode)0,
		};
		(void)load_device_status(ys, dev_ids[sel_dev], &ds);

		draw_header(&ds, sel_dev, dev_count);
		draw_pins(rows, nrows, sel_pin, top);
		refresh();

		int ch = getch();
		if (ch == ERR) continue;
		if (ch == 'q' || ch == 'Q' || ch == 27) break;

		if (ch == '\t') {
			sel_dev = (sel_dev + 1) % dev_count;
			sel_pin = 0; top = 0; last_refresh = 0;
			continue;
		}
		if (ch == KEY_UP) {
			if (sel_pin > 0) sel_pin--;
			if (top > (int)sel_pin) top = (int)sel_pin;
			continue;
		}
		if (ch == KEY_DOWN) {
			if (nrows && sel_pin + 1 < nrows) sel_pin++;
			int vis = LINES - 4 - 2;
			if ((int)sel_pin >= top + vis) top = (int)sel_pin - vis + 1;
			continue;
		}
		if (ch == 'r' || ch == 'R') { last_refresh = 0; continue; }

		if (!rows || !nrows || sel_pin >= nrows) continue;
		const char *pkg = rows[sel_pin].package_label;
		if (!pkg || !*pkg) { status_msg("Selected pin has no package_label"); continue; }

		if (ch == 's' || ch == 'S') {
			int v = state_menu();
			if (v >= 0) {
				int old = dpll_pin_set_state(ys, dev_ids[sel_dev], (char *)pkg, v);
				status_msg("set_state(%s, %s): old=%s", pkg, state_name(v), old >= 0 ? state_name(old) : "err");
				last_refresh = 0;
			}
			continue;
		}
		if (ch == 'p' || ch == 'P') {
			char in[64] = {0};
			prompt_str(in, sizeof(in), "Priority (int): ");
			int v;
			if (parse_i32(in, &v)) {
				int old = dpll_pin_set_priority(ys, dev_ids[sel_dev], (char *)pkg, v);
				status_msg("set_priority(%s, %d): old=%d", pkg, v, old);
				last_refresh = 0;
			} else {
				status_msg("invalid input");
			}
			continue;
		}
		if (ch == 'a' || ch == 'A') {
			char in[64] = {0};
			prompt_str(in, sizeof(in), "Phase adjust (fs, int64): ");
			int64_t v;
			if (parse_i64(in, &v)) {
				int64_t old = dpll_pin_set_phase_adjust(ys, (char *)pkg, v);
				status_msg("set_phase_adjust(%s, %" PRId64 "): old=%" PRId64, pkg, v, old);
				last_refresh = 0;
			} else {
				status_msg("invalid input");
			}
			continue;
		}
	}

	endwin();
	free(rows);
	return 0;
}
