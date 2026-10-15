# Real-Time Elevator Simulator

A C++17 elevator simulation with concurrent floor, scheduler, and elevator components communicating through real UDP sockets. Includes timed CSV requests, C-LOOK request scheduling, door and motor faults, terminal status output, and automated tests. Runs on Windows (Winsock) and Linux (POSIX sockets).

## Build and run

Requires CMake 3.16+, a C++17 compiler (MSVC, GCC, or Clang), and the platform thread/socket libraries. No third-party C++ dependencies.

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Linux / single-configuration generators:

```sh
./build/elevator_simulator --time-scale 0.01
```

Windows / Visual Studio generators (PowerShell):

```powershell
.\build\Release\elevator_simulator.exe --time-scale 0.01
```

Alternatively, build and test with Docker:

```sh
docker build -t elevator-simulator .
docker run --rm elevator-simulator
docker run --rm elevator-simulator --input resources/elevator_input3.csv --time-scale 0.01 --quiet
```

The Docker build runs the test suite. The default scenario intentionally injects faults, so some trips can fail. Exit codes: `0` = all trips completed, `1` = invalid input or runtime error, `2` = simulation finished with failed trips.

| Option | Default | Purpose |
| --- | --- | --- |
| `--input FILE` | `resources/elevator_input6.csv` | CSV scenario, relative to the working directory |
| `--floors N` | `22` | Building floors, numbered from 1 |
| `--elevators N` | `4` | Elevator workers, from 1 to 128 |
| `--time-scale FACTOR` | `1.0` | Wall time per unit of simulated time; `0.01` runs about 100× faster |
| `--timeout-ms N` | `600000` | Wall-clock watchdog; increase for longer scenarios |
| `--quiet` | off | Print only final request and fault counts |
| `--help` | | Show usage |

Movement takes 5 simulated seconds per floor; opening and closing each take 1 second; boarding takes 3 seconds. These values can also be configured through the C++ `Config` API. This is a soft real-time simulation using a monotonic clock and timed waits; it does not provide hard real-time deadline guarantees.

## Components and concurrency

- A floor producer thread releases requests according to their CSV timestamps, relative to the first request.
- A scheduler thread receives requests, maintains pending and active trips, and assigns work when idle elevators poll.
- Each elevator has its own worker thread and UDP client. Only that worker changes its cabin state.
- Concrete classes implement the State design pattern: Idle, Moving, Opening, Boarding, Closing, DoorInterrupt, and Stuck. Each state handles its own behavior and transitions.
- Scheduler queues are owned exclusively by its server thread. Shared cancellation uses an atomic flag; logging and error capture use a mutex. All threads are joined before results are read or resources are destroyed.

Components communicate over loopback UDP using dynamically allocated ports. A text protocol replaces Java object serialization. Requests, assignments, completion notices, and fault events all cross actual sockets. RPC calls have bounded retries and sequence numbers; the scheduler caches each client's last reply to avoid processing a retransmission twice. Invalid packets are rejected, and a watchdog cancels stalled simulations. This implementation runs the components in one process; it does not expose a remote or separate-process mode.

## C-LOOK scheduling

The scheduler applies C-LOOK to **pending pickup requests**. Starting at its current pickup cursor, it chooses the lowest pending pickup at or above the cursor. When none remains above, it wraps to the lowest pending pickup. Calls at the same floor retain arrival order. It never travels to an unused building boundary to complete a sweep.

For a cursor at floor 5 and pending pickups `[2, 8, 6, 1]`, selection order is `[6, 8, 1, 2]`.

An idle elevator receives the next selected request and completes its pickup and drop-off before requesting more work. C-LOOK controls dispatch order across the fleet; cabin travel follows the assigned passenger's destination, including downward travel. This is not a shared-cabin multi-stop route optimizer. Only requests already pending participate in each scheduling decision; later requests enter subsequent decisions. Continuous arrivals at one floor can delay other calls.

## Input and faults

The seven original CSV scenarios are in `resources/`. Header and example:

```csv
Time,Floor,Floor Button,Car Button,TimeError,Error
14:33:44.5,2,UP,4,0,null
14:33:46.2,4,DOWN,3,1000,DOOR_INTERRUPT
14:33:50.0,3,DOWN,1,3000,ELEVATOR_STUCK
```

Timestamps accept `HH:mm:ss` with optional 1–3 fractional digits (`.5` means 500 ms). Rows are stably sorted by timestamp. Floors, direction, fault names, and nonnegative delays are validated with line-number errors. Scenarios must fit within one day; midnight rollover is not inferred. A same-floor trip is allowed and completes a door cycle. A header-only file shuts down cleanly.

- `null` or `NONE`: no fault.
- `DOOR_INTERRUPT`: fires after `TimeError` simulated milliseconds during pickup boarding, reopens the doors, and restarts boarding once. A delay exceeding the boarding interval does not trigger an interruption.
- `ELEVATOR_STUCK`: fires after `TimeError` simulated milliseconds from pickup, interrupts the current action, disables the cabin, and marks its onboard trip failed. Onboard passengers require external rescue; they are not silently reassigned. Pending requests remain available to healthy cabins. If all cabins fail, remaining requests are counted as failed after the producer finishes.

The terminal reports state transitions and assignments. A final summary accounts for submitted, completed, and failed trips, plus door interruptions and stuck cabins. Terminal output replaces the former Swing interface; GUI panels, interactive buttons, and graphical lamps are not included.

## Tests

`ctest` runs a dependency-free test executable covering:

- CSV parsing, fractional timestamps, malformed input, and message serialization.
- C-LOOK ascending order, circular wrap, and same-floor ordering.
- Actual UDP round trips, receive timeouts, and RPC retries after a dropped reply.
- Concurrent service of 24 requests across four elevators.
- Same-floor and empty-input termination, invalid configuration, and watchdog cancellation.
- Door recovery, a fully stuck fleet, and continued service by a healthy elevator.
- End-to-end execution of all seven supplied CSV scenarios with request accounting.

## Resume claim check

| Claim | What this implementation supports |
| --- | --- |
| C++, UDP, and multithreading | C++17 sources, real UDP communication, and dedicated component threads. |
| State design pattern | Polymorphic state objects encapsulate elevator actions and transitions. |
| Synchronized shared state | Atomic cancellation, mutex-protected shared logging/error capture, and thread-confined queues and cabin state. |
| C-LOOK scheduling | Explicit circular ordering of pending pickup calls, tested independently. Be precise that this is dispatch scheduling, not multi-stop cabin routing. |
| Optimized routing and reduced service latency | C-LOOK is implemented, but a latency improvement has **not been measured** against another scheduler. Do not present a numerical or proven reduction based on this project alone. |

The technical parts of the supplied resume bullets now relate to the code. The claim about reducing latency remains unverified: demonstrating it would require comparative workloads and latency measurements.

## Layout

```text
include/elevator/   Public models, UDP client, and simulation API
src/model.cpp      CSV parsing, wire format, C-LOOK
src/udp.cpp        Winsock/POSIX UDP sockets and RPC retries
src/simulator.cpp  Scheduler, concrete elevator states, thread lifecycle
src/main.cpp       Command-line entry point
resources/         CSV scenarios
tests/tests.cpp    Unit and integration tests
```

The Java implementation and Java-specific project settings have been replaced by this C++ build. The port consolidates message classes into a validated request format and a small command protocol.
