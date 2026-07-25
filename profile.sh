#!/usr/bin/env bash

# Renderer profiling automation plan
# ===================================
#
# Goal
# ----
# Replace the current manual Tracy workflow with one command that:
#
#   1. Builds gsplat with Tracy enabled.
#   2. Starts a headless Tracy capture under Wine.
#   3. Launches gsplat and records a fixed-duration profiling session.
#   4. Saves the session as a .tracy file.
#   5. Exports individual CPU and GPU zones as CSV.
#   6. Discards warm-up/incomplete frames and aggregates steady-state frames.
#   7. Prints median, mean, and p90 timings plus a representative frame.
#
# This file is currently documentation/a scaffold, not an active script. Keep
# the commands below commented until the capture and analysis steps have been
# validated against a manually inspected Tracy session.


# Proposed interface
# ------------------
#
#   ./profile.sh --seconds 15 --warmup-frames 120 --output build/profiles
#
# Future deterministic benchmark support could add renderer-specific options:
#
#   ./profile.sh --frames 300 --warmup-frames 120 \
#       --render-mode gps --gps-ss 2 --gps-budget-m 250
#
# Environment/command overrides should eventually include:
#
#   PROFILE_SECONDS="${PROFILE_SECONDS:-15}"
#   PROFILE_WARMUP_FRAMES="${PROFILE_WARMUP_FRAMES:-120}"
#   PROFILE_OUTPUT_DIR="${PROFILE_OUTPUT_DIR:-build/profiles}"
#   WINE="${WINE:-wine}"
#   GAMESPLAT_ARGS=("$@")


# Dependencies and validation
# ---------------------------
#
# The repository currently bundles matching Tracy 0.13.1 Windows tools:
#
#   bin/tracy/tracy-capture.exe
#   bin/tracy/tracy-csvexport.exe
#
# The active implementation should verify that these commands exist:
#
#   command -v "$WINE" >/dev/null
#   command -v python3 >/dev/null
#   test -f bin/tracy/tracy-capture.exe
#   test -f bin/tracy/tracy-csvexport.exe
#
# It should create a timestamped output directory so repeated runs do not
# overwrite one another, for example:
#
#   run_id="$(date +%Y%m%d-%H%M%S)"
#   run_dir="$PROFILE_OUTPUT_DIR/$run_id"
#   mkdir -p "$run_dir"
#   trace_path="$run_dir/renderer.tracy"
#   cpu_csv="$run_dir/cpu-events.csv"
#   gpu_csv="$run_dir/gpu-events.csv"


# Step 1: build the profiled application
# --------------------------------------
#
#   ENABLE_PROFILER=1 ./build.sh
#
# The build must finish successfully before capture starts. Avoid rebuilding
# between compared runs unless the code or profiler configuration changed.


# Step 2: start a headless Tracy capture
# --------------------------------------
#
# tracy-capture waits for gsplat to connect. Its -s timer starts after the
# connection succeeds. -f allows replacing the selected trace path.
#
# Wine usually accepts native paths, but converting the absolute output path
# explicitly is less ambiguous:
#
#   trace_windows="$($WINE winepath -w "$(realpath -m "$trace_path")")"
#   "$WINE" bin/tracy/tracy-capture.exe \
#       -a 127.0.0.1 \
#       -p 8086 \
#       -o "$trace_windows" \
#       -f \
#       -s "$PROFILE_SECONDS" &
#   capture_pid=$!
#
# Install a cleanup trap before launching either background process. It should
# terminate only processes started by this script and should preserve the exit
# status of a failed capture/export:
#
#   app_pid=""
#   cleanup() {
#       if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
#           kill -TERM "$app_pid" 2>/dev/null || true
#           wait "$app_pid" 2>/dev/null || true
#       fi
#       if [[ -n "${capture_pid:-}" ]] && kill -0 "$capture_pid" 2>/dev/null; then
#           kill -INT "$capture_pid" 2>/dev/null || true
#           wait "$capture_pid" 2>/dev/null || true
#       fi
#   }
#   trap cleanup EXIT INT TERM


# Step 3: launch and exercise gsplat
# ----------------------------------
#
#   ./gsplat "${GAMESPLAT_ARGS[@]}" &
#   app_pid=$!
#
# Initial implementation:
#
#   - Keep camera movement/manual interaction during PROFILE_SECONDS.
#   - Print a prompt explaining that capture has started.
#   - Do not enable splat diagnostics during measured GPS runs because their
#     synchronous readback and shader atomics perturb the profile.
#
# This already eliminates manual Tracy connection, saving, frame-statistics
# setup, and clipboard export. Camera automation should be added separately;
# shell-driven xdotool input would be brittle and should not be the long-term
# benchmark mechanism.
#
# Wait for tracy-capture to finish and save the trace, then stop gsplat:
#
#   wait "$capture_pid"
#   capture_pid=""
#   kill -TERM "$app_pid" 2>/dev/null || true
#   wait "$app_pid" 2>/dev/null || true
#   app_pid=""
#
# Do not SIGKILL tracy-capture. It writes/finalizes the .tracy file only after
# its normal capture loop exits.


# Step 4: export individual CPU and GPU events
# --------------------------------------------
#
# tracy-csvexport cannot restrict aggregate output to a frame/time range, so
# export individual events and perform range selection ourselves:
#
#   "$WINE" bin/tracy/tracy-csvexport.exe -u "$trace_windows" > "$cpu_csv"
#   "$WINE" bin/tracy/tracy-csvexport.exe -g "$trace_windows" > "$gpu_csv"
#
# CPU -u columns in Tracy 0.13.1:
#
#   name,src_file,src_line,ns_since_start,exec_time_ns,thread,value
#
# GPU -g columns:
#
#   name,src_file,Time from start of program,GPU execution time
#
# Redirected stdout is written by the Linux shell, so the CSV paths themselves
# do not need Wine conversion.


# Step 5: aggregate frames with embedded Python
# ---------------------------------------------
#
# Embed a dependency-free Python program in this script rather than requiring
# pandas or a second repository script:
#
#   python3 - "$cpu_csv" "$gpu_csv" "$PROFILE_WARMUP_FRAMES" <<'PY'
#   # Parse with csv.DictReader and aggregate with statistics.
#   PY
#
# Analysis algorithm:
#
#   1. Read all CPU and GPU event rows and convert timestamps/durations to int.
#   2. Sort every zone's occurrences by timestamp because csvexport groups
#      records by source location rather than guaranteeing global time order.
#   3. Use CPU "render submit" occurrences as CPU frame anchors.
#   4. Use GPU "gaussian gpu cull/project" occurrences as GPU frame anchors.
#   5. Associate one occurrence of each expected renderer zone with each anchor
#      by timestamp/order. Discard incomplete first/last frames.
#   6. Drop PROFILE_WARMUP_FRAMES complete frames before calculating results.
#   7. Report count, median, mean, p90, min, and max for each available zone.
#   8. Compute total measured GPU renderer time per frame and report the frame
#      whose total is closest to the median as the representative frame.
#
# Start with these CPU zones:
#
#   render submit
#   render commit
#   gaussian gpu cull/project
#   gps clear
#   gps count
#   gps expand
#   gps splat
#   gps resolve
#   render imgui pass
#
# Start with these GPU zones:
#
#   gaussian gpu cull/project
#   gps clear
#   gps count
#   gps expand
#   gps splat
#   gps resolve
#   imgui pass
#
# Zones not present in a selected rendering mode should be shown as unavailable,
# not treated as zero. The output should identify the trace path and all run
# parameters so results can be reproduced.
#
# Prefer distributions across many steady-state frames over manually selecting
# one frame. A proposed text table is:
#
#   Renderer profile: 300 steady-state frames
#
#   Zone                         median_ms   mean_ms    p90_ms
#   CPU render submit                 2.41       2.53       2.88
#   GPU gaussian cull/project         3.72       3.75       3.91
#   GPU gps clear                     0.81       0.82       0.86
#   GPU gps count                     0.34       0.35       0.38
#   GPU gps expand                    1.20       1.24       1.35
#   GPU gps splat                    18.42      18.57      19.30
#   GPU gps resolve                   0.73       0.74       0.79
#   GPU measured total               25.22      25.47      26.40
#
#   Representative steady-state frame: 184
#   Trace: build/profiles/<run-id>/renderer.tracy


# Future: deterministic renderer benchmark mode
# ---------------------------------------------
#
# Fully reproducible runs require application support rather than synthetic
# keyboard/mouse input. Add command-line arguments to gsplat for:
#
#   - Rendering mode.
#   - GPS supersampling and work budget.
#   - Accumulation/TAA settings.
#   - Warm-up and measured frame counts.
#   - A fixed camera or deterministic camera path.
#   - Automatic exit after the measured frame count.
#
# Once available, profile.sh can sweep configurations such as 8M/32M/250M GPS
# budgets and print a comparison table from one invocation. Until then, record
# the manually selected renderer settings alongside every trace.
