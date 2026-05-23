
## architecture

```
┌────────────────────────────────────────────────┐                                                                   
│              EXCHANGE (Process 1)              │                                                                   
│                                                │                                                                   
│     [CREATOR]───────HEAP────────►[DISPATCHER]──┼─────────┐ 
│                                                │         │   
│   [ADVERSARIES]────────────────────────────────┼──────┐  │  give each producer its own spsc ring,                       
│                                                │     ┌▼──▼┐ and k-way merge on the enqueue timestamp.
│ [MATCHING ENGINE]◄─────────────────────────────┼─────┤SPSC◄─────┐                                                  
│         │                                      │     └────┘     │                                                  
└─────────┼──────────────────────────────────────┘    EXCHANGE    │                                                  
       ┌──▼─┐                                                     │                                                  
       │SPSC│ENGINE                                               │                                                  
       └──┬─┘                                                     │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │ RUNTIME ENGINE (Process 2)           │                │                                                  
│         │                                      │ DASHBOARD      │                                                  
│         ▼         ┌───────┐                    │ ┌────┐         │                                                  
│    [HOT PATH]─────►Seqlock┼────►[SNAPSHOTTER]──┼─►SPSC│         │                                                  
│         │         └───────┘                    │ └──┬─┘         │                                                  
└─────────┼──────────────────────────────────────┘    │           │                                                  
          │                                           │           │                                                  
       ┌──▼─┐                      ┌──────────────────┴──┐        │                                                  
       │SPSC│STRATEGY              │DASHBOARD (Process 4)│        │                                                  
       └──┬─┘                      └─────────────────────┘        │                                                  
          │                                                       │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │    STRATEGY (Process 3)              │                │                                                  
│         ▼   Dispatch to main or offload        │                │                                                  
│   [DISPATCHER]───────────────────────►[MAIN]   │                │                                                  
│         │                                │     │                │                                                  
│         ▼                                │     │                │                                                  
│   [ACCELERATOR]──────────────────────────┴─────┼────────────────┘                                                  
│                                                │                                                                   
└────────────────────────────────────────────────┘                                                                   
```

---

## shared memory layout

```
SharedMemoryBlock  (/engine_shm_mvp, 16MB, versioned)
  ├── ShmHeader              magic + version check (fail-fast on mismatch)
  ├── ExchangeInputArray     producers → matching engine   2 × SPSC, 4096 × 64-byte slots
  ├── EventRingBuffer        matching engine → runtime     SPSC, 8192 × 64-byte slots
  ├── StrategyRingBuffer     runtime → strategy            SPSC, 4096 × StrategyTick slots
  ├── BookSnapshot           runtime → dashboard           seqlock-protected, written at 30 Hz
  ├── AcceleratorBatch       runtime → accelerator         64-tick burst batch + metadata
  └── AcceleratorSignal      accelerator → runtime         EMA result, signal action, routing flag
```
