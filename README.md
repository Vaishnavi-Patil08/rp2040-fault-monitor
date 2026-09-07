# RP2040 Fault Monitor

A fault-monitoring and recovery system for the Raspberry Pi Pico (RP2040), written in C.

The project explores how embedded systems can detect failures at different levels and respond appropriately: execution overruns are logged, stalled tasks are detected through heartbeats and recovered in software, and a complete firmware hang is recovered by the RP2040 hardware watchdog.

A Python host-side validation tool communicates with the firmware over UART and automatically injects faults, observes system behavior, and verifies recovery.

## Overview

The firmware runs a small cooperative scheduler containing periodic tasks for GPIO activity, a monitored counter task, ADC sampling, UART command processing, and health monitoring.

The system supports three main fault scenarios:

| Fault | Detection | Response |
|---|---|---|
| Task execution overrun | Execution-time budget | Record warning in fault log |
| Counter task stall | Heartbeat timeout | Automatically remove injected stall and verify task recovery |
| Complete firmware hang | Hardware watchdog | Reset RP2040 and report watchdog reset on reboot |

This creates two levels of recovery:

```text
Individual task failure
        ↓
Heartbeat monitor
        ↓
Software recovery
        ↓
Heartbeat verifies task resumed


Entire firmware hangs
        ↓
Software can no longer execute
        ↓
Hardware watchdog expires
        ↓
RP2040 resets
        ↓
Firmware reports watchdog reset reason
```

## Architecture

```text
                    ┌──────────────────────┐
                    │      Python Host     │
                    │     Validation       │
                    └──────────┬───────────┘
                               │
                          USB ↔ UART
                               │
                    ┌──────────▼───────────┐
                    │   UART0 RX / TX      │
                    │ Interrupt-driven RX  │
                    └──────────┬───────────┘
                               │
                         RX Ring Buffer
                               │
                    ┌──────────▼───────────┐
                    │   Diagnostic CLI     │
                    │                      │
                    │ status               │
                    │ faults               │
                    │ inject on/off        │
                    │ stall on/off         │
                    │ hang                 │
                    └──────────┬───────────┘
                               │
           ┌───────────────────┼────────────────────┐
           │                   │                    │
           ▼                   ▼                    ▼
    Counter Task          ADC Monitor          Fault Injection
    250 ms period         500 ms period
           │                   │
           └─────────┬─────────┘
                     │
              Health Monitoring
                     │
          ┌──────────┴───────────┐
          │                      │
   Execution Budget        Heartbeat Monitor
          │                      │
      Overrun Log           Stall Detection
                                 │
                          Recovery Policy
                                 │
                       Verify Heartbeat Return

                 Main-loop watchdog refresh
                             │
                 ┌───────────▼───────────┐
                 │ RP2040 HW Watchdog    │
                 │ 2 second timeout      │
                 └───────────┬───────────┘
                             │
                       System Reset
```

## Features

### Cooperative task scheduling

Periodic firmware tasks execute from a non-blocking main loop using elapsed-time checks rather than blocking delays.

Current task periods include:

- Counter task: 250 ms
- ADC monitor: 500 ms
- LED task: 1000 ms
- Counter heartbeat timeout: 1000 ms
- Sensor heartbeat timeout: 2000 ms

### Low-level GPIO control

The external LED on GPIO 15 is controlled through direct RP2040 MMIO access to the SIO registers.

The firmware directly configures and uses registers including:

```text
SIO GPIO_OUT_SET
SIO GPIO_OUT_CLR
SIO GPIO_OE_SET
IO_BANK0 GPIO15_CTRL
```

This part of the project was used to work directly with memory-mapped peripheral registers, masks, `volatile`, and RP2040 register addressing.

### Execution-time monitoring

The counter task is timed using the RP2040 microsecond timer.

A task execution budget is defined as:

```text
100 us
```

If execution exceeds the budget:

```text
execution time > budget
        ↓
overrun counter incremented
        ↓
FaultOverrun recorded
        ↓
SEVERITY_WARNING
```

The CLI can intentionally inject an overrun for validation.

### ADC range monitoring

GPIO 26 / ADC0 is sampled periodically and converted from the RP2040 12-bit ADC result into millivolts.

Configured valid range:

```text
1000 mV – 2500 mV
```

A transition outside the valid range generates a sensor-range fault. The active-fault flag prevents the same continuous condition from flooding the event log.

### Fault event log

Faults are stored in a fixed-size circular event log.

Each record contains:

```c
FaultId
FaultSeverity
timestampUs
```

The log can hold up to 50 records and overwrites the oldest entries once full.

Current fault identifiers include:

```text
FaultOverrun
FaultStall
FaultSensorRange
FaultUART
```

The `faults` CLI command prints stored records in chronological order.

### Interrupt-driven UART

UART0 runs at:

```text
115200 baud
```

Pins:

```text
GP0  → UART0 TX
GP1  → UART0 RX
```

Incoming bytes are handled by a UART RX interrupt and placed into a circular receive buffer.

The main loop consumes bytes from that buffer and assembles complete CLI commands, keeping command parsing outside the interrupt service routine.

```text
UART hardware
      ↓
RX interrupt
      ↓
ISR
      ↓
circular RX buffer
      ↓
main loop
      ↓
command parser
```

### Diagnostic CLI

Supported commands:

```text
status
faults
inject on
inject off
stall on
stall off
hang
```

Examples:

```text
status
```

Returns runtime health information such as:

```text
Total OverRuns
overRunFault
Max Counter Exec Time
health counter
sensor value
sensor fault
recovery mode
recoveries
```

```text
faults
```

Prints the circular fault log.

```text
inject on
inject off
```

Enables or disables execution-overrun injection.

```text
stall on
stall off
```

Controls artificial counter-task stalling.

```text
hang
```

Intentionally stops the main firmware execution so the hardware watchdog can be tested.

## Heartbeat Monitoring

Tasks publish a heartbeat whenever they successfully execute.

For the counter task:

```text
counter executes
      ↓
lastHeartbeat updated
```

The heartbeat monitor periodically compares the current time against the last successful heartbeat.

```text
current_time - lastHeartbeat >= timeout
```

If the timeout is exceeded:

```text
stallFaultActive = true
FaultStall recorded
SEVERITY_CRITICAL
```

The fault is logged once per distinct stall rather than continuously on every scheduler iteration.

## Automatic Task Recovery

A separate recovery policy reacts to a detected counter stall.

```text
Counter heartbeat expires
        ↓
FaultStall logged
        ↓
Recovery mode entered
        ↓
Injected stall removed
        ↓
Counter allowed to execute again
        ↓
Counter updates heartbeat
        ↓
Heartbeat checker confirms health
        ↓
Recovery mode cleared
        ↓
Successful recovery count incremented
```

Detection and recovery are intentionally separated:

- The heartbeat monitor determines whether the task is healthy.
- The recovery policy decides what action to take.
- A fresh heartbeat verifies that recovery actually succeeded.

The system does not declare success immediately after attempting recovery.

## Hardware Watchdog Recovery

Task-level monitoring cannot help if the entire main loop stops executing.

To handle that case, the RP2040 hardware watchdog is configured with a two-second timeout.

During healthy execution:

```c
watchdog_update();
```

is reached repeatedly from the main loop.

The `hang` command intentionally traps execution:

```text
hang
  ↓
main loop stops
  ↓
watchdog is no longer refreshed
  ↓
watchdog expires
  ↓
RP2040 resets
```

During the next boot, the firmware checks the reset reason using the RP2040 watchdog API and reports:

```text
BOOT: Previous reset caused by watchdog
```

This provides recovery even when the software health-monitoring code itself can no longer execute.

## Host-Side Validation

`tools/validation.py` provides automated black-box validation over UART using PySerial.

Instead of manually issuing commands through a serial terminal, the Python program:

```text
sends command
      ↓
observes UART output
      ↓
parses firmware state
      ↓
injects fault
      ↓
waits for expected behavior
      ↓
queries final state
      ↓
asserts expected result
```

The validator also synchronizes the firmware command parser before testing so that stale or partial UART input does not affect the first command.

### Automated tests

#### Status communication

The script requests firmware health status and parses the response into structured Python values.

#### Overrun detection

```text
Read overrun count
      ↓
Enable overrun injection
      ↓
Allow counter task to execute
      ↓
Disable injection
      ↓
Read new overrun count
      ↓
Assert new count > old count
```

#### Stall recovery

```text
Read recovery count
      ↓
Inject counter stall
      ↓
Wait for:
  "Automatic recovery initiated"
  "System recovered successfully"
      ↓
Read final status
      ↓
Assert recovery count increased
      ↓
Assert recovery mode == 0
```

#### Watchdog recovery

```text
Verify firmware responds
      ↓
Send "hang"
      ↓
Wait for watchdog reboot message
      ↓
Resynchronize UART command parser
      ↓
Request status
      ↓
Verify firmware is operational again
```

### Example validation run

```text
[PASS] Status parsing successful:
{'Total OverRuns': 0, 'overRunFault': 0, ...}

[PASS] Overrun detection: 0 --> 6

[PASS] Stall recovery:
recoveries 0 --> 1, recovery mode=0

[PASS] Watchdog recovery:
firmware rebooted and responded to status, health counter=1
```

## Hardware

- Raspberry Pi Pico / RP2040
- USB-UART adapter
- Breadboard
- External LED
- 220 Ω resistor
- Potentiometer
- Jumper wires

### Connections

#### External LED

```text
GP15 → 220 Ω resistor → LED → GND
```

#### UART

```text
Pico GP0 TX → USB-UART RX
Pico GP1 RX ← USB-UART TX
Pico GND    ↔ USB-UART GND
```

The Pico is powered separately over USB. The USB-UART adapter VCC is not required.

#### Potentiometer

```text
3.3V ─────┐
          potentiometer
GND  ─────┘
            │
          wiper
            │
          GP26 / ADC0
```

## Software Requirements

Firmware:

- Raspberry Pi Pico SDK
- ARM GCC toolchain
- CMake
- Ninja

Host validation:

- Python 3
- PySerial

Install PySerial:

```bash
python3 -m pip install pyserial
```

## Build

Configure the project using the Pico SDK and build with CMake/Ninja.

Example:

```bash
ninja -C build
```

The resulting UF2 can be copied to the Pico while it is in BOOTSEL mode.

Example:

```bash
cp build/fault-monitor.uf2 /Volumes/RPI-RP2/
```

## Run Automated Validation

Identify the USB-UART device:

```bash
ls /dev/cu.usbserial*
```

Then run:

```bash
python3 tools/validation.py \
  --port /dev/cu.usbserial-A5069RR4
```

The actual serial-device name depends on the host machine and USB-UART adapter.

## Repository Structure

```text
fault-monitor/
├── CMakeLists.txt
├── fault-monitor.c
├── README.md
├── .gitignore
└── tools/
    └── validation.py
```

## Design Decisions

### Heartbeat and watchdog solve different failures

A heartbeat is useful while the monitoring software is still executing.

If one task stalls:

```text
monitor still runs
→ stall can be detected
→ software recovery is possible
```

If the entire main loop hangs:

```text
monitor also stops
→ software cannot detect its own failure
→ independent hardware watchdog is required
```

Using both mechanisms provides layered recovery.

### Recovery is verified, not assumed

Removing the injected stall is only a recovery attempt.

The firmware waits until the counter executes again and publishes a fresh heartbeat before marking recovery successful.

### UART ISR does minimal work

The interrupt handler receives bytes and places them into a ring buffer. Command parsing and command execution occur in the main loop instead of inside the ISR.

This keeps interrupt execution short and avoids performing complex application work at interrupt context.

### Fixed-size buffers

The firmware uses fixed-size structures for the UART RX buffer, CLI command buffer, and fault log rather than dynamic allocation.

This keeps memory behavior predictable for the embedded target.

## Current Scope

This project focuses on fault detection, recovery, diagnostics, and host-side validation rather than implementing a full RTOS.

Current recovery demonstrations are intentionally controlled through fault injection so that each failure mode can be reproduced and verified deterministically.

ADC range faults require changing the potentiometer position manually because the host validation program cannot physically manipulate the analog input.

## What I Learned

This project was built as a hands-on exploration of embedded/system software concepts including:

- RP2040 peripheral architecture
- memory-mapped I/O and `volatile`
- bit masking and register manipulation
- cooperative scheduling
- execution-time budgets
- UART communication
- interrupt service routines
- circular buffers
- ADC acquisition
- fault state management
- heartbeat-based health monitoring
- software recovery policies
- hardware watchdogs
- reset-reason diagnostics
- serial protocol synchronization
- Python/PySerial host automation
- black-box fault injection and validation
