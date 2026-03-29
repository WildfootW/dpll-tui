#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
#
# deps_check_fix_build.sh
# Interactive dependency checker for dpll-tui.
#
# Dependencies: libynl (kernel-tools-libs-devel), ncurses-devel
#
# Usage:
#   ./scripts/deps_check_fix_build.sh            # check + build
#   ./scripts/deps_check_fix_build.sh --no-build # check only

set -eu

SCRIPT_NAME="$(basename "$0")"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

CC="${CC:-gcc}"
DO_BUILD=1

say()  { printf "%s\n" "$*" >&2; }
die()  { say "ERROR: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

ask_yes_no() {
  printf "%s [y/N] " "$1" >&2
  IFS= read -r ans || ans=""
  case "$ans" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

report() { printf "  %-24s %s\n" "$1:" "$2" >&2; }

while [ "${1:-}" != "" ]; do
  case "$1" in
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)
      say "Usage: $SCRIPT_NAME [--no-build]"
      say "Checks ynl headers + ncurses, then builds dpll-tui."
      exit 0 ;;
    *) die "unknown arg: $1" ;;
  esac
done

TMP="$(mktemp -d)"
trap "rm -rf '$TMP'" EXIT INT TERM

say ""
say "==> 1) Check ynl headers (<ynl/ynl.h>, <ynl/dpll-user.h>)"
cat >"$TMP/ynl.c" <<'EOF'
#include <string.h>
#include <ynl/ynl.h>
#include <ynl/dpll-user.h>
int main(void) { return 0; }
EOF

ynl_ok=0
if "$CC" "$TMP/ynl.c" -o "$TMP/ynl" 2>"$TMP/ynl.err"; then
  report "ynl_headers" "OK"
  ynl_ok=1
else
  report "ynl_headers" "FAIL"
  say "---- compiler output ----"
  tail -n 15 "$TMP/ynl.err" >&2
  say "-------------------------"
  say ""
  say "Fix: install the package that provides ynl dev headers."
  say "  RHEL/Fedora:   sudo dnf install kernel-tools-libs-devel"
  say "  Debian/Ubuntu: check apt-file search ynl/ynl.h"
fi

say ""
say "==> 2) Check ncurses (-lncurses, <curses.h>)"
cat >"$TMP/nc.c" <<'EOF'
#include <curses.h>
int main(void) { initscr(); endwin(); return 0; }
EOF

nc_ok=0
if "$CC" "$TMP/nc.c" -o "$TMP/nc" -lncurses 2>"$TMP/nc.err"; then
  report "ncurses" "OK"
  nc_ok=1
else
  report "ncurses" "FAIL"
  say "---- compiler output ----"
  tail -n 10 "$TMP/nc.err" >&2
  say "-------------------------"
  say ""
  say "Fix: install ncurses development package."
  say "  RHEL/Fedora:   sudo dnf install ncurses-devel"
  say "  Debian/Ubuntu: sudo apt-get install libncurses-dev"
fi

say ""
say "==> 3) Check libynl linker (-lynl)"
cat >"$TMP/lynl.c" <<'EOF'
#include <string.h>
#include <ynl/ynl.h>
int main(void) { return 0; }
EOF

lynl_ok=0
if "$CC" "$TMP/lynl.c" -o "$TMP/lynl" -lynl 2>"$TMP/lynl.err"; then
  report "libynl_link" "OK"
  lynl_ok=1
else
  report "libynl_link" "FAIL"
  say "---- compiler output ----"
  tail -n 10 "$TMP/lynl.err" >&2
  say "-------------------------"
  say ""
  say "Fix: ensure libynl.so is installed and on the linker path."
fi

say ""
say "==> Summary"
all_ok=1
[ "$ynl_ok"  -eq 1 ] || all_ok=0
[ "$nc_ok"   -eq 1 ] || all_ok=0
[ "$lynl_ok" -eq 1 ] || all_ok=0

if [ "$all_ok" -eq 1 ]; then
  say "All dependencies OK."
else
  say "Some dependencies are missing (see above)."
  if [ "$DO_BUILD" -eq 1 ]; then
    if ! ask_yes_no "Try building anyway?"; then
      die "Fix dependencies first, then re-run."
    fi
  fi
fi

if [ "$DO_BUILD" -eq 1 ]; then
  say ""
  say "==> Build dpll-tui"
  cd "$REPO_ROOT"
  make clean
  make
  say "Done: $REPO_ROOT/dpll-tui"
else
  say ""
  say "--no-build: skipping make."
fi
