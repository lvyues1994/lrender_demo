#!/usr/bin/env bash
# Reusable GPU acceptance for E1–E8.
# Incomplete (exit 2) if display locked/blank OR any case SKIP.
# Fail (exit 1) if any case FAIL.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${DEMO_BUILD_DIR:-$ROOT/build}"
CTEST_BIN="${CTEST_BIN:-ctest}"

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

resolve_session() {
  if [[ -n "${XDG_SESSION_ID:-}" ]]; then
    echo "$XDG_SESSION_ID"
    return
  fi
  local display="${DISPLAY:-}"
  local bare="${display%%.*}"
  local id match=""
  local -a graphical=()
  while read -r id _; do
    [[ -z "$id" ]] && continue
    local props
    props="$(loginctl show-session "$id" -p Display -p Type -p Class -p Active -p Remote 2>/dev/null || true)"
    local sess_display type class active remote
    sess_display="$(printf '%s\n' "$props" | sed -n 's/^Display=//p')"
    type="$(printf '%s\n' "$props" | sed -n 's/^Type=//p')"
    class="$(printf '%s\n' "$props" | sed -n 's/^Class=//p')"
    active="$(printf '%s\n' "$props" | sed -n 's/^Active=//p')"
    remote="$(printf '%s\n' "$props" | sed -n 's/^Remote=//p')"
    local sd="${sess_display%%.*}"
    if [[ -n "$bare" && -n "$sess_display" && ( "$sd" == "$bare" || "$sess_display" == "$display" ) ]]; then
      match="$id"
    fi
    if [[ "$class" == "user" && "$remote" != "yes" && ( "$type" == "x11" || "$type" == "wayland" ) && "$active" == "yes" ]]; then
      graphical+=("$id")
    fi
  done < <(loginctl list-sessions --no-legend 2>/dev/null || true)
  if [[ -n "$match" ]]; then
    echo "$match"
    return
  fi
  if [[ ${#graphical[@]} -eq 1 ]]; then
    echo "${graphical[0]}"
    return
  fi
  echo ""
}

screen_locked_or_blank() {
  local session
  session="$(resolve_session)"
  if [[ -n "$session" ]]; then
    local hint
    hint="$(loginctl show-session "$session" -p LockedHint 2>/dev/null || true)"
    if [[ "$hint" == *LockedHint=yes* ]]; then
      echo "loginctl session $session LockedHint=yes"
      return 0
    fi
  fi
  local xq
  xq="$(xset q 2>/dev/null || true)"
  if [[ "$xq" == *"Monitor is Off"* ]]; then
    echo "xset: Monitor is Off"
    return 0
  fi
  return 1
}

echo "=== acceptance_gpu.sh (E1–E8 reusable) ==="
echo "ROOT=$ROOT BUILD=$BUILD"
echo "DISPLAY=${DISPLAY:-} XDG_SESSION_ID=${XDG_SESSION_ID:-}"
echo "session=$(resolve_session)"

detail=""
if detail="$(screen_locked_or_blank)"; then
  red "INCOMPLETE: display locked or blank — $detail"
  red "Waiting for unlock. Do not treat this run as acceptance."
  exit 2
fi
green "display unlocked and monitor on"

if [[ ! -d "$BUILD" ]]; then
  red "INCOMPLETE: build dir missing: $BUILD"
  exit 2
fi

LOG_DIR="${TMPDIR:-/tmp}/acceptance_gpu_$$"
mkdir -p "$LOG_DIR"
trap 'rm -rf "$LOG_DIR"' EXIT

failed=0
skipped=0

run_ctest() {
  local name="$1"
  local log="$LOG_DIR/$name.log"
  yellow ">> $name"
  local rc=0
  "$CTEST_BIN" --test-dir "$BUILD" -R "^${name}$" --output-on-failure -V >"$log" 2>&1 || rc=$?

  if grep -Eq '^\s*[0-9]+/[0-9]+ Test\s+#.+:.+(Skipped|\*\*\*Skipped)' "$log" \
     || grep -Eq 'SKIP_RETURN_CODE|black-flash SKIP|exit code[: ]+77' "$log"; then
    red "INCOMPLETE: $name SKIPPED"
    tail -40 "$log"
    skipped=1
    return 0
  fi
  if [[ $rc -ne 0 ]]; then
    red "FAIL: $name (ctest exit=$rc)"
    tail -60 "$log"
    failed=1
    return 0
  fi
  # Also treat "No tests were found" as fail
  if grep -Eq 'No tests were found|0 tests failed out of 0' "$log" && ! grep -Eq '1/1 Test' "$log"; then
    # ctest with -R miss returns 0 sometimes with 0 tests
    if ! grep -Eq 'Test #|1/1 Test' "$log"; then
      red "FAIL: $name — no matching test"
      failed=1
      return 0
    fi
  fi
  green "PASS: $name"
  grep -E 'blackSamples|hostChanges|sharedShownGap|latency|sharedShown|resize capture|Resize capture|host change|black-flash|DPR' "$log" \
    | tee -a "$LOG_DIR/metrics.txt" >/dev/null || true
  return 0
}

echo
echo "--- black-flash ×3 ---"
for i in 1 2 3; do
  # Distinct log names so scrapes keep all runs
  yellow ">> e2e_shared_black_flash ($i/3)"
  log="$LOG_DIR/e2e_shared_black_flash_$i.log"
  rc=0
  "$CTEST_BIN" --test-dir "$BUILD" -R '^e2e_shared_black_flash$' --output-on-failure -V >"$log" 2>&1 || rc=$?
  if grep -Eq '^\s*[0-9]+/[0-9]+ Test\s+#.+:.+(Skipped|\*\*\*Skipped)' "$log" \
     || grep -Eq 'SKIP_RETURN_CODE|black-flash SKIP|exit code[: ]+77' "$log"; then
    red "INCOMPLETE: e2e_shared_black_flash ($i/3) SKIPPED"
    tail -40 "$log"
    skipped=1
  elif [[ $rc -ne 0 ]]; then
    red "FAIL: e2e_shared_black_flash ($i/3) (ctest exit=$rc)"
    tail -60 "$log"
    failed=1
  else
    green "PASS: e2e_shared_black_flash ($i/3)"
    grep -E 'blackSamples|hostChanges|sharedShownGap|latency|sharedShown|resize capture|Resize capture|host change|black-flash' "$log" \
      | tee -a "$LOG_DIR/metrics.txt" >/dev/null || true
  fi
done

echo
echo "--- DPR1.25 ---"
run_ctest e2e_shared_dpr125

echo
echo "--- cycle ---"
run_ctest e2e_shared_cycle

echo
echo "--- 5 negatives ---"
for name in e2e_submit_fault e2e_fault_load e2e_fault_present e2e_zero_shared e2e_strict_offscreen; do
  run_ctest "$name"
done

echo
echo "=== SUMMARY METRICS ==="
if [[ -f "$LOG_DIR/metrics.txt" ]]; then
  cat "$LOG_DIR/metrics.txt"
else
  yellow "(no metric lines captured)"
fi
echo "--- scraped ---"
grep -hE 'blackSamples|hostChanges|sharedShownGap|latency|sharedShown|resize capture|Resize capture' "$LOG_DIR"/*.log 2>/dev/null | sort -u || true

echo
if [[ $skipped -ne 0 ]]; then
  red "INCOMPLETE: at least one case SKIPPED — not an acceptance run"
  exit 2
fi
if [[ $failed -ne 0 ]]; then
  red "FAIL: at least one case failed"
  exit 1
fi
green "ACCEPTANCE COMPLETE"
exit 0
