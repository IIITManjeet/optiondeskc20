#!/bin/sh
# Record BTC market data in hourly journal chunks until told to stop.
#
#   tools/record_loop.sh [BUILD_DIR] [OUT_DIR]     (defaults: ~/build/optionsdesk, ~/rec)
#   touch ~/rec/STOP                               stop after the current chunk
#
# Runs od_feedd (restarted if it exits) and od_record --duration 3600 in a loop,
# writing OUT_DIR/btc_YYYY-MM-DD_HHMM.odj. An hour is ~400 MB; recording stops when
# less than MIN_FREE_MB is left on the output disk.
B=${1:-$HOME/build/optionsdesk}
R=${2:-$HOME/rec}
MIN_FREE_MB=${MIN_FREE_MB:-3000}
mkdir -p "$R"
rm -f "$R/STOP"
log() { echo "$(date '+%F %T') $*" >> "$R/loop.log"; }

FEED=
start_feed() {
    "$B/od_feedd" --currency BTC >> "$R/feedd_$(date +%F).log" 2>&1 &
    FEED=$!
    log "od_feedd started (pid $FEED)"
    sleep 15  # let it connect and fill the last-value cache
}
trap 'log "stopping"; [ -n "$FEED" ] && kill $FEED 2>/dev/null; exit 0' INT TERM

start_feed
while [ ! -e "$R/STOP" ]; do
    free_mb=$(df -Pm "$R" | awk 'NR == 2 { print $4 }')
    if [ "$free_mb" -lt "$MIN_FREE_MB" ]; then
        log "only ${free_mb} MB free, stopping"
        break
    fi
    kill -0 "$FEED" 2>/dev/null || start_feed
    out="$R/btc_$(date +%F_%H%M).odj"
    log "recording $out"
    "$B/od_record" --out "$out" --duration 3600 >> "$R/record_$(date +%F).log" 2>&1
    log "finished $out ($(du -m "$out" | cut -f1) MB)"
done
kill "$FEED" 2>/dev/null
log "stopped"
