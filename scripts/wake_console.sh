#!/bin/bash
# Wakes the console from sleep by power-cycling the smart plug it's
# plugged into, via Home Assistant. Kept as a standalone script (not
# compiled into capture2cloud) so swapping the plug/service later never
# requires touching the C code.
#
# Written against a Nintendo Switch (where a power cut reliably wakes it)
# but nothing here is Switch-specific -- capture2cloud captures and
# drives any console, so this stays deliberately generic.
#
# Logic:
#   - plug currently OFF -> turn it on, confirm it actually reports on.
#   - plug currently ON  -> turn it off, confirm it actually reports
#     off, hold for OFF_HOLD_SECONDS, turn it back on, confirm it
#     reports on again. The real power loss in the middle is what
#     triggers the console to wake -- simply "making sure it's on"
#     wouldn't do anything if it was already on.
#
# Usage: ./wake_switch.sh [entity_id]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"

if [ ! -f "$ENV_FILE" ]; then
    echo "Error: $ENV_FILE not found. Copy .env.example to .env and fill in HA_URL/HA_TOKEN." >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$ENV_FILE"

: "${HA_URL:?HA_URL not set in .env}"
: "${HA_TOKEN:?HA_TOKEN not set in .env}"

ENTITY_ID="${1:-${HA_PLUG_ENTITY:-switch.prise_trigger}}"
# How long the plug stays off before coming back. The console treats the
# power cut as its wake trigger, and 2s was not always long enough for it
# to register.
OFF_HOLD_SECONDS=3
POLL_INTERVAL=1
POLL_TIMEOUT=10

get_state() {
    curl -sS -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" \
        "$HA_URL/api/states/$ENTITY_ID" | jq -r '.state'
}

call_service() {
    local service="$1"
    local code
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" \
        -d "{\"entity_id\": \"$ENTITY_ID\"}" \
        "$HA_URL/api/services/switch/$service")
    if [ "$code" != "200" ]; then
        echo "Error: switch.$service returned HTTP $code" >&2
        exit 1
    fi
}

# Polls get_state until it matches $1, up to POLL_TIMEOUT seconds.
wait_for_state() {
    local expected="$1"
    local waited=0
    while [ "$waited" -lt "$POLL_TIMEOUT" ]; do
        if [ "$(get_state)" = "$expected" ]; then
            return 0
        fi
        sleep "$POLL_INTERVAL"
        waited=$((waited + POLL_INTERVAL))
    done
    return 1
}

echo "Checking current state of $ENTITY_ID..."
current_state="$(get_state)"
echo "Current state: $current_state"

case "$current_state" in
    off)
        echo "Plug is off -- turning on..."
        call_service turn_on
        if wait_for_state on; then
            echo "Confirmed: plug is on."
        else
            echo "Error: plug did not turn on within ${POLL_TIMEOUT}s." >&2
            exit 1
        fi
        ;;
    on)
        echo "Plug is on -- power-cycling to trigger a wake..."
        call_service turn_off
        if ! wait_for_state off; then
            echo "Error: plug did not turn off within ${POLL_TIMEOUT}s." >&2
            exit 1
        fi
        echo "Confirmed: plug is off. Waiting ${OFF_HOLD_SECONDS}s..."
        sleep "$OFF_HOLD_SECONDS"
        call_service turn_on
        if ! wait_for_state on; then
            echo "Error: plug did not turn back on within ${POLL_TIMEOUT}s." >&2
            exit 1
        fi
        echo "Confirmed: plug is back on."
        ;;
    *)
        echo "Error: unexpected state '$current_state' for $ENTITY_ID (expected on/off)." >&2
        exit 1
        ;;
esac

echo "Done."
