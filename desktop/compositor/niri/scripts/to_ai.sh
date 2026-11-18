#!/bin/env bash
# located in ~/.local/bin/to_ai.sh

# u can set your browser name
APP_ID="helium"

# define window
WIN_ID=$(niri msg --json windows | \
  	jq -r --arg app "$APP_ID" \
   '.[] | select(.app_id == $app) | .id' | head -1)

if xdg-open https://chat.openai.com/; then
	if [ -n "$WIN_ID" ]; then
  		niri msg action focus-window --id "$WIN_ID"
	else
		exit 1
	fi
else
	exit 1
fi
