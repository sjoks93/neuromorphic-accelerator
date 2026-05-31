# Digital Neuromorphic Core - Stage 1 C Simulation

This repository models the first design stage of a digital neuromorphic core specialized for spike inputs and spike outputs.

The current implementation focuses on the **input interface**, **output interface**, and the scheduling/marking behavior around input and output neuron groups. The neuron compute path is intentionally simplified so the interface concepts can be evolved before adding detailed datapath timing, accumulator banking, SIMD lanes, or adder-tree behavior.

## Core concepts

- **Spike tiles are byte-array bitmaps**: an input or output tile carries one bit per neuron in a group, with group widths constrained to multiples of 8.
- **Neurons are organized into groups** addressed by compact zero-based indices.
- **An output group** is a set of output neurons that share the same subscribed input groups.
- **Input groups may overlap** across output groups, enabling mappings such as windowed convolution.
- The **input interface uses a two-stage LUT**:
  - stage 1: the input index indexes the input group table
  - stage 1 output: start/end addresses into the stage-2 LUT
  - stage 2: each address is one input/output pair and stores the output index plus the starting address of the contiguous weight matrix slice
- The **output interface also uses a two-stage LUT**:
  - stage 1: the output index indexes the output group table
  - stage 1 output: start/end addresses into the stage-2 destination LUT
  - stage 2: each address is one output destination and stores the target core ID plus the target group index used by that core
- Input and output group tables share the same indirect addresser datatype: valid bit, start address, and end address.
- For every input-group / output-group pair, weights are stored contiguously in memory and referenced by the LUT.
- When an input tile is received, every matching input/output pair is consumed once for the current step.
- Each output group stores a compile-time input count and a runtime remaining-input counter.
- When all subscribed inputs of an output group are consumed, the remaining-input counter reaches zero, the group is activated, and the counter is reset to the compile-time input count.
- Each activated output group emits one network tile with a destination-address header and bitmap payload.

## Stage 1 simplifications

The current C model captures the core interface and execution semantics, while simplifying compute:

- Input and output group widths are represented by byte-array payloads, allowing the maximum payload width to scale by changing `NMC_MAX_GROUP_NEURONS`.
- Group widths must be positive multiples of 8 bits.
- Weights are signed 16-bit integers in contiguous row-major matrices:
  - row = output neuron index
  - column = input neuron index
- Accumulators are signed 32-bit integers.
- Output accumulator and threshold state are stored per output neuron.
- For each active input bit, the corresponding column is added to each output neuron accumulator.
- Output activation thresholds accumulators into a spike bitmap.
- Accumulators are reset after activation, modeling accumulator reuse.
- The mapping stage records each output group's subscribed input count in the output group table. Runtime consumption decrements this counter instead of using consumed masks.

## Output-interface hardware mechanism

For each activated output group:

1. The zero-based output index directly indexes the output group table, which is the first-stage output LUT.
2. The first-stage entry supplies the inclusive/exclusive range `[destination_start, destination_end)` in the second-stage destination LUT.
3. The core walks the second-stage destination LUT range.
4. Each destination entry supplies:
  - target core ID
  - target group index as interpreted by the target core
5. The core emits one network tile containing:
  - header field: number of destinations
  - header field: tile size / width
  - header array: destination addresses, each `{core_id, group_index}`
  - payload: spike bitmap with `width` bits

## Communication and tile consumption

The generated network tile is router-facing. Routers inspect the destination-address array and forward/copy the tile toward each target core.

At a destination core, the router presents a local input tile with only the information the destination needs:

- header field: tile size / width
- header field: local group index
- payload: spike bitmap

The destination core consumes this local input tile by using the group index as the stage-1 input indirection address.

## Input-interface hardware mechanism

For each received input tile:

1. The router has already selected the destination core and presents a local input tile.
2. The tile's zero-based group index directly indexes the input group table, which is the first-stage LUT.
3. The first-stage entry supplies:
  - the inclusive/exclusive range `[pair_start, pair_end)` in the second-stage pair LUT
4. The core walks the second-stage pair LUT range.
5. Each second-stage entry supplies:
  - output index
  - starting address of the contiguous weight matrix for this input/output pair
6. The compute model consumes the input spikes against the addressed weight matrix.
7. The output group's remaining-input counter decrements.
8. If the counter reaches zero, the output group activates and emits a network tile.

## Repository layout

```text
.
├── Makefile
├── README.md
├── include/
│   └── neuromorphic_core.h
└── src/
    ├── main.c
    └── neuromorphic_core.c
```

## Build and run

```sh
make
./build/neuromorphic_core_demo
```

Clean build outputs:

```sh
make clean
```

## Demo topology

The demo instantiates one core with:

- Two input groups at indices `0` and `1`, each eight neurons wide.
- Two first-stage input group entries:
  - input index `0` maps to pair LUT range `[0, 2)`
  - input index `1` maps to pair LUT range `[2, 3)`
- Two output groups:
  - output index `0`, subscribed to both input groups
  - output index `1`, subscribed only to input index `0`
- Contiguous weight memory slices for each input/output pair.
- Output destination LUT entries that map:
  - output index `0` to target cores `1` and `2`, target group index `0`
  - output index `1` to target core `3`, target group index `1`

Expected behavior:

1. Receiving input index `0` updates output indices `0` and `1`.
2. Output index `1` activates immediately because all of its inputs are consumed.
3. Output index `0` waits until input index `1` arrives.
4. Receiving input index `1` completes output index `0`, causing it to emit an output tile.

## Suggested next iterations

1. Add explicit input-event parallelization with an adder-tree reduction model.
2. Add SIMD output-neuron lanes and cycle-level scheduling.
3. Add accumulator-bank allocation and reuse constraints.
4. Add memory-bank layout constraints for contiguous weights.
5. Add multi-core routing and tile delivery simulation.
6. Add traces and tests for overlapping input windows.
