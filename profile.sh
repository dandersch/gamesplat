#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

PROFILE_SECONDS="${PROFILE_SECONDS:-15}"
PROFILE_WARMUP_FRAMES="${PROFILE_WARMUP_FRAMES:-120}"
PROFILE_OUTPUT_DIR="${PROFILE_OUTPUT_DIR:-build/profiles}"
PROFILE_PORT="${PROFILE_PORT:-}"
PROFILE_RENDER_MODE="${PROFILE_RENDER_MODE:-alpha}"
PROFILE_CAMERA_MOTION="${PROFILE_CAMERA_MOTION:-look}"
PROFILE_GPS_SS="${PROFILE_GPS_SS:-2}"
PROFILE_GPS_BUDGET_M="${PROFILE_GPS_BUDGET_M:-8}"
PROFILE_GPS_ACCUMULATION="${PROFILE_GPS_ACCUMULATION:-off}"
WINE="${WINE:-wine}"
BUILD=1
LABEL="renderer"
ANALYZE_TRACE=""
APP_ARGS=()
capture_pid=""
app_pid=""

usage() {
    cat <<'EOF'
Usage: ./profile.sh [options] [-- gsplat-arguments...]

Build gsplat with Tracy, capture a timed interactive renderer session, and
print steady-state CPU/GPU zone statistics.

Options:
  -s, --seconds N          Capture duration after connection (default: 15)
  -r, --render-mode MODE   Startup mode: alpha, stochastic, or gps (default: alpha)
  -c, --camera-motion MODE Camera motion: look or none (default: look)
      --gps-ss N           GPS supersampling factor, 1..4 (default: 2)
      --gps-budget-m N     GPS work budget in Mi items, 1..250 (default: 8)
      --gps-accumulation M GPS accumulation: on or off (default: off)
  -p, --port N             Tracy port (default: discover from launched gsplat)
  -w, --warmup-frames N    Complete frames to discard (default: 120)
  -o, --output DIR         Profile output root (default: build/profiles)
  -l, --label NAME         Label used in the run directory (default: renderer)
      --no-build           Use the existing profiler-enabled ./gsplat binary
      --analyze TRACE      Analyze an existing .tracy file without capturing
  -h, --help               Show this help

Examples:
  ./profile.sh
  ./profile.sh --render-mode gps --gps-budget-m 32 --label gps-32m
  ./profile.sh --render-mode gps --camera-motion none
  ./profile.sh --seconds 20 --warmup-frames 100 -- ply=res/scene.sog
  ./profile.sh --analyze bin/tracy/gsplat.tracy --warmup-frames 30

Keep splat diagnostics disabled when measuring GPS because they perturb timing.
EOF
}

die() {
    echo "profile.sh: $*" >&2
    exit 1
}

require_uint() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer"
}

while (($# > 0)); do
    case "$1" in
        -s|--seconds)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_SECONDS="$2"
            shift 2
            ;;
        -w|--warmup-frames)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_WARMUP_FRAMES="$2"
            shift 2
            ;;
        -r|--render-mode)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_RENDER_MODE="$2"
            shift 2
            ;;
        -c|--camera-motion)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_CAMERA_MOTION="$2"
            shift 2
            ;;
        --gps-ss)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_GPS_SS="$2"
            shift 2
            ;;
        --gps-budget-m)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_GPS_BUDGET_M="$2"
            shift 2
            ;;
        --gps-accumulation)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_GPS_ACCUMULATION="$2"
            shift 2
            ;;
        -p|--port)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_PORT="$2"
            shift 2
            ;;
        -o|--output)
            (($# >= 2)) || die "$1 requires a value"
            PROFILE_OUTPUT_DIR="$2"
            shift 2
            ;;
        -l|--label)
            (($# >= 2)) || die "$1 requires a value"
            LABEL="$2"
            shift 2
            ;;
        --no-build)
            BUILD=0
            shift
            ;;
        --analyze)
            (($# >= 2)) || die "$1 requires a trace path"
            ANALYZE_TRACE="$2"
            BUILD=0
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            APP_ARGS=("$@")
            break
            ;;
        *)
            die "unknown option '$1' (pass gsplat arguments after --)"
            ;;
    esac
done

require_uint "capture seconds" "$PROFILE_SECONDS"
require_uint "warm-up frame count" "$PROFILE_WARMUP_FRAMES"
require_uint "GPS supersampling factor" "$PROFILE_GPS_SS"
require_uint "GPS work budget" "$PROFILE_GPS_BUDGET_M"
((PROFILE_SECONDS > 0)) || die "capture seconds must be greater than zero"
((PROFILE_GPS_SS >= 1 && PROFILE_GPS_SS <= 4)) || die "GPS supersampling factor must be between 1 and 4"
((PROFILE_GPS_BUDGET_M >= 1 && PROFILE_GPS_BUDGET_M <= 250)) || die "GPS work budget must be between 1 and 250"
case "$PROFILE_RENDER_MODE" in
    alpha|stochastic|gps) ;;
    *) die "render mode must be alpha, stochastic, or gps" ;;
esac
case "$PROFILE_CAMERA_MOTION" in
    look|none) ;;
    *) die "camera motion must be look or none" ;;
esac
case "$PROFILE_GPS_ACCUMULATION" in
    on|off) ;;
    *) die "GPS accumulation must be on or off" ;;
esac
if [[ -n "$PROFILE_PORT" ]]; then
    require_uint "Tracy port" "$PROFILE_PORT"
    ((PROFILE_PORT > 0 && PROFILE_PORT < 65536)) || die "Tracy port must be between 1 and 65535"
fi

command -v "$WINE" >/dev/null 2>&1 || die "Wine executable '$WINE' was not found"
command -v winepath >/dev/null 2>&1 || die "winepath was not found"
command -v python3 >/dev/null 2>&1 || die "python3 was not found"
[[ -f bin/tracy/tracy-capture.exe ]] || die "bin/tracy/tracy-capture.exe is missing"
[[ -f bin/tracy/tracy-csvexport.exe ]] || die "bin/tracy/tracy-csvexport.exe is missing"

safe_label="$(printf '%s' "$LABEL" | tr -cs '[:alnum:]_.-' '_')"
safe_label="${safe_label#_}"
safe_label="${safe_label%_}"
[[ -n "$safe_label" ]] || safe_label="renderer"
run_id="$(date +%Y%m%d-%H%M%S)-${safe_label}"
run_dir="$PROFILE_OUTPUT_DIR/$run_id"
mkdir -p "$run_dir"
run_dir="$(realpath "$run_dir")"
cpu_csv="$run_dir/cpu-events.csv"
gpu_csv="$run_dir/gpu-events.csv"
summary_csv="$run_dir/summary.csv"

stop_process() {
    local pid="$1"
    local signal="${2:-TERM}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "-$signal" "$pid" 2>/dev/null || true
        for _ in {1..20}; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_process "$app_pid" TERM
    stop_process "$capture_pid" INT
}
trap cleanup EXIT INT TERM

discover_tracy_port() {
    local pid="$1"
    local port=""
    command -v ss >/dev/null 2>&1 || return 1
    for _ in {1..150}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        port="$(ss -H -ltnp 2>/dev/null | awk -v owner="pid=$pid," '
            index($0, owner) {
                count = split($4, address, ":")
                print address[count]
                exit
            }
        ')"
        if [[ "$port" =~ ^[0-9]+$ ]]; then
            printf '%s\n' "$port"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if [[ -n "$ANALYZE_TRACE" ]]; then
    [[ -f "$ANALYZE_TRACE" ]] || die "trace '$ANALYZE_TRACE' does not exist"
    trace_path="$(realpath "$ANALYZE_TRACE")"
    echo "Analyzing existing trace: $trace_path"
else
    if ((BUILD)); then
        echo "Building profiler-enabled gsplat..."
        ENABLE_PROFILER=1 ./build.sh
    fi
    [[ -x ./gsplat ]] || die "./gsplat is missing or not executable"

    ./gsplat "render_mode=$PROFILE_RENDER_MODE" "camera_motion=$PROFILE_CAMERA_MOTION" \
        "gps_ss=$PROFILE_GPS_SS" "gps_budget_m=$PROFILE_GPS_BUDGET_M" \
        "gps_accumulation=$PROFILE_GPS_ACCUMULATION" \
        "${APP_ARGS[@]}" >"$run_dir/gsplat.log" 2>&1 &
    app_pid=$!
    if [[ -n "$PROFILE_PORT" ]]; then
        tracy_port="$PROFILE_PORT"
    else
        echo "Waiting for gsplat's Tracy endpoint..."
        if ! tracy_port="$(discover_tracy_port "$app_pid")"; then
            cat "$run_dir/gsplat.log" >&2
            die "could not discover the Tracy port for gsplat; use --port to specify it"
        fi
    fi

    echo "gsplat Tracy endpoint: 127.0.0.1:$tracy_port"
    trace_path="$run_dir/renderer.tracy"
    trace_windows="$(WINEDEBUG=-all winepath -w "$trace_path" 2>/dev/null | tail -n 1 | tr -d '\r')"
    [[ -n "$trace_windows" ]] || die "winepath failed to convert '$trace_path'"

    echo "Starting ${PROFILE_SECONDS}s Tracy capture (camera: $PROFILE_CAMERA_MOTION)..."
    WINEDEBUG=-all "$WINE" bin/tracy/tracy-capture.exe \
        -a 127.0.0.1 \
        -p "$tracy_port" \
        -o "$trace_windows" \
        -f \
        -s "$PROFILE_SECONDS" \
        >"$run_dir/capture.log" 2>&1 &
    capture_pid=$!

    set +e
    wait "$capture_pid"
    capture_status=$?
    set -e
    capture_pid=""

    stop_process "$app_pid" TERM
    app_pid=""

    if ((capture_status != 0)); then
        cat "$run_dir/capture.log" >&2
        die "Tracy capture failed with status $capture_status"
    fi
    [[ -s "$trace_path" ]] || die "Tracy did not produce a non-empty trace"
fi

trace_windows="$(WINEDEBUG=-all winepath -w "$trace_path" 2>/dev/null | tail -n 1 | tr -d '\r')"
[[ -n "$trace_windows" ]] || die "winepath failed to convert '$trace_path'"

echo "Exporting CPU events..."
if ! WINEDEBUG=-all "$WINE" bin/tracy/tracy-csvexport.exe -u "$trace_windows" \
    >"$cpu_csv" 2>"$run_dir/cpu-export.log"; then
    cat "$run_dir/cpu-export.log" >&2
    die "CPU event export failed"
fi

echo "Exporting GPU events..."
if ! WINEDEBUG=-all "$WINE" bin/tracy/tracy-csvexport.exe -g "$trace_windows" \
    >"$gpu_csv" 2>"$run_dir/gpu-export.log"; then
    cat "$run_dir/gpu-export.log" >&2
    die "GPU event export failed"
fi

python3 - "$cpu_csv" "$gpu_csv" "$PROFILE_WARMUP_FRAMES" "$summary_csv" "$trace_path" <<'PY'
import csv
import math
import statistics
import sys
from collections import defaultdict

cpu_path, gpu_path, warmup_arg, summary_path, trace_path = sys.argv[1:]
warmup = int(warmup_arg)


def read_events(path, gpu=False):
    events = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            return events
        for row in reader:
            try:
                if gpu:
                    timestamp = int(row["Time from start of program"])
                    duration = int(row["GPU execution time"])
                else:
                    timestamp = int(row["ns_since_start"])
                    duration = int(row["exec_time_ns"])
            except (KeyError, TypeError, ValueError):
                continue
            name = (row.get("name") or "").strip()
            if name and duration >= 0:
                events.append((timestamp, duration, name))
    return events


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def stats(values):
    return {
        "count": len(values),
        "total": sum(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "p90": percentile(values, 0.90),
        "min": min(values),
        "max": max(values),
    }


def grouped(events, begin, end):
    result = defaultdict(list)
    for timestamp, duration, name in events:
        if begin <= timestamp < end:
            result[name].append((timestamp, duration))
    for values in result.values():
        values.sort()
    return result


def ordered_names(groups, preferred, minimum_count):
    present = [name for name, values in groups.items() if len(values) >= minimum_count]
    first = [name for name in preferred if name in present]
    return first + sorted(set(present) - set(first))


cpu_events = read_events(cpu_path)
gpu_events = read_events(gpu_path, gpu=True)
cpu_anchors = sorted(event for event in cpu_events if event[2] == "render submit")

if len(cpu_anchors) < warmup + 2:
    print(
        f"profile.sh: trace has {len(cpu_anchors)} complete render anchors, "
        f"not enough to discard {warmup} warm-up frames",
        file=sys.stderr,
    )
    sys.exit(2)

# Exclude the final anchor because GPU query results for the last submitted
# frame may not have been collected before capture ended.
steady_anchors = cpu_anchors[warmup:-1]
range_begin = steady_anchors[0][0]
range_end = cpu_anchors[-1][0]
cpu_groups = grouped(cpu_events, range_begin, range_end)
gpu_groups = grouped(gpu_events, range_begin, range_end)
frame_count = len(steady_anchors)
minimum_count = max(1, frame_count // 2)

cpu_preferred = [
    "render submit",
    "render commit",
    "frame update",
    "imgui",
    "gaussian gpu cull/project",
    "gaussian gpu sort",
    "gps clear",
    "gps count",
    "gps expand",
    "gps splat",
    "gps resolve",
    "render imgui pass",
]
gpu_preferred = [
    "gaussian gpu cull/project",
    "gaussian gpu sort",
    "radix hist",
    "radix prefix",
    "radix scatter",
    "gps clear",
    "gps count",
    "gps expand",
    "gps splat",
    "gps resolve",
    "splat pass",
    "mesh pass",
    "overlay pass",
    "wireframe pass",
    "imgui pass",
]

cpu_names = ordered_names(cpu_groups, cpu_preferred, minimum_count)
gpu_names = ordered_names(gpu_groups, gpu_preferred, minimum_count)
rows = []


def print_section(title, names, groups, domain):
    if not names:
        print(f"\n{title}: no recurring zones found")
        return
    print(f"\n{title}")
    print(f"{'Zone':34} {'count':>7} {'median_ms':>11} {'mean_ms':>10} "
          f"{'p90_ms':>10} {'min_ms':>10} {'max_ms':>10}")
    for name in names:
        durations = [duration for _, duration in groups[name]]
        value = stats(durations)
        print(
            f"{name[:34]:34} {value['count']:7d} "
            f"{value['median'] / 1e6:11.3f} {value['mean'] / 1e6:10.3f} "
            f"{value['p90'] / 1e6:10.3f} {value['min'] / 1e6:10.3f} "
            f"{value['max'] / 1e6:10.3f}"
        )
        rows.append({"domain": domain, "name": name, **value})


print(f"\nRenderer profile: {frame_count} steady-state frames")
print(f"Discarded warm-up frames: {warmup}")
print_section("CPU instrumentation", cpu_names, cpu_groups, "CPU")
print_section("GPU zones", gpu_names, gpu_groups, "GPU")

# GPS stages occur once and in the same order each frame. Align them by
# occurrence to identify an actual frame nearest the median measured GPS time.
gps_names = [
    "gaussian gpu cull/project",
    "gps clear",
    "gps count",
    "gps expand",
    "gps splat",
    "gps resolve",
]
available_gps = [name for name in gps_names if name in gpu_groups]
if len(available_gps) >= 2:
    aligned_count = min(len(gpu_groups[name]) for name in available_gps)
    frame_totals = [
        sum(gpu_groups[name][index][1] for name in available_gps)
        for index in range(aligned_count)
    ]
    median_total = statistics.median(frame_totals)
    representative = min(
        range(aligned_count), key=lambda index: abs(frame_totals[index] - median_total)
    )
    capture_frame = warmup + representative + 1
    print(f"\nRepresentative steady-state frame: {capture_frame}")
    print(f"Measured GPU total: {frame_totals[representative] / 1e6:.3f} ms")
    for name in available_gps:
        print(f"  {name:31} {gpu_groups[name][representative][1] / 1e6:8.3f} ms")

with open(summary_path, "w", newline="", encoding="utf-8") as f:
    fieldnames = ["domain", "name", "count", "total_ns", "median_ns", "mean_ns", "p90_ns", "min_ns", "max_ns"]
    writer = csv.DictWriter(f, fieldnames=fieldnames)
    writer.writeheader()
    for row in rows:
        writer.writerow({
            "domain": row["domain"],
            "name": row["name"],
            "count": row["count"],
            "total_ns": row["total"],
            "median_ns": row["median"],
            "mean_ns": row["mean"],
            "p90_ns": row["p90"],
            "min_ns": row["min"],
            "max_ns": row["max"],
        })

print(f"\nTrace: {trace_path}")
print(f"Summary CSV: {summary_path}")
PY

echo "Raw CPU CSV: $cpu_csv"
echo "Raw GPU CSV: $gpu_csv"
