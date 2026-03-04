# Heltec V4 Support Verification Evidence (Task 9)

Date: 2026-02-23 (PST)
Repo: `/home/justin/src/bramble`

## Environment bootstrap used for ESP-IDF commands

All `idf.py` commands were run from repo root with ESP-IDF environment activation so `idf.py` resolves correctly:

```bash
IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "${IDF_VENV:-}" ]]; then export PATH="$IDF_VENV/bin:$PATH"; fi
source ~/src/esp-idf/export.sh
```

## Verification matrix

### 1) Heltec V3 build

Command:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build
```

Result: ❌ FAILED

Failure context:

- Compile failed in `main/rpc_methods.c`:

```text
/home/justin/src/bramble/main/rpc_methods.c: In function 'handle_get_gps_position':
/home/justin/src/bramble/main/rpc_methods.c:1009:16: error: 'RPC_ERR_NOT_SUPPORTED' undeclared (first use in this function); did you mean 'ESP_ERR_NOT_SUPPORTED'?
 1009 |         return RPC_ERR_NOT_SUPPORTED;
      |                ^~~~~~~~~~~~~~~~~~~~~
      |                ESP_ERR_NOT_SUPPORTED
```

- Build system reported:

```text
ninja: build stopped: subcommand failed.
ninja failed with exit code 1
```

### 2) Heltec V4 build

Command:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
```

Result: ❌ FAILED

Failure context:

- Same compile failure in `main/rpc_methods.c`:

```text
/home/justin/src/bramble/main/rpc_methods.c:1009:16: error: 'RPC_ERR_NOT_SUPPORTED' undeclared (first use in this function); did you mean 'ESP_ERR_NOT_SUPPORTED'?
 1009 |         return RPC_ERR_NOT_SUPPORTED;
      |                ^~~~~~~~~~~~~~~~~~~~~
      |                ESP_ERR_NOT_SUPPORTED
```

- Build system reported:

```text
ninja: build stopped: subcommand failed.
ninja failed with exit code 1
```

### 3) T-Deck build

Command:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build
```

Result: ❌ FAILED

Failure context:

- Same compile failure in `main/rpc_methods.c`:

```text
/home/justin/src/bramble/main/rpc_methods.c:1009:16: error: 'RPC_ERR_NOT_SUPPORTED' undeclared (first use in this function); did you mean 'ESP_ERR_NOT_SUPPORTED'?
 1009 |         return RPC_ERR_NOT_SUPPORTED;
      |                ^~~~~~~~~~~~~~~~~~~~~
      |                ESP_ERR_NOT_SUPPORTED
```

- Build system reported:

```text
ninja: build stopped: subcommand failed.
ninja failed with exit code 1
```

### 4) Host tests

Command:

```bash
bash test/run_all_tests.sh
```

Result: ✅ PASSED

Summary:

```text
Test suites: 53 total, 53 passed, 0 failed
ALL TESTS PASSED ✓
```

## Hardware validation status (tonight)

The following **on-device physical checks are still pending** and were not executed in this Task 9 software-only verification run:

- Heltec V4 hardware smoke test (boot, display, buttons, radio bring-up).
- Heltec V4 GNSS/L76K on-device acquisition and telemetry path validation.
- Cross-board RF smoke confirmation on physical hardware (Heltec V3 / Heltec V4 / T-Deck).
