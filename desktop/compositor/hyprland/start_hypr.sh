#!/bin/env bash

# designed to be run from a terminal
# script located in tilde

START=$(date +%s)

pwrapper() {
	echo "Running: $*"
	"$@"
}

{
	echo "Started from: $(tty) at $(date)"
	echo
	pwrapper dbus-run-session Hyprland

	END=$(date +%s)
	DURATION=$((END - START))
	HOURS=$((DURATION / 3600))
	MINUTES=$(((DURATION % 3600) / 60))
	SECONDS=$((DURATION % 60))

	echo
	echo "Session ended at $(date)"
	echo "Duration: ${HOURS}h ${MINUTES}m ${SECONDS}s"
} | less +G
