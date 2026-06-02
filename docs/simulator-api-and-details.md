# Simulator API and Detailed Architecture Notes

This document preserves the detailed simulator description and API/module notes that were previously in the project README. The README is now a shorter project landing page; this file is the longer technical reference for the C simulator.

## Project scope

The project is an architecture-stage simulator, not a production neural-network runtime. It is intentionally explicit about tables, counters, SRAM-sized arrays, queues, and routing metadata so the C model stays close to the hardware mechanisms being explored.

The simulator currently models:

- A fixed-size, hardware-style neuromorphic core that consumes spike bitmap tiles, applies sparse event-driven weighted accumulation, runs programmable spike-only activation, and emits multicast spike tiles.
- A standalone 2D mesh router that routes spike tiles and ACK messages with deterministic dimension-order routing and multicast splitting.

The executable in `src/main.c` is a self-checking test bench. It prints a trace of modeled behavior and exits with a nonzero status if an expected tile, ACK, counter, memory value, activation result, or route result is wrong.

## Repository layout

```text
.
├── Makefile
├── README.md
├── docs/
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

- Input group table: configured input width plus first-stage lookup from a local input group index to a range in the input/output pair LUT.
- Input/output pair LUT: second-stage lookup entries that identify which output group receives the input and where that pair's contiguous weight matrix starts.
- ACK group table: first-stage lookup from a local ACK group index to a range in the ACK/output pair LUT.
- ACK/output pair LUT: second-stage lookup entries that increment successor-ACK counters on output groups.
- Output group table: bitmap width, activation program binding, activation immediates, optional activation SRAM ranges, input readiness counters, successor ACK counters, and output route starts.
- Output route LUT: successor destinations for output spike tiles followed by predecessor destinations for multicast ACKs.
- Accumulator LUT: per-output starting lane addresses into unified memory.
- Unified memory: signed 16-bit lanes shared by input-bank-major weights, packed accumulator values, and optional activation state or parameters.
- Accumulator values: signed 32-bit membrane potentials packed across `NMC_ACCUMULATOR_LANES` neighboring weight lanes and selected through an accumulator MUX.
- Activation instruction memory and program descriptors for the spike-only v-ALU.
- Output and ACK queues used by the surrounding router/fabric model.

The core uses start-plus-terminal LUTs throughout. For example, input group `g` reads `start(g)` and `start(g + 1)` to find its exclusive second-stage LUT range. Output routes use the same idea, but each output has two starts: one for successor tile routes and one for predecessor ACK routes.

## Input and output flow

The core-facing input tile format is `NmcInputTile`:

- `width`: number of neurons/spikes represented by the payload.
- `group_index`: local input group index.
- `payload`: bit-packed spike bitmap.

When a tile arrives:

1. The input group index selects a range in the input/output pair LUT.
2. Each pair entry selects an output group and a weight-bank base offset.
3. The bitmap is converted into sparse input events.
4. Each sparse input event selects one output-wide weight bank row and accumulates it into that output group's accumulator row.
5. The output group's `input_count` is incremented.
6. If all required inputs have arrived and successor synchronization is complete, the output group activates.

Activation runs the output group's bound spike-only v-ALU program over the accumulator slice. The default integrate-and-fire program treats each accumulator as membrane potential, compares it with a threshold immediate, emits a spike bitmap, resets the accumulator slice, and emits an `NmcNetworkTile` with one payload and a multicast destination list. Heterogeneous thresholds or other per-neuron state can be stored in unified SRAM and loaded by activation instructions. Feed-forward predecessor ACKs are emitted with activation; recurrent predecessor ACKs are emitted earlier when the destination input window completes.

ACK messages are consumed through the ACK LUT rather than by directly naming an output group. This keeps ACK handling index-based in the same style as input-tile consumption:

1. The ACK group index selects a range in the ACK/output pair LUT.
2. Each second-stage entry increments one output group's successor-ACK counter.
3. Any output groups that were input-complete but ACK-blocked are flushed when their ACK counters reach the successor count.

## Natural step ordering and flow control

The model avoids explicit timestep tags. Instead, it uses a one-in-flight style protocol over mapped edges:

- Input readiness is tracked by `input_count` versus `input_requirement` on each output group.
- Successor synchronization is tracked by `ack_count` versus the number of successor route entries.
- Output emission is allowed only when required inputs have arrived, successor ACKs for the previous emission have returned, and the output queue can accept the tile.
- A successful feed-forward emission returns predecessor ACKs, allowing predecessors to send the next natural-step tile on feed-forward edges.
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

- For a connected input width `N` and output width `M`, the unified memory weight slice has `N` logical input banks/rows.
- Each input bank row is `M`-wide: one sparse event selects all `M` weights for that input channel in one memory row.
- The accumulator slice is `M`-wide logically, but each membrane potential occupies `NMC_ACCUMULATOR_LANES` 16-bit memory lanes. With the default 32-bit accumulator and 16-bit weight lane, one membrane potential uses two lanes.
- Accumulators are packed into rows of `NMC_INPUT_PARALLELISM` lanes. The controller selects the accumulator MUX index for the requested output channel and reads or writes only the lanes needed for that one accumulator, leaving neighboring packed accumulators in the same row untouched.
- During one event step, up to `NMC_INPUT_PARALLELISM` weight-bank rows are routed to the adder tree, reduced per output channel, and added to values read through the accumulator MUX.
- The runtime banked weight address is `weight_offset + event_index * output_width + output_index`.
- Caller-provided kernels in `NmcInputConnectionMappingSpec` remain conventional output-major matrices indexed as `[output][input]`; `nmc_core_configure_mapping()` transposes them into banked unified-memory lanes.

The public counters on `NmcCore` expose the abstract input schedule:

- `last_input_tile_event_count`
- `last_input_tile_encoder_cycles`
- `last_input_tile_compute_cycles`
- `total_encoder_cycles`
- `total_compute_cycles`

Activation adds its own counters:

- `last_activation_instruction_count`
- `last_activation_cycles`
- `last_activation_output_width`
- `total_activation_cycles`
- `activation_fault_count`

## Activation v-ALU model

The activation engine is a bounded spike-only vector ALU. It executes a microprogram bound to each output group and produces exactly one output bitmap payload.

The default built-in program is integrate-and-fire with an immediate threshold:

1. `LD_ACC`: read accumulator/membrane potential.
2. `LD_IMM`: load threshold immediate.
3. `CMP_GE`: compare membrane potential to threshold.
4. `EMIT_PRED`: pack the spike predicate into the output payload.
5. `LD_IMM`: load reset value.
6. `ST_ACC`: reset or update membrane potential.
7. `END`: terminate the program.

A second built-in path supports threshold values stored in unified SRAM for heterogeneous per-neuron thresholds. Program descriptors and SRAM ranges keep activation memory accesses explicit and bounded.

Supported instruction classes include:

- Accumulator loads/stores: `NMC_ACT_OP_LD_ACC`, `NMC_ACT_OP_ST_ACC`.
- Unified-SRAM word loads/stores: `NMC_ACT_OP_LD_WORD`, `NMC_ACT_OP_ST_WORD`.
- Immediate loads: `NMC_ACT_OP_LD_IMM`.
- Integer arithmetic: `NMC_ACT_OP_ADD`, `NMC_ACT_OP_SUB`, `NMC_ACT_OP_MUL`, `NMC_ACT_OP_MAC`.
- Predicate generation: `NMC_ACT_OP_CMP_GE`.
- Payload emission: `NMC_ACT_OP_EMIT_PRED`.
- Termination: `NMC_ACT_OP_END`.

Activation is deliberately not arbitrary host code. It has fixed register limits, fixed instruction memory limits, validated SRAM ranges, deterministic execution budgets, and explicit failure behavior.

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

The public API is intentionally small and lives in `include/nmc/core.h` and `include/nmc/router.h`. The split files under `src/` are implementation modules. They share private declarations through `include/nmc/internal/core.h`, but callers outside the core should continue to include only the public headers.

Unless otherwise noted, functions returning `bool` return `true` when the requested operation was accepted and completed, and `false` when validation fails, a fixed-capacity array is full, a requested queue is empty, or the current protocol state does not allow the operation.

### Public core API: `include/nmc/core.h`

The core API exposes the complete state type `NmcCore`, message types, LUT entry types, static hardware limits, activation program types, mapping specs, and the functions used by tests or future fabric code.

#### Configuration functions

| Function | Behavior |
| --- | --- |
| `nmc_core_init()` | Clears one `NmcCore`, assigns its `core_id`, and installs built-in activation programs. This must be called before any configuration or runtime operation. |
| `nmc_core_add_input_group()` | Appends one input group table entry. The terminal LUT start is configured separately with `nmc_core_set_input_lut_start()`. |
| `nmc_core_add_ack_group()` | Appends one ACK group table entry. ACK group indices are independent from input tile group indices. |
| `nmc_core_add_output_group()` | Appends one output group, validates its bitmap width, and binds the default IF activation program. Uniform thresholds become an activation immediate; if `thresholds` is `NULL`, the threshold defaults to `1`. |
| `nmc_core_add_activation_program()` | Appends a bounded spike-only v-ALU microprogram to the core-local instruction memory. |
| `nmc_core_bind_output_activation_program()` | Binds an output group to an activation program descriptor. |
| `nmc_core_bind_activation_sram_range()` | Binds one validated unified-SRAM range for activation state or per-neuron parameters. |
| `nmc_core_set_activation_immediate()` | Sets one per-output activation immediate such as threshold, reset, leakage, or refractory constants. |
| `nmc_core_set_output_accumulator_lut_start()` | Maps an output group to its accumulator base lane in unified memory and validates that the packed output slice fits. |
| `nmc_core_add_output_successor_lut_entry()` | Appends a route LUT entry used as a successor destination for output spike tiles. |
| `nmc_core_add_output_predecessor_lut_entry()` | Appends a route LUT entry used as a predecessor destination for generated ACK messages. |
| `nmc_core_add_recurrent_output_predecessor_lut_entry()` | Appends a predecessor ACK destination whose credit is returned at input-window completion, for cycle-safe recurrent feedback. |
| `nmc_core_set_output_lut_starts()` | Configures an output group's successor and predecessor route ranges. Passing `output_index == output_group_count` configures the terminal sentinel. |
| `nmc_core_add_input_output_pair_lut_entry()` | Appends one stage-2 input pair entry from an input group range to an output group and weight offset. It also increments that output group's `input_requirement`. |
| `nmc_core_add_recurrent_input_output_pair_lut_entry()` | Appends a recurrent stage-2 input pair and primes that input with an implicit zero state for the first natural step. |
| `nmc_core_set_input_lut_start()` | Configures the stage-1 input LUT start for an input group or the terminal input sentinel. |
| `nmc_core_add_ack_output_pair_lut_entry()` | Appends one stage-2 ACK pair entry. One ACK index may therefore increment one or more output groups. |
| `nmc_core_set_ack_lut_start()` | Configures the stage-1 ACK LUT start for an ACK group or the terminal ACK sentinel. |
| `nmc_core_configure_mapping()` | Clears and configures a core from one high-level `NmcCoreMappingSpec`, automatically creating groups, route LUTs, ACK LUTs, input pair LUTs, terminal sentinels, accumulator starts, activation state, and input requirements. |

Configuration order matters because several start entries are validated against the current second-stage LUT counts. The test bench generally appends second-stage entries first, then records the start-plus-terminal ranges.

For new code, prefer `nmc_core_configure_mapping()` when the mapping is known up front. The lower-level configuration functions remain useful for tests that need to exercise invalid intermediate states or build tables step by step.

#### Runtime functions

| Function | Behavior |
| --- | --- |
| `nmc_core_process_input_tile()` | Consumes one destination-local spike tile. It validates the input group and configured width, encodes the bitmap into sparse events, walks the input/output pair range, accumulates into selected output groups, increments input counters, and attempts output activation when a group becomes complete. |
| `nmc_core_process_ack()` | Consumes one destination-local ACK message. The ACK destination `group_index` is interpreted as an ACK LUT index. Matching stage-2 entries increment successor-ACK counters and may flush outputs that were already input-complete. |
| `nmc_core_flush_ready_outputs()` | Scans output groups and attempts to activate any group whose input counter has reached its configured requirement. This is mainly used after ACK counters change. |
| `nmc_core_pop_output_tile()` | Pops the oldest router-facing output spike tile from the core output queue. |
| `nmc_core_pop_ack()` | Pops the oldest router-facing multicast predecessor ACK from the core ACK queue. |
| `nmc_network_tile_get_input_tile()` | Decodes one destination of a router-facing `NmcNetworkTile` into a destination-core-local `NmcInputTile`. |
| `nmc_print_payload()` | Prints a bit-packed payload as a binary string, most significant bit first. |

### Configuration module: `src/core/config.c`

This module implements the public configuration half of the core API. It owns group creation, activation program installation/binding, LUT population, and static address validation. Core-level initialization lives in `src/core.c`. It does not process runtime input tiles or ACKs.

Key behavior:

- `nmc_core_init()` zeroes the complete `NmcCore`, including queues, counters, LUTs, activation programs, and unified memory.
- Built-in activation programs are installed during initialization.
- Group append functions enforce the fixed maxima from `include/nmc/core.h`.
- Output widths are validated through the shared width helper: widths must be positive, byte-aligned, and no larger than `NMC_MAX_GROUP_NEURONS`.
- Output groups bind to the default IF activation program unless another program is explicitly selected.
- `nmc_core_set_output_lut_starts()` also initializes `ack_count` to the output group's successor count, modeling the initial state where no previous output tile is in flight.
- `nmc_core_configure_mapping()` stores configured input widths on input groups so runtime tiles must match the `N` dimension used to lay out banked weight lanes.
- `nmc_core_add_input_output_pair_lut_entry()` is the point where mapping metadata turns into an output group's `input_requirement`.

### High-level mapping module: `src/core/mapping.c`

This module implements `nmc_core_configure_mapping()` and `nmc_generate_core_mappings()`, convenience APIs that convert compact mapping specifications into all of the core's low-level LUTs.

Specification types:

| Type | Meaning |
| --- | --- |
| `NmcInputGroupMappingSpec` | Describes one local input group width. The array index is the local input `group_index`; use `NMC_INPUT_GROUP(width)` for concise declarations. |
| `NmcInputConnectionMappingSpec` | Describes one local input group feeding an output group. Use `NMC_INPUT_CONNECTION(input_group, weights)` and `NMC_AUTO_WEIGHT_OFFSET` to let the mapper place that input/output weight matrix. If `weights` is non-`NULL`, provide it as an output-major `[output][input]` kernel; the mapper transposes it into input-bank-major, output-wide unified-memory lanes. |
| `NmcOutputSuccessorMappingSpec` | Describes one successor edge driven by an output group as destination core plus destination-local input group. The mapper automatically creates one local ACK group for each unique `(core_id, input_group)` successor endpoint. |
| `NmcPredecessorMappingSpec` | Describes one predecessor ACK destination as predecessor core plus predecessor-local ACK group. Use `nmc_core_mapping_ack_group_for_successor()` on the predecessor mapping to discover the ACK group generated for a successor endpoint. |
| `NmcOutputGroupMappingSpec` | Describes one output group: bitmap width, optional thresholds, accumulator base lane, input edges, successor edges, and predecessor ACK destinations. Use `NMC_AUTO_ACCUMULATOR_OFFSET` to let the mapper pack accumulators in unified memory. |
| `NmcCoreMappingSpec` | Describes the full static mapping for one core as local input groups plus output groups. Input LUTs, ACK indices, weight offsets, route LUTs, input/output pair LUT entries, and ACK/output pair LUT entries are generated from those direct group-index connections. |
| `NmcNetworkMappingSpec` | Describes a whole logical network using placed groups, optional external input edges, and group-to-group connections. `nmc_generate_core_mappings()` lowers it into one `NmcCoreMappingSpec` per used core, automatically assigning local input group indices, ACK group indices, weight starts, accumulator starts, successor routes, and predecessor ACK routes. Graph-level callers do not pass auto-generated offset sentinels. |

`nmc_core_configure_mapping()` behavior:

1. Validate the top-level spec pointer and clear the target core with `nmc_core_init()`.
2. Create local input groups exactly in the supplied input-group array order.
3. Order output groups by their array order, add them to the core, and configure packed accumulator slices in unified memory. Outputs using `NMC_AUTO_ACCUMULATOR_OFFSET` are packed first.
4. Configure output activation. Uniform thresholds become per-group immediates; heterogeneous thresholds are copied into unified SRAM and bound as activation ranges.
5. Build the input/output pair LUT by inverting output input edges. Connections using `NMC_AUTO_WEIGHT_OFFSET` are packed after accumulator and activation slices in the same unified memory, and non-`NULL` kernels are transposed from caller layout into banked weight slices.
6. Assign local ACK group indices to each unique successor endpoint in first-seen order. Reusing the same successor `(core_id, input_group)` from multiple outputs intentionally makes one incoming ACK fan out through the ACK/output pair LUT to all matching outputs.
7. Build output router LUTs from successor input-group indices and predecessor ACK destinations.
8. Build the ACK/output pair LUT by inverting output successor edges: every generated ACK group maps back to each output group that waits for that successor endpoint.

The mapping API is deterministic and hardware-like: it uses fixed-capacity arrays and explicit capacity checks. If any group, route, pair, instruction, or memory range exceeds a fixed limit, configuration returns `false`. On failure, the core may be partially configured; callers should treat failure as fatal for that core instance and call `nmc_core_configure_mapping()` or `nmc_core_init()` again before reuse.

For whole-network generation, callers provide:

- Logical groups with `group_id`, placed `core_id`, width, and thresholds.
- Optional external input edges into source groups, including external `input_id`, input width, and weights. Reusing the same `input_id` maps one logical external input stream to multiple destination groups; destinations on the same core share one generated local input group.
- Logical group-to-group connections with weight matrices. Reusing the same `source_group` for distinct destination groups models source fanout; the mapper emits one successor route per destination edge so ACK synchronization remains one ACK per successor route.

The generated storage is owned by `NmcGeneratedMappings`. Use `nmc_generated_mappings_find_core()` to retrieve a generated core mapping, then pass that mapping to `nmc_core_configure_mapping()`.

Example shape of a one-input, one-output mapping:

```c
enum { INPUT_A = 0, DOWNSTREAM_INPUT = 0 };
int16_t kernel_8x8[64];
int32_t thresholds[8];

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

### Input interface module: `src/core/input.c`

This module implements `nmc_core_process_input_tile()`.

Behavior:

1. Validate `tile->group_index` and `tile->width`.
2. Reset the per-input-tile schedule counters on `NmcCore`.
3. Call `nmc_core_encode_input_events()` to convert the bitmap payload into per-lane sparse event streams.
4. Read the current and next input LUT entries to obtain the pair range `[start(input), start(input + 1))`.
5. For each pair entry, validate the target output group and weight slice.
6. Call `nmc_core_accumulate_pair()` to route active weight lanes to the adder tree, route packed accumulator lanes through the accumulator MUX, and update the selected output group's accumulator slice.
7. Increment that output group's `input_count` once for this tile arrival.
8. If the output group is input-complete, call `nmc_core_activate_output_group()`. Activation may still fail harmlessly if successor ACK synchronization or queue space is not ready yet.

The function returns `false` if the tile does not match any configured pair range, if any table range is invalid, or if accepting the tile would over-count an output group that is already input-complete.

### ACK interface module: `src/core/ack.c`

This module implements `nmc_core_process_ack()`.

Behavior:

1. Accept only local ACK messages with `destination_count == 1`.
2. Interpret `ack->destinations[0].group_index` as the ACK LUT index on this core.
3. Read the ACK LUT range `[start(ack), start(ack + 1))`.
4. For each stage-2 ACK entry, increment the selected output group's `ack_count`.
5. Reject ACKs that would overflow a counter or exceed the configured successor count.
6. Call `nmc_core_flush_ready_outputs()` so ACK-unblocked groups can emit immediately.

ACK indices are deliberately indirect: a single incoming ACK group can update multiple output groups when one successor acknowledges a shared downstream route.

### Output interface module: `src/core/output.c`

This module owns output readiness, activation integration, queueing, and router-facing decode helpers.

Private helpers in this module derive route ranges from the output route LUT:

- Successors are in `[successor_start, predecessor_start)`.
- Predecessor ACK destinations are in `[predecessor_start, successor_start(next_output))`.
- The terminal output LUT entry supplies the final predecessor exclusive end.

Implemented public or internal functions:

| Function | Behavior |
| --- | --- |
| `nmc_core_output_group_ready()` | Returns whether an output group has a nonzero input requirement and has received all required input tiles for the current natural step. |
| `nmc_core_output_group_successor_count()` | Returns the number of successor destinations in the output route range. |
| `nmc_core_activate_output_group()` | Validates successor synchronization and queue space, runs the output group's activation program, enqueues the output network tile, enqueues predecessor ACKs, and resets the output group's synchronization window only after successful emission. |
| `nmc_core_flush_ready_outputs()` | Scans all output groups and activates any group that is input-ready and output-ready. |
| `nmc_core_pop_output_tile()` | FIFO pop from the output tile queue. |
| `nmc_core_pop_ack()` | FIFO pop from the predecessor ACK queue. |
| `nmc_network_tile_get_input_tile()` | Converts one destination entry of a multicast network tile into a local input tile for the destination core. |

Output activation is intentionally conservative: it checks successor route validity, predecessor route validity, destination count limits, output queue capacity, ACK queue capacity, activation program validity, and packed accumulator fit before mutating the output group synchronization window.

### Activation module: `src/core/activation.c`

This module implements `nmc_core_execute_activation_program()`.

Behavior:

1. Validate the output index, activation program descriptor, instruction range, and payload pointer.
2. Execute the output group's program for each output lane.
3. Validate register operands, immediate operands, SRAM range accesses, and accumulator accesses.
4. Route accumulator reads/writes through the same packed accumulator helpers used by the compute path.
5. Route activation state/parameter reads/writes through explicit unified-SRAM word helpers.
6. Pack predicate emissions into the output payload.
7. Track activation instruction count and cycle estimates.
8. Increment `activation_fault_count` and return `false` on invalid instructions, invalid memory ranges, missing `END`, duplicate payload emission, or watchdog exhaustion.

The current simulator executes the program per lane and scales cycle estimates by `NMC_OUTPUT_PARALLELISM` to approximate vector execution. It remains an architecture-stage model rather than a cycle-accurate RTL pipeline.

### Encoder module: `src/core/encoder.c`

This module implements the sparse bitmap-to-event model used by input processing.

Internal API:

| Function | Behavior |
| --- | --- |
| `nmc_core_encode_input_events()` | Splits an input bitmap across `NMC_INPUT_PARALLELISM` lanes, encodes each lane into ascending event indices, reports total emitted events, and reports the maximum lane encoder-cycle count. |

Each lane scans `NMC_EVENT_ENCODER_WINDOW` bits at a time. It emits at most one event per encoder cycle. If a search window is empty, it can replace that window with the next window in the same modeled cycle, preserving sparse skip behavior while keeping the modeled priority search window narrow.

### Compute module: `src/core/compute.c`

This module owns the event-driven accumulation schedule.

Internal API:

| Function | Behavior |
| --- | --- |
| `nmc_core_pair_weights_fit()` | Validates that an input/output pair's contiguous banked weight matrix fits inside `NMC_UNIFIED_MEMORY_SIZE`. |
| `nmc_core_accumulate_pair()` | Applies one encoded input tile to one input/output pair. It walks sparse event steps across input lanes, routes weight lanes to the adder tree, routes packed accumulator lanes through the accumulator MUX, writes back only the selected accumulator lanes, and advances compute-cycle counters. |

The compute module assumes the encoder has already produced per-lane event indices. It does not inspect bitmap payloads directly.

### Shared core utilities: `src/core/utils.c`

This module contains validation, unified-memory access, accumulator packing, and bit-payload helpers shared by the other core modules.

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
| `nmc_core_output_accumulators_fit()` | Validates that an output group's accumulator LUT entry is valid and that the packed output slice fits in unified memory. |
| `nmc_core_memory_read_weight_lane()` | Routes one unified-memory lane to the adder tree for weight accumulation. |
| `nmc_core_memory_read_accumulator()` | Routes only the selected packed accumulator lanes through the accumulator MUX and reconstructs one membrane potential. |
| `nmc_core_memory_write_accumulator()` | Writes only the selected packed accumulator lanes back to unified memory. |
| `nmc_core_memory_read_activation_word()` | Reads one signed 16-bit activation state/parameter lane as a widened 32-bit value. |
| `nmc_core_memory_write_activation_word()` | Writes one activation state/parameter lane after validating that the value fits in 16 bits. |
| `nmc_print_payload()` | Public debug helper for printing payload bitmaps. |

Payload bits are little-endian within each byte for storage and computation. `nmc_print_payload()` reverses the display order so traces read like conventional binary literals.

### Private core interface: `include/nmc/internal/core.h`

This header is the private contract between core implementation modules. It defines `NmcInputLaneEvents`, activation built-in IDs, activation immediate/range indices, memory-routing enums, and shared helper declarations that are not part of the public API. New application or test code should not include it unless it is deliberately testing internals.

### Router API: `include/nmc/router.h` and `src/router.c`

The router API is public because future fabric or mesh tests need to instantiate routers directly.

| Function | Behavior |
| --- | --- |
| `nmc_router_init()` | Clears one router and assigns its attached `core_id`, mesh coordinate, and routing order. |
| `nmc_router_port_name()` | Returns a static human-readable name for a router output port. |
| `nmc_router_route_message()` | Validates a `NmcRouterMessage`, chooses one next-hop port per destination, splits multicast destinations by port, and enqueues one split message per non-empty output port if all required queues have capacity. |
| `nmc_router_pop_port_message()` | FIFO pop from one router output port queue. |
| `nmc_router_message_get_input_tile()` | Decodes one destination of a local router message with `width > 0` into a core-facing `NmcInputTile`. |
| `nmc_router_message_get_ack()` | Decodes one destination of a local router message with `width == 0` into a core-facing `NmcAckMessage`, but only if the destination coordinate equals the current router coordinate. |

Routing uses `NMC_ROUTER_XY` or `NMC_ROUTER_YX` dimension order. A routed message with several destinations may produce several output messages, but each output message keeps only the destinations that share that next-hop port.

## Demo and regression coverage

All core setups in the executable use the high-level `nmc_core_configure_mapping()` API, and one scenario uses `nmc_generate_core_mappings()` to create those per-core mappings from logical graph connectivity. The demo exercises automatic input LUT, ACK LUT, output route LUT, terminal sentinel, accumulator LUT, input requirement generation, activation program execution, and graph-level group-index assignment.

### Single-core natural-step test

One core is configured with:

- Two input groups.
- Two output groups.
- Multiple ACK groups.
- Shared and output-specific successor ACK paths.
- Contiguous weight slices for each input/output pair.

The trace checks that:

- An output with one required input can fire before an output with two required inputs.
- Outputs send predecessor ACKs after injection.
- Later natural-step inputs can be consumed while outputs are successor-ACK blocked.
- Returning successor ACKs flush the correct pending outputs.
- Reversed input arrival order still produces correct readiness and blocking behavior.

### Encoder scheduling test

A sparse input checks that the event encoder skips empty windows and still produces the expected event count, encoder-cycle count, and compute-cycle count.

### Unified banked memory test

A non-uniform kernel checks that high-level output-major weights are transposed into input-bank-major unified-memory rows, that auto-placed weight slices start after packed accumulator slices, and that one sparse event step updates only the selected accumulator lanes through the accumulator MUX.

### Activation test coverage

The activation tests check that:

- The default IF activation program reproduces threshold-and-reset behavior.
- Uniform thresholds are represented as immediates.
- Heterogeneous thresholds can be stored in unified SRAM and loaded through activation ranges.
- Activation counters and fault behavior are deterministic.

### Router and mesh tests

The router tests check that:

- An XY router splits a multicast across local, east, west, north, and south output ports.
- Multiple destinations sharing the same first hop are grouped into one forwarded message.
- Local spike-tile messages decode into core-facing input tiles.
- YX routing chooses a different first hop for an off-axis destination.

The multi-core mesh test connects several cores through a 2D router mesh and verifies injected input, source-to-consumer routing, consumer-to-observer routing, ACK return to the source, and a second source emission after ACK credit arrives.

## Important limits and knobs

The main hardware-style limits are defined in `include/nmc/core.h` and `include/nmc/router.h`.

| Constant | Meaning |
| --- | --- |
| `NMC_MAX_GROUP_NEURONS` | Maximum neurons/spikes represented by one tile payload. |
| `NMC_MAX_INPUT_GROUPS` | Maximum input groups on one core. |
| `NMC_MAX_ACK_GROUPS` | Maximum ACK input groups on one core. |
| `NMC_MAX_OUTPUT_GROUPS` | Maximum output groups on one core. |
| `NMC_MAX_TILE_DESTINATIONS` | Maximum multicast destinations in one tile or ACK message. |
| `NMC_WEIGHT_MEMORY_SIZE` | Legacy weight-lane capacity contribution to unified memory. |
| `NMC_ACCUMULATOR_MEMORY_SIZE` | Legacy accumulator-value capacity contribution to unified memory. |
| `NMC_ACCUMULATOR_LANES` | Number of 16-bit unified-memory lanes occupied by one accumulator value. |
| `NMC_UNIFIED_MEMORY_SIZE` | Signed 16-bit memory lanes shared by weights, packed accumulators, and optional activation state/parameters. |
| `NMC_INPUT_PARALLELISM` | Number of input event lanes in the compute schedule. |
| `NMC_OUTPUT_PARALLELISM` | Output vector-lane knob used by activation cycle estimates; the current memory model updates each output group with its full output-wide accumulator row. |
| `NMC_MAX_ACTIVATION_PROGRAMS` | Maximum activation program descriptors per core. |
| `NMC_MAX_ACTIVATION_INSTRUCTIONS` | Maximum activation microinstructions per core. |
| `NMC_MAX_ACTIVATION_REGISTERS` | Register count visible to activation microprograms. |
| `NMC_MAX_ACTIVATION_SRAM_RANGES` | Number of per-output validated SRAM ranges for activation state/parameters. |
| `NMC_MAX_ACTIVATION_IMMEDIATES` | Number of per-output immediate operands visible to activation microprograms. |
| `NMC_MAX_ACTIVATION_STEPS` | Default activation watchdog budget. |
| `NMC_ACTIVATION_MUL_LATENCY` | Modeled extra latency for activation multiply/MAC instructions. |
| `NMC_ACTIVATION_SRAM_LANES` | Activation SRAM access lane grouping. |
| `NMC_ACTIVATION_MEMBRANE_WORDS` | Number of weight-sized words occupied by one membrane/accumulator value. |
| `NMC_ROUTER_MAX_PORT_QUEUE` | Queue depth per router output port. |

Tile widths must be positive multiples of 8 and no larger than `NMC_MAX_GROUP_NEURONS`.

## Development notes

- Keep the model dependency-free unless a new dependency is clearly worth it.
- Preserve the fixed-size-array style in core datapath structures; it mirrors SRAM/register-file limits and makes capacity failures explicit.
- Keep `include/nmc/core.h` and `include/nmc/router.h` as the canonical public API headers.
- Shared implementation-only helpers belong in `include/nmc/internal/core.h`; it is private despite living under the include tree.
- Keep the core submodules narrow:
  - `src/core.c`: top-level lifecycle and built-in activation program installation.
  - `src/core/config.c`: low-level configuration.
  - `src/core/mapping.c`: high-level mapping and graph lowering.
  - `src/core/input.c`: input handling.
  - `src/core/ack.c`: ACK handling.
  - `src/core/activation.c`: activation v-ALU execution.
  - `src/core/output.c`: output readiness, activation integration, and queues.
  - `src/core/encoder.c`: sparse encoding.
  - `src/core/compute.c`: compute scheduling.
  - `src/core/utils.c`: common helpers.
  - `src/router.c`: router model.
- Configure LUT terminal entries carefully. Many ranges are derived from entry `i` and entry `i + 1`.
- Treat core IDs and mesh coordinates as separate namespaces. The test bench currently maps between them before injecting core-generated messages into routers.
- If changing function signatures or table semantics, update both the core tests and router/mesh tests in `src/main.c`.

## Suggested next iterations

The README contains the current high-level roadmap. Technical follow-up work includes:

1. Extract the ad hoc mesh wiring in `src/main.c` into a reusable mesh-fabric module.
2. Add explicit NACK/BUSY retry behavior for full queue back-pressure modeling.
3. Model lower-level pipeline timing and bank conflict penalties for overloaded parallel event schedules.
4. Add probes for SRAM accesses, cycle count, communication overhead, communication delay, accumulator reuse, multicast splits, and ACK blocking time.
5. Add tests for overlapping input windows and convolution-like mappings.
6. Add importers that produce `NmcNetworkMappingSpec` from common model or graph formats.
7. Add deeper traces for deadlock-prone recurrent or cyclic core graphs.
8. Incrementally translate the stable C simulation mechanisms into synthesizable RTL.
