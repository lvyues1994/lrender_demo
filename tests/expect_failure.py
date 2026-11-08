"""Negative acceptance: require exit1 AND graceful root join, never any-nonzero."""
import subprocess
import sys
p = subprocess.run(sys.argv[2:], capture_output=True, text=True)
print(p.stdout, end="")
print(p.stderr, end="", file=sys.stderr)
ok = p.returncode == 1 and "viewer stopped GUI heartbeats=" in p.stdout and sys.argv[1] in (p.stdout+p.stderr)
print(f"expected-exit=1 actual-exit={p.returncode} graceful-root-join={'yes' if 'viewer stopped GUI heartbeats=' in p.stdout else 'no'} negative-acceptance={'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
