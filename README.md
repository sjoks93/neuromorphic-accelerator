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
  - the input bitmap width is not stored in the input group table; it comes from the received tile header
- The **output interface also uses a two-stage LUT**:
  - stage 1: the output index indexes the output group table
  - stage 1 output: output bitmap width plus successor start, predecessor start, and predecessor end addresses into the stage-2 route LUT
  - stage 2: route entries are ordered as `successors(G0), predecessors(G0), successors(G1), predecessors(G1), ...`
  - successor entries route output network tiles forward
  - predecessor entries route ACK/credit-return messages backward
- The input group table uses a generic indirect address (`valid`, `start`, `end`) into the pair LUT; the output group table uses a route address (`valid`, `bitmap_width`, `successor_start`, `predecessor_start`, `predecessor_end`) into the route LUT.
- For every input-group / output-group pair, weights are stored contiguously in memory and referenced by the LUT.
- When an input tile is received, every matching input/output pair is consumed once for the current step.
- Each output group stores a compile-time input count and a runtime remaining-input counter.
- When all subscribed inputs of an output group are consumed, the remaining-input counter reaches zero, the group is activated, and the counter is reset to the compile-time input count.
- Each activated output group emits one network tile with a destination-address header and bitmap payload.

## Stage 1 simplifications

The current C model captures the core interface and execution semantics, while simplifying compute:

- Bitmap payloads are represented as byte arrays, allowing the maximum payload width to scale by changing `NMC_MAX_GROUP_NEURONS`.
- Input bitmap width is carried by the received input tile. The core does not duplicate that width in the input group table.
- Output bitmap width is stored in the output route table so the output router can determine generated message length.
- Tile widths must be positive multiples of 8 bits.
- Weights are signed 16-bit integers in contiguous row-major matrices:
  - row = output neuron index
  - column = input neuron index
- Accumulators are signed 32-bit integers.
- Output accumulator and threshold state are stored per output neuron.
- For each active input bit, the corresponding column is added to each output neuron accumulator.
- Output activation thresholds accumulators into a spike bitmap.
- Accumulators are reset after activation, modeling accumulator reuse.
- The mapping stage records each output group's subscribed input count in the output group table. Runtime consumption decrements this counter instead of using consumed masks.
- Output successor route entries carry credit state. Credits are initialized as available, modeling an initially ready system.
- Output activation produces routed predecessor ACK messages once the output network tile is accepted.

## Output-interface hardware mechanism

For each activated output group:

1. The zero-based output index directly indexes the output group table, which is the first-stage output LUT.
2. The first-stage entry supplies the output bitmap width and three addresses into the second-stage route LUT:
  - `bitmap_width`
  - `successor_start`
  - `predecessor_start`
  - `predecessor_end`
3. The successor route range is `[successor_start, predecessor_start)`.
4. The predecessor ACK route range is `[predecessor_start, predecessor_end)`.
5. Each successor entry supplies:
  - target core ID
  - target group index as interpreted by the target core
  - a credit bit indicating whether that successor ACKed the previous tile on this edge
6. Each predecessor entry supplies:
  - predecessor core ID
  - predecessor group index to receive an ACK/credit-return message
7. The core emits one network tile containing:
  - header field: number of destinations
  - header field: tile size / width
  - header array: successor destination addresses, each `{core_id, group_index}`
  - payload: spike bitmap with `width` bits
8. After output injection succeeds, the core emits ACK messages to the predecessor route entries.

## Communication and tile consumption

The generated network tile is router-facing. Routers inspect the destination-address array and forward/copy the tile toward each target core.

At a destination core, the router presents a local input tile with only the information the destination needs:

- header field: tile size / width
- header field: local group index
- payload: spike bitmap

The destination core consumes this local input tile by using the group index as the stage-1 input indirection address.

## Back-pressure and natural step ordering

The core should not need explicit timestep tags. Step ordering can be a natural consequence of local credits and ACKs: a predecessor is allowed to send the next tile on a mapped edge only after the successor has finished using the previous tile on that edge.

The key question is: **when is it safe to let an output tile leave the core?** It is safe only when these conditions hold:

1. **All required inputs for the output group have arrived.** The output group's `remaining_input_count` has reached zero.
2. **All destination edges have credit.** Each successor/destination has previously ACKed the prior tile on that edge, so the current tile will not overrun a successor that is still waiting for other inputs.
3. **The output tile has a place to go.** The output network queue/router can accept the generated network tile for all of its destinations.

Only after these conditions are true may the core emit the output tile and open the next natural step for its predecessor edges.

The successor ACK is therefore a **precondition for injection**, represented as credit on each outgoing destination edge. The core should not wait for a successor to process the just-sent tile before letting the tile leave; that would turn local flow control into a long end-to-end barrier. Instead, the successor processes the tile later and returns credit for the next tile on that same edge.

The initial FSM state assumes every outgoing destination edge starts with one available credit. In other words, every successor is initially ready to accept the first activation.

The proposed ACK/NACK pressure can be stated as follows:

1. **Compile-time mapping records predecessors.** For each output group, the mapping stage records `input_count`. It must also be possible to enumerate the predecessor edges represented by that count: source core/group, destination core/group, and destination output group.
2. **Accepting an input tile closes that predecessor edge.** When a destination core accepts a local input tile for group `g`, that predecessor edge becomes busy and cannot accept the next tile from the same predecessor yet.
3. **Consumption decrements output readiness counters.** The accepted tile is applied to every input/output pair selected by the input LUT, and each affected output group's `remaining_input_count` decrements.
4. **The output group becomes complete at zero.** When `remaining_input_count == 0`, the output group has all inputs for the current natural step. It may compute its spike bitmap, but it should not inject the output tile until all outgoing destination credits are available.
5. **Output injection consumes successor credit.** When the generated network tile is accepted by the output queue/router, the core consumes one credit on every outgoing destination edge carried by that tile.
6. **Output acceptance releases predecessor credit.** Once the generated network tile is accepted by the output queue/router, the output group resets `remaining_input_count = input_count` and sends ACK/credit-return messages to all predecessor edges that contributed to that output group.
7. **Early next tiles are stalled or rejected.** If a predecessor tries to send again before its ACK/credit return, the destination returns `NACK/BUSY` or the router/source stalls because the edge has no credit.

This makes the protocol equivalent to one in-flight tile per mapped predecessor edge. Natural succession comes from credit return order rather than explicit timestep fields. If the network can reorder traffic, the implementation should preserve per-edge ordering for data and ACK messages, or keep the one-credit rule strict enough that there is never more than one unacknowledged tile per edge.

The current C model starts this mechanism by storing credit on output successor route entries and enqueueing routed predecessor ACK messages after successful output injection. A complete multi-core simulation should route these ACK messages back to predecessor cores and call credit-return logic when successors finish their downstream work.

Important design gaps to resolve in the next implementation stage:

- Whether back-pressure is **credit-based** (preferred for hardware) or explicit `NACK` retry.
- Whether ACKs are sent per input/output pair, per input group after all local output consumers finish, or aggregated by routers for multicast fanout.
- How output-network back-pressure is represented when an output group is complete but its network tile cannot yet be accepted.
- How predecessor route entries are generated for remote cores during mapping.
- How to avoid deadlock when recurrent/cyclic core graphs exist; at minimum, bounded queues and deterministic credit initialization are needed.

## Input-interface hardware mechanism

For each received input tile:

1. The router has already selected the destination core and presents a local input tile.
2. The tile's zero-based group index directly indexes the input group table, which is the first-stage LUT.
3. The first-stage entry supplies:
  - the inclusive/exclusive range `[pair_start, pair_end)` in the second-stage pair LUT
4. The input bitmap width is taken from the received tile header, not from the input group table.
5. The core walks the second-stage pair LUT range.
6. Each second-stage entry supplies:
  - output index
  - starting address of the contiguous weight matrix for this input/output pair
7. The compute model consumes the input spikes against the addressed weight matrix, using the received tile width as the input matrix dimension.
8. The output group's remaining-input counter decrements.
9. If the counter reaches zero, the output group becomes ready; it emits a network tile when successor credits and output queue space are available.

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

- Two input groups at indices `0` and `1`; their widths are supplied by the incoming input tiles.
- Two first-stage input group entries:
  - input index `0` maps to pair LUT range `[0, 2)`
  - input index `1` maps to pair LUT range `[2, 3)`
- Two output groups:
  - output index `0`, eight output neurons wide, subscribed to both input groups
  - output index `1`, eight output neurons wide, subscribed only to input index `0`
- Contiguous weight memory slices for each input/output pair.
- Output route LUT entries ordered as successors then predecessors per output group:
  - output index `0`: successors target cores `1` and `2`, then predecessors input groups `0` and `1`
  - output index `1`: successor target core `3`, then predecessor input group `0`

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
