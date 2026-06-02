# Neuromorphic Accelerator C Simulator

This repository is a dependency-free C11 simulator for a digital neuromorphic accelerator. It currently models two pieces of the architecture:

- A fixed-size, hardware-style neuromorphic core that consumes spike bitmap tiles, applies sparse event-driven weighted accumulation, thresholds output neurons, and emits multicast spike tiles.
- A small 2D mesh router that routes spike tiles and ACK messages with deterministic dimension-order routing and multicast splitting.

The executable is a self-checking test bench. It prints a trace of the modeled behavior and exits with a nonzero status if an expected tile, ACK, counter, or route result is wrong.

## Current status

The project is best understood as an architecture-stage simulator, not a production neural-network runtime. It is intentionally explicit about tables, counters, SRAM-sized arrays, queues, and routing metadata so that the software model stays close to the hardware mechanism being explored.

Implemented today:

- Input spike tiles as byte-array bitmaps.
- Output spike tiles as multicast router-facing messages.
- Per-output accumulator SRAM lookup and threshold activation.
- Sparse bitmap-to-event encoding split across input lanes.
- Output-neuron SIMD lane scheduling with accumulator reuse.
- Two-stage input LUTs for spike-tile consumption.
- Two-stage ACK LUTs for successor synchronization returns.
- Two-stage output route LUTs for successor tile destinations and predecessor ACK destinations.
- Natural step ordering through ACK counters rather than explicit timestep tags.
- Standalone XY/YX 2D mesh routing with multicast fanout splitting.
- A small multi-core mesh scenario wired together in the test bench.

Still intentionally simplified:

- The router and cores are connected by the test bench rather than by a reusable mesh-fabric module.
- Queue back-pressure is represented by bounded arrays, but full retry/NACK behavior is not modeled yet.
- Compute-cycle counters approximate the high-level SIMD/encoder schedule; memory-bank conflicts and lower-level pipeline timing are not yet modeled.
- Mapping is configured manually in the test bench instead of generated from a graph compiler.

## Repository layout

```text
.
├── Makefile
├── README.md
├── include/
│   ├── neuromorphic_core.h
│   └── neuromorphic_router.h
└── src/
    ├── main.c
    ├── neuromorphic_core.c
    └── neuromorphic_router.c
```

## Build and run

Requirements:

- A C11 compiler such as GCC or Clang.
- `make`.

Build:

```sh
make
```

Run the self-checking demo/test bench:

```sh
make run
```

Equivalent direct invocation:

```sh
./build/neuromorphic_core_demo
```

Clean generated files:

```sh
make clean
```

The default build uses strict warnings:

```text
-std=c11 -Wall -Wextra -Wpedantic -Werror -O2
```

## Core model

An `NmcCore` models one accelerator core with local SRAM-like arrays and fixed maximum capacities. It has:

- Input group table: first-stage lookup from a local input group index to a range in the input/output pair LUT.
- Input/output pair LUT: second-stage lookup entries that identify which output group receives the input and where that pair's contiguous weight matrix starts.
- ACK group table: first-stage lookup from a local ACK group index to a range in the ACK/output pair LUT.
- ACK/output pair LUT: second-stage lookup entries that increment successor-ACK counters on output groups.
- Output group table: per-output thresholds, input readiness counters, successor ACK counters, and output route starts.
- Output route LUT: successor destinations for output spike tiles followed by predecessor destinations for multicast ACKs.
- Accumulator LUT: per-output starting addresses into accumulator SRAM.
- Weight SRAM: signed 16-bit contiguous row-major matrices.
- Accumulator SRAM: signed 32-bit output-neuron accumulators.
- Output and ACK queues used by the surrounding router/fabric model.

The core uses start-plus-terminal LUTs throughout. For example, input group `g` reads `start(g)` and `start(g + 1)` to find its exclusive second-stage LUT range. Output routes use the same idea, but each output has two starts: one for successor tile routes and one for predecessor ACK routes.

## Input and output flow

The core-facing input tile format is `NmcInputTile`:

- `width`: number of neurons/spikes represented by the payload.
- `group_index`: local input group index.
- `payload`: bit-packed spike bitmap.

When a tile arrives:

1. The input group index selects a range in the input/output pair LUT.
2. Each pair entry selects an output group and a weight-matrix offset.
3. The bitmap is converted into sparse input events.
4. The selected row-major weight matrix is accumulated into that output group's accumulator slice.
5. The output group's `input_count` is incremented.
6. If all required inputs have arrived and successor synchronization is complete, the output group activates.

Activation thresholds the output accumulators into a spike bitmap, resets the accumulator slice, and emits an `NmcNetworkTile` with one payload and a multicast destination list. After successful output injection, the core also emits one multicast `NmcAckMessage` to the predecessors that fed the completed output group.

ACK messages are consumed through the ACK LUT rather than by directly naming an output group. This keeps ACK handling index-based in the same style as input-tile consumption:

1. The ACK group index selects a range in the ACK/output pair LUT.
2. Each second-stage entry increments one output group's successor-ACK counter.
3. Any output groups that were input-complete but ACK-blocked are flushed when their ACK counters reach the successor count.

## Natural step ordering and flow control

The model avoids explicit timestep tags. Instead, it uses a one-in-flight style protocol over mapped edges:

- Input readiness is tracked by `input_count` versus `input_requirement` on each output group.
- Successor synchronization is tracked by `ack_count` versus the number of successor route entries.
- Output emission is allowed only when required inputs have arrived, successor ACKs for the previous emission have returned, and the output queue can accept the tile.
- A successful emission immediately returns predecessor ACKs, allowing predecessors to send the next natural-step tile on those edges.
- New inputs may be consumed while the output is waiting for successor ACKs, but the next output tile cannot leave until those ACKs arrive.

This models local ordering pressure without a global timestep barrier. A future fabric module can make this stricter by adding explicit NACK/BUSY retries, per-edge ordering checks, or more detailed queue back-pressure.

## Compute scheduling model

The compute path models the main datapath scheduling choices rather than a cycle-accurate RTL pipeline.

Input bitmap handling:

- Payloads are bitmaps with widths that must be positive multiples of 8.
- The bitmap is split across `NMC_INPUT_PARALLELISM` input lanes.
- Each lane has a `NMC_EVENT_ENCODER_WINDOW`-bit priority-search window.
- A lane emits at most one nonzero event index per encoder cycle.
- Empty windows can be skipped to reach later sparse events without widening the priority encoder.

Accumulation scheduling:

- Output neurons are processed in blocks of `NMC_OUTPUT_PARALLELISM` lanes.
- A block of accumulators is held while the model walks all sparse input-event steps for that block.
- For each output lane and event step, up to `NMC_INPUT_PARALLELISM` weights are reduced and added to the output accumulator.
- Weight matrices are contiguous and row-major: `weight_offset + output_index * input_width + event_index`.

The public counters on `NmcCore` expose the abstract schedule:

- `last_input_tile_event_count`
- `last_input_tile_encoder_cycles`
- `last_input_tile_compute_cycles`
- `total_encoder_cycles`
- `total_compute_cycles`

## Router model

`NmcRouter` is intentionally separate from `NmcCore`. It operates on coordinate-addressed `NmcRouterMessage` values:

- `width > 0` means a spike tile with payload.
- `width == 0` means an ACK message with no payload.
- Each destination contains a mesh coordinate and a destination-local group index.

For each destination, the router chooses one next-hop port:

- `LOCAL` if the target coordinate equals the router coordinate.
- `EAST` or `WEST` for x-direction movement.
- `NORTH` or `SOUTH` for y-direction movement.

The routing order is configurable:

- `NMC_ROUTER_XY`: resolve x before y.
- `NMC_ROUTER_YX`: resolve y before x.

For multicast messages, the router partitions the destination list by next-hop port and enqueues one split message per non-empty output port. Local messages can be decoded into either a core-facing `NmcInputTile` or an `NmcAckMessage`.

## Demo and regression coverage

The executable in `src/main.c` covers three scenarios.

### Single-core natural-step test

One core is configured with:

- Two input groups.
- Two output groups.
- Three ACK groups.
- Shared and output-specific successor ACK paths.
- Contiguous weight slices for each input/output pair.

The trace checks that:

- An output with one required input can fire before an output with two required inputs.
- Outputs immediately send predecessor ACKs after injection.
- Later natural-step inputs can be consumed while outputs are successor-ACK blocked.
- Returning successor ACKs flush the correct pending outputs.
- Reversed input arrival order still produces correct readiness and blocking behavior.

### Encoder scheduling test

A sparse 32-bit input checks that the event encoder skips empty windows and still produces the expected event count, encoder-cycle count, and compute-cycle count.

### Router and mesh tests

The router tests check that:

- An XY router splits a multicast across local, east, west, north, and south output ports.
- Multiple destinations sharing the same first hop are grouped into one forwarded message.
- Local spike-tile messages decode into core-facing input tiles.
- YX routing chooses a different first hop for an off-axis destination.

The multi-core mesh test connects three cores through a 2x2 router mesh:

- Source core `10` at `(0, 0)`.
- Observer core `12` at `(1, 0)`.
- Consumer core `11` at `(1, 1)`.
- A pass-through router at `(0, 1)`.

It verifies a west-side injected input, source-to-consumer routing, consumer-to-observer routing, ACK return to the source, and a second source emission after the ACK arrives.

## Important limits and knobs

The main hardware-style limits are defined in `include/neuromorphic_core.h` and `include/neuromorphic_router.h`.

| Constant | Meaning |
| --- | --- |
| `NMC_MAX_GROUP_NEURONS` | Maximum neurons/spikes represented by one tile payload. |
| `NMC_MAX_INPUT_GROUPS` | Maximum input groups on one core. |
| `NMC_MAX_ACK_GROUPS` | Maximum ACK input groups on one core. |
| `NMC_MAX_OUTPUT_GROUPS` | Maximum output groups on one core. |
| `NMC_MAX_TILE_DESTINATIONS` | Maximum multicast destinations in one tile or ACK message. |
| `NMC_WEIGHT_MEMORY_SIZE` | Signed 16-bit weight SRAM entries per core. |
| `NMC_ACCUMULATOR_MEMORY_SIZE` | Signed 32-bit accumulator SRAM entries per core. |
| `NMC_INPUT_PARALLELISM` | Number of input event lanes in the compute schedule. |
| `NMC_OUTPUT_PARALLELISM` | Number of output-neuron lanes in the compute schedule. |
| `NMC_ROUTER_MAX_PORT_QUEUE` | Queue depth per router output port. |

Tile widths must be positive multiples of 8 and no larger than `NMC_MAX_GROUP_NEURONS`.

## Development notes

- Keep the model dependency-free unless a new dependency is clearly worth it.
- Preserve the fixed-size-array style in core datapath structures; it mirrors SRAM/register-file limits and makes capacity failures explicit.
- Configure LUT terminal entries carefully. Many ranges are derived from entry `i` and entry `i + 1`.
- Treat core IDs and mesh coordinates as separate namespaces. The test bench currently maps between them before injecting core-generated messages into routers.
- If changing function signatures or table semantics, update both the core tests and router/mesh tests in `src/main.c`.

## Suggested next iterations

1. Extract the ad hoc mesh wiring in `src/main.c` into a reusable mesh-fabric module.
2. Add explicit NACK/BUSY retry behavior for full queue back-pressure modeling.
3. Model accumulator-bank allocation and memory-bank conflicts for the parallel event schedule.
4. Add tests for overlapping input windows and convolution-like mappings.
5. Add a small mapping builder so route/LUT tables are generated instead of manually assembled in the test bench.
6. Add deeper traces for deadlock-prone recurrent or cyclic core graphs.
