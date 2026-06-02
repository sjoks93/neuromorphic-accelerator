# Proposal: Programmable Neuron Activation ALU

## Summary

The current core has a fixed output activation stage: when an output group is ready, each accumulator is compared against a stored threshold and the result bit is emitted. This is sufficient for a simple integrate-and-fire (IF) neuron, but it does not model a programmable neuron processor or vector ALU (v-ALU) that can execute small per-group programs for spike-based neuron models such as IF, leaky integrate-and-fire (LIF), refractory IF, adaptive threshold IF, or other event-driven spiking rules.

This proposal adds a small programmable activation engine after accumulation and before output-tile emission. The engine executes a bounded microprogram over each output group, producing only a spike bitmap payload and updating local neuron state. In IF-like models, the output accumulator is the membrane potential itself. Threshold is assumed constant by default, either one threshold per output group or one shared threshold per core, and should be encoded as an immediate operand unless a mapping explicitly requests per-neuron thresholds. Other per-neuron state or parameters, such as per-neuron thresholds, refractory counters, adaptation variables, or reset values, are stored in unified SRAM only when needed. Per-group constants, such as leakage, reset voltage, or refractory duration, can also be encoded as immediate operands in the activation program. The software simulator should model this as explicit instruction memory, group-level program binding, unified-SRAM state ranges, deterministic execution budget, and hardware-like failure/back-pressure behavior.

## Goals

- Replace hard-coded threshold activation with a programmable output-stage pipeline.
- Preserve the existing event-driven sparse accumulation, ACK protocol, route LUTs, and output queue behavior.
- Support IF neurons as the default built-in program, so existing mappings remain valid.
- Support small, deterministic programs suitable for hardware realization rather than arbitrary host code.
- Model memory/register pressure, instruction count, lane parallelism, and activation latency explicitly.
- Support programmable spike-generation rules without redesigning the core datapath.

## Non-goals

- Do not embed a general-purpose CPU in each core.
- Do not support unbounded loops, recursion, dynamic allocation, indirect arbitrary memory access, or host callbacks.
- Do not change the router packet format; programmable activation emits spike bitmaps only.
- Do not support value activations such as ReLU feature maps, sigmoid outputs, or low-bit quantized ANN tensors in this proposal.
- Do not require floating point. Fixed-point arithmetic, including fixed-point multiply, should be the hardware-oriented baseline.

## Hardware concept

Add a programmable activation engine connected to the existing output accumulator memory.

```text
input tile -> event encoder -> weight/accumulate -> accumulator SRAM
                                                   |
                                                   v
                                    programmable activation engine
                                                   |
                                                   v
                                      output payload + state update
                                                   |
                                                   v
                                      output/ACK queues and router
```

The activation engine is a small vector processor specialized for output groups:

- It is invoked when `input_count == input_requirement` and successor ACK credit is available.
- It reads and updates the output group's accumulator vector as the membrane-potential vector.
- It reads and updates optional per-neuron state or parameter vectors from assigned unified-SRAM ranges.
- It executes a short program bound to the output group.
- It writes exactly one emitted spike bitmap payload.
- It updates per-neuron state such as refractory counter, adaptation current, trace, or per-neuron threshold.
- It clears, resets, leaks, saturates, or otherwise updates the accumulator/membrane potential according to the program rather than always resetting it to zero.

### Main blocks

1. **Activation instruction memory**
   - Small SRAM or ROM, e.g. 256-1024 instructions per core.
   - Programs are stored once and referenced by output groups through a program descriptor.
   - Existing IF behavior is encoded as a standard program.

2. **Program descriptor table**
   - One descriptor per output group.
   - Contains `program_start`, `program_length`, fixed-point mode, unified-SRAM state/parameter base addresses, immediate operands, and an execution budget.
   - Can share one program across many groups with different constants and state bases.

3. **Vector lane datapath**
   - Reuses `NMC_OUTPUT_PARALLELISM` as the number of output neurons processed per cycle or block.
   - Operates over vector registers containing membrane-potential accumulators, per-neuron SRAM values, and immediate constants.
   - Uses fixed-point integer ALU operations: add, subtract, multiply, multiply-accumulate, min, max, compare, shift, saturate, bit-test, select.
   - Fixed-point multiply is required for programmable leakage, adaptation dynamics, and multi-compartment coupling. Non-spiking NN activation functions remain out of scope.

4. **SRAM-to-v-ALU load MUX**
   - The v-ALU reads activation operands through a MUX between unified SRAM and the v-ALU input registers.
   - The MUX can select either one SRAM word, equivalent to one weight-lane datatype, or one packed membrane word made of `P` SRAM words.
   - `P` is the number of weight-lane words needed to represent one membrane/accumulator value, e.g. `P = NMC_ACCUMULATOR_LANES`.
   - `P` must be a factor of the physical SRAM lane count `N`, where `N` is the number of lanes in a memory row or v-ALU access group.
   - Packed membrane values are aligned and non-fragmented: the first accumulator starts at lane 0, and each following accumulator starts at a multiple of `P`.
   - This keeps accumulator loads deterministic: a membrane load is a fixed aligned `P`-word slice rather than a scattered gather.

5. **Unified-SRAM activation state and parameter ranges**
   - Stores per-neuron activation state and per-neuron parameters in the existing unified SRAM address space.
   - Examples: threshold vector, refractory counter vector, adaptation current, output trace, reset vector.
   - Threshold is not stored in SRAM by default; the default IF/LIF threshold is an immediate constant shared by one output group or by the whole core.
   - Per-neuron threshold vectors are allocated in unified SRAM only when the mapping explicitly requests heterogeneous thresholds.
   - Accumulators remain the membrane-potential storage, so a basic IF neuron does not need a separate membrane-voltage state vector.
   - Accessed by the activation engine through validated state/parameter ranges to keep arbitrary memory access out of the program model.

6. **Predicate and payload packer**
   - Converts compare/predicate vectors into the existing spike bitmap payload.
   - There is no `EMIT_VALUE` mode in this proposal; all activation programs emit spikes.

7. **Controller and watchdog**
   - Fetch/decode/execute loop with a bounded instruction count.
   - No unbounded branches. Branches are either forbidden or limited to statically bounded counted loops over vector blocks.
   - Activation stalls output emission until the program completes and the output queue can accept the result.

## Instruction-set approach

Use a compact domain-specific micro-ISA rather than C-like code. The minimum useful instruction classes are:

| Class | Examples | Purpose |
| --- | --- | --- |
| Loads/stores | `LD_ACC`, `ST_ACC`, `LD_WORD`, `ST_WORD`, `LD_IMM` | Move packed membrane accumulators, single unified-SRAM words, and immediates into vector registers. |
| Arithmetic | `ADD`, `SUB`, `MUL`, `MAC`, `ABS`, `NEG`, `SHL`, `SHR`, `SAT` | Fixed-point neuron-state update rules, leakage, adaptation, and compartment coupling. |
| Comparisons | `CMP_GE`, `CMP_GT`, `CMP_EQ` | Generate spike predicates or piecewise-linear conditions. |
| Select | `SEL`, `MIN`, `MAX`, `CLAMP` | Reset behavior, leak bounds, refractory gating, threshold adaptation. |
| Payload | `EMIT_PRED` | Pack a spike bitmap from predicate lanes. |
| Control | `NEXT_BLOCK`, `END` | Iterate over output-vector blocks and terminate. |

The initial ISA can be intentionally small:

- `LD_ACC r0`
- `LD_IMM r1, threshold`
- `CMP_GE p0, r0, r1`
- `EMIT_PRED p0`
- `LD_IMM r2, 0`
- `ST_ACC r2`
- `END`

This exactly reproduces the current IF behavior for the default shared-threshold case. `LD_ACC` uses the MUX's packed `P`-word membrane path, while `LD_IMM` broadcasts the group/core threshold constant to all lanes. If the mapping explicitly requests per-neuron thresholds, `LD_IMM` is replaced by `LD_WORD r1, threshold_base` and thresholds are read through the single-word SRAM path.

## Example supported neuron programs

### 1. Current IF neuron

Behavior:

- Read accumulator as membrane potential.
- Emit spike if accumulator is greater than or equal to the default immediate threshold.
- Clear or reset the accumulator/membrane potential after spike evaluation.

This remains the default program assigned to every output group when no program is specified.

### 2. Leaky integrate-and-fire (LIF)

Behavior:

- Read membrane potential `v` from the accumulator.
- Compute `v = clamp(alpha * v, v_min, v_max)` after accumulation has already added the weighted input current, or use another fixed-point leakage update selected by the program.
- Emit spike if `v >= threshold`.
- Reset accumulator/membrane potential to `reset_value` for spiking lanes, or keep the leaked value for non-spiking lanes.

Hardware requirements:

- Accumulator read/write for `v`.
- Per-group or per-core immediates for threshold, leak coefficient `alpha`, `v_min`, `v_max`, and `reset_value`, or unified-SRAM vectors if any of these are explicitly per-neuron.
- Fixed-point multiply, compare, select, clamp, and accumulator store operations.

### 3. Refractory IF

Behavior:

- Read membrane potential from the accumulator.
- Read per-neuron refractory counters from unified SRAM.
- Suppress spike emission for lanes whose refractory counter is nonzero.
- Decrement active refractory counters.
- For newly spiking lanes, reset membrane potential and set the refractory counter to a per-group immediate or per-neuron SRAM value.

Hardware requirements:

- Accumulator read/write.
- Unified-SRAM load/store for refractory counters.
- Compare, select, decrement, and predicate masking.

### 4. Adaptive-threshold IF

Behavior:

- Read membrane potential from the accumulator.
- Read per-neuron threshold or adaptation state from unified SRAM.
- Emit spike when `v >= threshold`.
- Increase threshold/adaptation state for spiking lanes and decay it for non-spiking lanes.
- Reset or leak the accumulator/membrane potential.

Hardware requirements:

- Accumulator read/write.
- Unified-SRAM load/store for threshold or adaptation vectors.
- Per-group immediate decay/increment constants or per-neuron SRAM parameter vectors.
- Fixed-point multiply for adaptation decay and threshold/current scaling.

### 5. Multi-compartment spiking neuron

Behavior:

- Treat the primary accumulator as the soma membrane potential.
- Store additional compartment states, such as dendritic or adaptation compartments, in unified SRAM.
- Apply fixed-point coupling terms, for example `v_soma += g_dend * (v_dend - v_soma)`.
- Update compartment states with their own leakage coefficients.
- Emit a spike from the soma predicate and update/reset affected compartments.

Hardware requirements:

- Accumulator read/write for soma voltage.
- Unified-SRAM load/store for additional compartment voltages and coupling parameters.
- Fixed-point multiply and multiply-accumulate for leakage and inter-compartment coupling.
- Predicate generation and selective reset/update.

## Interaction with the existing core protocol

The programmable activation stage should be inserted inside output activation, not input accumulation.

Existing order:

1. Input tile arrives.
2. Sparse events accumulate weighted sums into output accumulators.
3. Output group becomes input-ready.
4. Successor ACKs confirm previous output was consumed.
5. Fixed threshold activation emits one output tile.
6. Predecessor ACKs are generated and synchronization window resets.

Proposed order:

1. Input tile arrives.
2. Sparse events accumulate weighted sums into output accumulators.
3. Output group becomes input-ready.
4. Successor ACKs confirm previous output was consumed.
5. Activation engine executes the output group's bound program.
6. Program emits one spike bitmap and updates accumulator membrane potential plus any unified-SRAM state.
7. Output tile and predecessor ACKs are generated.
8. Synchronization window resets.

The output group should not reset `input_count`, `ack_count`, recurrent ACK state, or predecessor ACK state until the activation program has completed and the output tile has been accepted. This preserves the one-in-flight ordering model.

## Simulator model changes

### Public data model

Add fixed hardware limits:

- `NMC_MAX_ACTIVATION_PROGRAMS`
- `NMC_MAX_ACTIVATION_INSTRUCTIONS`
- `NMC_MAX_ACTIVATION_REGISTERS`
- `NMC_MAX_ACTIVATION_SRAM_RANGES`
- `NMC_MAX_ACTIVATION_IMMEDIATES`
- `NMC_MAX_ACTIVATION_STEPS`
- `NMC_ACTIVATION_MUL_LATENCY`
- `NMC_ACTIVATION_SRAM_LANES`
- `NMC_ACTIVATION_MEMBRANE_WORDS`

Add types:

- `NmcActivationOpcode`
- `NmcActivationInstruction`
- `NmcActivationProgramDescriptor`
- `NmcActivationSramRange`
- `NmcActivationProgramMappingSpec`

Extend each output group with:

- `activation_program_index`
- optional validated unified-SRAM state/parameter ranges
- optional per-group immediate operands
- accumulator/membrane packing metadata, where each membrane uses `P = NMC_ACTIVATION_MEMBRANE_WORDS` SRAM words
- activation status/counters for instrumentation

The existing `NmcNeuron.threshold` field should become a compatibility path for mappings that already provide threshold arrays. The target hardware model should treat threshold as an immediate by default: one constant per output group, or one core-wide default threshold shared by groups that do not override it. Per-neuron threshold vectors should be stored in unified SRAM only when heterogeneous thresholds are explicitly requested.

### Runtime model

Replace the hard-coded threshold loop in `nmc_core_activate_output_group()` with:

1. Validate output readiness and destination credit.
2. Validate accumulator, unified-SRAM state/parameter, immediate, and program ranges.
3. Invoke `nmc_core_execute_activation_program()`.
4. If execution succeeds and produces one spike bitmap, enqueue the output tile.
5. Reset group synchronization state.

`nmc_core_execute_activation_program()` should:

- Execute one output-vector block at a time, using `NMC_OUTPUT_PARALLELISM` lanes.
- Fetch instructions from the activation instruction array.
- Decode operands and validate register, accumulator, immediate, and unified-SRAM accesses.
- Model SRAM-to-v-ALU MUX selection for either a single SRAM word or an aligned packed `P`-word membrane value.
- Execute fixed-point multiply and multiply-accumulate with explicit rounding, shifting, and saturation semantics.
- Track cycles and instruction count.
- Reject programs that exceed their maximum step budget.
- Produce deterministic success/failure without undefined behavior.

Suggested counters:

- `last_activation_instruction_count`
- `last_activation_cycles`
- `total_activation_cycles`
- `last_activation_output_width`
- `activation_fault_count`

### Mapping model

Mapping should support three levels:

1. **Implicit default**: if no activation program is supplied, use the built-in IF program with an immediate threshold. Existing threshold arrays are folded into a per-group immediate when all entries are equal, or copied into unified SRAM only when they are heterogeneous.
2. **Named built-in programs**: `IF`, `LIF`, `REFRACTORY_IF`, `ADAPTIVE_THRESHOLD_IF`, and `MULTI_COMPARTMENT_IF` can map to predefined microcode templates.
3. **Explicit microprogram**: advanced callers provide instruction arrays, per-group immediates, and unified-SRAM state/parameter layout.

For graph-level mapping, each logical group should be able to specify:

- activation program kind or program id
- fixed-point format
- membrane packing width `P`, normally derived from the accumulator datatype and weight-lane datatype
- threshold scope: core-wide immediate, per-group immediate, or explicit per-neuron SRAM vector
- per-group immediate constants
- per-neuron state and parameter initialization in unified SRAM
- output payload type, restricted to spike bitmap

The mapper must allocate unified-SRAM ranges for activation state and per-neuron parameters similarly to how it already allocates weight and accumulator offsets. Accumulator/membrane ranges must be aligned so the first accumulator starts at lane 0 and every membrane starts at a multiple of `P`; no fragmented accumulator packing is allowed. Thresholds and other values that do not vary by neuron should be kept as core-wide or per-group instruction immediates to avoid wasting SRAM bandwidth and capacity.

## Hardware timing model

The simulator should remain architecture-stage rather than cycle-accurate RTL, but activation latency should be visible.

A reasonable first model:

- One vector instruction executes per activation cycle per `NMC_OUTPUT_PARALLELISM` lanes.
- A v-ALU load can transfer either one SRAM word or one aligned `P`-word membrane value through the SRAM-to-v-ALU MUX.
- Fixed-point multiply and multiply-accumulate may have a higher configured latency than simple ALU instructions.
- A group of width `M` requires `ceil(M / NMC_OUTPUT_PARALLELISM)` vector blocks.
- Program cycle cost is approximately `program_instruction_count * vector_blocks`, with optional extra costs for MUX selections and unified-SRAM state/parameter loads and stores.
- Output emission happens only after activation completes.

This gives a simple hardware-relevant latency estimate:

```text
activation_cycles = vector_blocks * (simple_instruction_cycles + mul_instruction_cycles * mul_latency) + memory_penalty_cycles
```

Later refinements can model pipeline stages, unified-SRAM port conflicts, instruction-cache conflicts, or shared activation-engine arbitration across output groups.

## Safety and validation

Programmability should not make the simulator behave like arbitrary software. Validation should reject invalid mappings before runtime when possible:

- Program length must fit instruction memory.
- Register operands must be in range.
- Unified-SRAM state/parameter ranges and accumulator ranges must fit fixed SRAMs.
- `P` must be a factor of the SRAM/v-ALU lane count `N`.
- Accumulator/membrane starts must be aligned to `P`, with the first accumulator in a packed row starting at lane 0.
- Programs may load a single word or one aligned `P`-word membrane value, but may not issue arbitrary scattered loads.
- Immediate operands must fit their encoded width.
- Multiply instructions must declare their fixed-point shift/round/saturation mode or use the program descriptor's default mode.
- Program must end with `END`.
- Program must emit exactly one valid spike bitmap.
- Payload width must match the output group width.
- Programs must not write outside their assigned unified-SRAM state/parameter ranges or accumulator range.
- Execution steps must not exceed a configured watchdog limit.
- Unsupported opcodes or any non-spike payload mode fail configuration.

Runtime faults should return `false` from the activation path and increment a fault counter, matching the existing hardware-like bounded-array failure style.

## Phased implementation plan

### Phase 1: Microprogrammed IF parity

- Add activation instruction/program storage to `NmcCore`.
- Add one default built-in IF program.
- Modify output activation to execute the program instead of directly comparing thresholds.
- Keep output payloads as bitmaps.
- Treat accumulators as membrane potential and clear/reset them through `ST_ACC` in the program.
- Add activation cycle counters.
- Validate that existing tests pass unchanged.

### Phase 2: Fixed-point multiply and LIF

- Add activation unified-SRAM range descriptors.
- Add `LD_WORD`, `ST_WORD`, `MUL`, `MAC`, `SEL`, `CLAMP`, and immediate operands for leak/reset constants.
- Add SRAM-to-v-ALU MUX modeling for single-word loads and aligned `P`-word membrane loads.
- Define fixed-point multiply shift, rounding, and saturation behavior.
- Add mapping support for optional per-neuron threshold/state initialization in unified SRAM when heterogeneous thresholds or state are explicitly requested.
- Add LIF tests covering accumulator-as-membrane behavior, reset-on-spike, leakage, and recurrent synchronization.

### Phase 3: Stateful spiking programs

- Add refractory IF and adaptive-threshold IF built-ins.
- Add unified-SRAM state allocation for refractory counters, adaptation variables, and per-neuron thresholds.
- Add tests for refractory suppression, multiply-based adaptation updates, and SRAM-range validation.

### Phase 4: Multi-compartment spiking programs

- Add multi-compartment IF built-ins that store extra compartment voltages in unified SRAM.
- Add coupling parameters as immediates or per-neuron unified-SRAM vectors.
- Add tests for soma/dendrite coupling, leakage, selective reset, aligned `P`-word loads, and activation-cycle accounting for multiply-heavy programs.

### Phase 5: Hardware scheduling refinement

- Model instruction fetch, unified-SRAM port conflicts, multiply latency/throughput, vector-lane utilization, and activation-engine sharing.
- Add optional detailed counters for instruction mix and memory stalls.
- Use these counters to compare fixed IF hardware versus programmable v-ALU overhead.

## Recommended first simulator interface

Start with a compact API that keeps existing users working:

- Existing `nmc_core_add_output_group()` continues to assign the default IF program.
- New configuration calls add activation programs and bind them to output groups.
- Existing `NmcOutputGroupMappingSpec.thresholds` remains valid. Uniform threshold arrays become per-group immediates; heterogeneous threshold arrays are copied into unified SRAM only when explicitly supported by the selected program.
- Optional fields in the mapping spec select program kind, per-group immediates, and unified-SRAM state/parameter initialization.

Suggested conceptual API additions:

- `nmc_core_add_activation_program(core, instructions, instruction_count, descriptor)`
- `nmc_core_bind_output_activation_program(core, output_index, program_index)`
- `nmc_core_bind_activation_sram_range(core, output_index, range_index, base, count)`
- `nmc_core_set_activation_immediate(core, output_index, immediate_index, value)`
- `nmc_core_set_activation_membrane_packing(core, output_index, words_per_membrane)`
- `nmc_core_configure_mapping()` automatically installs built-ins when needed.

## Key design trade-offs

### Per-core shared v-ALU vs per-output dedicated ALU

A shared per-core activation engine is recommended first. It is smaller, easier to model, and matches the existing serialized output activation path. Dedicated per-output activation units increase area and are unnecessary unless multiple output groups must activate concurrently.

### Micro-ISA vs lookup-table activation

A lookup table is simple for small scalar non-linearities but scales poorly for stateful neurons and vector post-processing. A micro-ISA is more flexible while still bounded and hardware-like.

### Spike-only vs value outputs

Spike-only output is a hard scope boundary for this proposal. Value payloads are useful for ANN-style accelerators, but they require a different packet-format and input-path design and should be handled as a separate architecture proposal.

### Unified SRAM vs separate activation state SRAM

Using unified SRAM for activation state and per-neuron parameters is recommended because it matches the current memory direction and avoids adding another logical memory block to the simulator. The cost is stricter address validation and more explicit scheduling of SRAM reads/writes during activation. Per-group parameters should remain immediates whenever possible to reduce unified-SRAM pressure.

## Open questions

- Are activation programs loaded at configuration time only, or can they be updated at runtime between inference phases?
- Should `NmcNeuron.threshold` be removed after compatibility migration, with thresholds represented only as core/group immediates or explicit per-neuron SRAM vectors?
- Should multiple output groups share one activation program with different constants, or should each group own a fully expanded program image?
- What fixed-point formats are required for the target models?
- Should multiply be fully lane-parallel, time-multiplexed across lanes, or implemented as a lower-throughput shared unit?
- Should `P` be fixed globally from accumulator width, or can different output groups select different membrane widths if they still divide `N` and remain aligned?
- Should activation faults block the core, drop the output, or expose a recoverable error state?

## Recommendation

Implement Phase 1 as a minimal programmable activation engine that exactly reproduces the current IF behavior through microcode. This validates the abstraction without changing external behavior. Then add fixed-point multiply, unified-SRAM activation parameters, and LIF support as Phase 2. Keep packet payloads as spike bitmaps by design.

This path gives a clean hardware story: the core remains a sparse event-driven accumulator followed by a bounded programmable vector activation stage. The accumulator file is the membrane-potential state, unified SRAM holds any additional per-neuron state or parameters, program immediates hold default core/group constants such as threshold and leakage, and the activation ALU includes fixed-point multiply for leakage, adaptation, and compartment coupling. The v-ALU reads operands through an SRAM MUX that selects either one weight-sized word or one aligned `P`-word membrane value, where `P` divides the SRAM lane count `N` and packed accumulators are non-fragmented. The simulator remains close to hardware by modeling instruction memory, unified-SRAM ranges, vector width, MUX load granularity, multiply latency, execution budgets, and activation cycle counters instead of executing arbitrary host-side activation callbacks.
