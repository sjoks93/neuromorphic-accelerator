# Digital Neuromorphic Core - C Simulation Stage

This repository models the first design stage of a digital neuromorphic core specialized for spike inputs and spike outputs.

The current implementation focuses on the **input interface**, **output interface**, and the scheduling/marking behavior around input and output neuron groups. The neuron compute path is intentionally simplified so the interface concepts can be evolved before adding detailed datapath timing, accumulator banking, SIMD lanes, or adder-tree behavior.

The next implementation layer has also started: a standalone **2D mesh router** model can route tile or multicast ACK messages with XY or YX dimension-order routing and multicast splitting before the router is integrated with multiple cores.

## Core concepts

- **Spike tiles are byte-array bitmaps**: an input or output tile carries one bit per neuron in a group, with group widths constrained to multiples of 8.
- **Neurons are organized into groups** addressed by compact zero-based indices.
- **An output group** is a set of output neurons that share the same subscribed input groups.
- **Input groups may overlap** across output groups, enabling mappings such as windowed convolution.
- The **input side uses two independent two-stage LUTs**:
  - **input-tile consumption LUT**:
    - stage 1: the input index indexes the input group table
    - stage 1 output: one start address into the stage-2 LUT; the next input group's start address supplies the exclusive end
    - stage 2: each address is one input/output pair and stores the output index plus the starting address of the contiguous weight matrix slice
    - the input bitmap width is not stored in the input group table; it comes from the received tile header
  - **ACK-return LUT**:
    - stage 1: the ACK index carried by an ACK message indexes the ACK group table
    - stage 1 output: one start address into the ACK stage-2 LUT; the next ACK group's start supplies the exclusive end
    - stage 2: each address stores the output index whose ACK counter must be incremented
- The **output interface also uses a two-stage LUT**:
  - stage 1: the output index indexes the output group table
  - stage 1 output: output bitmap width plus successor start and predecessor start addresses into the stage-2 route LUT
  - a terminal output-table entry after the last output group supplies the final predecessor exclusive end
  - stage 2: route entries are ordered as `successors(G0), predecessors(G0), successors(G1), predecessors(G1), ...`
  - successor entries route output network tiles forward
  - predecessor entries route multicast ACK messages backward
- The input group table and ACK group table both store valid start addresses into their stage-2 pair LUTs, including one terminal start after the last real group; the output group table uses route addresses (`valid`, `bitmap_width`, `successor_start`, `predecessor_start`), including one terminal start entry after the last output group.
- For every input-group / output-group pair, weights are stored contiguously in memory and referenced by the LUT.
- When an input tile is received, every matching input/output pair is consumed once for the current step.
- Each output group stores a compile-time input requirement plus two runtime counters: received input tiles and received successor ACKs.
- When all subscribed inputs of an output group are consumed and successor ACK synchronization is complete, the group is activated and both runtime counters reset to zero after output injection.
- Each activated output group emits one network tile with a destination-address header and bitmap payload, plus one ACK message with a destination-address header and no payload for that output group's predecessors.

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
- The mapping stage records each output group's subscribed input requirement in the output group table. Runtime consumption increments the output group's input-tile counter instead of using consumed masks.
- Each output group carries two runtime counters: an input-tile counter for activation readiness and an ACK counter for successor synchronization.
- Output injection immediately produces one routed multicast predecessor ACK message, while successor ACK synchronization gates the next output injection.
- Incoming successor ACK messages are consumed through the ACK input LUT rather than by directly naming a route entry. This keeps ACK handling index-based, just like input tile consumption.

## Output-interface hardware mechanism

For each activated output group:

1. The zero-based output index directly indexes the output group table, which is the first-stage output LUT.
2. The first-stage entry supplies the output bitmap width and two addresses into the second-stage route LUT:
  - `bitmap_width`
  - `successor_start`
  - `predecessor_start`
3. The successor route range is `[successor_start, predecessor_start)`.
4. The predecessor ACK route range is `[predecessor_start, successor_start(next_output))`; the final output group uses the terminal output-table entry as `next_output`.
5. Each successor entry supplies:
  - target core ID
  - target group index as interpreted by the target core
6. Each predecessor entry supplies:
  - predecessor core ID
  - predecessor group index to receive an ACK message
7. The core emits one network tile containing:
  - header field: number of destinations
  - header field: tile size / width
  - header array: successor destination addresses, each `{core_id, group_index}`
  - payload: spike bitmap with `width` bits
8. After output injection succeeds, the core immediately emits one multicast ACK message containing the predecessor route entries and resets the output group's counters. The predecessor destination group index is interpreted as an ACK index by the receiving core.

## Communication and tile consumption

Generated network tiles and ACK messages are router-facing. Routers inspect the destination-address array and forward/copy each message toward each target core. A nonzero tile size/width indicates a spike tile with payload; width `0` represents an ACK message with no payload.

The standalone router model uses mesh coordinates rather than core-local IDs.  For each destination it chooses one next-hop port:

- `LOCAL` if the destination coordinate equals the router coordinate
- `EAST` or `WEST` if the selected routing order resolves the x dimension next
- `NORTH` or `SOUTH` if the selected routing order resolves the y dimension next

For multicast, the router partitions the destination list by next-hop port and enqueues one updated message per non-empty port.  The same header structure is used for tiles and ACKs: destination count, tile size/width, then the destination-address array.  Tile messages carry a payload after the destination header; ACK messages use width `0` and carry no payload. A destination whose next hop is `LOCAL` can be decoded into a core-facing input tile or ACK message using the router's attached local core ID.  This makes the router a pure communication component for now; a later multi-core wrapper can connect each directional queue to a neighboring router and connect the local queue to an `NmcCore`.

At a destination core, the router presents a local input tile with only the information the destination needs:

- header field: tile size / width
- header field: local group index
- payload: spike bitmap

The destination core consumes this local input tile by using the group index as the stage-1 input indirection address.

## Back-pressure and natural step ordering

The core should not need explicit timestep tags. Step ordering can be a natural consequence of local ACK synchronization: a predecessor is allowed to send the next tile on a mapped edge only after the successor has finished using the previous tile on that edge.

The key question is: **when is it safe to let an output tile leave the core?** It is safe only when these conditions hold:

1. **All required inputs for the output group have arrived.** The output group's input-tile counter equals the configured input requirement.
2. **All successors have ACKed the previous emission.** The output group's ACK counter equals the number of successor destinations in its output route range.
3. **The output tile has a place to go.** The output network queue/router can accept the generated network tile for all of its destinations.

Only after these conditions are true may the core emit the output tile. As soon as the tile is emitted, the core has consumed the input tiles that produced it, so it immediately sends ACKs back to the predecessor edges. Those predecessors may then send their next natural-step tiles. The output group can consume those next inputs right away, but it cannot emit the next output until successor ACKs for the previous emission have returned.

The successor ACK is therefore a **precondition for injection**, represented as a per-output ACK counter. The core should not wait for a successor to process the just-sent tile before letting the tile leave; that would turn local flow control into a long end-to-end barrier. Instead, the successor processes the tile later and sends an ACK that increments the relevant output group's ACK counter.

The initial FSM state assumes every output group starts synchronized: its ACK counter is initialized to its successor count because no previous output tile is in flight.

The proposed ACK/NACK pressure can be stated as follows:

1. **Compile-time mapping records predecessors.** For each output group, the mapping stage records `input_count`. It must also be possible to enumerate the predecessor edges represented by that count: source core/group, destination core/group, and destination output group.
2. **Accepting an input tile closes that predecessor edge.** When a destination core accepts a local input tile for group `g`, that predecessor edge becomes busy and cannot accept the next tile from the same predecessor yet.
3. **Consumption increments output readiness counters.** The accepted tile is applied to every input/output pair selected by the input LUT, and each affected output group's input-tile counter increments.
4. **The output group becomes complete when its input counter reaches its requirement.** It may compute its spike bitmap, but it should not inject the output tile until its ACK counter also reaches the successor count derived from the output route LUT.
5. **Output injection consumes the inputs immediately.** When the generated network tile is accepted by the output queue/router, the core sends one multicast ACK message to all predecessor edges that contributed to that output group.
6. **Output injection starts a new synchronization window.** The core resets that output group's input-tile counter and ACK counter to zero. New predecessor tiles may be consumed and counted immediately, but the next output remains blocked while the ACK counter is below the successor count.
7. **Successor ACK completion releases the next output, not the next input.** As successor ACK messages arrive, the ACK input LUT increments the output group's ACK counter. If the next input set is already complete, reaching the successor count flushes the pending output and immediately ACKs its predecessors.

This makes the protocol equivalent to one in-flight tile per mapped predecessor edge. Natural succession comes from ACK return order rather than explicit timestep fields. If the network can reorder traffic, the implementation should preserve per-edge ordering for data and ACK messages, or keep the one-in-flight rule strict enough that there is never more than one unacknowledged tile per edge.

The current C model starts this mechanism by storing input and ACK counters on each output group. Successful output injection enqueues one multicast predecessor ACK message immediately and resets both counters. A complete multi-core simulation should route these ACK messages back to predecessor cores, splitting the destination header as needed. At each receiving predecessor, the local ACK message's group index selects the ACK input LUT; that LUT can increment one or more output-group ACK counters and potentially flush a pending output.

Important design gaps to resolve in the next implementation stage:

- How explicit `NACK/BUSY` retry should be represented if a source violates the ACK ordering rule.
- Whether ACKs are sent per input/output pair, per input group after all local output consumers finish, or aggregated by routers for multicast fanout.
- How output-network back-pressure is represented when an output group is complete but its network tile or multicast ACK cannot yet be accepted.
- How predecessor route entries are generated for remote cores during mapping.
- How to avoid deadlock when recurrent/cyclic core graphs exist; at minimum, bounded queues and deterministic ACK-counter initialization are needed.

## Input-interface hardware mechanism

The core has two first-stage input-facing tables with the same start-plus-terminal structure:

- input tile groups for spike-data consumption
- ACK groups for successor synchronization returns

Both use a compact zero-based group index supplied by the router in the local message header.

For each received input tile:

1. The router has already selected the destination core and presents a local input tile.
2. The tile's zero-based group index directly indexes the input group table, which is the first-stage LUT.
3. The first-stage entry and the following entry supply:
  - the inclusive/exclusive range `[start(input_index), start(input_index + 1))` in the second-stage pair LUT
4. The input bitmap width is taken from the received tile header, not from the input group table.
5. The core walks the second-stage pair LUT range.
6. Each second-stage entry supplies:
  - output index
  - starting address of the contiguous weight matrix for this input/output pair
7. The compute model consumes the input spikes against the addressed weight matrix, using the received tile width as the input matrix dimension.
8. The output group's input-tile counter increments.
9. If the input-tile counter reaches the configured input requirement, the output group becomes activation-ready; it emits a network tile when its ACK counter indicates synchronization and output queue space is available.

For each received ACK message:

1. The router presents a local ACK message whose group index is an ACK index.
2. The ACK index directly indexes the ACK group table, which is the first-stage ACK LUT.
3. The ACK entry and following entry supply the range `[start(ack_index), start(ack_index + 1))` in the ACK stage-2 LUT.
4. Each ACK stage-2 entry supplies:
  - output index
5. The core increments the ACK counter for each listed output group.
6. After ACK counters are updated, the core flushes any output groups that were complete but blocked by successor synchronization.
7. Any output emitted by that flush immediately sends predecessor ACKs and starts the next synchronization window.

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
  - starts are `{0, 2, 3}`
  - input index `0` maps to pair LUT range `[0, 2)`
  - input index `1` maps to pair LUT range `[2, 3)`
- Three ACK group entries:
  - starts are `{0, 2, 3, 4}`
  - ACK index `0` is the common successor ACK from core `1` and increments both output indices `0` and `1`
  - ACK index `1` increments output index `0`'s ACK counter for the successor route to core `2`
  - ACK index `2` increments output index `1`'s ACK counter for the successor route to core `3`
- Two output groups:
  - output index `0`, eight output neurons wide, subscribed to both input groups
  - output index `1`, eight output neurons wide, subscribed only to input index `0`
- Contiguous weight memory slices for each input/output pair.
- Output route LUT entries ordered as successors then predecessors per output group:
  - output index `0`: successors target cores `1` and `2`, then one multicast ACK header with predecessor ACK indices `0` and `1` on the predecessor core
  - output index `1`: successors target cores `1` and `3`, then one multicast ACK header with predecessor ACK index `0` on the predecessor core
  - terminal output entry: successor/predecessor starts are both `7`, terminating output index `1`'s predecessor range

The executable is now a small self-checking test bench rather than a single-shot demo. It exercises:

1. Initial ACK synchronization on every output group.
2. A first natural step where input index `0` arrives first:
  - output index `1` emits immediately because it only depends on input index `0`
  - output index `0` waits for input index `1`
3. Completion of that first step when input index `1` arrives and output index `0` emits.
4. Each output emission immediately generates one multicast predecessor ACK message, so natural step `1` can arrive before successor ACKs for step `0` have returned.
5. A second natural step whose inputs are consumed while both output groups are still successor-ACK blocked.
6. Explicit successor ACK returns for the first step:
  - returning the common core-`1` successor ACK increments both output groups, but neither group is complete yet
  - returning output index `1`'s core-`3` successor ACK flushes output index `1`'s pending step-`1` output and immediately sends one multicast ACK to its predecessor list
  - returning output index `0`'s core-`2` successor ACK flushes output index `0`'s pending step-`1` output and immediately sends one multicast ACK to its predecessor list
7. A third natural step with reversed input arrival order:
  - input index `1` arrives first and completes no output
  - input index `0` arrives second and completes both output groups, but both stay blocked until successor ACKs from step `1` return
  - those successor ACKs then flush the pending step-`2` outputs

The test bench fails with a nonzero exit code if any expected output tile count or ACK count is wrong.

The same executable also includes a standalone router test bench. It verifies that:

1. An XY router at coordinate `(1, 1)` splits a six-destination multicast message into local, east, west, north, and south output-port messages.
2. The east output receives two destinations when two target coordinates need the same first XY hop.
3. The local output can be decoded back into a core-facing input tile.
4. A YX router sends the same off-axis destination vertically first, demonstrating that the routing-order configuration changes the first hop.

Finally, the executable now includes a small multi-core mesh test bench. It uses three configured cores attached to a 2x2 router mesh:

- source core `10` at coordinate `(0, 0)`
- observer core `12` at coordinate `(1, 0)`
- consumer core `11` at coordinate `(1, 1)`
- one pass-through router at coordinate `(0, 1)`

The multi-core test assumes external spike input enters from the west side through the edge router at `(0, 0)`. The test then checks that:

1. The side-injected tile is routed to the source core's local input port.
2. The source core emits a tile toward the consumer core.
3. XY routing forwards that tile east from `(0, 0)` to `(1, 0)`, then south to `(1, 1)`.
4. The consumer core receives the tile, emits an output tile to the observer core, and immediately emits a multicast predecessor ACK message back toward the source core.
5. The observer tile routes north from `(1, 1)` to `(1, 0)` and is consumed by the observer core.
6. The ACK routes west from `(1, 1)` to `(0, 1)`, then north to `(0, 0)`, where it is consumed by the source core's ACK LUT.
7. After the ACK returns, a second west-side injected input can make the source core emit another output tile.

## Suggested next iterations

1. Add explicit input-event parallelization with an adder-tree reduction model.
2. Add SIMD output-neuron lanes and cycle-level scheduling.
3. Add accumulator-bank allocation and reuse constraints.
4. Add memory-bank layout constraints for contiguous weights.
5. Turn the multi-core test harness into a reusable mesh fabric module.
6. Add traces and tests for overlapping input windows.
