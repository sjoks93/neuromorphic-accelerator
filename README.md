# Neuromorphic Accelerator Simulator

This repository contains a dependency-free C11 simulator for a digital neuromorphic accelerator architecture. The simulator models a tiled, event-driven core with hardware-like SRAM tables, sparse bitmap communication, routed multicast traffic, ACK-based flow control, unified memory, and a programmable activation v-ALU.

The goal is not to build a production neural-network runtime yet. The goal is to keep a C model close enough to the intended hardware that memory layout, mapping pressure, synchronization, communication overhead, accumulator reuse, and activation flexibility can be studied before moving the design into synthesizable RTL.

## Why this architecture

The simulator explores an accelerator organization built around groups of neurons rather than fixed crossbar blocks. Logical groups are mapped into compact SRAM-backed structures, communicated as bitmaps, reduced through sparse event processing, and activated by a small vector ALU.

Key ideas:

- **Group-to-group mapping**: logical source and destination groups are mapped through compact LUT ranges instead of a static crossbar. This avoids fragmentation, supports many group sizes, allows flexible fan-in/fan-out, and can support weight sharing because multiple group edges may point at the same weight slice.
- **Unified memory structure**: weights, packed accumulators, and optional activation state/parameters share one memory space, so mappings can handle different weight-to-neuron ratios without hard partition waste.
- **Grouped bitmaps**: spikes are carried as bit-packed group payloads instead of one-address-per-event AER packets. This reduces communication metadata and gives the destination core a dense representation that can be converted into parallel sparse event streams.
- **On-the-fly encoder**: incoming bitmaps are translated into sparse input events at the destination core. Empty windows are skipped, so sparse activity avoids unnecessary compute work.
- **Adder-tree reduction**: sparse events from parallel encoder lanes are consumed concurrently, routed through weight lanes, reduced by output channel, and accumulated into reused accumulator storage.
- **Parallel output neurons**: output groups are processed in SIMD-like rows, allowing one input event to select an output-wide weight row and update many output neuron accumulators.
- **Neuron-first schedule**: within a bitmap window, each neuron state's accumulator lanes are reused across the full bitmap/event schedule before moving to the next set of neurons, improving local state reuse.
- **Programmable v-ALU activation**: the activation stage is a small spike-only vector ALU. It can adapt to different neuron models with different states, parameters, and functions without dedicating dead hardware or fragmenting memory for every possible neuron type.

## Implemented features

### Mapping and memory

- High-level network mapping from logical groups, external inputs, group-to-group edges, and core placement.
- Automatic lowering into per-core input groups, output groups, input/output pair LUTs, ACK LUTs, route LUTs, predecessor ACK routes, terminal sentinels, accumulator starts, and input requirements.
- Efficient group-to-group mapping with flexible fan-in and fan-out.
- Shared route/ACK metadata for multicast outputs and successor synchronization.
- Input-bank-major, output-wide weight storage in unified memory.
- Caller-facing weights can remain conventional output-major matrices; the mapper transposes them into the banked memory layout.
- Packed accumulators stored in the same signed 16-bit unified memory lane space as weights.
- Optional activation state and per-neuron parameters stored in unified memory only when a model requires them.

### Input and compute path

- Byte-array bitmap input tiles with byte-aligned group widths.
- Sparse bitmap-to-event encoding split across parallel input lanes.
- Event encoder windows that skip empty sparse regions.
- Sparse event-driven weighted accumulation.
- Input event rows select output-wide weight slices.
- Accumulator MUX logic reads and writes only the packed accumulator lanes needed by a selected output neuron value.
- Adder-tree-style reduction of concurrent lane events into output accumulators.
- Schedule counters for event count, encoder cycles, compute cycles, and accumulated totals.

### Output, activation, and synchronization

- Output groups track input readiness with `input_count` versus `input_requirement`.
- Successor flow control tracks ACK credit with `ack_count` versus route successor count.
- Natural step ordering is modeled without explicit timestep tags.
- Output activation waits for input completion, successor ACK credit, valid route metadata, and queue capacity.
- Feed-forward predecessor ACKs are emitted when output activation succeeds.
- Recurrent predecessor ACKs can be emitted at input-window completion to break ACK cycles while preserving one-tile ordering.
- Recurrent/cyclic group edges can be auto-detected or explicitly marked, and recurrent inputs are initially primed with an all-zero state.
- Programmable spike-only activation v-ALU with default integrate-and-fire behavior.
- Uniform thresholds use immediate operands by default; heterogeneous thresholds are stored in unified SRAM only when requested.
- Output spike payloads are emitted as grouped bitmaps and multicast to successor destinations.

### Router and mesh model

- Standalone 2D mesh router model.
- Coordinate-addressed router messages for spike tiles and ACK messages.
- Configurable XY or YX dimension-order routing.
- Multicast fanout splitting by next-hop port.
- Router-facing network tiles can be decoded into destination-local input tiles.
- Router-facing ACK messages can be decoded into destination-local ACK messages.
- The demo includes a small multi-core mesh scenario that exercises routed spike traffic and ACK return.

## Repository layout

```text
.
├── Makefile
├── README.md
├── docs/
│   ├── hardware-architecture.md
│   ├── programmable-activation-alu.md
│   └── simulator-api-and-details.md
├── include/
│   └── nmc/
│       ├── core.h
│       ├── router.h
│       └── internal/
│           └── core.h
└── src/
    ├── core.c
    ├── main.c
    ├── router.c
    └── core/
        ├── ack.c
        ├── activation.c
        ├── compute.c
        ├── config.c
        ├── encoder.c
        ├── input.c
        ├── mapping.c
        ├── output.c
        └── utils.c
```

## Build and run

Requirements:

- C11 compiler such as GCC or Clang.
- `make`.

Build:

```sh
make
```

Run the self-checking simulator demo:

```sh
make run
```

Clean generated files:

```sh
make clean
```

The default build uses strict warnings:

```text
-std=c11 -Wall -Wextra -Wpedantic -Werror -O2
```

The executable is a self-checking test bench. It prints a trace of modeled behavior and exits with a nonzero status if an expected tile, ACK, counter, memory value, or route result is wrong.

## Core model

Hardware architecture figures and block diagrams are available in [docs/hardware-architecture.md](docs/hardware-architecture.md).

An `NmcCore` models one accelerator core with fixed-capacity SRAM-like arrays and queues. It contains:

- Input group table: local input group metadata and first-stage ranges into the input/output pair LUT.
- Input/output pair LUT: second-stage entries that connect an input group tile to one or more output groups and weight offsets.
- ACK group table: local ACK group metadata and first-stage ranges into the ACK/output pair LUT.
- ACK/output pair LUT: second-stage entries that increment successor ACK counters on output groups.
- Output group table: bitmap width, activation program binding, activation immediates/ranges, readiness counters, synchronization counters, and route metadata.
- Output route LUT: successor destinations for emitted spike tiles followed by predecessor destinations for ACK return.
- Accumulator LUT: per-output-group base addresses into unified memory.
- Unified memory: signed 16-bit lanes shared by banked weights, packed accumulators, and optional activation state/parameters.
- Output and ACK queues: router-facing messages awaiting fabric delivery.

The core uses start-plus-terminal LUT layouts throughout. A group range is derived from entry `i` and entry `i + 1`, mirroring simple hardware address generation and avoiding per-entry dynamic allocation.

## Dataflow

### 1. Mapping

The graph-level mapper accepts logical group descriptions, external input edges, group-to-group connections, weight matrices, recurrent flags, and core placement. It lowers this network into concrete per-core tables.

This mapping approach is compact because memory is allocated per actual group edge rather than per possible source/destination crossbar slot. Different output groups can have different fan-in, fan-out, widths, and weight-to-neuron ratios. When desired, multiple connections can point to the same weight offset to model weight sharing.

### 2. Input tile consumption

An incoming `NmcInputTile` contains a local input group index, bitmap width, and bit-packed payload.

When a tile arrives:

1. The input group selects its input/output pair LUT range.
2. Each pair entry selects a destination output group and a weight matrix base offset.
3. The bitmap payload is encoded into sparse event streams.
4. Sparse event indices select input-bank rows in unified memory.
5. Output-wide weights are routed to the reduction path.
6. Reduced values are accumulated into the destination output group's packed accumulator slice.
7. The destination output group's input readiness counter is advanced.

### 3. Sparse compute schedule

For an input width `N` and output width `M`, the weight slice is stored as `N` logical input banks, each containing `M` output weights. A sparse input event selects one bank row. The row is consumed across output neurons, reduced through the modeled adder tree, and accumulated into packed accumulator values in unified memory.

This models the intended hardware schedule: grouped bitmap communication first, sparse event extraction at the destination, then high-reuse output-wide accumulation.

### 4. Activation

When an output group has received all required input tiles and successor credits are available, the activation v-ALU executes the program bound to that output group.

The built-in default is integrate-and-fire:

1. Read membrane potential from the accumulator slice.
2. Compare against an immediate or per-neuron threshold.
3. Pack the spike predicate into an output bitmap.
4. Reset or update accumulator state according to the activation program.
5. Emit a multicast `NmcNetworkTile`.

The same instruction model can express variants such as leaky integrate-and-fire, refractory IF, adaptive thresholds, or other spike-only state updates by adding state ranges and immediates rather than changing the packet format or hardwiring a new neuron block.

### 5. Routing and ACK flow

Output spike tiles carry one bitmap payload and a destination list. The router partitions multicast messages by next-hop port and forwards them through the mesh.

ACKs are also routed messages. They return synchronization credit to predecessor output groups through an ACK LUT, allowing one ACK input group to update one or more waiting output groups. This creates a one-in-flight natural-step protocol without global timestep tags.

## Public API

The canonical public headers are:

- `include/nmc/core.h`: core types, mapping specs, configuration functions, input/ACK processing, output queue access, and debug helpers.
- `include/nmc/router.h`: router types, message routing, port queues, and local message decode helpers.

Implementation-only declarations live in `include/nmc/internal/core.h`. Application and test code should include only the public headers unless deliberately testing internals.

For the longer simulator description, module-by-module behavior, and API details, see [docs/simulator-api-and-details.md](docs/simulator-api-and-details.md).

Important public operations include:

- `nmc_core_init()`
- `nmc_core_configure_mapping()`
- `nmc_generate_core_mappings()`
- `nmc_core_process_input_tile()`
- `nmc_core_process_ack()`
- `nmc_core_flush_ready_outputs()`
- `nmc_core_pop_output_tile()`
- `nmc_core_pop_ack()`
- `nmc_router_route_message()`
- `nmc_router_pop_port_message()`

## Demo and regression coverage

The self-checking executable covers:

- Single-core natural-step ordering.
- Multiple input groups and output groups.
- Shared and output-specific successor ACK paths.
- ACK-blocked outputs that flush after credit returns.
- Sparse bitmap encoder scheduling.
- Unified memory layout and output-major to input-bank-major weight transposition.
- Packed accumulator lane access through the accumulator MUX.
- Programmable activation v-ALU default IF behavior.
- Recurrent/cyclic connections with initial zero-state priming.
- Router multicast splitting.
- XY/YX routing behavior.
- A small multi-core mesh scenario with spike forwarding and ACK return.

## Important limits and knobs

Hardware-style limits are defined in the public headers. The most important knobs are:

- `NMC_MAX_GROUP_NEURONS`
- `NMC_MAX_INPUT_GROUPS`
- `NMC_MAX_ACK_GROUPS`
- `NMC_MAX_OUTPUT_GROUPS`
- `NMC_MAX_TILE_DESTINATIONS`
- `NMC_UNIFIED_MEMORY_SIZE`
- `NMC_ACCUMULATOR_LANES`
- `NMC_INPUT_PARALLELISM`
- `NMC_EVENT_ENCODER_WINDOW`
- `NMC_MAX_ACTIVATION_PROGRAMS`
- `NMC_MAX_ACTIVATION_INSTRUCTIONS`
- `NMC_ROUTER_MAX_PORT_QUEUE`

Tile widths must be positive, byte-aligned, and no larger than `NMC_MAX_GROUP_NEURONS`.

## Development notes

- Keep the simulator dependency-free unless a dependency is clearly justified.
- Preserve fixed-size arrays and explicit capacity checks; they represent hardware resource limits.
- Keep public API declarations in `include/nmc/core.h` and `include/nmc/router.h`.
- Keep internal helpers in `include/nmc/internal/core.h`.
- Keep core modules focused:
  - `src/core.c`: top-level lifecycle.
  - `src/core/config.c`: low-level configuration.
  - `src/core/mapping.c`: graph and core mapping generation.
  - `src/core/input.c`: input tile handling.
  - `src/core/ack.c`: ACK handling.
  - `src/core/encoder.c`: bitmap-to-event encoding.
  - `src/core/compute.c`: sparse event accumulation.
  - `src/core/activation.c`: programmable v-ALU execution.
  - `src/core/output.c`: output readiness, activation integration, queues, and tile decode.
  - `src/core/utils.c`: validation and payload helpers.
  - `src/router.c`: standalone router model.
- Configure LUT terminal entries carefully. Many ranges depend on entry `i + 1`.
- Treat core IDs and mesh coordinates as separate namespaces.

## Next steps

### Possible architecture features

- Explore support for non-stateful neurons such as binary ReLU without wasting accumulator memory.
- One candidate is time-multiplexing accumulator space across different output lines.
- This is intentionally delayed because it complicates scheduling, mapping, ACK timing, and accumulator ownership.

### Simulator next steps

- Add probes and traces to measure how effective each architectural feature is.
- Track SRAM reads and writes by table, weight memory, accumulator lanes, and activation state.
- Track cycle estimates for encoding, memory access, adder-tree reduction, activation, routing, and queue stalls.
- Track communication overhead versus AER-style event traffic.
- Track communication delay through the mesh.
- Track accumulator reuse and inactive/dead memory regions.
- Track fan-in/fan-out pressure, multicast split counts, and ACK blocking time.

### Hardware next steps

- Incrementally translate the C simulator mechanisms into synthesizable RTL.
- Start with the stable datapath pieces: bitmap input, sparse encoder, banked weight read, adder-tree reduction, accumulator lane MUX, and grouped output bitmap packing.
- Add the mapping tables and ACK protocol as explicit SRAM/register-file structures.
- Bring the programmable activation v-ALU over after the IF baseline is verified.
- Use simulator probes as the reference for RTL counters, performance expectations, and regression checks.
