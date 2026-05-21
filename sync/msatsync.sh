#!/bin/bash
# ============================================================================
# MSAT Sync  (msatsync.sh) - download titration data files from the MSAT
# device (ESP32) to a PC over WiFi, with an optional "clean up space" mode
# that deletes old files on the device SD card. Invoked by runmsatsync.bat.
#
# Copyright (c) 2026 Burapha University. All rights reserved.
# Inventor / developer: Teeranan Nongnual <teeranan.no@buu.ac.th>
#   Department of Chemistry, Faculty of Science, Burapha University, Thailand
# Petty Patent pending: Application No. 2603001145 (filed 2026-05-12).
#
# License: PolyForm Noncommercial 1.0.0 (see /LICENSE) - noncommercial use,
# modification allowed, selling prohibited.
# SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 Burapha University.
#
# Notes: HTTP keep-alive, retry with backoff, WiFi monitoring, live SD listing.
# ============================================================================

# Terminal colors: Black background, white text

# Terminal colors: Black background, white text
printf '\033[40m\033[37m'
clear

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_DEST="$SCRIPT_DIR/MSAT-Output"
DEST_DIR="$DEFAULT_DEST"
SCRIPT_VERSION="V.Y2026.88.20"

SSID_NAME="YOUR_WIFI_SSID"   # <-- EDIT ME: the WiFi network the MSAT is on
MSAT_IP="192.168.1.200"      # <-- device static IP (change if you changed it)
BASE_URL="http://$MSAT_IP"
MANIFEST_URL="$BASE_URL/sync_manifest"
MANIFEST_REBUILD_URL="$BASE_URL/sync_manifest?rebuild=1"
LIST_URL="$BASE_URL/listfiles_raw?limit=all"
LIST_URL_FALLBACK="$BASE_URL/listfiles?limit=all"
LIST_CURSOR_URL="$BASE_URL/listfiles_cursor"
LIST_CURSOR_PAGE_LIMIT=120
LISTCOUNT_URL="$BASE_URL/listcount"
LISTCOUNT_REFRESH_URL="$BASE_URL/listcount?refresh=1"
LIST_REFRESH_URL="$BASE_URL/listcount?refresh=1"
SYNC_URL="$BASE_URL/sync"
FWID_URL="$BASE_URL/fwid"
FILENAME_REGEX='[0-9]{8}_[0-9]{6}[^"<>/]*\.(txt|TXT)$'
# Same pattern WITHOUT the trailing $ anchor - used when we need to
# concatenate "|<size>" after the filename for name|size lines. The
# previous expression FILENAME_REGEX\|[0-9]+$ had two $ anchors which
# matched nothing, so REMOTE_SIZE_MAP was always empty and cleanup mode
# could never confirm sizes for any candidate.
FILENAME_BARE='[0-9]{8}_[0-9]{6}[^"<>/]*\.(txt|TXT)'
DOWNLOAD_TIMEOUT_SEC=45
REMOTE_COUNT_PASSES_MAX=6
REMOTE_COUNT_STABLE_HITS=2
FORCE_LEGACY_LIST="${MSAT_NO_MANIFEST:-0}"
MANIFEST_MAX_TRIES="${MSAT_MANIFEST_MAX_TRIES:-18}"
RUN_MODE="${MSAT_MODE:-sync}"          # sync | cleanup | both
CLEANUP_DAYS="${MSAT_CLEANUP_DAYS:-30}"
ALLOW_UNBACKED_DELETE=0
DELETE_URL="$BASE_URL/delete_batch"

for arg in "$@"; do
    case "$arg" in
        --no-manifest|--legacy-list)
            FORCE_LEGACY_LIST=1
            ;;
        --manifest)
            FORCE_LEGACY_LIST=0
            ;;
        --cleanup)
            RUN_MODE="cleanup"
            ;;
        --sync)
            RUN_MODE="sync"
            ;;
        --both)
            RUN_MODE="both"
            ;;
        --mode=*)
            RUN_MODE="${arg#--mode=}"
            ;;
        --cleanup-days=*)
            CLEANUP_DAYS="${arg#--cleanup-days=}"
            ;;
        --unsafe-delete-unbacked)
            ALLOW_UNBACKED_DELETE=1
            ;;
        --*)
            ;;
        *)
            DEST_DIR="$arg"
            ;;
    esac
done

case "$RUN_MODE" in
    sync|cleanup|both) ;;
    *) RUN_MODE="sync" ;;
esac
if ! [[ "$CLEANUP_DAYS" =~ ^[0-9]+$ ]] || (( CLEANUP_DAYS < 1 )); then
    CLEANUP_DAYS=30
fi

is_list_placeholder() {
    local body="$1"
    [[ "$body" == *"[Loading...]"* || "$body" == *"[Empty-Reloading]"* || "$body" == *"SD Refreshing"* || "$body" == *"File list not ready"* || "$body" == *"SD Busy"* || "$body" == *"SD Fail"* ]]
}

count_file_entries_in_text() {
    local body="$1"
    local c1 c2
    c1=$(printf '%s' "$body" | tr '\r' '\n' | awk -F'|' 'NF>=2 {name=$1; sub(/^.*\//, "", name); print name}' | grep -icE "$FILENAME_REGEX" || true)
    c2=$(printf '%s' "$body" | grep -oEi "$FILENAME_REGEX" | wc -l | tr -d '[:space:]')
    if [[ -z "$c1" ]]; then c1=0; fi
    if [[ -z "$c2" ]]; then c2=0; fi
    if (( c1 > c2 )); then
        echo "$c1"
    else
        echo "$c2"
    fi
}

sort_unique_desc() {
    LC_ALL=C sort -ru
}

# 🆕 WiFi OPTIMIZATION SETTINGS
CURL_KEEPALIVE_FLAGS="--keepalive-time 60 -H 'Connection: keep-alive' -H 'Keep-Alive: timeout=60, max=10'"
SYNC_MODE="SAFE"
ACTIVE_IDLE_RECHECK_EVERY_FILES=5
ACTIVE_DOWNLOAD_GAP_SEC=0.15  # Increased from 0 in FAST mode (fair bandwidth share)
REMOTE_LIST_TRUNCATED=0
COOKIE_JAR="$SCRIPT_DIR/.msat_session_cookie"

# 🆕 WiFi MONITORING
WIFI_CHECK_INTERVAL=10  # Check WiFi every 10 files
WIFI_CHECK_ENABLED=1
SYNC_SUPPORTED=0
SYNC_ACTIVE=0

# 🆕 DASHBOARD PRIORITY MODE
DASHBOARD_PRIORITY_MODE=1
PAUSE_EVERY_N_FILES=15
PAUSE_DURATION_SEC=2.5

# Cleanup function - releases SYNC mode and clears session
cleanup() {
    sync_end
  rm -f "$COOKIE_JAR"
}
trap cleanup EXIT INT TERM

# Guard against CRLF endings when running in Git Bash
SSID_NAME="${SSID_NAME%$'\r'}"
MSAT_IP="${MSAT_IP%$'\r'}"
BASE_URL="${BASE_URL%$'\r'}"
LIST_URL="${LIST_URL%$'\r'}"
LIST_URL_FALLBACK="${LIST_URL_FALLBACK%$'\r'}"
LIST_CURSOR_URL="${LIST_CURSOR_URL%$'\r'}"
FILENAME_REGEX="${FILENAME_REGEX%$'\r'}"

echo "MSAT SYNC ${SCRIPT_VERSION} (WiFi-Optimized)"
echo "Script Dir : $SCRIPT_DIR"
echo "Output Dir : $DEST_DIR"
echo "[i] WiFi optimizations enabled: Keep-Alive, fair bandwidth sharing"
if [[ "$FORCE_LEGACY_LIST" == "1" ]]; then
    echo "[i] List mode: LEGACY (manifest disabled by option)"
else
    echo "[i] List mode: AUTO (manifest -> fallback)"
fi
case "$RUN_MODE" in
    sync)    echo "[i] Run mode : SYNC (download new files only)" ;;
    cleanup) echo "[i] Run mode : CLEANUP (delete files older than ${CLEANUP_DAYS} days)" ;;
    both)    echo "[i] Run mode : BOTH (sync, then cleanup older than ${CLEANUP_DAYS} days)" ;;
esac

mkdir -p "$DEST_DIR"

extract_json_number() {
    local json="$1"
    local key="$2"
    echo "$json" | sed -n "s/.*\"$key\":\([0-9]\+\).*/\1/p" | head -n 1
}

extract_json_string() {
    local json="$1"
    local key="$2"
    echo "$json" | sed -n "s/.*\"$key\":\"\([^\"]*\)\".*/\1/p" | head -n 1
}

get_remote_list_updated_ms() {
    local json
    json="$(curl -s -b "$COOKIE_JAR" --max-time 4 --keepalive-time 60 "$LISTCOUNT_URL" 2>/dev/null || true)"
    extract_json_number "$json" "updated"
}

get_remote_list_generation() {
    local json
    json="$(curl -s -b "$COOKIE_JAR" --max-time 4 --keepalive-time 60 "$LISTCOUNT_URL" 2>/dev/null || true)"
    extract_json_number "$json" "generation"
}

wait_for_remote_list_refresh() {
    local baseline_updated="${1:-0}"
    local max_wait_sec="${2:-25}"
    local elapsed=0
    local seen_refresh=0
    local json refreshing updated

    while (( elapsed < max_wait_sec )); do
        json="$(curl -s -b "$COOKIE_JAR" --max-time 4 --keepalive-time 60 "$LISTCOUNT_URL" 2>/dev/null || true)"
        refreshing="$(extract_json_number "$json" "refreshing")"
        updated="$(extract_json_number "$json" "updated")"

        if [[ "$refreshing" == "1" ]]; then
            seen_refresh=1
        fi

        if [[ "$updated" =~ ^[0-9]+$ ]] && [[ "$baseline_updated" =~ ^[0-9]+$ ]] && (( updated > baseline_updated )); then
            if [[ "$refreshing" != "1" ]]; then
                return 0
            fi
        fi

        if (( seen_refresh == 1 )) && [[ "$refreshing" != "1" ]]; then
            return 0
        fi

        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# 🆕 HELPER: WiFi connectivity check
check_wifi_connectivity() {
    if ! ping -n 1 "$MSAT_IP" > /dev/null 2>&1; then
        echo "[X] WiFi LOST - Device 192.168.1.200 unreachable!"
        return 1
    fi
    return 0
}

# 🆕 HELPER: WiFi reconnection
ensure_wifi_connected() {
    local max_retries=3
    local attempt=1
    
    while ! check_wifi_connectivity; do
        if (( attempt > max_retries )); then
            echo "[X] WiFi reconnection failed after $max_retries attempts"
            return 1
        fi
        echo "[!] WiFi lost - reconnecting (attempt $attempt/$max_retries)..."
        netsh wlan disconnect > /dev/null 2>&1
        sleep 5
        netsh wlan connect name="$SSID_NAME" > /dev/null 2>&1
        sleep 3
        attempt=$((attempt + 1))
    done
    echo "[i] WiFi restored"
    return 0
}

# 🆕 IMPROVED: Exponential backoff retry
retry_with_backoff() {
    local max_attempts=5
    local attempt=1
    local delay=2
    
    while (( attempt <= max_attempts )); do
        if "$@"; then
            return 0
        fi
        if (( attempt < max_attempts )); then
            echo "[!] Attempt $attempt failed, waiting ${delay}s..." >&2
            sleep "$delay"
            delay=$((delay < 32 ? delay * 2 : 32))  # Cap at 32s
        fi
        attempt=$((attempt + 1))
    done
    return 1
}

fetch_data_json() {
    # 🆕 WiFi-optimized: Added keep-alive
    curl -s -b "$COOKIE_JAR" --max-time 5 \
        --keepalive-time 60 \
        -H "Connection: keep-alive" \
        "$BASE_URL/data" 2>/dev/null || true
}

is_data_json_ok() {
    local body="$1"
    [[ "$body" == *"\"state\":"* ]]
}

detect_sync_endpoint() {
    local code
    code="$(curl -s -o /dev/null -w "%{http_code}" --max-time 3 --keepalive-time 30 "$SYNC_URL?cmd=stop" 2>/dev/null || echo 000)"
    if [[ "$code" != "404" && "$code" != "000" ]]; then
        SYNC_SUPPORTED=1
        echo "  [i] SYNC endpoint: available"
    else
        SYNC_SUPPORTED=0
        echo "  [i] SYNC endpoint: not available (firmware without /sync)"
    fi
}

SYNC_STATUS_URL="$BASE_URL/rebuild_status"

wait_for_rebuild() {
    local max_wait="${1:-90}"
    local elapsed=0
    local json=""
    local rebuilding=""
    echo -n "  [i] Waiting for index rebuild to finish (max ${max_wait}s)..." >&2
    while (( elapsed < max_wait )); do
        sync_heartbeat
        json="$(curl -s -b "$COOKIE_JAR" --max-time 4 --keepalive-time 60 "$SYNC_STATUS_URL" 2>/dev/null || true)"
        if [[ -n "$json" ]]; then
            rebuilding="$(echo "$json" | sed -n 's/.*"rebuilding":\([0-9]\+\).*/\1/p' | head -n 1)"
            if [[ "$rebuilding" == "0" ]]; then
                echo " done (${elapsed}s)" >&2
                return 0
            fi
        fi
        sleep 3
        elapsed=$((elapsed + 3))
        echo -n "." >&2
    done
    echo " timeout" >&2
    return 1
}

get_firmware_id() {
    curl -s -b "$COOKIE_JAR" --max-time 3 --keepalive-time 30 "$FWID_URL" 2>/dev/null || true
}

get_system_state() {
    local data stateCode stateName syncFlag page
    data="$(fetch_data_json)"
    
    if is_data_json_ok "$data"; then
        # Check for sync mode first (firmware V87+)
        syncFlag="$(extract_json_number "$data" "sync")"
        if [[ "$syncFlag" == "1" ]]; then
            echo "SYNC"
            return
        fi

        stateCode="$(extract_json_number "$data" "state")"
        if [[ -n "$stateCode" ]]; then
            if [[ "$stateCode" == "0" ]]; then
                echo "IDLE"
                return
            elif [[ "$stateCode" == "1" || "$stateCode" == "2" || "$stateCode" == "3" || "$stateCode" == "4" ]]; then
                echo "BUSY"
                return
            fi
        fi
        
        stateName="$(extract_json_string "$data" "state_name")"
        if [[ "$stateName" == "IDLE" ]]; then
            echo "IDLE"
            return
        elif [[ "$stateName" == "PENDING" || "$stateName" == "RUNNING" || "$stateName" == "RINSING" || "$stateName" == "REC" ]]; then
            echo "BUSY"
            return
        fi
    fi

    page="$(curl -s -b "$COOKIE_JAR" --max-time 5 \
        --keepalive-time 60 \
        "$BASE_URL/dashboard" 2>/dev/null || true)"
    
    if [[ -z "$page" ]]; then
        page="$(curl -s -b "$COOKIE_JAR" --max-time 5 \
            --keepalive-time 60 \
            "$BASE_URL/" 2>/dev/null || true)"
    fi
    
    if [[ -n "$page" ]]; then
        if [[ "$page" == *"REC ONLY"* || "$page" == *"RUNNING"* || "$page" == *"RINSING"* || "$page" == *"PENDING"* || "$page" == *"Recording"* || "$page" == *"BUSY"* ]]; then
            echo "BUSY"
            return
        elif [[ "$page" == *"IDLE"* || "$page" == *"Ready"* || "$page" == *"Idle"* ]]; then
            echo "IDLE"
            return
        fi
    fi
    
    echo "UNKNOWN"
}

get_system_state_with_retry() {
    local max_try="${1:-8}"
    local i=0
    local st="UNKNOWN"
    while (( i < max_try )); do
        i=$((i + 1))
        st="$(get_system_state)"
        if [[ "$st" == "IDLE" || "$st" == "BUSY" ]]; then
            echo "$st"
            return 0
        fi
        if (( i < max_try )); then
            echo "   [retry $i/$max_try] State still $st, waiting..." >&2
        fi
        sleep 1
    done
    echo "UNKNOWN"
    return 1
}

fetch_remote_files() {
    # 🆕 WiFi-optimized: Keep-alive headers
    curl -s -b "$COOKIE_JAR" --max-time 8 \
        --keepalive-time 60 \
        "$LIST_URL" 2>/dev/null \
        | grep -oE "$FILENAME_REGEX" \
        | sort -u
}

fetch_remote_manifest_raw() {
    local no_rebuild="${1:-0}"  # pass 1 to skip ?rebuild=1 trigger (use after wait_for_rebuild)
    local resp=""
    local out=""
    local http_code=""
    local tries=0
    local max_tries="$MANIFEST_MAX_TRIES"
    local use_url=""
    local rebuild_ack=0
    local rebuild_waits=0
    local index_waits=0
    local hard_fail=0
    local need_rebuild=0
    echo -n "[i] Loading file list via /sync_manifest (attempt" >&2
    while (( tries < max_tries )); do
        tries=$((tries + 1))
        echo -n " $tries/$max_tries" >&2

        if (( tries > 1 )); then
            sync_heartbeat
            sleep 1
        fi

        # Use the existing index first. Only trigger ?rebuild=1 if the
        # device actually reports the index missing/not ready. Forcing a
        # rebuild on every run put the device into a perpetual REBUILDING
        # state for large indexes and broke the same run.
        if (( no_rebuild == 0 && need_rebuild == 1 && rebuild_ack == 0 )); then
            use_url="$MANIFEST_REBUILD_URL"
        else
            use_url="$MANIFEST_URL"
        fi

        resp="$(curl -sS -L -b "$COOKIE_JAR" --max-time 30 --connect-timeout 8 \
            --keepalive-time 60 \
            -H "Accept: text/plain,*/*;q=0.8" \
            -H "Connection: keep-alive" \
            -w "\n__HTTP_CODE__:%{http_code}" \
            "$use_url" 2>/dev/null | tr -d '\0' || true)"

        http_code="$(printf '%s\n' "$resp" | sed -n 's/^__HTTP_CODE__:\([0-9][0-9][0-9]\)$/\1/p' | tail -n 1)"
        out="$(printf '%s\n' "$resp" | sed '/^__HTTP_CODE__:[0-9][0-9][0-9]$/d')"

        if [[ "$out" == *"cannot open _index.txt"* || "$out" == *"INDEX_BUILD_FAILED"* || "$out" == *"REBUILD_FAILED"* || "$out" == *"Index rebuild failed"* ]]; then
            hard_fail=1
        fi

        if (( hard_fail == 1 )); then
            echo ") [manifest rebuild failed - fallback required]" >&2
            return 3
        fi

        if [[ "$http_code" == "404" ]]; then
            if [[ "$out" == *"INDEX_NOT_FOUND"* || "$out" == *"INDEX_NOT_READY"* ]]; then
                need_rebuild=1
                if (( tries < max_tries )); then
                    sleep 1
                    continue
                fi
            fi
            echo ") [HTTP 404: /sync_manifest not found]" >&2
            return 2
        fi

        if [[ "$http_code" == "503" ]]; then
            if [[ "$out" == *"INDEX_NOT_READY"* ]]; then
                need_rebuild=1
                index_waits=$((index_waits + 1))
                if (( index_waits >= 4 )); then
                    echo ") [index not ready - fallback required]" >&2
                    return 3
                fi
            fi
            if (( tries < max_tries )); then
                sleep 2
                continue
            fi
        fi

        if [[ "$out" == *"REBUILD_QUEUED"* || "$out" == *"REBUILDING"* ]]; then
            rebuild_ack=1
            rebuild_waits=$((rebuild_waits + 1))
            if (( rebuild_waits >= 6 )); then
                echo ") [manifest rebuild timeout - fallback required]" >&2
                return 3
            fi
            if (( tries < max_tries )); then
                sleep 1
                continue
            fi
        fi

        if [[ "$out" == *"INDEX_NOT_FOUND"* || "$out" == *"INDEX_NOT_READY"* ]]; then
            need_rebuild=1
            index_waits=$((index_waits + 1))
            if (( index_waits >= 6 )); then
                echo ") [manifest index not ready - fallback required]" >&2
                return 3
            fi
            if (( tries < max_tries )); then
                sleep 1
                continue
            fi
        fi

        if [[ "$http_code" == "200" ]]; then
            local file_count=0
            file_count="$(count_file_entries_in_text "$out")"
            if [[ "$file_count" =~ ^[0-9]+$ ]] && (( file_count > 0 )); then
                local line_count=0
                line_count=$(printf '%s' "$out" | grep -c . || echo 0)
                echo ") [OK: $line_count lines, $file_count files]" >&2
                printf '%s' "$out" | tr '\r' '\n'
                return 0
            fi

            if [[ -z "$out" ]]; then
                echo ") [OK: empty manifest, 0 files]" >&2
                return 0
            fi

            if [[ "$out" != *"REBUILD_QUEUED"* && "$out" != *"REBUILDING"* && "$out" != *"INDEX_NOT_FOUND"* && "$out" != *"INDEX_NOT_READY"* ]]; then
                echo ") [OK: manifest loaded, 0 matching files]" >&2
                printf '%s' "$out" | tr '\r' '\n'
                return 0
            fi
        fi

        if (( tries < max_tries )); then
            sleep 1
        fi
    done
    echo ") FAILED" >&2
    return 1
}

fetch_remote_index_via_down() {
    local out=""
    local http_code=""
    local tries=0
    local max_tries=8
    local idx_url="$BASE_URL/down?f=/autotttoutput/_index.txt"

    echo -n "[i] Loading file list via /down (_index.txt) (attempt" >&2
    while (( tries < max_tries )); do
        tries=$((tries + 1))
        echo -n " $tries/$max_tries" >&2

        if (( tries > 1 )); then
            sync_heartbeat
            sleep 1
        fi

        out="$(curl -sS -L -b "$COOKIE_JAR" --max-time 20 --connect-timeout 8 \
            --keepalive-time 60 \
            -H "Accept: text/plain,*/*;q=0.8" \
            -H "Connection: keep-alive" \
            -w "\n__HTTP_CODE__:%{http_code}" \
            "$idx_url" 2>/dev/null | tr -d '\0' || true)"

        http_code="$(printf '%s\n' "$out" | sed -n 's/^__HTTP_CODE__:\([0-9][0-9][0-9]\)$/\1/p' | tail -n 1)"
        out="$(printf '%s\n' "$out" | sed '/^__HTTP_CODE__:[0-9][0-9][0-9]$/d')"

        if [[ "$http_code" == "404" ]]; then
            if (( tries < max_tries )); then
                sleep 1
                continue
            fi
        fi

        if [[ "$http_code" == "200" && -z "$out" ]]; then
            echo ") [OK: empty index, 0 files]" >&2
            return 0
        fi

        if [[ -n "$out" && "$out" != *"File not found on SD Card"* ]]; then
            local file_count=0
            file_count="$(count_file_entries_in_text "$out")"
            if [[ "$file_count" =~ ^[0-9]+$ ]] && (( file_count > 0 )); then
                local line_count=0
                line_count=$(printf '%s' "$out" | grep -c . || echo 0)
                echo ") [OK: $line_count lines, $file_count files]" >&2
                printf '%s' "$out" | tr '\r' '\n'
                return 0
            fi

            if [[ "$http_code" == "200" ]]; then
                echo ") [OK: index loaded, 0 matching files]" >&2
                printf '%s' "$out" | tr '\r' '\n'
                return 0
            fi
        fi

        if (( tries < max_tries )); then
            sleep 1
        fi
    done

    echo ") FAILED" >&2
    return 1
}

fetch_remote_files_raw() {
    local out=""
    local tries=0
    local max_tries=6
    local list_ready=0
    local use_url=""
    echo -n "[i] Loading file list (attempt" >&2
    while (( tries < max_tries )); do
        tries=$((tries + 1))
        echo -n " $tries/$max_tries" >&2
        
        if (( tries > 1 )); then
            sync_heartbeat
            sleep 1
        fi
        
        use_url="$LIST_URL"
        # 🆕 WiFi-optimized: Keep-alive + relaxed speed limit (100 B/s vs 500)
        out="$(curl -sS -L -b "$COOKIE_JAR" --max-time 30 --connect-timeout 8 \
            --keepalive-time 60 \
            -H "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36" \
            -H "Accept: text/plain,text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" \
            -H "Connection: keep-alive" \
            "$use_url" 2>/dev/null | tr -d '\0' || true)"

        if [[ -z "$out" || "$out" == *"404 Not Found"* || "$out" == *"Not Found"* ]]; then
            use_url="$LIST_URL_FALLBACK"
            out="$(curl -sS -L -b "$COOKIE_JAR" --max-time 30 --connect-timeout 8 \
                --keepalive-time 60 \
                -H "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36" \
                -H "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" \
                -H "Connection: keep-alive" \
                "$use_url" 2>/dev/null | tr -d '\0' || true)"
        fi
        
        if [[ -n "$out" ]]; then
            if is_list_placeholder "$out"; then
                if (( tries < max_tries )); then
                    sleep 2
                    continue
                fi
            fi

            local file_count=0
            file_count="$(count_file_entries_in_text "$out")"
            if [[ "$file_count" =~ ^[0-9]+$ ]] && (( file_count > 0 )); then
                local line_count=$(printf '%s' "$out" | grep -c . || echo 0)
                list_ready=1
                echo -n " ($line_count lines, $file_count files via ${use_url##*/})" >&2
                break
            fi

            if (( tries == max_tries )); then
                local preview
                preview="$(printf '%s' "$out" | tr '\r\n' ' ' | cut -c1-140)"
                echo -n " (last payload: ${preview})" >&2
            fi
        fi
        
        if (( tries < max_tries )); then
            sleep 2
        fi
    done
    
    if (( list_ready )); then
        echo ") [OK]" >&2
    else
        echo ") FAILED" >&2
        echo "[X] File list not available after $tries attempts" >&2
        return 1
    fi
    
    printf '%s' "$out" | tr '\r' '\n'
}

fetch_remote_files_cursor_paged() {
    local cursor=0
    local limit="${LIST_CURSOR_PAGE_LIMIT:-120}"
    local page=0
    local max_pages=10000
    local body=""
    local next_cursor=""
    local hdr_file="$SCRIPT_DIR/.msat_hdr_$$.tmp"
    local ok_any=0
    local expected_gen="${EXPECTED_LIST_GEN:-}"
    local page_gen=""
    local placeholder_hits=0
    local placeholder_limit=8

    if (( limit < 1 )); then limit=120; fi
    if (( limit > 500 )); then limit=500; fi

    echo "[i] Loading file list via cursor paging..." >&2

    while (( page < max_pages )); do
        page=$((page + 1))
        : > "$hdr_file"

        echo "[i] list page=$page cursor=$cursor limit=$limit" >&2

        # 🆕 WiFi-optimized: Keep-alive + relaxed speed
        local refresh_q=""
        if (( page == 1 )); then
            refresh_q=""
        fi

        body="$(curl -sS -L -b "$COOKIE_JAR" --max-time 30 --connect-timeout 8 \
            -D "$hdr_file" \
            --keepalive-time 60 \
            -H "Accept: text/plain,*/*;q=0.8" \
            -H "Connection: keep-alive" \
            "$LIST_CURSOR_URL?cursor=$cursor&limit=$limit$refresh_q" 2>/dev/null | tr -d '\0' || true)"

        if grep -qiE '^HTTP/.* 404' "$hdr_file" 2>/dev/null; then
            rm -f "$hdr_file"
            return 2
        fi

        if [[ -z "$body" ]]; then
            rm -f "$hdr_file"
            if (( ok_any )); then
                return 0
            fi
            return 1
        fi

        if [[ "$body" == *"SD Busy"* || "$body" == *"SD Fail"* || "$body" == *"File list not ready"* ]]; then
            placeholder_hits=$((placeholder_hits + 1))
            if (( placeholder_hits >= placeholder_limit )); then
                rm -f "$hdr_file"
                return 1
            fi
            sleep 1
            continue
        fi
        placeholder_hits=0

        local file_count=0
        file_count="$(count_file_entries_in_text "$body")"
        if [[ "$file_count" =~ ^[0-9]+$ ]] && (( file_count > 0 )); then
            ok_any=1
            printf '%s\n' "$body"
        fi

        next_cursor="$(tr -d '\r' < "$hdr_file" | sed -n 's/^X-Next-Cursor:[[:space:]]*//Ip' | tail -n 1)"
        page_gen="$(tr -d '\r' < "$hdr_file" | sed -n 's/^X-List-Generation:[[:space:]]*//Ip' | tail -n 1)"
        local page_lines=0
        page_lines=$(printf '%s' "$body" | grep -c . || echo 0)
        echo "[i] list page=$page lines=$page_lines files=$file_count gen=${page_gen:-?} next_cursor=${next_cursor:-?}" >&2
        rm -f "$hdr_file"

        if [[ -n "$expected_gen" && "$expected_gen" =~ ^[0-9]+$ && "$page_gen" =~ ^[0-9]+$ ]]; then
            if (( page_gen != expected_gen )); then
                return 3
            fi
        fi

        if (( file_count > 0 )) && [[ -z "$next_cursor" ]]; then
            return 0
        fi

        if [[ -z "$next_cursor" ]]; then
            return 1
        fi
        if [[ "$next_cursor" == "-1" ]]; then
            return 0
        fi
        if ! [[ "$next_cursor" =~ ^[0-9]+$ ]]; then
            return 1
        fi
        if (( next_cursor <= cursor )); then
            return 1
        fi

        cursor="$next_cursor"

        # Do not ping /sync periodically here; some firmware does not support it.
    done

    rm -f "$hdr_file"
    return 1
}

fetch_remote_count_fast() {
    local json count
    # NOTE: do NOT use ?refresh=1 here - on this firmware it triggers an
    # index REBUILD that makes the next /listfiles call return REBUILDING.
    json="$(curl -s -b "$COOKIE_JAR" --max-time 3 \
        --keepalive-time 60 \
        "$LISTCOUNT_URL" 2>/dev/null || true)"
    count="$(echo "$json" | sed -n 's/.*"count":\([0-9]\+\).*/\1/p' | head -n 1)"
    
    if [[ -z "$count" && -n "$REMOTE_FILES_STR" ]]; then
        count="$(echo "$REMOTE_FILES_STR" | wc -l)"
    fi
    
    echo "$count"
}

request_remote_list_refresh() {
    curl -s -b "$COOKIE_JAR" --max-time 6 \
        --keepalive-time 60 \
        "$LIST_REFRESH_URL" > /dev/null 2>&1 || true
}

sync_begin() {
    if (( SYNC_SUPPORTED == 0 )); then
        return 0
    fi
    curl -s -b "$COOKIE_JAR" --max-time 5 \
        --keepalive-time 60 \
        "$SYNC_URL?cmd=start" > /dev/null 2>&1 || true
    SYNC_ACTIVE=1
}

sync_heartbeat() {
    if (( SYNC_SUPPORTED == 0 || SYNC_ACTIVE == 0 )); then
        return 0
    fi
    curl -s -b "$COOKIE_JAR" --max-time 4 \
        --keepalive-time 60 \
        "$SYNC_URL?cmd=heartbeat" > /dev/null 2>&1 || true
}

sync_end() {
    if (( SYNC_SUPPORTED == 0 )); then
        return 0
    fi
    if (( SYNC_ACTIVE == 0 )); then
        return 0
    fi
    curl -s -b "$COOKIE_JAR" --max-time 5 \
        --keepalive-time 60 \
        "$SYNC_URL?cmd=stop" > /dev/null 2>&1 || true
    SYNC_ACTIVE=0
}

fetch_remote_files_from_url() {
    fetch_remote_files_raw \
        | awk -F'|' 'NF>=2 {name=$1; sub(/^.*\//, "", name); print name}' \
        | grep -E "$FILENAME_REGEX" \
    | sort_unique_desc
}

fetch_remote_name_size_from_url() {
    fetch_remote_files_raw \
        | awk -F'|' 'NF>=2 {name=$1; sub(/^.*\//, "", name); print name "|" $2}' \
        | grep -E "$FILENAME_BARE\|[0-9]+$" \
    | sort_unique_desc
}

extract_remote_files_from_page() {
    local page="$1"
    if printf '%s' "$page" | grep -qE '\|[0-9]+'; then
                printf '%s' "$page" \
                    | tr '\r' '\n' \
                    | while IFS= read -r line; do
                                line="${line%%$'\r'}"
                                [ -z "$line" ] && continue
                                name="${line%%|*}"
                                name="${name##*/}"
                                [ -z "$name" ] && continue
                                echo "$name"
                        done \
                    | grep -E "$FILENAME_REGEX" \
                    | sort_unique_desc
        return
    fi
    printf '%s' "$page" | grep -oE "$FILENAME_REGEX" | sort_unique_desc
}

extract_remote_files_from_text() {
    local text="$1"
    if printf '%s' "$text" | grep -qE '\|[0-9]+'; then
        printf '%s' "$text" \
            | tr '\r' '\n' \
            | awk -F'|' 'NF>=2 {name=$1; sub(/^.*\//, "", name); print name}' \
            | grep -iE "$FILENAME_REGEX" \
            | sort_unique_desc
        return
    fi
    extract_remote_files_from_page "$text"
}

extract_remote_name_size_from_text() {
    local text="$1"
    if printf '%s' "$text" | grep -qE '\|[0-9]+'; then
        printf '%s' "$text" \
            | tr '\r' '\n' \
            | awk -F'|' 'NF>=2 {name=$1; sub(/^.*\//, "", name); print name "|" $2}' \
            | grep -iE "$FILENAME_BARE\|[0-9]+$" \
            | sort_unique_desc
        return
    fi
    extract_remote_name_size_from_page "$text"
}

extract_remote_name_size_from_page() {
    local page="$1"
    if printf '%s' "$page" | grep -qE '\|[0-9]+'; then
        printf '%s' "$page" \
            | tr '\r' '\n' \
                    | while IFS= read -r line; do
                                line="${line%%$'\r'}"
                                [ -z "$line" ] && continue
                                name="${line%%|*}"
                                name="${name##*/}"
                                size="${line##*|}"
                                if [[ -n "$name" && "$size" =~ ^[0-9]+$ ]]; then
                                        echo "$name|$size"
                                fi
                        done \
                    | grep -E "$FILENAME_BARE\\|[0-9]+$" \
                            | sort_unique_desc
                return
        fi
    printf '%s' "$page" \
            | tr '\n' ' ' \
            | sed 's#<li>#\n<li>#g' \
            | sed -n "s#.*href='/down?f=/autotttoutput/\([^']*\)'.*(\([0-9][0-9]*\) b).*#\1|\2#p" \
                        | sort_unique_desc
}

detect_remote_list_truncated() {
    local page="$1"
    if [[ "$page" == *"List truncated."* ]]; then
        echo 1
    else
        echo 0
    fi
}

fetch_local_files() {
    find "$DEST_DIR" -maxdepth 1 -type f -printf "%f\n" 2>/dev/null \
        | grep -E "$FILENAME_REGEX" \
    | sort_unique_desc
}

short_name() {
    local n="$1"
    local max_len=48
    n=$(echo "$n" | sed 's/[^ -~]/?/g')
    if (( ${#n} <= max_len )); then
        echo "$n"
    else
        echo "${n:0:max_len-3}..."
    fi
}

url_encode_filename() {
    local s="$1"
    local out=""
    local i len byte hex_byte
    local LC_ALL=C
    len=${#s}
    for (( i=0; i<len; i++ )); do
        byte="${s:i:1}"
        case "$byte" in
            [a-zA-Z0-9.~_-]) out+="$byte" ;;
            *)
                # Encode each byte individually using printf %d -> printf %%02x
                hex_byte=$(printf '%02x' "'$byte" 2>/dev/null || true)
                if [[ -n "$hex_byte" && "$hex_byte" != "00" ]]; then
                    out+="%${hex_byte}"
                else
                    # Fallback: use od for multi-byte or problematic chars
                    hex_byte=$(printf '%s' "$byte" | od -An -tx1 | tr -d ' \n')
                    while [[ -n "$hex_byte" ]]; do
                        out+="%${hex_byte:0:2}"
                        hex_byte="${hex_byte:2}"
                    done
                fi
                ;;
        esac
    done
    echo "$out"
}

print_download_table_header() {
    echo "+--------------------------------------------------+--------------+----------+----------------------------+"
    echo "| File                                             |     Size (B) | Progress | Remark                     |"
    echo "+--------------------------------------------------+--------------+----------+----------------------------+"
}

print_download_table_footer() {
    echo "+--------------------------------------------------+--------------+----------+----------------------------+"
}

print_download_row() {
    local file_name="$1"
    local size_b="$2"
    local progress="$3"
    local remark="$4"
    local file_label remark_label
    file_label="$(short_name "$file_name")"
    remark_label="$(echo "$remark" | sed 's/[^ -~]/?/g')"
    printf "| %-48s | %12s | %8s | %-26s |\n" "$file_label" "$size_b" "$progress" "$remark_label"
}

download_with_progress() {
    local file_name="$1"
    local url="$2"
    local output_path="$3"
    # Download straight to the final name (no ".part" + rename). A rename
    # needs Delete permission on the source, which the optional folder
    # protection (menu [4]) deliberately removes - that broke the old
    # mv ".part" -> ".txt" step. curl -o only needs create + write-data
    # (allowed by protection), and a truncated/partial download self-heals:
    # next sync sees local < remote and re-downloads (overwrite in place).
    local temp_path="$output_path"
    local file_label
    file_label="$(short_name "$file_name")"
    local last_heartbeat_ts
    last_heartbeat_ts="$(date +%s)"

    local total_bytes
    # 🆕 WiFi-optimized: Keep-alive + relaxed speed
    total_bytes=$(curl -sI -L -b "$COOKIE_JAR" --max-time 10 \
        --keepalive-time 60 \
        --speed-time 30 --speed-limit 100 \
        "$url" 2>/dev/null \
        | tr -d '\r' \
        | awk -F': ' 'tolower($1)=="content-length" {print $2}' \
        | tail -n 1)

    # 🆕 WiFi-optimized: Improved retry strategy (3→5 retries, 1→3s delay)
    curl -s -L -b "$COOKIE_JAR" --fail --max-time "$DOWNLOAD_TIMEOUT_SEC" \
         --keepalive-time 60 \
         --speed-time 30 --speed-limit 100 \
         --retry 5 --retry-delay 3 --retry-max-time "$DOWNLOAD_TIMEOUT_SEC" \
         -o "$temp_path" "$url" 2>/dev/null &
    local curl_pid=$!

    local last_bytes=0
    local stall_count=0
    while kill -0 "$curl_pid" 2>/dev/null; do
        local now_ts
        now_ts="$(date +%s)"
        if (( now_ts - last_heartbeat_ts >= 5 )); then
            sync_heartbeat
            last_heartbeat_ts="$now_ts"
        fi

        local current_bytes=0
        if [ -f "$temp_path" ]; then
            current_bytes=$(stat -c%s "$temp_path" 2>/dev/null || echo 0)
        fi

        if (( current_bytes == last_bytes )); then
            stall_count=$((stall_count + 1))
        else
            stall_count=0
        fi
        last_bytes=$current_bytes

        if (( stall_count > 240 )); then
            kill -9 "$curl_pid" 2>/dev/null
            break
        fi

        if [[ -n "$total_bytes" && "$total_bytes" =~ ^[0-9]+$ && "$total_bytes" -gt 0 ]]; then
            local pct=$(( current_bytes * 100 / total_bytes ))
            if (( pct > 100 )); then pct=100; fi
            printf "\r| %-48s | %12d | %8s | %-26s |" "$file_label" "$current_bytes" "${pct}%" ""
        else
            printf "\r| %-48s | %12d | %8s | %-26s |" "$file_label" "$current_bytes" "--%" ""
        fi
        sleep 0.25
    done

    wait "$curl_pid"
    local rc=$?

    if (( rc == 0 )) && [ -f "$temp_path" ]; then
        local final_bytes
        final_bytes=$(stat -c%s "$temp_path" 2>/dev/null || echo 0)
        
        if [[ -n "$total_bytes" && "$total_bytes" =~ ^[0-9]+$ && "$total_bytes" -gt 0 ]]; then
            if (( final_bytes < total_bytes )); then
                printf "\r| %-48s | %12d | %8s | %-26s |\n" "$file_label" "$final_bytes" "PARTIAL" "*** SIZE_MISMATCH"
                # Leave the partial in place (rm may be blocked by folder
                # protection); next run re-downloads it by size check.
                rm -f "$temp_path" 2>/dev/null || true
                return 1
            fi
        fi

        printf "\r| %-48s | %12d | %8s | %-26s |\n" "$file_label" "$final_bytes" "100%" "OK"
        return 0
    fi

    printf "\r| %-48s | %12s | %8s | %-26s |\n" "$file_label" "-" "FAILED" "*** CURL_ERR_$rc"
    rm -f "$temp_path" 2>/dev/null || true
    return 1
}

wait_until_idle() {
    local max_wait_sec=120
    local waited=0
    while (( waited < max_wait_sec )); do
        local st
        st="$(get_system_state)"
        if [[ "$st" == "IDLE" || "$st" == "SYNC" ]]; then
            return 0
        fi
        echo "[!] MSAT State: $st (waiting for IDLE...)"
        sleep 5
        waited=$((waited + 5))
    done
    return 1
}

ensure_idle_periodic() {
    local file_index="$1"
    if (( file_index % ACTIVE_IDLE_RECHECK_EVERY_FILES != 0 )); then
        return 0
    fi
    local st
    st="$(get_system_state)"
    if [[ "$st" == "IDLE" || "$st" == "SYNC" ]]; then
        return 0
    fi
    echo "[!] State changed to $st, waiting for IDLE..."
    wait_until_idle
}

# 🆕 DASHBOARD PRIORITY: Periodic pause for firmware REST
dashboard_priority_pause() {
    local file_index="$1"
    if (( DASHBOARD_PRIORITY_MODE == 0 )); then
        return 0
    fi
    if (( file_index % PAUSE_EVERY_N_FILES != 0 )); then
        return 0
    fi
    echo "[i] Dashboard REST: pause ${PAUSE_DURATION_SEC}s (file #${file_index})"
    sleep "$PAUSE_DURATION_SEC"
}

# 🆕 MONITORING: Periodic WiFi check during sync
periodic_wifi_check() {
    local file_index="$1"
    if (( WIFI_CHECK_ENABLED == 0 || file_index % WIFI_CHECK_INTERVAL != 0 )); then
        return 0
    fi
    
    if ! check_wifi_connectivity; then
        echo "[!] WiFi connectivity lost at file #$file_index"
        if ! ensure_wifi_connected; then
            echo "[X] Could not restore WiFi"
            return 1
        fi
    fi
    return 0
}

set_sync_mode_from_list_page() {
    local page="$1"
    local data stsd
    data="$(fetch_data_json)"
    if is_data_json_ok "$data"; then
        stsd="$(extract_json_number "$data" "st_sd")"
        if [[ "$stsd" == "1" ]]; then
            SYNC_MODE="FAST"
            ACTIVE_IDLE_RECHECK_EVERY_FILES=10
            ACTIVE_DOWNLOAD_GAP_SEC=1.2  # Dashboard-Priority: slower for responsiveness
            return
        fi
    fi

    if [[ "$page" == *"File list not ready"* || "$page" == *"SD Fail"* || "$page" == *"SD Busy"* ]]; then
        SYNC_MODE="SAFE"
        ACTIVE_IDLE_RECHECK_EVERY_FILES=2
        ACTIVE_DOWNLOAD_GAP_SEC=0.15
    else
        SYNC_MODE="FAST"
        ACTIVE_IDLE_RECHECK_EVERY_FILES=10
        ACTIVE_DOWNLOAD_GAP_SEC=1.2  # Dashboard-Priority: slower for responsiveness
    fi
}

enforce_state_gate_for_sync_mode() {
    local state="$1"
    if [[ "$state" != "IDLE" && "$state" != "SYNC" ]]; then
        SYNC_MODE="SAFE"
        ACTIVE_IDLE_RECHECK_EVERY_FILES=2
        ACTIVE_DOWNLOAD_GAP_SEC=0.15
    fi
}

# ============================================================
# Cleanup mode: delete device-side files older than CLEANUP_DAYS.
#   - Filenames are parsed for the embedded YYYYMMDD date prefix.
#   - Default safety: only delete files that ALSO exist in the local
#     backup with a matching size (set ALLOW_UNBACKED_DELETE=1 to skip).
#   - Requires the firmware /delete_batch endpoint (>= V.Y2026.88.12).
#   - Expects REMOTE_FILES / REMOTE_SIZE_MAP / LOCAL_SET to be populated
#     by the main flow before being called.
# ============================================================
do_cleanup() {
    echo ""
    echo "[Cleanup] Files older than ${CLEANUP_DAYS} days"
    if (( ${#REMOTE_FILES[@]} == 0 )); then
        echo "  [!] No remote files - nothing to clean"
        return 0
    fi

    local cutoff_ymd
    cutoff_ymd="$(date -d "${CLEANUP_DAYS} days ago" +%Y%m%d 2>/dev/null || true)"
    if ! [[ "$cutoff_ymd" =~ ^[0-9]{8}$ ]]; then
        echo "  [X] date arithmetic failed - aborting cleanup"
        return 1
    fi
    echo "  [i] Cutoff date: $cutoff_ymd (files with date < this are candidates)"
    echo "  [i] Safety: $( ((ALLOW_UNBACKED_DELETE)) && echo 'OFF (will delete unbacked files!)' || echo 'ON (only delete files already backed up locally with matching size)')"

    local -a CANDIDATES=()
    local total_bytes=0
    local skipped_unbacked=0
    local skipped_no_rsize=0
    local skipped_local_smaller=0
    local debug_shown=0
    local f ymd rsize lpath lsize
    for f in "${REMOTE_FILES[@]}"; do
        ymd="${f:0:8}"
        if ! [[ "$ymd" =~ ^[0-9]{8}$ ]]; then continue; fi
        if (( 10#$ymd >= 10#$cutoff_ymd )); then continue; fi
        rsize="${REMOTE_SIZE_MAP[$f]:-}"
        if (( ALLOW_UNBACKED_DELETE == 0 )); then
            if [[ -z "${LOCAL_SET[$f]+x}" ]]; then
                skipped_unbacked=$((skipped_unbacked + 1))
                continue
            fi
            lpath="$DEST_DIR/$f"
            lsize=$(stat -c%s "$lpath" 2>/dev/null || echo 0)
            # Safety: local must be AT LEAST as big as remote (no truncated
            # backup). Equal-or-larger is OK; on Windows the downloaded file
            # gets CRLF (+1 byte per line) while the device may write LF
            # only, so lsize == rsize doesn't always hold.
            if [[ -z "$rsize" ]]; then
                skipped_no_rsize=$((skipped_no_rsize + 1))
                if (( debug_shown < 3 )); then
                    echo "  [debug] no rsize: f='$f' lsize=$lsize"
                    debug_shown=$((debug_shown + 1))
                fi
                continue
            fi
            if ! [[ "$lsize" =~ ^[0-9]+$ ]]; then
                skipped_no_rsize=$((skipped_no_rsize + 1))
                continue
            fi
            if (( lsize < rsize )); then
                skipped_local_smaller=$((skipped_local_smaller + 1))
                if (( debug_shown < 3 )); then
                    echo "  [debug] lsize<rsize: f='$f' lsize=$lsize rsize=$rsize diff=$((rsize - lsize))"
                    debug_shown=$((debug_shown + 1))
                fi
                continue
            fi
        fi
        CANDIDATES+=("$f")
        if [[ "$rsize" =~ ^[0-9]+$ ]]; then total_bytes=$((total_bytes + rsize)); fi
    done

    local n=${#CANDIDATES[@]}
    echo "  [i] Candidates : $n files (~$((total_bytes / 1024)) KB)"
    if (( ALLOW_UNBACKED_DELETE == 0 )); then
        echo "  [i] Skipped (not backed up)      : $skipped_unbacked"
        echo "  [i] Skipped (rsize unknown/local missing): $skipped_no_rsize"
        echo "  [i] Skipped (local smaller than remote)  : $skipped_local_smaller"
    fi
    if (( n == 0 )); then
        echo "  [OK] Nothing to delete"
        return 0
    fi

    local show=10
    if (( n < show )); then show=$n; fi
    echo "  Preview (first $show):"
    local i
    for (( i = 0; i < show; i++ )); do echo "    - ${CANDIDATES[$i]}"; done
    if (( n > show )); then echo "    ... and $((n - show)) more"; fi

    echo ""
    echo -n "  Type DELETE (uppercase) to confirm, anything else to abort: "
    local confirm
    read -r confirm
    if [[ "$confirm" != "DELETE" ]]; then
        echo "  [i] Cancelled - no files deleted"
        return 0
    fi

    # Send in modest chunks (devices on ESPAsyncWebServer have small body
    # buffers; a single 15 KB POST was returning an empty response). 50
    # filenames per batch keeps body well under 2 KB and lets us show
    # per-batch progress + recover gracefully if one batch fails.
    local batch_size=50
    local total=${#CANDIDATES[@]}
    local sent=0
    local total_deleted=0
    local total_failed=0
    local total_errors=0
    local payload_file="$SCRIPT_DIR/.msat_delete_payload_$$.tmp"
    local hdr_file="$SCRIPT_DIR/.msat_delete_hdr_$$.tmp"
    while (( sent < total )); do
        local end=$((sent + batch_size))
        if (( end > total )); then end=$total; fi
        : > "$payload_file"
        local k
        for (( k = sent; k < end; k++ )); do printf '%s\n' "${CANDIDATES[$k]}" >> "$payload_file"; done
        local body_bytes=$(stat -c%s "$payload_file" 2>/dev/null || echo "?")
        echo "  [i] Batch $((sent / batch_size + 1)): files $((sent + 1))-$end (body ${body_bytes}B)"
        local resp http_code
        resp="$(curl -sS -X POST -b "$COOKIE_JAR" --max-time 60 --connect-timeout 8 \
            --keepalive-time 60 \
            -D "$hdr_file" \
            -H "Content-Type: text/plain" \
            -H "Expect:" \
            --data-binary "@$payload_file" \
            "$DELETE_URL" 2>/dev/null | tr -d '\0' || true)"
        http_code="$(awk '/^HTTP/ {code=$2} END {print code}' "$hdr_file" 2>/dev/null)"
        if [[ -z "$resp" ]]; then
            echo "    [X] no body (HTTP ${http_code:-?})"
            total_errors=$((total_errors + (end - sent)))
            sent=$end
            sleep 1
            continue
        fi
        local d fc
        d="$(echo "$resp" | sed -n 's/.*"deleted":\([0-9]\+\).*/\1/p' | head -n 1)"
        fc="$(echo "$resp" | sed -n 's/.*"failed_count":\([0-9]\+\).*/\1/p' | head -n 1)"
        [[ -z "$d" ]] && d=0
        [[ -z "$fc" ]] && fc=0
        total_deleted=$((total_deleted + d))
        total_failed=$((total_failed + fc))
        echo "    [OK] http=$http_code deleted=$d failed=$fc"
        if (( fc > 0 )); then echo "    raw: $resp"; fi
        sent=$end
        # Brief pacing so the device can flush the index append between
        # batches and not become unresponsive on the next request.
        sleep 0.3
    done
    rm -f "$payload_file" "$hdr_file"

    echo "  [OK] Total deleted: $total_deleted | Failed: $total_failed | Errors: $total_errors"
    echo "  [i] Device index rebuild queued automatically"
    return 0
}

echo "[1/5] WiFi & Network Check"
echo "  - Connecting to 192.168.1.200..."
CURRENT_SSID=$(netsh wlan show interfaces 2>/dev/null | grep -i " SSID" | awk -F': ' '{print $2}' | xargs)
if [ "$CURRENT_SSID" != "$SSID_NAME" ]; then
    netsh wlan connect name="$SSID_NAME" > /dev/null 2>&1
    sleep 5
fi
if ! check_wifi_connectivity; then
    echo "[X] MSAT Offline. Check WiFi connection."
    exit 1
fi
echo "  [i] Network reachable"
detect_sync_endpoint
FW_ID="$(get_firmware_id)"
if [[ -n "$FW_ID" ]]; then
    echo "  [i] Firmware ID: $FW_ID"
else
    echo "  [!] Firmware ID endpoint not found (/fwid)"
fi

echo ""
echo "[2/5] Analyze MSAT state before download"
echo "  - Reading current state..."
STATE="$(get_system_state_with_retry 8)"
echo "  - State: $STATE"
if [[ "$STATE" == "UNKNOWN" ]]; then
    echo ""
    echo "[X] State is still UNKNOWN after 8 retries."
    echo "    Troubleshooting:"
    echo "    1. Check if 192.168.1.200 is reachable: ping 192.168.1.200"
    echo "    2. Check if firmware is running: curl -v http://192.168.1.200/"
    echo "    3. Check if /data endpoint exists: curl -v http://192.168.1.200/data"
    echo "    4. Check /dashboard: curl -v http://192.168.1.200/dashboard"
    echo "    5. Verify ESP32 is in IDLE state (check LCD display on device)"
    echo ""
    exit 1
fi
if [[ "$STATE" != "IDLE" && "$STATE" != "SYNC" ]]; then
    echo "[X] MSAT is busy ($STATE). Please wait for IDLE state."
    exit 1
fi
if (( SYNC_SUPPORTED == 1 )); then
    echo "  [i] Entering SYNC mode (Web UI locked: IDLE -> SYNC)..."
    sync_begin
    echo "  [i] SYNC mode active (dashboard locked, LCD: -SYNC-)"
else
    echo "  [i] SYNC mode skipped (endpoint not supported by firmware)"
fi

echo ""
echo "[3/5] Prepare file list"
echo "  - Reading file list from MSAT..."
SD_DATA="$(fetch_data_json)"
if is_data_json_ok "$SD_DATA"; then
    SD_STATUS="$(extract_json_number "$SD_DATA" "st_sd")"
    [[ "$SD_STATUS" == "1" ]] && echo "  - SD: Ready" || echo "  - SD: Not Ready (may retry)"
fi
if [[ "${MSAT_COUNT_ONLY:-0}" == "1" ]]; then
    if [[ "$FORCE_LEGACY_LIST" == "1" ]]; then
        RAW_COUNT_TMP="$(fetch_remote_files_raw 2>/dev/null || true)"
    else
        RAW_COUNT_TMP="$(fetch_remote_manifest_raw 2>/dev/null || fetch_remote_files_raw 2>/dev/null || true)"
    fi
    REMOTE_COUNT_FAST="$(count_file_entries_in_text "$RAW_COUNT_TMP")"
    mapfile -t LOCAL_FILES < <(fetch_local_files)
    echo "  - Remote: $REMOTE_COUNT_FAST files"
    echo "  - Local : ${#LOCAL_FILES[@]} files"
    exit 0
fi
LIST_PAGE=""
RAW_LIST_FILE="$SCRIPT_DIR/.msat_remote_list_$$.txt"
BEST_RAW_LIST_FILE="$SCRIPT_DIR/.msat_remote_list_best_$$.txt"
rm -f "$RAW_LIST_FILE" "$BEST_RAW_LIST_FILE"

BEST_COUNT=0
PREV_COUNT=-1
STABLE_HITS=0
LIST_SOURCE=""

if [[ "$FORCE_LEGACY_LIST" == "1" ]]; then
    echo "  [i] Skip /sync_manifest by option, using legacy list flow"
else
    if fetch_remote_manifest_raw > "$RAW_LIST_FILE"; then
        LIST_SOURCE="sync_manifest"
        PASS_TEXT="$(cat "$RAW_LIST_FILE")"
        BEST_COUNT="$(extract_remote_files_from_text "$PASS_TEXT" | wc -l | tr -d '[:space:]')"
    else
        manifest_rc=$?
        if (( manifest_rc == 2 || manifest_rc == 3 )); then
            echo "  [!] /sync_manifest unavailable, waiting for rebuild then retrying..."
            if wait_for_rebuild 90; then
                echo "  [i] Rebuild done, retrying /sync_manifest..."
                if fetch_remote_manifest_raw 1 > "$RAW_LIST_FILE"; then
                    LIST_SOURCE="sync_manifest"
                    PASS_TEXT="$(cat "$RAW_LIST_FILE")"
                    BEST_COUNT="$(extract_remote_files_from_text "$PASS_TEXT" | wc -l | tr -d '[:space:]')"
                fi
            fi
            if [[ -z "$LIST_SOURCE" ]]; then
                echo "  [!] Retrying /down _index.txt fallback..."
                if fetch_remote_index_via_down > "$RAW_LIST_FILE"; then
                    LIST_SOURCE="index_via_down"
                fi
            fi
        fi
    fi
fi

if [[ -z "$LIST_SOURCE" ]]; then
        echo "  [!] /sync_manifest unavailable, fallback to legacy /listfiles flow"
    echo "  [i] Progressive counting: collecting multiple passes..."
    SNAPSHOT_GEN="$(get_remote_list_generation)"
    if [[ "$SNAPSHOT_GEN" =~ ^[0-9]+$ ]]; then
        echo "  [i] Snapshot generation: $SNAPSHOT_GEN"
    fi
    curl -s -b "$COOKIE_JAR" --max-time 8 \
        --keepalive-time 60 \
        "$LIST_URL" > /dev/null 2>&1 || true
    sleep 1
    LIST_PAGE="$(curl -s -b "$COOKIE_JAR" --max-time 8 \
        --keepalive-time 60 \
        "$LIST_URL_FALLBACK" 2>/dev/null || true)"

    for (( PASS=1; PASS<=REMOTE_COUNT_PASSES_MAX; PASS++ )); do
        rm -f "$RAW_LIST_FILE"
        PASS_SOURCE=""
        EXPECTED_LIST_GEN="$SNAPSHOT_GEN"
        # This firmware bumps "generation" on every HTTP request, so cursor
        # paging can never lock a stable snapshot (it restarts forever).
        # /listfiles?limit=all returns the full live list in one request.
        if fetch_remote_files_raw > "$RAW_LIST_FILE"; then
            PASS_SOURCE="listfiles"
        else
            echo "  [!] Pass $PASS: unable to load list"
            if (( PASS < REMOTE_COUNT_PASSES_MAX )); then sleep 1; continue; fi
            break
        fi

        if [[ -s "$RAW_LIST_FILE" ]]; then
            PASS_TEXT="$(cat "$RAW_LIST_FILE")"
            PASS_COUNT="$(extract_remote_files_from_text "$PASS_TEXT" | wc -l | tr -d '[:space:]')"
        else
            PASS_COUNT=0
        fi

        LISTCOUNT_NOW="$(fetch_remote_count_fast)"
        echo "  [i] Pass $PASS: parsed=$PASS_COUNT, listcount=${LISTCOUNT_NOW:-?}, source=$PASS_SOURCE"

        if [[ "$PASS_COUNT" =~ ^[0-9]+$ ]] && (( PASS_COUNT > BEST_COUNT )); then
            BEST_COUNT=$PASS_COUNT
            cp -f "$RAW_LIST_FILE" "$BEST_RAW_LIST_FILE" 2>/dev/null || true
            LIST_SOURCE="$PASS_SOURCE"
        fi

        # /listfiles?limit=all is a complete live SD scan - one good pass
        # is authoritative. Stop here to avoid extra requests that can
        # push the device back into REBUILDING.
        if [[ "$PASS_COUNT" =~ ^[0-9]+$ ]] && (( PASS_COUNT > 0 )); then
            echo "  [i] Got complete live list at pass $PASS"
            break
        fi

        if [[ "$PASS_COUNT" =~ ^[0-9]+$ ]] && [[ "$PASS_COUNT" == "$PREV_COUNT" ]] && (( PASS_COUNT > 0 )); then
            STABLE_HITS=$((STABLE_HITS + 1))
        else
            STABLE_HITS=0
        fi
        PREV_COUNT="$PASS_COUNT"

        if [[ "$LISTCOUNT_NOW" =~ ^[0-9]+$ ]] && (( PASS_COUNT >= LISTCOUNT_NOW )) && (( STABLE_HITS >= REMOTE_COUNT_STABLE_HITS )); then
            echo "  [i] Count stabilized at pass $PASS"
            break
        fi

        if (( PASS < REMOTE_COUNT_PASSES_MAX )); then
            sleep 1
        fi
    done
fi

if [[ -s "$BEST_RAW_LIST_FILE" ]]; then
    cp -f "$BEST_RAW_LIST_FILE" "$RAW_LIST_FILE" 2>/dev/null || true
fi

if [[ -z "$LIST_SOURCE" ]] && [[ ! -s "$RAW_LIST_FILE" ]]; then
    echo "  [X] Unable to load file list from MSAT"
    rm -f "$RAW_LIST_FILE" "$BEST_RAW_LIST_FILE"
    exit 1
fi

echo "  [i] File list source: ${LIST_SOURCE:-unknown}"

echo "  - Parsing file list..."
set_sync_mode_from_list_page "$LIST_PAGE"
enforce_state_gate_for_sync_mode "$STATE"
REMOTE_LIST_TRUNCATED="$(detect_remote_list_truncated "$LIST_PAGE")"
echo "  [i] File list loaded"
if [[ "$SYNC_MODE" == "SAFE" ]]; then
    echo "  [i] SAFE mode - check IDLE every file"
else
    echo "  [i] FAST mode - full speed transfer (with WiFi fairness)"
fi
if [[ -s "$RAW_LIST_FILE" ]]; then
    RAW_LIST_TEXT="$(cat "$RAW_LIST_FILE")"
    mapfile -t REMOTE_FILES < <(extract_remote_files_from_text "$RAW_LIST_TEXT")
    mapfile -t REMOTE_NAME_SIZE < <(extract_remote_name_size_from_text "$RAW_LIST_TEXT")
else
    mapfile -t REMOTE_FILES < <(extract_remote_files_from_page "$LIST_PAGE")
    mapfile -t REMOTE_NAME_SIZE < <(extract_remote_name_size_from_page "$LIST_PAGE")
fi
rm -f "$RAW_LIST_FILE" "$BEST_RAW_LIST_FILE"
mapfile -t LOCAL_FILES < <(fetch_local_files)
REMOTE_COUNT=${#REMOTE_FILES[@]}
LOCAL_COUNT=${#LOCAL_FILES[@]}
echo "  [i] Remote count (MSAT): $REMOTE_COUNT"
echo "  [i] Local count (PC)  : $LOCAL_COUNT"
if [[ "$REMOTE_LIST_TRUNCATED" == "1" ]]; then
    echo "  [!] MSAT list truncated - count is partial"
fi
LATEST_REMOTE=""
LATEST_LOCAL=""
if (( REMOTE_COUNT > 0 )); then
    LATEST_REMOTE="${REMOTE_FILES[0]}"
fi
if (( LOCAL_COUNT > 0 )); then
    LATEST_LOCAL="${LOCAL_FILES[0]}"
fi
echo "  [i] Latest Remote: ${LATEST_REMOTE:--}"
echo "  [i] Latest Local : ${LATEST_LOCAL:--}"
# Skip the listcount cross-check for the live "listfiles" source:
# /listcount is the capped/stale index number while the parsed count is
# the live SD scan, so they legitimately differ. Forcing a refresh here
# would re-trigger the very index REBUILD we work to avoid.
if [[ "$LIST_SOURCE" != "sync_manifest" && "$LIST_SOURCE" != "listfiles" ]]; then
    REMOTE_COUNT_FAST="$(fetch_remote_count_fast)"
    if [[ "$REMOTE_COUNT_FAST" =~ ^[0-9]+$ ]] && (( REMOTE_COUNT < REMOTE_COUNT_FAST )); then
        echo "  [!] Parsed still lower than listcount (parsed=$REMOTE_COUNT, listcount=$REMOTE_COUNT_FAST)"
        LIST_UPDATED_BEFORE="$(get_remote_list_updated_ms)"
        request_remote_list_refresh
        if ! wait_for_remote_list_refresh "${LIST_UPDATED_BEFORE:-0}" 25; then
            echo "  [!] Timed out waiting for retry refresh; data may still be stale"
        fi
    fi
fi

if (( REMOTE_COUNT == 0 )); then
    echo "  [!] No matching files found on MSAT - exit"
    exit 0
fi

echo ""
echo "[4/5] Analyze and sync"
echo "  - Remote: $REMOTE_COUNT files"
echo "  - Local : $LOCAL_COUNT files"
declare -A LOCAL_SET
for f in "${LOCAL_FILES[@]}"; do
    LOCAL_SET["$f"]=1
done
declare -A REMOTE_SIZE_MAP
for item in "${REMOTE_NAME_SIZE[@]}"; do
    name="${item%%|*}"
    size="${item##*|}"
    if [[ -n "$name" && "$size" =~ ^[0-9]+$ ]]; then
        REMOTE_SIZE_MAP["$name"]="$size"
    fi
done
MISSING_COUNT=0
SMALLER_COUNT=0
NEED_SYNC_COUNT=0
declare -a NEED_SYNC_FILES
for f in "${REMOTE_FILES[@]}"; do
    if [[ -z "${LOCAL_SET[$f]+x}" ]]; then
        MISSING_COUNT=$((MISSING_COUNT + 1))
        NEED_SYNC_COUNT=$((NEED_SYNC_COUNT + 1))
        NEED_SYNC_FILES+=("$f")
        continue
    fi
    remote_size="${REMOTE_SIZE_MAP[$f]:-}"
    local_path="$DEST_DIR/$f"
    if [[ -n "$remote_size" && -f "$local_path" ]]; then
        local_size=$(stat -c%s "$local_path" 2>/dev/null || echo 0)
        if [[ "$local_size" =~ ^[0-9]+$ ]] && (( local_size < remote_size )); then
            SMALLER_COUNT=$((SMALLER_COUNT + 1))
            NEED_SYNC_COUNT=$((NEED_SYNC_COUNT + 1))
            NEED_SYNC_FILES+=("$f")
        fi
    fi
done
echo "  [i] Missing: $MISSING_COUNT files, Smaller: $SMALLER_COUNT files"

declare -A REMOTE_SET
for f in "${REMOTE_FILES[@]}"; do
    REMOTE_SET["$f"]=1
done
EXTRA_COUNT=0
declare -a EXTRA_FILES
for f in "${LOCAL_FILES[@]}"; do
    if [[ -z "${REMOTE_SET[$f]+x}" ]]; then
        EXTRA_COUNT=$((EXTRA_COUNT + 1))
        EXTRA_FILES+=("$f")
    fi
done

if (( EXTRA_COUNT > 0 )); then
    # Quiet summary - after cleanup it's normal for many local files to be
    # absent from the remote, listing them every run is just noise.
    echo "  [i] Local-only files (backed up here, not on MSAT): $EXTRA_COUNT"
fi

if [[ "$RUN_MODE" == "cleanup" ]]; then
    do_cleanup
    echo ""
    echo "[5/5] Finalize"
    echo "  [i] Exiting SYNC mode (SYNC -> IDLE)..."
    sync_end
    echo "  [OK] Cleanup complete"
    echo ""
    rm -f "$COOKIE_JAR"
    sleep 2
    echo "======================================"
    echo -n "Press any key to exit..."
    read -rs -n 1
    exit 0
fi

if (( NEED_SYNC_COUNT == 0 )); then
    echo "  [i] All files synced"
    if [[ "$RUN_MODE" == "both" ]]; then
        do_cleanup
    fi
    echo ""
    echo "[5/5] Finalize"
    echo "  [i] Exiting SYNC mode (SYNC -> IDLE)..."
    sync_end
    echo "  [OK] Sync complete"
    echo ""
    rm -f "$COOKIE_JAR"
    exit 0
fi
echo ""
echo "[5/5] Downloading files"
echo "  [i] WiFi monitoring active (check every $WIFI_CHECK_INTERVAL files)"
print_download_table_header
SYNCED=0
FAILED=0
FILE_INDEX=0
declare -A NEED_SYNC_SET
for f in "${NEED_SYNC_FILES[@]}"; do
    NEED_SYNC_SET["$f"]=1
done

for f in "${REMOTE_FILES[@]}"; do
    FILE_INDEX=$((FILE_INDEX + 1))
    if [[ -z "${NEED_SYNC_SET[$f]+x}" ]]; then
        continue
    fi

    sync_heartbeat

    # 🆕 WiFi monitoring check
    if ! periodic_wifi_check "$FILE_INDEX"; then
        echo "[X] WiFi disconnection detected, stopping sync"
        break
    fi

    if ! ensure_idle_periodic "$FILE_INDEX"; then
        echo "[X] MSAT stayed busy. Stop syncing."
        break
    fi

    enc_f="$(url_encode_filename "$f")"
    dl_url="$BASE_URL/down?f=/autotttoutput/$enc_f"

    # Check if filename has non-ASCII characters (potential encoding issue)
    has_special_chars=0
    if echo "$f" | LC_ALL=C grep -q '[^ -~]'; then
        has_special_chars=1
    fi

    if download_with_progress "$f" "$dl_url" "$DEST_DIR/$f"; then
        SYNCED=$((SYNCED + 1))
        if [[ "$ACTIVE_DOWNLOAD_GAP_SEC" != "0" ]]; then
            sleep "$ACTIVE_DOWNLOAD_GAP_SEC"
        fi
        dashboard_priority_pause "$FILE_INDEX"
    else
        # If file has special chars, try raw (non-encoded) URL as fallback
        if (( has_special_chars )); then
            dl_url_raw="$BASE_URL/down?f=/autotttoutput/$f"
            print_download_row "$f" "-" "RETRY" "*** RAW_URL_FALLBACK"
            rm -f "$DEST_DIR/$f"
            if download_with_progress "$f" "$dl_url_raw" "$DEST_DIR/$f"; then
                SYNCED=$((SYNCED + 1))
                if [[ "$ACTIVE_DOWNLOAD_GAP_SEC" != "0" ]]; then
                    sleep "$ACTIVE_DOWNLOAD_GAP_SEC"
                fi
                dashboard_priority_pause "$FILE_INDEX"
                continue
            fi
        fi
        print_download_row "$f" "-" "RETRY" "*** RETRY_1"
        rm -f "$DEST_DIR/$f"
        sync_heartbeat
        if download_with_progress "$f" "$dl_url" "$DEST_DIR/$f"; then
            SYNCED=$((SYNCED + 1))
            if [[ "$ACTIVE_DOWNLOAD_GAP_SEC" != "0" ]]; then
                sleep "$ACTIVE_DOWNLOAD_GAP_SEC"
            fi
            dashboard_priority_pause "$FILE_INDEX"
        else
            print_download_row "$f" "-" "FAILED" "*** FINAL_FAIL"
            FAILED=$((FAILED + 1))
            rm -f "$DEST_DIR/$f"
        fi
    fi
done
print_download_table_footer

REMOTE_COUNT_AFTER="$REMOTE_COUNT"
mapfile -t LOCAL_FILES_AFTER < <(fetch_local_files)

echo ""
echo "Sync Status"
echo "  Downloaded: $SYNCED | Failed: $FAILED"
echo "  Remote: ${REMOTE_COUNT_AFTER} | Local: ${#LOCAL_FILES_AFTER[@]}"

if (( REMOTE_COUNT_AFTER == ${#LOCAL_FILES_AFTER[@]} )); then
    echo "  [OK] Counts equal"
else
    echo "  [!] Counts differ - review required"
fi

if [[ "$RUN_MODE" == "both" ]]; then
    do_cleanup
fi

sleep 2

echo ""
echo "======================================"
echo -n "Press any key to exit..."
read -rs -n 1
