#!/bin/bash
# Read-only test: connects to Home Assistant and reads the current state
# of a switch entity (default: switch.prise_trigger, the smart plug
# meant to eventually trigger a Nintendo Switch wake-from-sleep by
# power-cycling it). Does NOT toggle anything -- this is only meant to
# confirm connectivity/auth/entity-id before any actual control script
# is written.
#
# Usage: ./check_plug_state.sh [entity_id]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"

if [ ! -f "$ENV_FILE" ]; then
    echo "Error: $ENV_FILE not found. Copy .env.example to .env and fill in HA_URL/HA_TOKEN." >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$ENV_FILE"

: "${HA_URL:?HA_URL not set in .env -- fill in your Home Assistant base URL}"
: "${HA_TOKEN:?HA_TOKEN not set in .env}"

ENTITY_ID="${1:-${HA_PLUG_ENTITY:-switch.prise_trigger}}"

echo "Checking $ENTITY_ID on $HA_URL ..."

response=$(curl -sS -w '\n%{http_code}' \
    -H "Authorization: Bearer $HA_TOKEN" \
    -H "Content-Type: application/json" \
    "$HA_URL/api/states/$ENTITY_ID")

http_code=$(echo "$response" | tail -n1)
body=$(echo "$response" | sed '$d')

if [ "$http_code" != "200" ]; then
    echo "Error: HTTP $http_code from Home Assistant" >&2
    echo "$body" >&2
    exit 1
fi

if command -v jq >/dev/null 2>&1; then
    state=$(echo "$body" | jq -r '.state')
    friendly=$(echo "$body" | jq -r '.attributes.friendly_name // empty')
    echo "State: $state${friendly:+ ($friendly)}"
else
    echo "$body"
fi
