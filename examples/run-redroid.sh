#!/usr/bin/env bash

set -euo pipefail

CPU_CORE=${CPU_CORE:-4}
MEM_SIZE=${MEM_SIZE:-4g}
ADB_PORT=${ADB_PORT:-5556}
CONTAINER_NAME=${CONTAINER_NAME:-floral}
IMAGE=${IMAGE:-floral-houdini-ndk-magisk:latest}
INSTANCE_DIR=${INSTANCE_DIR:-/srv/floral/instance-01}
LXCFS_ROOT=${LXCFS_ROOT:-/var/lib/floral-lxcfs}

case "$CPU_CORE" in
  ''|*[!0-9]*)
    echo "CPU_CORE must be a positive integer" >&2
    exit 2
    ;;
esac
if (( CPU_CORE < 1 )); then
  echo "CPU_CORE must be a positive integer" >&2
  exit 2
fi

# Use the launcher's effective host cpuset and select the requested number of CPUs.
# This constrains the real cgroup, so /proc/self/status cannot expose extra CPUs.
allowed_cpus=$(awk '/^Cpus_allowed_list:/ { print $2; exit }' /proc/self/status)
cpuset=$(awk -v spec="$allowed_cpus" -v want="$CPU_CORE" '
  BEGIN {
    count = split(spec, ranges, ",")
    for (i = 1; i <= count && selected < want; ++i) {
      parts = split(ranges[i], bounds, "-")
      first = bounds[1] + 0
      last = parts == 2 ? bounds[2] + 0 : first
      for (cpu = first; cpu <= last && selected < want; ++cpu) {
        output = output (output == "" ? "" : ",") cpu
        ++selected
      }
    }
    if (selected != want) exit 1
    print output
  }
') || {
  echo "Only '$allowed_cpus' is available; cannot allocate $CPU_CORE CPUs" >&2
  exit 1
}

exec docker run -d \
  --name "$CONTAINER_NAME" \
  --privileged \
  --network bridge \
  --cpus "$CPU_CORE" \
  --cpuset-cpus "$cpuset" \
  --memory "$MEM_SIZE" \
  -p "$ADB_PORT:5555" \
  --mount "type=bind,src=$INSTANCE_DIR,dst=/ipc/floral_stream" \
  --mount "type=bind,src=$LXCFS_ROOT,dst=/run/floral-lxcfs,readonly" \
  "$IMAGE"
