# CS144 Lab 1: Technical Archive & Writeup (Stitching Substrings into a Byte Stream)

## 1. Scope & Objectives

This lab focuses on resolving the packet reordering, duplication, and fragmentation challenges inherent to best-effort packet-switched networks. It delivers the implementation of the **`StreamReassembler`**, the core component within the TCP receiver responsible for reconstructing a contiguous byte stream from discrete, out-of-order substrings while operating under strict memory capacity bounds.

---

## 2. Architecture & Capacity Invariants

### 2.1 Receiver Memory Contract

To prevent denial-of-service vulnerabilities through unbounded buffer growth under adversarial reordering, the receiver imposes an absolute upper bound `_capacity` across all held memory:

$$\text{buffer\_size}() + \text{unassembled\_bytes}() \le \_capacity$$

```
Receiver Memory Layout:
[ Consumed Data (Popped) ] | [ Assembled in ByteStream ] | [ Unassembled Substrings ] | [ Unacceptable Domain ]
                           |<------- buffer_size() ----->|<---- unassembled_bytes() -->|
                           |<----------------------- _capacity ------------------------>|
                           first_unread                  first_unassembled              limit
```

- **`first_unassembled`**: The lowest sequence index awaited by the `ByteStream`.
- **`limit`**: The absolute sequence index marking the boundary of acceptable data. Any byte at or beyond `limit` is discarded:

$$\text{limit} = \text{first\_unassembled} + (\_capacity - \_output.\text{buffer\_size}())$$

### 2.2 Invariant State Accounting

Because bytes are appended to `_output` strictly in stream order starting at sequence 0:

$$\text{first\_unassembled} \equiv \_output.\text{bytes\_written}()$$

This identity eliminates redundant cursor state. The member `_stored_bytes` is maintained incrementally during interval insertions and extractions, ensuring $O(1)$ query complexity for `unassembled_bytes()`.

---

## 3. Data Structure & Algorithmic Design

### 3.1 Architectural Trade-Off: Byte-Level vs. Interval Mapping

Two candidate architectures were evaluated:

1. **Byte-Level Map (`std::map<uint64_t, char>`)**:
   - *Characteristics*: Trivial deduplication via map keys.
   - *Bottlenecks*: Red-black tree nodes introduce significant dynamic allocation overhead (32–48 bytes per byte of payload). Pervasive cache misses degrade throughput under high-bandwidth workloads.
2. **Interval Coalescing Map (`std::map<uint64_t, std::string>`)**:
   - *Characteristics*: Disjoint substrings stored as contiguous strings, keyed by initial absolute index.
   - *Advantages*: Enforces strict pairwise disjointness and non-adjacency, maximizing spatial locality and reducing node allocations by multiple orders of magnitude.

### 3.2 Interval Coalescing Algorithm

The internal container `_stored` enforces a strict invariant: **adjacent entries are always separated by at least one missing byte (hole)**.

When a substring arrives and is clipped to the acceptable window $[start, end)$, the coalescing routine executes as follows:

```cpp
void StreamReassembler::merge_pending_bytes(const string &data,
                                            const uint64_t index,
                                            const uint64_t start,
                                            const uint64_t end) {
    // 1. Locate the first candidate interval that may overlap or touch [start, end)
    auto first = _stored.lower_bound(start);
    if (first != _stored.begin()) {
        const auto previous = prev(first);
        if (previous->first + previous->second.size() >= start) {
            first = previous;
        }
    }

    // 2. Early exit: incoming range is already entirely subsumed
    if (first != _stored.end() and first->first <= start and end <= first->first + first->second.size()) {
        return;
    }

    // 3. Scan and coalesce overlapping and adjacent intervals
    auto last = first;
    uint64_t merged_start = start;
    uint64_t merged_end = end;
    while (last != _stored.end() and last->first <= end) {
        merged_start = min(merged_start, last->first);
        merged_end = max(merged_end, last->first + last->second.size());
        ++last;
    }

    // 4. Construct unified contiguous span
    string merged(merged_end - merged_start, '\0');
    for (auto known = first; known != last; ++known) {
        merged.replace(known->first - merged_start, known->second.size(), known->second);
        _stored_bytes -= known->second.size();
    }
    merged.replace(start - merged_start, end - start, data, start - index, end - start);

    // 5. Replace node range
    _stored.erase(first, last);
    _stored_bytes += merged.size();
    _stored.emplace(merged_start, move(merged));
}
```

- **Backward Inspection (`prev(first)`)**: Standard `lower_bound(start)` returns the first interval with `key >= start`. If an earlier interval starts before `start` but extends into or touches it, inspecting `prev(first)` ensures proper coalescing.
- **Neighbor Merging (`last->first <= end`)**: The inclusion of equality ensures contiguous contiguous intervals (e.g., $[0, 5)$ and $[5, 10)$) fuse into a single interval, preventing map bloat.
- **In-Place Segment Overwrite**: A single contiguous allocation spans $[merged\_start, merged\_end)$, populating existing and novel data without repeated reallocation.

### 3.3 Drain Routine & EOF Invariant

- **Contiguous Drainage**: Following insertion, intervals matching `first_unassembled` are popped and written to `_output` in order.
- **Truncation-Aware EOF**: An `eof` flag is recorded **only if the incoming segment was accepted in full without tail truncation**. Discarding any portion of an EOF-bearing segment invalidates knowledge of the true stream terminus.

---

## 4. Benchmark Performance & Evaluation

Benchmarking was conducted on a Linux x86-64 platform over 64 MiB payload workloads:

| Workload Configuration (64 MiB) | Byte-Level Map | Interval Coalescing Map | Speedup |
| :--- | :--- | :--- | :--- |
| **In-Order, 1460 B segments** | 0.216 Gbit/s | **8.742 Gbit/s** | **40.5x** |
| **Reordered, 1460 B segments** | 0.068 Gbit/s | **1.632 Gbit/s** | **24.0x** |
| **Reordered, 64 B segments** | 0.039 Gbit/s | **0.422 Gbit/s** | **10.8x** |

The interval coalescing architecture comfortably exceeds the laboratory performance baseline of 0.1 Gbit/s under arbitrary reordering, validating the computational efficiency of bulk interval updates over individual element node insertions.

---

## 5. Verification & Differential Testing

Correctness was verified using both standard test suites and a custom randomized **Differential Fuzzing Harness**:
- **Oracle Comparison**: The interval-based implementation was verified against the byte-by-byte map implementation as an axiomatic baseline.
- **Fuzzing Parameters**: Ranging from 1-byte capacities up to 64 KiB, randomized segment offsets, varying payload lengths, overlapping slices, and asynchronous EOF assertions were applied.
- **Results**: Across 7 configurations totaling **140,000 trials**, the internal state, buffer sizes, written byte totals, and stream flags exhibited 100% equivalence with zero deviations.

---

## 6. Summary

The `StreamReassembler` guarantees deterministic reassembly under arbitrary network permutation while adhering to bounded memory guarantees. All automated tests (`t_strm_reassem_*`) pass successfully.
