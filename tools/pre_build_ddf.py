Import("env")

import os
import subprocess
import sys

# Regenerates src/ddf_zip.h from ddf-source/ before every build.
#
# The header is compiled into the firmware, so editing ddf-source/ alone
# changes nothing that the device actually serves - the generator has to run,
# and until now it had to be remembered. It was not: a new object type was
# declared in device.json, the firmware was rebuilt and flashed, and the
# device went on serving the previous DDF. The designer, doing exactly the
# right thing, fetched that stale DDF live from the device and greyed the new
# tool out. The evidence was in every HIL run - an unchanged ddfHash across a
# change to device.json, which is impossible - and it took a person noticing
# a greyed-out button to catch it (2026-08-23).
#
# generate-ddf-header.js's own header already suggested wiring it into CI or
# a hook, and says its output is byte-deterministic by construction (fixed
# DOS timestamps, entries sorted by name). So running it unconditionally
# rewrites nothing when nothing changed, and a build never churns the tree.
#
# The same gap exists in every other firmware in this family; each carries
# its own copy of the generator and its own platformio.ini.

# env["PROJECT_DIR"], not __file__ - SCons execs this script rather than
# importing it, so __file__ is not defined here.
REPO = env["PROJECT_DIR"]
GENERATOR = os.path.join(REPO, "tools", "generate-ddf-header.js")


try:
    result = subprocess.run(["node", GENERATOR], cwd=REPO, capture_output=True, text=True)
except (OSError, FileNotFoundError):
    # No node on this machine. Loud rather than silent, because the
    # alternative is a firmware that serves a DDF older than its own source
    # and gives no sign of it - but not fatal, since someone building
    # without node can still produce a working image from a checked-in
    # header that is already current.
    print("[ddf] WARNING: node not found - ddf_zip.h was NOT regenerated.")
    print("[ddf] WARNING: the compiled-in DDF may be older than ddf-source/.")
else:
    if result.returncode != 0:
        print("[ddf] generate-ddf-header.js failed:")
        print(result.stdout)
        print(result.stderr)
        sys.exit(1)
    for line in result.stdout.splitlines():
        if line.strip():
            print("[ddf] " + line.strip())
