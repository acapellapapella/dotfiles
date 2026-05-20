#!/bin/env bash

APP_ID="spotify"

get() {
	track=$(niri msg --json windows | jq -r '.[] | select(.app_id=="'$APP_ID'") | "" + .title')

	if [[ -n "$track" && "$track" != "" ]]; then
		echo "$track"
	else
		echo ""
	fi
}

while true; do
	get; sleep 1
done
