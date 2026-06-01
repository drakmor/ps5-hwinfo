# ps5-hwinfo

`ps5-hwinfo` is a small PlayStation 5 payload for collecting hardware and
runtime telemetry from a console. It is built with
[`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) and produces two
payload ELFs:

- `hwinfo_sysinfo.elf` prints system telemetry.
- `hwinfo_bench.elf` runs read-only disk read benchmarks.

The payload writes its results to stdout, so use it with a payload loader or
deployment tool that shows the payload console output.

## What It Reports

The system telemetry payload currently prints:

- hardware model, system software version, main SoC ID, and current process
  `AUTHID`
- SoC and CPU temperature
- fan duty, fan policy/mode summary, and optional raw ICC fan debug data
- USB and Blu-ray drive power state
- CPU frequency and per-core CPU clocks
- SoC clock domains, including known and inferred labels such as `GFXCLK`,
  `FCLK`, `UCLK`, cache clocks, video clocks, memory clock, and boost limit
- ShellCore-style state such as `HLT`, `GC`, `BAPM`, and VM stats
- partially decoded SoC power rails from `sceKernelGetSocPowerConsumption`
- system RAM usage and a VRAM page-table proxy when available
- logical CPU load estimated from idle-thread deltas

Some low-level fields are still reverse-engineering notes rather than confirmed
Sony API names. The payload marks uncertain labels with `?` or prints them as
raw/debug data.

The benchmark payload scans readable regular files from these locations when
available:

- `/mnt/sandbox/pfsmnt/*-app0`
- `/data`
- `/mnt/shadowmnt`
- `/mnt/ext0`
- `/mnt/ext1`
- `/mnt/usb0`
- `/mnt/usb1`

For each source it runs a single-thread read pass and an 8-thread read pass,
printing progress, file count, read volume, elapsed time, throughput, CPU load,
and per-core clocks. It is intended to read existing data only.

## Requirements

- A PS5 environment capable of running payload ELFs.
- `ps5-payload-sdk`, with `PS5_PAYLOAD_SDK` pointing to the SDK root.
- A payload listener/loader on the console. The Makefile defaults to host
  `ps5` and port `9021`.

The default SDK path is `/opt/ps5-payload-sdk`.

## Build

```sh
make
```

To use a non-default SDK path:

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

Build outputs:

```text
hwinfo_sysinfo.elf
hwinfo_bench.elf
```

Clean generated payloads:

```sh
make clean
```

## Deploy

Deploy the system telemetry payload:

```sh
make test-sysinfo PS5_HOST=<console-ip>
```

Deploy the disk benchmark payload:

```sh
make test-bench PS5_HOST=<console-ip>
```

If your listener uses a different port:

```sh
make test-sysinfo PS5_HOST=<console-ip> PS5_PORT=<port>
make test-bench PS5_HOST=<console-ip> PS5_PORT=<port>
```

You can also send either ELF with your usual payload loader.

## Runtime Configuration

The Makefile builds separate payloads instead of relying on command-line
arguments. `prospero-deploy` does not forward extra `argv` values to the
payload, so runtime options are compile-time defines near the top of `main.c`:

```c
#define HWINFO_CFG_WATCH false
#define HWINFO_CFG_DUMP_FAN_MODE true
#define HWINFO_CFG_SET_FAN_MODE false
#define HWINFO_CFG_FAN_MODE_RAW 0u
#define HWINFO_CFG_INTERVAL_SEC 1u
```

- `HWINFO_CFG_WATCH`: repeat system telemetry until the payload is stopped.
- `HWINFO_CFG_DUMP_FAN_MODE`: include raw fan mode/debug state.
- `HWINFO_CFG_SET_FAN_MODE`: set a raw ICC fan mode before printing telemetry.
- `HWINFO_CFG_FAN_MODE_RAW`: raw fan mode value used by the setter.
- `HWINFO_CFG_INTERVAL_SEC`: sampling interval for CPU-load deltas and watch
  mode.

Rebuild after changing any of these values.

## Notes

- The payload attempts to switch its process `AUTHID` to
  `0x4800000000000010` on startup when the SDK helper is available.
- CPU logical load is an idle-thread delta estimate, not a confirmed bit-exact
  clone of ShellCore's CPU utilization display.
- Fan policy labels are inferred from ShellCore behavior and registry state.
  Raw ICC state is kept as debug output.
- SoC power data is only partially decoded. Known-looking rail groups are
  labeled, while uncertain values remain raw or marked as inferred.
