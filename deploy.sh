#!/bin/bash

set -euo pipefail

# Accept arguments, default to 'n' and 'raspberrypi.local' if not provided.
REMOTE_USER=${1:-n}
REMOTE_HOST=${2:-raspberrypi.local}

BINARY="build/release-arm/digital_clock_v3"
SERVICE="digital-clock.service"
REMOTE_TARGET="~/digital_clock_v3"
CONTROL_PATH="${HOME}/.ssh/cm-%C"
SSH_OPTS=(
    -o ControlMaster=auto
    -o ControlPersist=10m
    -o ControlPath="${CONTROL_PATH}"
)
REMOTE="${REMOTE_USER}@${REMOTE_HOST}"
STARTED_MASTER=0

if [ ! -f "${BINARY}" ]; then
    echo "Missing ${BINARY}. Build it first with: cmake --build --preset release-arm" >&2
    exit 1
fi

mkdir -p "${HOME}/.ssh"

cleanup() {
    if [ "${STARTED_MASTER}" -eq 1 ]; then
        ssh -O exit "${SSH_OPTS[@]}" "${REMOTE}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if ssh -O check "${SSH_OPTS[@]}" "${REMOTE}" >/dev/null 2>&1; then
    echo "Reusing existing SSH connection to ${REMOTE}..."
else
    echo "Opening SSH connection to ${REMOTE}..."
    ssh -Nf "${SSH_OPTS[@]}" "${REMOTE}"
    STARTED_MASTER=1
fi

echo "Stopping ${SERVICE}..."
ssh -t "${SSH_OPTS[@]}" "${REMOTE}" "sudo systemctl stop ${SERVICE}"

echo "Copying ${BINARY} to ${REMOTE}:${REMOTE_TARGET}..."
scp "${SSH_OPTS[@]}" "${BINARY}" "${REMOTE}:${REMOTE_TARGET}"

echo "Starting ${SERVICE}..."
ssh -t "${SSH_OPTS[@]}" "${REMOTE}" "chmod +x ${REMOTE_TARGET} && sudo systemctl start ${SERVICE}"

echo "Checking ${SERVICE} status..."
ssh "${SSH_OPTS[@]}" "${REMOTE}" "systemctl is-active --quiet ${SERVICE} && systemctl --no-pager --full status ${SERVICE}"

echo "Deploy complete. ${SERVICE} is running."
