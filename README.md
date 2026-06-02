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
- Graph-level mapping generation from logical groups, external inputs, group-to-group connections, and core placement.
- Recurrent/cyclic group connections, including initial zero-state priming and cycle-safe feedback credits.
- Natural step ordering through ACK counters rather than explicit timestep tags.
- Standalone XY/YX 2D mesh routing with multicast fanout splitting.
- A small multi-core mesh scenario wired together in the test bench.

Still intentionally simplified:

- The router and cores are connected by the test bench rather than by a reusable mesh-fabric module.
- Queue back-pressure is represented by bounded arrays, but full retry/NACK behavior is not modeled yet.
- Compute-cycle counters approximate the high-level SIMD/encoder schedule; memory-bank conflicts and lower-level pipeline timing are not yet modeled.
- Full neural-network import/parsing is not modeled yet; callers provide a compact in-memory graph mapping spec.

## Repository layout

```text
.
├── Makefile
├── README.md
├── include/
│   ├── nmc/
│   │   ├── core.h
│   │   ├── internal/
│   │   │   └── core.h
│   │   └── router.h
└── src/
    ├── core.c
    ├── core/
    │   ├── ack.c
    │   ├── compute.c
    │   ├── config.c
    │   ├── encoder.c
    │   ├── input.c
    │   ├── mapping.c
    │   ├── output.c
    │   └── utils.c
    ├── main.c
    └── router.c
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

Activation thresholds the output accumulators into a spike bitmap, resets the accumulator slice, and emits an `NmcNetworkTile` with one payload and a multicast destination list. Feed-forward predecessor ACKs are emitted with activation; recurrent predecessor ACKs are emitted earlier when the destination input window completes.

ACK messages are consumed through the ACK LUT rather than by directly naming an output group. This keeps ACK handling index-based in the same style as input-tile consumption:

1. The ACK group index selects a range in the ACK/output pair LUT.
2. Each second-stage entry increments one output group's successor-ACK counter.
3. Any output groups that were input-complete but ACK-blocked are flushed when their ACK counters reach the successor count.

## Natural step ordering and flow control

The model avoids explicit timestep tags. Instead, it uses a one-in-flight style protocol over mapped edges:

- Input readiness is tracked by `input_count` versus `input_requirement` on each output group.
- Successor synchronization is tracked by `ack_count` versus the number of successor route entries.
- Output emission is allowed only when required inputs have arrived, successor ACKs for the previous emission have returned, and the output queue can accept the tile.
- A successful emission returns predecessor ACKs, allowing predecessors to send the next natural-step tile on feed-forward edges.
- Recurrent predecessor ACKs are returned as soon as the destination group has completed its input window. This breaks ACK cycles without a global timestep barrier: every feedback edge is still one-tile ordered, but its credit is no longer blocked behind the destination's own successor credits.
- Recurrent input connections are primed with an implicit all-zero initial state during configuration. The first natural step can therefore run from external/feed-forward inputs, and later steps require real recurrent tiles.
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

## Module APIs and behavior

The public API is intentionally small and lives in [include/nmc/core.h](include/nmc/core.h) and [include/nmc/router.h](include/nmc/router.h).  The split files under [src](src) are implementation modules.  They share private declarations through [include/nmc/internal/core.h](include/nmc/internal/core.h), but callers outside the core should continue to include only the public headers.

Unless otherwise noted, functions returning `bool` return `true` when the requested operation was accepted and completed, and `false` when validation fails, a fixed-capacity array is full, a requested queue is empty, or the current protocol state does not allow the operation.

### Public core API: [include/nmc/core.h](include/nmc/core.h)

The core API exposes the complete state type `NmcCore`, message types, LUT entry types, static hardware limits, and the functions used by tests or future fabric code.

#### Configuration functions

| Function | Behavior |
| --- | --- |
| `nmc_core_init()` | Clears one `NmcCore` and assigns its `core_id`.  This must be called before any configuration or runtime operation. |
| `nmc_core_add_input_group()` | Appends one input group table entry.  The terminal LUT start is configured separately with `nmc_core_set_input_lut_start()`. |
| `nmc_core_add_ack_group()` | Appends one ACK group table entry.  ACK group indices are independent from input tile group indices. |
| `nmc_core_add_output_group()` | Appends one output group, validates its bitmap width, and initializes per-neuron thresholds.  If `thresholds` is `NULL`, thresholds default to `1`. |
| `nmc_core_set_output_accumulator_lut_start()` | Maps an output group to its accumulator SRAM base address and validates that the output slice fits. |
| `nmc_core_add_output_successor_lut_entry()` | Appends a route LUT entry used as a successor destination for output spike tiles. |
| `nmc_core_add_output_predecessor_lut_entry()` | Appends a route LUT entry used as a predecessor destination for generated ACK messages. |
| `nmc_core_add_recurrent_output_predecessor_lut_entry()` | Appends a predecessor ACK destination whose credit is returned at input-window completion, for cycle-safe recurrent feedback. |
| `nmc_core_set_output_lut_starts()` | Configures an output group's successor and predecessor route ranges.  Passing `output_index == output_group_count` configures the terminal sentinel. |
| `nmc_core_add_input_output_pair_lut_entry()` | Appends one stage-2 input pair entry from an input group range to an output group and weight offset.  It also increments that output group's `input_requirement`. |
| `nmc_core_add_recurrent_input_output_pair_lut_entry()` | Appends a recurrent stage-2 input pair and primes that input with an implicit zero state for the first natural step. |
| `nmc_core_set_input_lut_start()` | Configures the stage-1 input LUT start for an input group or the terminal input sentinel. |
| `nmc_core_add_ack_output_pair_lut_entry()` | Appends one stage-2 ACK pair entry.  One ACK index may therefore increment one or more output groups. |
| `nmc_core_set_ack_lut_start()` | Configures the stage-1 ACK LUT start for an ACK group or the terminal ACK sentinel. |
| `nmc_core_configure_mapping()` | Clears and configures a core from one high-level `NmcCoreMappingSpec`, automatically creating groups, route LUTs, ACK LUTs, input pair LUTs, terminal sentinels, accumulator starts, and input requirements. |

Configuration order matters because several start entries are validated against the current second-stage LUT counts.  The test bench generally appends second-stage entries first, then records the start-plus-terminal ranges.

For new code, prefer `nmc_core_configure_mapping()` when the mapping is known up front.  The lower-level configuration functions remain useful for tests that need to exercise invalid intermediate states or build tables step by step.

#### Runtime functions

| Function | Behavior |
| --- | --- |
| `nmc_core_process_input_tile()` | Consumes one destination-local spike tile.  It validates the input group, encodes the bitmap into sparse events, walks the input/output pair range, accumulates into selected output groups, increments input counters, and attempts output activation when a group becomes complete. |
| `nmc_core_process_ack()` | Consumes one destination-local ACK message.  The ACK destination `group_index` is interpreted as an ACK LUT index.  Matching stage-2 entries increment successor-ACK counters and may flush outputs that were already input-complete. |
| `nmc_core_flush_ready_outputs()` | Scans output groups and attempts to activate any group whose input counter has reached its configured requirement.  This is mainly used after ACK counters change. |
| `nmc_core_pop_output_tile()` | Pops the oldest router-facing output spike tile from the core output queue. |
| `nmc_core_pop_ack()` | Pops the oldest router-facing multicast predecessor ACK from the core ACK queue. |
| `nmc_network_tile_get_input_tile()` | Decodes one destination of a router-facing `NmcNetworkTile` into a destination-core-local `NmcInputTile`. |
| `nmc_print_payload()` | Prints a bit-packed payload as a binary string, most significant bit first. |

### Configuration module: [src/core/config.c](src/core/config.c)

This module implements the public configuration half of the core API.  It owns group creation, LUT population, and static address validation.  Core-level initialization lives in [src/core.c](src/core.c).  It does not process runtime input tiles or ACKs.

Key behavior:

- `nmc_core_init()` zeroes the complete `NmcCore`, including queues, counters, LUTs, weights, and accumulators.
- Group append functions enforce the fixed maxima from [include/nmc/core.h](include/nmc/core.h).
- Output widths are validated through the shared width helper: widths must be positive, byte-aligned, and no larger than `NMC_MAX_GROUP_NEURONS`.
- `nmc_core_set_output_lut_starts()` also initializes `ack_count` to the output group's successor count, modeling the initial state where no previous output tile is in flight.
- `nmc_core_add_input_output_pair_lut_entry()` is the point where mapping metadata turns into an output group's `input_requirement`.

### High-level mapping module: [src/core/mapping.c](src/core/mapping.c)

This module implements `nmc_core_configure_mapping()`, a convenience API that converts compact mapping specifications into all of the core's low-level LUTs.

Specification types:

| Type | Meaning |
| --- | --- |
| `NmcInputGroupMappingSpec` | Describes one local input group width.  The array index is the local input `group_index`; use `NMC_INPUT_GROUP(width)` for concise declarations. |
| `NmcInputConnectionMappingSpec` | Describes one local input group feeding an output group.  Use `NMC_INPUT_CONNECTION(input_group, weights)` and `NMC_AUTO_WEIGHT_OFFSET` to let the mapper place that input/output weight matrix.  If `weights` is non-`NULL`, the mapper copies that kernel into the assigned weight SRAM slice. |
| `NmcOutputSuccessorMappingSpec` | Describes one successor edge driven by an output group as destination core plus destination-local input group.  The mapper automatically creates one local ACK group for each unique `(core_id, input_group)` successor endpoint. |
| `NmcPredecessorMappingSpec` | Describes one predecessor ACK destination as predecessor core plus predecessor-local ACK group.  Use `nmc_core_mapping_ack_group_for_successor()` on the predecessor mapping to discover the ACK group generated for a successor endpoint. |
| `NmcOutputGroupMappingSpec` | Describes one output group: bitmap width, optional thresholds, accumulator SRAM start, input edges, successor edges, and predecessor ACK destinations.  Use `NMC_AUTO_ACCUMULATOR_OFFSET` to let the mapper pack accumulator SRAM. |
| `NmcCoreMappingSpec` | Describes the full static mapping for one core as local input groups plus output groups.  Input LUTs, ACK indices, weight offsets, route LUTs, input/output pair LUT entries, and ACK/output pair LUT entries are generated from those direct group-index connections. |
| `NmcNetworkMappingSpec` | Describes a whole logical network using placed groups, optional external input edges, and group-to-group connections.  `nmc_generate_core_mappings()` lowers it into one `NmcCoreMappingSpec` per used core, automatically assigning local input group indices, ACK group indices, weight starts, accumulator starts, successor routes, and predecessor ACK routes.  Graph-level callers do not pass auto-generated offset sentinels. |

`nmc_core_configure_mapping()` behavior:

1. Validate the top-level spec pointer and clear the target core with `nmc_core_init()`.
2. Create local input groups exactly in the supplied input-group array order.
3. Order output groups by their array order, add them to the core, and configure accumulator SRAM.  Outputs using `NMC_AUTO_ACCUMULATOR_OFFSET` are packed contiguously.
4. Build the input/output pair LUT by inverting output input edges.  Connections using `NMC_AUTO_WEIGHT_OFFSET` are packed contiguously in weight memory, and non-`NULL` kernels are copied into the assigned slices.
5. Assign local ACK group indices to each unique successor endpoint in first-seen order.  Reusing the same successor `(core_id, input_group)` from multiple outputs intentionally makes one incoming ACK fan out through the ACK/output pair LUT to all matching outputs.
6. Build output router LUTs from successor input-group indices and predecessor ACK destinations.
7. Build the ACK/output pair LUT by inverting output successor edges: every generated ACK group maps back to each output group that waits for that successor endpoint.

The mapping API is still deterministic and hardware-like: it does not allocate memory and it uses the same fixed-capacity arrays as the lower-level configuration API.  If any group, route, or pair count exceeds a fixed limit, configuration returns `false`.  On failure, the core may be partially configured; callers should treat failure as fatal for that core instance and call `nmc_core_configure_mapping()` or `nmc_core_init()` again before reuse.

For whole-network generation, callers only need to provide:

- Logical groups with `group_id`, placed `core_id`, width, and thresholds.
- Optional external input edges into source groups, including external `input_id`, input width, and weights.  Reusing the same `input_id` maps one logical external input stream to multiple destination groups; destinations on the same core share one generated local input group.
- Logical group-to-group connections with weight matrices.  Reusing the same `source_group` for distinct destination groups models source fanout; the mapper emits one successor route per destination edge so ACK synchronization remains one ACK per successor route.

The generated storage is owned by `NmcGeneratedMappings`.  Use `nmc_generated_mappings_find_core()` to retrieve a generated core mapping, then pass that mapping to `nmc_core_configure_mapping()`.

Example shape of a one-input, one-output mapping:

```c
enum { INPUT_A = 0, DOWNSTREAM_INPUT = 0 };
int16_t kernel_8x8[64];

const NmcInputGroupMappingSpec input_groups[] = {
    [INPUT_A] = NMC_INPUT_GROUP(8),
};
const NmcOutputSuccessorMappingSpec successors[] = {
    NMC_SUCCESSOR(1, DOWNSTREAM_INPUT),
};
const NmcInputConnectionMappingSpec output_inputs[] = {
    NMC_INPUT_CONNECTION(INPUT_A, kernel_8x8),
};
const NmcOutputGroupMappingSpec outputs[] = {
    {
        .width = 8,
        .thresholds = thresholds,
        .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
        .inputs = output_inputs,
        .input_count = ARRAY_COUNT(output_inputs),
        .successors = successors,
        .successor_count = ARRAY_COUNT(successors),
    },
};
const NmcCoreMappingSpec mapping = {
    .input_groups = input_groups,
    .input_group_count = ARRAY_COUNT(input_groups),
    .output_groups = outputs,
    .output_group_count = ARRAY_COUNT(outputs),
};

nmc_ack_index_t ack_from_core_1;
nmc_core_mapping_ack_group_for_successor(&mapping, 1, DOWNSTREAM_INPUT, &ack_from_core_1);

NmcCore core;
nmc_core_configure_mapping(&core, 0, &mapping);
```

### Input interface module: [src/core/input.c](src/core/input.c)

This module implements `nmc_core_process_input_tile()`.

Behavior:

1. Validate `tile->group_index` and `tile->width`.
2. Reset the per-input-tile schedule counters on `NmcCore`.
3. Call `nmc_core_encode_input_events()` to convert the bitmap payload into per-lane sparse event streams.
4. Read the current and next input LUT entries to obtain the pair range `[start(input), start(input + 1))`.
5. For each pair entry, validate the target output group and weight slice.
6. Call `nmc_core_accumulate_pair()` to update the selected output group's accumulator SRAM slice.
7. Increment that output group's `input_count` once for this tile arrival.
8. If the output group is input-complete, call `nmc_core_activate_output_group()`.  Activation may still fail harmlessly if successor ACK synchronization or queue space is not ready yet.

The function returns `false` if the tile does not match any configured pair range, if any table range is invalid, or if accepting the tile would over-count an output group that is already input-complete.

### ACK interface module: [src/core/ack.c](src/core/ack.c)

This module implements `nmc_core_process_ack()`.

Behavior:

1. Accept only local ACK messages with `destination_count == 1`.
2. Interpret `ack->destinations[0].group_index` as the ACK LUT index on this core.
3. Read the ACK LUT range `[start(ack), start(ack + 1))`.
4. For each stage-2 ACK entry, increment the selected output group's `ack_count`.
5. Reject ACKs that would overflow a counter or exceed the configured successor count.
6. Call `nmc_core_flush_ready_outputs()` so ACK-unblocked groups can emit immediately.

ACK indices are deliberately indirect: a single incoming ACK group can update multiple output groups when one successor acknowledges a shared downstream route.

### Output interface module: [src/core/output.c](src/core/output.c)

This module owns output readiness, activation, queueing, and router-facing decode helpers.

Private helpers in this module derive route ranges from the output route LUT:

- Successors are in `[successor_start, predecessor_start)`.
- Predecessor ACK destinations are in `[predecessor_start, successor_start(next_output))`.
- The terminal output LUT entry supplies the final predecessor exclusive end.

Implemented public or internal functions:

| Function | Behavior |
| --- | --- |
| `nmc_core_output_group_ready()` | Returns whether an output group has a nonzero input requirement and has received all required input tiles for the current natural step. |
| `nmc_core_output_group_successor_count()` | Returns the number of successor destinations in the output route range. |
| `nmc_core_activate_output_group()` | Validates successor synchronization and queue space, thresholds accumulators into an output bitmap, enqueues the output network tile, enqueues predecessor ACKs, clears accumulators, and resets the output group's `input_count` and `ack_count`. |
| `nmc_core_flush_ready_outputs()` | Scans all output groups and activates any group that is input-ready and output-ready. |
| `nmc_core_pop_output_tile()` | FIFO pop from the output tile queue. |
| `nmc_core_pop_ack()` | FIFO pop from the predecessor ACK queue. |
| `nmc_network_tile_get_input_tile()` | Converts one destination entry of a multicast network tile into a local input tile for the destination core. |

Output activation is intentionally conservative: it checks successor route validity, predecessor route validity, destination count limits, output queue capacity, ACK queue capacity, and accumulator SRAM fit before mutating the output group synchronization window.

### Encoder module: [src/core/encoder.c](src/core/encoder.c)

This module implements the sparse bitmap-to-event model used by input processing.

Internal API:

| Function | Behavior |
| --- | --- |
| `nmc_core_encode_input_events()` | Splits an input bitmap across `NMC_INPUT_PARALLELISM` lanes, encodes each lane into ascending event indices, reports total emitted events, and reports the maximum lane encoder-cycle count. |

Each lane scans `NMC_EVENT_ENCODER_WINDOW` bits at a time.  It emits at most one event per encoder cycle.  If a search window is empty, it can replace that window with the next window in the same modeled cycle, preserving sparse skip behavior while keeping the modeled priority search window narrow.

### Compute module: [src/core/compute.c](src/core/compute.c)

This module owns the event-driven accumulation schedule.

Internal API:

| Function | Behavior |
| --- | --- |
| `nmc_core_pair_weights_fit()` | Validates that an input/output pair's contiguous row-major weight matrix fits inside `NMC_WEIGHT_MEMORY_SIZE`. |
| `nmc_core_accumulate_pair()` | Applies one encoded input tile to one input/output pair.  It walks output-neuron blocks of `NMC_OUTPUT_PARALLELISM`, walks sparse event steps across input lanes, sums active lane weights, updates accumulator SRAM, and advances compute-cycle counters. |

The compute module assumes the encoder has already produced per-lane event indices.  It does not inspect bitmap payloads directly.

### Shared core utilities: [src/core/utils.c](src/core/utils.c)

This module contains validation and bit-payload helpers shared by the other core modules.

Internal API:

| Function | Behavior |
| --- | --- |
| `nmc_core_valid_input_index()` | Checks an input group index against `input_group_count`. |
| `nmc_core_valid_ack_index()` | Checks an ACK group index against `ack_group_count`. |
| `nmc_core_valid_output_index()` | Checks an output group index against `output_group_count`. |
| `nmc_core_valid_width()` | Checks that a tile width is positive, byte-aligned, and within `NMC_MAX_GROUP_NEURONS`. |
| `nmc_core_payload_bytes()` | Converts a bit width to the number of payload bytes. |
| `nmc_core_payload_bit_is_set()` | Reads one little-endian bit from a payload byte array. |
| `nmc_core_payload_set_bit()` | Sets one little-endian bit in a payload byte array. |
| `nmc_core_output_accumulators_fit()` | Validates that an output group's accumulator LUT entry is valid and that the output slice fits in accumulator SRAM. |
| `nmc_print_payload()` | Public debug helper for printing payload bitmaps. |

Payload bits are little-endian within each byte for storage and computation.  `nmc_print_payload()` reverses the display order so traces read like conventional binary literals.

### Private core interface: [include/nmc/internal/core.h](include/nmc/internal/core.h)

This header is the private contract between core implementation modules.  It defines `NmcInputLaneEvents` and declares shared helpers that are not part of the public API.  New application or test code should not include it unless it is deliberately testing internals.

### Router API: [include/nmc/router.h](include/nmc/router.h) and [src/router.c](src/router.c)

The router API is public because future fabric or mesh tests need to instantiate routers directly.

| Function | Behavior |
| --- | --- |
| `nmc_router_init()` | Clears one router and assigns its attached `core_id`, mesh coordinate, and routing order. |
| `nmc_router_port_name()` | Returns a static human-readable name for a router output port. |
| `nmc_router_route_message()` | Validates a `NmcRouterMessage`, chooses one next-hop port per destination, splits multicast destinations by port, and enqueues one split message per non-empty output port if all required queues have capacity. |
| `nmc_router_pop_port_message()` | FIFO pop from one router output port queue. |
| `nmc_router_message_get_input_tile()` | Decodes one destination of a local router message with `width > 0` into a core-facing `NmcInputTile`. |
| `nmc_router_message_get_ack()` | Decodes one destination of a local router message with `width == 0` into a core-facing `NmcAckMessage`, but only if the destination coordinate equals the current router coordinate. |

Routing uses `NMC_ROUTER_XY` or `NMC_ROUTER_YX` dimension order.  A routed message with several destinations may produce several output messages, but each output message keeps only the destinations that share that next-hop port.

## Demo and regression coverage

The executable in `src/main.c` covers three scenarios.

All core setups in the executable use the high-level `nmc_core_configure_mapping()` API, and one scenario uses `nmc_generate_core_mappings()` to create those per-core mappings from logical graph connectivity.  The demo exercises automatic input LUT, ACK LUT, output route LUT, terminal sentinel, accumulator LUT, input requirement generation, and graph-level group-index assignment.

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

The main hardware-style limits are defined in `include/nmc/core.h` and `include/nmc/router.h`.

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
- Keep [include/nmc/core.h](include/nmc/core.h) and [include/nmc/router.h](include/nmc/router.h) as the canonical public API headers.  Shared implementation-only helpers belong in [include/nmc/internal/core.h](include/nmc/internal/core.h); it is private despite living under the include tree.
- Keep the core submodules narrow: top-level lifecycle in [src/core.c](src/core.c), configuration in [src/core/config.c](src/core/config.c), high-level mapping in [src/core/mapping.c](src/core/mapping.c), input handling in [src/core/input.c](src/core/input.c), ACK handling in [src/core/ack.c](src/core/ack.c), output activation/queues in [src/core/output.c](src/core/output.c), sparse encoding in [src/core/encoder.c](src/core/encoder.c), compute scheduling in [src/core/compute.c](src/core/compute.c), and common helpers in [src/core/utils.c](src/core/utils.c).
- Configure LUT terminal entries carefully. Many ranges are derived from entry `i` and entry `i + 1`.
- Treat core IDs and mesh coordinates as separate namespaces. The test bench currently maps between them before injecting core-generated messages into routers.
- If changing function signatures or table semantics, update both the core tests and router/mesh tests in `src/main.c`.

## Suggested next iterations

1. Extract the ad hoc mesh wiring in `src/main.c` into a reusable mesh-fabric module.
2. Add explicit NACK/BUSY retry behavior for full queue back-pressure modeling.
3. Model accumulator-bank allocation and memory-bank conflicts for the parallel event schedule.
4. Add tests for overlapping input windows and convolution-like mappings.
5. Add importers that produce `NmcNetworkMappingSpec` from common model or graph formats.
6. Add deeper traces for deadlock-prone recurrent or cyclic core graphs.
