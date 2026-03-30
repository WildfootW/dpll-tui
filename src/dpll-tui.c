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
	{ DPLL_PIN_STATE_CONNECTED,    "conn"  },
	{ DPLL_PIN_STATE_DISCONNECTED, "disc"  },
	{ DPLL_PIN_STATE_SELECTABLE,   "sel"   },
};
#define N_PIN_STATES (int)(sizeof(pin_states) / sizeof(pin_states[0]))

static const char *state_name(int st)
{
	for (int i = 0; i < N_PIN_STATES; i++)
		if (pin_states[i].value == st)
			return pin_states[i].name;
	return "?";
}

static const char *state_name_full(int st)
{
	switch (st) {
	case DPLL_PIN_STATE_CONNECTED:    return "connected";
	case DPLL_PIN_STATE_DISCONNECTED: return "disconnected";
	case DPLL_PIN_STATE_SELECTABLE:   return "selectable";
	default:                          return "?";
	}
}

static const char *pin_type_str(int t)
{
	switch (t) {
	case DPLL_PIN_TYPE_MUX:             return "MUX";
	case DPLL_PIN_TYPE_EXT:             return "EXT";
	case DPLL_PIN_TYPE_SYNCE_ETH_PORT:  return "SyncE";
	case DPLL_PIN_TYPE_INT_OSCILLATOR:  return "Osc";
	case DPLL_PIN_TYPE_GNSS:            return "GNSS";
	default:                            return "?";
	}
}

static const char *pin_dir_str(int d)
{
	switch (d) {
	case DPLL_PIN_DIRECTION_INPUT:  return "input";
	case DPLL_PIN_DIRECTION_OUTPUT: return "output";
	default:                        return "?";
	}
}

static const char *lock_err_str(int e)
{
	switch (e) {
	case DPLL_LOCK_STATUS_ERROR_NONE:      return "none";
	case DPLL_LOCK_STATUS_ERROR_UNDEFINED: return "undefined";
	case DPLL_LOCK_STATUS_ERROR_MEDIA_DOWN: return "media-down";
	case DPLL_LOCK_STATUS_ERROR_FRACTIONAL_FREQUENCY_OFFSET_TOO_HIGH:
		return "ffo-high";
	default: return "-";
	}
}

static void caps_str(uint32_t c, char *buf, size_t len)
{
	buf[0] = '\0';
	if (c & DPLL_PIN_CAPABILITIES_DIRECTION_CAN_CHANGE)
		strncat(buf, "dir,", len - strlen(buf) - 1);
	if (c & DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE)
		strncat(buf, "prio,", len - strlen(buf) - 1);
	if (c & DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE)
		strncat(buf, "state,", len - strlen(buf) - 1);
	size_t l = strlen(buf);
	if (l > 0 && buf[l - 1] == ',')
		buf[l - 1] = '\0';
	if (!buf[0])
		strncpy(buf, "-", len);
}

/* ---- per-pin row ---- */
typedef struct {
	const char *label;
	const char *package_label;
	const char *board_label;
	const char *panel_label;
	uint32_t pin_id;
	int state;
	int prio;
	int pin_type;
	int direction;
	bool has_phase_offset;
	int64_t phase_offset;
	bool has_frequency;
	uint64_t frequency;
	bool has_phase_adjust;
	int32_t phase_adjust;
	bool has_ffo;
	int64_t ffo;
	uint32_t capabilities;
} PinRow;

/* ---- device status ---- */
typedef struct {
	uint32_t device_id;
	enum dpll_type type;
	const char *type_name;
	enum dpll_lock_status lock_status;
	enum dpll_mode mode;
	char module_name[64];
	bool has_clock_id;
	uint64_t clock_id;
	int lock_status_error;
	enum dpll_mode mode_supported[8];
	int n_mode_supported;
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
			mvprintw(row_base + i, 2, "[%d] %s", pin_states[i].value, state_name_full(pin_states[i].value));
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

	out->module_name[0] = '\0';
	if (rsp->module_name)
		strncpy(out->module_name, rsp->module_name, sizeof(out->module_name) - 1);

	out->has_clock_id = rsp->_present.clock_id ? true : false;
	out->clock_id = rsp->_present.clock_id ? rsp->clock_id : 0;

	out->lock_status_error = rsp->_present.lock_status_error
		? (int)rsp->lock_status_error : -1;

	out->n_mode_supported = 0;
	if (rsp->_count.mode_supported) {
		for (unsigned i = 0; i < rsp->_count.mode_supported &&
		     i < 8; i++) {
			out->mode_supported[out->n_mode_supported++] =
				(enum dpll_mode)rsp->mode_supported[i];
		}
	}

	dpll_device_get_rsp_free(rsp);
	return 0;
}

static void free_pin_rows(PinRow *rows, size_t n)
{
	if (!rows) return;
	for (size_t i = 0; i < n; i++) {
		free((void *)rows[i].package_label);
		free((void *)rows[i].board_label);
		free((void *)rows[i].panel_label);
	}
	free(rows);
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
			rows[n].package_label = pin->package_label ? strdup(pin->package_label) : NULL;
			rows[n].board_label = pin->board_label ? strdup(pin->board_label) : NULL;
			rows[n].panel_label = pin->panel_label ? strdup(pin->panel_label) : NULL;
			rows[n].label = rows[n].package_label;
			rows[n].state = pd->_present.state ? (int)pd->state : -1;
			rows[n].prio = pd->_present.prio ? (int)pd->prio : -1;
			rows[n].has_phase_offset = pd->_present.phase_offset ? true : false;
			rows[n].phase_offset = pd->_present.phase_offset ? (int64_t)pd->phase_offset : 0;
			rows[n].pin_type = pin->_present.type ? (int)pin->type : -1;
			rows[n].direction = pd->_present.direction ? (int)pd->direction : -1;
			rows[n].has_frequency = pin->_present.frequency ? true : false;
			rows[n].frequency = pin->_present.frequency ? pin->frequency : 0;
			rows[n].has_phase_adjust = pin->_present.phase_adjust ? true : false;
			rows[n].phase_adjust = pin->_present.phase_adjust ? pin->phase_adjust : 0;
			rows[n].has_ffo = pin->_present.fractional_frequency_offset ? true : false;
			rows[n].ffo = pin->_present.fractional_frequency_offset ? pin->fractional_frequency_offset : 0;
			rows[n].capabilities = pin->_present.capabilities ? pin->capabilities : 0;
			n++;
		}
		dpll_pin_get_list_free(list);
	}
	*out = rows;
	return n;
}

/* ---- drawing ---- */
#define HDR_ROWS  4   /* rows 0-3: header area */
#define DETAIL_ROWS 4 /* rows at bottom: detail pane + status */

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

	char modes_buf[128] = "";
	for (int i = 0; i < ds->n_mode_supported; i++) {
		if (i) strncat(modes_buf, ",", sizeof(modes_buf) - strlen(modes_buf) - 1);
		strncat(modes_buf, dpll_mode_str(ds->mode_supported[i]),
			sizeof(modes_buf) - strlen(modes_buf) - 1);
	}
	if (!modes_buf[0]) strncpy(modes_buf, "-", sizeof(modes_buf));

	mvprintw(1, 0, " module=%-12s clock=0x%016" PRIx64 "  modes=[%s]  lock_err=%s",
	         ds->module_name[0] ? ds->module_name : "-",
	         ds->clock_id,
	         modes_buf,
	         lock_err_str(ds->lock_status_error));

	mvprintw(2, 0, " q quit | Tab device | Up/Down select | s state | p prio | a phase_adj | r refresh");
	mvhline(3, 0, ACS_HLINE, COLS);
}

static void draw_pins(const PinRow *rows, size_t n, size_t sel, int top)
{
	int hdr_row = HDR_ROWS, data_start = HDR_ROWS + 1;
	attron(A_UNDERLINE);
	mvprintw(hdr_row, 0, " %-4s %-6s %-6s %-5s %-6s %-16s %-14s %-s",
	         "#", "pin_id", "type", "state", "prio", "freq", "phase_off(fs)", "label");
	attroff(A_UNDERLINE);

	int visible = LINES - data_start - DETAIL_ROWS;
	if (visible < 1) visible = 1;
	for (int i = 0; i < visible; i++) {
		int idx = top + i;
		if (idx < 0 || (size_t)idx >= n) break;

		const PinRow *r = &rows[idx];
		char ph[24], freq[20];
		if (r->has_phase_offset)
			snprintf(ph, sizeof(ph), "%" PRId64, r->phase_offset);
		else
			snprintf(ph, sizeof(ph), "-");
		if (r->has_frequency)
			snprintf(freq, sizeof(freq), "%" PRIu64, r->frequency);
		else
			snprintf(freq, sizeof(freq), "-");

		if ((size_t)idx == sel) attron(A_REVERSE);
		mvprintw(data_start + i, 0, " %-4d %-6" PRIu32 " %-6s %-5s %-6d %-16s %-14s %-.*s",
		         idx, r->pin_id,
		         r->pin_type >= 0 ? pin_type_str(r->pin_type) : "-",
		         state_name(r->state), r->prio, freq, ph,
		         COLS - 65, r->label ? r->label : "(null)");
		if ((size_t)idx == sel) attroff(A_REVERSE);
	}
}

static void draw_pin_detail(const PinRow *r)
{
	int base = LINES - DETAIL_ROWS;
	mvhline(base, 0, ACS_HLINE, COLS);

	char padj_buf[32], ffo_buf[32], caps_buf[64];
	if (r->has_phase_adjust)
		snprintf(padj_buf, sizeof(padj_buf), "%" PRId32 " fs", r->phase_adjust);
	else
		snprintf(padj_buf, sizeof(padj_buf), "-");
	if (r->has_ffo)
		snprintf(ffo_buf, sizeof(ffo_buf), "%" PRId64 " ppb", r->ffo);
	else
		snprintf(ffo_buf, sizeof(ffo_buf), "-");
	caps_str(r->capabilities, caps_buf, sizeof(caps_buf));

	mvprintw(base + 1, 0, " pin %-4" PRIu32 " dir=%-6s  phase_adj=%-12s  ffo=%s",
	         r->pin_id,
	         r->direction >= 0 ? pin_dir_str(r->direction) : "-",
	         padj_buf, ffo_buf);
	mvprintw(base + 2, 0, "      board=%-20s  panel=%-20s  caps=[%s]",
	         r->board_label ? r->board_label : "-",
	         r->panel_label ? r->panel_label : "-",
	         caps_buf);
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
			free_pin_rows(rows, nrows); rows = NULL;
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
			.lock_status_error = -1,
		};
		(void)load_device_status(ys, dev_ids[sel_dev], &ds);

		draw_header(&ds, sel_dev, dev_count);
		draw_pins(rows, nrows, sel_pin, top);
		if (rows && nrows && sel_pin < nrows)
			draw_pin_detail(&rows[sel_pin]);
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
			int vis = LINES - (HDR_ROWS + 1) - DETAIL_ROWS;
			if (vis < 1) vis = 1;
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
				status_msg("set_state(%s, %s): old=%s", pkg, state_name_full(v), old >= 0 ? state_name_full(old) : "err");
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
	free_pin_rows(rows, nrows);
	return 0;
}
