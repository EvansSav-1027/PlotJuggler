# SAV Log Loader (`data_load_savb`)

Opens SAV Helm OS `SensorDataLogger` flight-recorder logs (`logs/log_*_IST.bin`,
magic `SAVB`) directly in PlotJuggler 4 — no `sav_log_to_csv.py` step.

## Build and run

Standalone CMake project; it builds the plugin SDK at the version the app pins
(or uses an installed one) and fetches any missing dependency.

```bash
cmake -S plugins/data_load_savb -B build-savb -DCMAKE_BUILD_TYPE=Release
cmake --build build-savb -j
ctest --test-dir build-savb --output-on-failure
./run.sh --plugin-dir build-savb/bin      # then File → Load data → a .bin log
```

`--plugin-dir` replaces the default extension directory for that run; to keep
the marketplace plugins too, copy `build-savb/bin/libsavb_source_plugin.so`
(and its `.pjmanifest.json`) into the app's extension directory instead.

## What you get

Exactly what loading the CSV from `sav_log_to_csv.py` with PlotJuggler's CSV
loader gives, so a layout made for the CSV works on the `.bin` unchanged:

- One topic named after the file (`log_20261013_170242_IST`), with one field
  per CSV column — the same names, in the same order (the order is stored in
  the log's header), nothing added.
- One record per CSV row (one per logged tick), carrying that row's non-blank
  cells; a blank CSV cell is simply absent, as with the CSV loader. Text
  columns (`STATUS/active_behavior`, `BUILD/git_ref`, the UTC text columns) are
  strings, everything else float64; `nan`/`ovf` cells are NaN.
- Rows are built the way the decoder builds them: `GAIN`, `MODE` and `ORGN`
  values carry forward onto every later row, and the derived columns
  (`TIME/host_time`, `GPS/time` — `*_utc` in older logs — `SONAR/age_s`, `IMU/accel_cal/*`,
  `MAG/cal/*`, `MAG/accel_cal/*`) are recomputed from the logged values and
  `CAL`.
- Time axis: each row's `TIME/host_epoch_ns`, as local wall-clock time. Newer logs
  hold it that way already (the logger adds the host's UTC offset). Older logs (header
  column `TIME/host_time_utc`) hold true UTC, so the plugin adds this machine's offset,
  fixed once per log — the same shift `sav_log_to_csv.py` applies — and
  `GPS/epoch_ns` gets the same treatment.

A truncated tail (power cut) or corrupt stretch is skipped with a warning;
everything readable is kept.

The format is defined in `sav-os/src/hal/plugins_real/sensor_data_logger.cpp`;
this reader follows `sav-os/tools/sav_log_to_csv.py`. Formats are read from the
log's own FMT records, so new or changed messages need no plugin change — only
a framing change (`kFormatVersion` bump) does.
