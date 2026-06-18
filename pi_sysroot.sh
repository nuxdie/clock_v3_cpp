#!/bin/bash

# Accept arguments, default to 'pi' and 'raspberrypi.local' if not provided
REMOTE_USER=${1:-pi}
REMOTE_HOST=${2:-raspberrypi.local}

# Define the local output directory
SYSROOT_DIR="pi_sysroot"
RSYNC_SSH="ssh -o ControlMaster=auto -o ControlPersist=10m -o ControlPath=${HOME}/.ssh/cm-%C"

echo "Creating sysroot for ${REMOTE_USER}@${REMOTE_HOST} in directory '${SYSROOT_DIR}'..."

# Create local directories
mkdir -p "${SYSROOT_DIR}/lib"
mkdir -p "${SYSROOT_DIR}/usr/lib"
mkdir -p "${SYSROOT_DIR}/usr/share"
mkdir -p "${HOME}/.ssh"

# Sync architecture-specific runtime libraries
echo "Syncing /lib/arm-linux-gnueabihf..."
rsync -avz -e "${RSYNC_SSH}" --rsync-path="rsync" "${REMOTE_USER}@${REMOTE_HOST}:/lib/arm-linux-gnueabihf" "${SYSROOT_DIR}/lib/"

# Sync /usr/include
echo "Syncing /usr/include..."
rsync -avz -e "${RSYNC_SSH}" --rsync-path="rsync" "${REMOTE_USER}@${REMOTE_HOST}:/usr/include" "${SYSROOT_DIR}/usr/"

# Sync architecture-specific development/runtime libraries
echo "Syncing /usr/lib/arm-linux-gnueabihf..."
rsync -avz -e "${RSYNC_SSH}" --rsync-path="rsync" "${REMOTE_USER}@${REMOTE_HOST}:/usr/lib/arm-linux-gnueabihf" "${SYSROOT_DIR}/usr/lib/"

# Sync pkg-config metadata
echo "Syncing pkg-config metadata..."
rsync -avz -e "${RSYNC_SSH}" --rsync-path="rsync" "${REMOTE_USER}@${REMOTE_HOST}:/usr/lib/pkgconfig" "${SYSROOT_DIR}/usr/lib/" || true
rsync -avz -e "${RSYNC_SSH}" --rsync-path="rsync" "${REMOTE_USER}@${REMOTE_HOST}:/usr/share/pkgconfig" "${SYSROOT_DIR}/usr/share/" || true

echo "Done."
