# CS144 Lab 0: Technical Archive & Writeup (Networking Warmup)

## 1. Scope & Objectives

This lab establishes the foundational building blocks of the Sponge networking protocol stack, encompassing two primary software artifacts:
1. **`webget` Client**: An HTTP/1.1-compliant client implemented atop the POSIX stream-socket abstraction (`TCPSocket`), handling automated request serialization and exhaustive stream response reception.
2. **`ByteStream`**: An in-memory, flow-controlled, finite byte stream supporting asymmetric reader/writer interfaces with strict capacity bounds, serving as the central FIFO buffer throughout subsequent transport-layer layers.

---

## 2. ByteStream Architecture & Design

### 2.1 Memory Layout & Lazy Compaction Strategy

The `ByteStream` class utilizes a contiguous `std::string` buffer alongside an explicit head offset `_buffer_start` designating the boundary of unread data.

```
Physical Memory Layout:
[   Dead Prefix (Consumed)   ][      Active Data (Unread)      ]
0                       _buffer_start                       _buffer.size()
|<--- Reclaimable Space ---->|<---------- buffer_size() -------->|
```

In scenarios characterized by fine-grained pops (e.g., character-by-character consumption of a multi-megabyte stream), executing `std::string::erase(0, len)` upon every invocation induces quadratic $O(N^2)$ execution overhead due to continuous memory reallocation and forward element shifting.

To mitigate this bottleneck, a **thresholded lazy compaction mechanism** is adopted:

```cpp
namespace {
constexpr size_t COMPACT_THRESHOLD = 4096;
}

void ByteStream::pop_output(const size_t len) {
    const size_t popped = std::min(len, buffer_size());
    _buffer_start += popped;
    _bytes_read += popped;

    if (_buffer_start == _buffer.size()) {
        _buffer.clear();
        _buffer_start = 0;
    } else if (_buffer_start >= COMPACT_THRESHOLD && _buffer_start * 2 >= _buffer.size()) {
        _buffer.erase(0, _buffer_start);
        _buffer_start = 0;
    }
}
```

- **Amortized Analysis**: Prefix truncation occurs exclusively when the invalid prefix surpasses $4096$ bytes and comprises at least half of the underlying capacity. Because a linear shift of $O(M)$ elements is preceded by at least $\frac{M}{2}$ independent pop operations, the amortized runtime per `pop_output` invocation remains strictly $O(1)$.
- **Full Drain Optimization**: When the buffer is completely exhausted (`_buffer_start == _buffer.size()`), storage is reset via `clear()`, zeroing the logical head offset without reallocating underlying capacity.

### 2.2 Invariant Accounting & Flow Control

The flow-control contract dictates that total unread buffered content must not exceed the preconfigured limit `_capacity`:

$$\text{buffer\_size}() = \_buffer.\text{size}() - \_buffer\_start \le \_capacity$$

$$\text{remaining\_capacity}() = \_capacity - \text{buffer\_size}()$$

Incoming data is clamped to available capacity prior to insertion:

$$\text{accepted\_bytes} = \min(\text{data.size}(), \text{remaining\_capacity}())$$

Global telemetry is decoupled from volatile buffer states via two monotonically incrementing 64-bit integer counters:
- `_bytes_written`: Cumulative payload bytes accepted from the writer.
- `_bytes_read`: Cumulative payload bytes dispatched to and acknowledged by the reader.

### 2.3 Lifecycle & State Machine Semantics

The lifecycle of a stream conforms to a decoupled two-stage termination model:

```
+------------------------------------+
| Active State:                      |
| input_ended: false,  eof: false    |
+-----------------+------------------+
                  | Writer calls end_input()
                  v
+------------------------------------+
| Half-Closed State:                 |
| input_ended: true,   eof: false    |
+-----------------+------------------+
                  | Reader drains remaining bytes (buffer_empty == true)
                  v
+------------------------------------+
| Terminated State:                  |
| input_ended: true,   eof: true     |
+------------------------------------+
```

- `input_ended()` models writer intent: evaluates to `true` immediately following an explicit `end_input()` call.
- `eof()` models reader consumption: evaluates to `true` if and only if both the writer has terminated input and all buffered data has been consumed:
  $$\text{eof}() \iff \text{input\_ended}() \land \text{buffer\_empty}()$$

---

## 3. Network Client Implementation (`webget`)

### 3.1 Framing & Wire Specifications

The `apps/webget.cc` utility structures an HTTP/1.1 request conforming to RFC specifications:
- **Delimiter Invariance**: Every header line terminates with strict CRLF (`\r\n`).
- **Framing Closure**: A standalone blank line (`\r\n\r\n`) signals the terminus of the HTTP request header block.
- **Connection Teardown Directive**: Specifying `Connection: close\r\n` commands the remote HTTP daemon to initiate an active TCP FIN teardown upon transmitting the response payload, delivering an observable EOF marker to the local reader.

### 3.2 Stream Ingestion Loop

Because TCP preserves no application-layer framing boundaries, incoming responses may arrive fragmented into arbitrary segment lengths. The client guarantees exhaustive receipt by streaming through a persistent ingestion loop bounded exclusively by socket EOF:

```cpp
while (not socket.eof()) {
    std::cout << socket.read();
}
```

---

## 4. Verification & Test Suite Execution

All target test suites execute successfully with zero failures:
- `t_byte_stream_construction`: Validates initial invariant constraints.
- `t_byte_stream_one_write` / `two_writes`: Validates capacity-aware slicing and append logic.
- `t_byte_stream_capacity`: Validates saturation handling and rejection of excess data.
- `t_byte_stream_many_writes`: Stresses random read/write interleaving and verifies amortized $O(1)$ compaction.
- `t_address_dt` / `t_parser_dt` / `t_socket_dt`: Validates underlying OS socket wrappers.
