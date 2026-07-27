Import("env")
from datetime import datetime
import subprocess

ts = datetime.now().strftime("%b %d %Y %H:%M:%S")
try:
    branch = subprocess.check_output(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    branch = "unknown"

with open("src/build_info.h", "w") as f:
    f.write('#pragma once\n')
    f.write(f'#define BUILD_TIMESTAMP_STR "{ts}"\n')
    f.write(f'#define BUILD_BRANCH_STR "{branch}"\n')
