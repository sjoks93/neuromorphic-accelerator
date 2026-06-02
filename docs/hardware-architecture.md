# Hardware Architecture Figures

This page summarizes the modeled hardware blocks in the simulator. The diagrams use Mermaid so they render directly in Markdown viewers that support Mermaid, including GitHub and VS Code Markdown preview.

## System-level tile and mesh organization

Each accelerator core is paired with an independent mesh router. Cores emit grouped spike tiles and ACK messages into the local router. The router performs dimension-order routing and splits multicast traffic by next-hop port.

```mermaid
flowchart LR
    Host[Graph-level mapping and input source]

    subgraph Tile00[Tile / router coordinate 0,0]
        Core00[NmcCore 0]
        Router00[Mesh router 0,0]
        Core00 <--> Router00
    end

    subgraph Tile10[Tile / router coordinate 1,0]
        Core10[NmcCore 1]
        Router10[Mesh router 1,0]
        Core10 <--> Router10
    end

    subgraph Tile01[Tile / router coordinate 0,1]
        Core01[NmcCore 2]
        Router01[Mesh router 0,1]
        Core01 <--> Router01
    end

    subgraph Tile11[Tile / router coordinate 1,1]
        Core11[NmcCore 3]
        Router11[Mesh router 1,1]
        Core11 <--> Router11
    end

    Host -->|external grouped input tile| Core00
    Router00 <-->|E/W mesh link| Router10
    Router01 <-->|E/W mesh link| Router11
    Router00 <-->|N/S mesh link| Router01
    Router10 <-->|N/S mesh link| Router11

    Core00 -->|multicast spike tile| Router00
    Router00 -->|split fanout by next hop| Router10
    Router10 -->|local delivery as input tile| Core10
    Core10 -->|routed ACK credit| Router10
    Router10 --> Router00
    Router00 -->|local delivery as ACK| Core00
```

## Single-core datapath

The core datapath is event-driven. An incoming bitmap selects a local input group. The input LUT expands the group into one or more output-group fanout entries, the encoder extracts sparse events, and each event selects an output-wide row from unified memory. Accumulation, activation, routing metadata lookup, output emission, and predecessor ACK generation are all controlled by hardware-like tables.

```mermaid
flowchart TB
    InTile[Local input tile\ninput group + width + bitmap]
    AckIn[Local ACK message\nACK group index]

    subgraph Tables[SRAM-backed mapping tables]
        InputGroup[Input group table\nstart entries]
        PairLUT[Input/output pair LUT\noutput group + weight offset]
        AckGroup[ACK group table\nstart entries]
        AckPairLUT[ACK/output pair LUT\noutput group credit update]
        OutGroup[Output group table\ninput_count, ack_count, activation binding]
        AccLUT[Accumulator LUT\nper-output base]
        RouteLUT[Output route LUT\nsuccessors + predecessors]
    end

    subgraph Compute[Input and sparse compute pipeline]
        Encoder[Bitmap-to-event encoder\nwindow skip + parallel lanes]
        WeightRead[Input-bank row read\noutput-wide weight slice]
        AdderTree[Adder-tree reduction\nper output lane]
        AccMux[Accumulator lane MUX\npacked 32-bit values]
    end

    subgraph Unified[Unified signed 16-bit SRAM]
        Weights[Banked weights\ninput-major, output-wide]
        Accums[Packed accumulators\nmembrane potential]
        ActState[Optional activation\nstate / parameter vectors]
    end

    subgraph Activation[Output activation pipeline]
        Ready[Ready and flow-control gate\ninputs complete + ACK credit]
        VALU[Programmable spike-only v-ALU]
        Packer[Predicate bitmap packer]
    end

    subgraph Egress[Router-facing queues]
        OutQ[Output tile queue]
        AckQ[Predecessor ACK queue]
    end

    InTile --> InputGroup --> PairLUT --> Encoder --> WeightRead --> AdderTree --> AccMux
    PairLUT --> OutGroup
    WeightRead --> Weights
    AccMux <--> Accums
    AccMux --> OutGroup

    AckIn --> AckGroup --> AckPairLUT --> OutGroup
    OutGroup --> Ready
    AccLUT --> AccMux
    Ready --> VALU
    VALU <--> Accums
    VALU <--> ActState
    VALU --> Packer --> OutQ
    RouteLUT --> OutQ
    RouteLUT --> AckQ
    Ready --> AckQ
```

## Unified-memory layout

The simulator models one unified signed 16-bit memory space. Weight matrices are transposed from caller-facing output-major form into input-bank-major, output-wide rows. Accumulators use packed lanes, so one logical membrane value occupies `NMC_ACCUMULATOR_LANES` adjacent 16-bit words. Optional per-neuron activation state or parameters are allocated only when a program requires them.

```mermaid
flowchart LR
    subgraph Caller[Mapper input format]
        OM["Output-major weights\nweights[output][input]"]
    end

    subgraph SRAM[Unified memory address space]
        W0[Weight slice for edge A\ninput 0 row: all output channels]
        W1[Weight slice for edge A\ninput 1 row: all output channels]
        WN[Weight slice for edge A\ninput N-1 row: all output channels]
        A0[Output group accumulator slice\npacked membrane lanes]
        S0[Optional activation state\nthresholds, refractory counters, traces]
    end

    OM -->|transpose during mapping| W0
    W0 --> W1 --> WN --> A0 --> S0

    Event[Sparse input event index] -->|selects one input bank row| W1
    W1 -->|output-wide row| Reduce[Reduction and accumulator update]
    A0 <--> Reduce
```

## LUT range generation pattern

Most table lookups use start-plus-terminal indirection. A group entry stores only the inclusive start address. The exclusive end address comes from the next group entry, including the terminal sentinel at index `group_count`.

```mermaid
flowchart TB
    GroupI[Group entry i\nvalid + start]
    GroupNext[Group entry i + 1\nterminal or next start]
    Range["Derived stage-2 range\n[start_i, start_i_plus_1)"]
    Entry0[Stage-2 LUT entry start_i]
    Entry1[Stage-2 LUT entry start_i + 1]
    EntryN[Stage-2 LUT entry start_i_plus_1 - 1]

    GroupI -->|start_i| Range
    GroupNext -->|exclusive end| Range
    Range --> Entry0
    Range --> Entry1
    Range --> EntryN
```

This structure is used by input-to-output fanout tables, ACK-to-output credit tables, and output route metadata. It mirrors simple hardware address generation and avoids dynamic per-group allocation metadata.

## Natural-step flow control

Output activation is gated by both input readiness and successor ACK credit. Feed-forward outputs emit predecessor ACKs after activation succeeds. Recurrent predecessor ACKs can be emitted when an input window completes so cyclic graphs do not deadlock while still preserving one-tile ordering.

```mermaid
sequenceDiagram
    participant Src as Source output group
    participant R1 as Source router
    participant Dst as Destination core input group
    participant Out as Destination output group
    participant R2 as Destination router

    Src->>R1: Emit grouped spike tile
    R1->>Dst: Deliver local input tile after mesh routing
    Dst->>Out: Accumulate sparse events and increment input_count
    Out->>Out: Wait until input_count == input_requirement
    Out->>Out: Wait until ack_count covers successor routes
    Out->>R2: Activate and emit successor spike tile
    Out->>R2: Emit predecessor ACK credit
    R2->>Src: Route ACK back to predecessor ACK group
    Src->>Src: ACK LUT increments successor credit
```
