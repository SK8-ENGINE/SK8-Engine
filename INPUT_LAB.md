# Skate 3 Input Lab

This build exposes guest controller 0 through the local Windows named pipe
`\\.\pipe\Skate3InputLab`. The game can be unfocused or behind other windows:
the commands are inserted after the recomp polls its ordinary input backends.

The server accepts connections only from the local computer. Automation uses
`replace` mode by default, which ignores physical-controller state while a
synthetic state or replay is active. Run `disable` to immediately return to
ordinary controller input.

## Quick start

Launch the newly built game and wait for the main menu. In another PowerShell
window:

```powershell
cd .\harness

.\Input.cmd status
.\Input.cmd reset-observation
.\Input.cmd tap -Buttons A -Polls 5
.\Input.cmd set -RightY -32768
.\Input.cmd neutral
.\Input.cmd disable
```

`set` holds the complete supplied controller state. `tap` queues that state
and then releases it. Durations are guest input polls, not milliseconds, so
they remain independent of window focus and host scheduling.

## Replays

```powershell
.\Input.cmd replay -Path .\examples\ollie-test.json
Start-Sleep -Milliseconds 800
.\Input.cmd observe
```

An internally confirmed ollie reports `sequence_completed=1`,
`polls_consumed` greater than zero, one or both `B_OLLIE` animation flags,
and `ollie_confirmed=1`. This detector reads the game state machine's own
animation identifiers; it does not inspect rendered images.

Replay fields are:

- `polls`: number of guest controller polls
- `buttons`: array such as `["A", "LB"]`, a string such as `"A+LB"`, or a mask
- `lt`, `rt`: triggers from 0 through 255
- `lx`, `ly`, `rx`, `ry`: sticks from -32768 through 32767

Missing fields are neutral. A replay supports up to 1024 steps.

## Repeatable experiments

Stand at a suitable flat-ground starting position and create the baseline once:

```powershell
.\Experiment.cmd set-marker
```

Run a complete reset, replay, internal observation and JSONL recording:

```powershell
.\Experiment.cmd run -Replay .\examples\ollie-test.json
```

The reset uses Skate 3's native session-marker system (LB + D-pad Up). This
restores the gameplay starting position and normal respawn state without
keyboard focus or visual checks. Results are appended to
`results\experiments.jsonl`.

Each experiment also records the complete set of generated guest functions
entered during its observation window. Run a matched idle replay and subtract
its `coverage_addresses` from a trick run to locate trick-specific code paths.

## Safety

Return control to the physical controller:

```powershell
.\Input.cmd disable
```

Closing the game also clears all automation state. The pipe cannot send
keyboard or mouse input and does not require the Skate 3 window to be focused.
