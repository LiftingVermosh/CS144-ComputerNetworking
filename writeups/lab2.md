# CS144 Lab 2: Technical Archive & Writeup (The TCP Receiver)

## 1. Scope & Objectives

This lab implements the receiving core of the TCP transport protocol, comprising two primary software components:
1. **`WrappingInt32`**: A circular 32-bit sequence number abstraction handling bidirectional conversions (`wrap` and `unwrap`) between 32-bit wire sequence numbers (`seqno`) and 64-bit absolute sequence numbers (`absolute seqno`) in $O(1)$ constant time.
2. **`TCPReceiver`**: The connection state machine and sliding-window controller that accepts incoming `TCPSegment` payloads, tracks control flags (`SYN`, `FIN`), enforces receiver window bounds, delivers in-window bytes to `StreamReassembler`, and synthesizes feedback (`ackno`, `window_size`).

---

## 2. Sequence Number Systems

### 2.1 Coordinate Systems & Invariant Mapping

The protocol stack manages three distinct sequence number systems:

| Dimension | Sequence Number (Seqno) | Absolute Sequence Number | Stream Index |
| :--- | :--- | :--- | :--- |
| **Type** | `WrappingInt32` (32-bit unsigned) | `uint64_t` (64-bit unsigned) | `uint64_t` (64-bit unsigned) |
| **Origin** | Random `ISN` | `0` (SYN entry) | `0` (First byte of payload) |
| **Wrapping** | Wraps modulo $2^{32}$ (~4 GiB) | Monotonic, non-wrapping | Monotonic, non-wrapping |
| **Flag Ingestion** | Ingests SYN & FIN (1 slot each) | Ingests SYN & FIN (1 slot each) | **Excludes** SYN & FIN (payload only) |
| **Scope** | Wire protocol header field | Receiver/Sender internal FSM | `StreamReassembler` & `ByteStream` |

```
Coordinate Space Alignments:
Seqno:           [  ISN  ]   [ ISN + 1 ]   [ ISN + 2 ] ... [ ISN + N ]   [ ISN + N + 1 ]
                     |             |             |               |               |
Absolute Seqno:      0             1             2               N              N + 1
                     |             |             |               |               |
Payload Content:    SYN           'A'           'B'             'Z'             FIN
                                   |             |               |
Stream Index:     (None)           0             1              N - 1          (None)
```

### 2.2 Forward Projection (`wrap`)

Mapping from 64-bit absolute space to 32-bit wire space is injective. Exploiting standard 32-bit unsigned arithmetic overflow delivers natural modulo $2^{32}$ truncation:

```cpp
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    return WrappingInt32(isn.raw_value() + static_cast<uint32_t>(n));
}
```

The operation executes in $O(1)$ without conditional branches.

### 2.3 Bayesian Nearest-Neighbor Inversion (`unwrap`)

Inverting a 32-bit circular value back to 64-bit absolute space represents a one-to-infinite surjective problem. Introducing `checkpoint` (the most recent absolute sequence number observed) supplies the Bayesian prior required to identify the unique candidate minimizing Euclidean circular distance.

Exploiting two's-complement signed subtraction yields the geodesic circular offset in constant time:

```cpp
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    // 1. Calculate the minimal signed circular displacement on the modulo 2^32 ring
    int32_t offset = n - wrap(checkpoint, isn);

    // 2. Translate the prior checkpoint by the signed offset
    int64_t candidate = static_cast<int64_t>(checkpoint) + offset;

    // 3. Clamp negative underflow to the initial epoch [0, 2^32 - 1]
    if (candidate < 0) {
        candidate += (1ULL << 32);
    }
    return static_cast<uint64_t>(candidate);
}
```

- **Mathematical Correctness**: Casting `uint32_t` subtraction to `int32_t` maps differences into the signed interval $[-2^{31}, 2^{31} - 1]$, naturally choosing between forward and backward circular trajectories without iterative loops.

---

## 3. TCPReceiver Architecture & Finite State Machine

### 3.1 State Transition Model

`TCPReceiver` progresses across three distinct lifecycle phases:

```
        Before SYN                     SYN Received                   FIN Fully Reassembled
  +--------------------+         +--------------------+         +-------------------------+
  |       LISTEN       | ======> |      SYN_RECV      | ======> |        FIN_RECV         |
  |  (!_isn.has_value) |  SYN段  |  (_isn.has_value)  |  FIN段  |   (stream.input_ended)  |
  +--------------------+         +--------------------+         +-------------------------+
            |                              |                                   |
            v                              v                                   v
      ackno: nullopt             ackno: written + 1                  ackno: written + 2
```

1. **`LISTEN`**: Initial state where non-SYN segments are rejected. Receipt of a SYN registers `_isn = head.seqno`, initializes the stream offset at 0, and shifts the receiver into `SYN_RECV`.
2. **`SYN_RECV`**: Operational data phase. Absolute sequence numbers are resolved via `_output.bytes_written()` as checkpoint, evaluated against window bounds, and pushed to `StreamReassembler`.
3. **`FIN_RECV`**: Terminal state reached when a FIN flag has been assembled alongside all prior data into the `ByteStream` (`stream_out().input_ended() == true`). The ACK number increments to acknowledge the FIN.

### 3.2 Member Initialization

Because `StreamReassembler` requires explicit capacity parameters during construction, `TCPReceiver` initializes the reassembler instance directly within its member initializer list:

```cpp
TCPReceiver(const size_t capacity) : _reassembler(capacity), _capacity(capacity) {}
```

---

## 4. Sliding Window & Segment Containment Verification

### 4.1 Window Boundaries

The receiver window forms a half-open interval in absolute sequence space:

$$[\text{abs\_ackno}, \text{abs\_ackno} + \text{window\_size})$$

Where:
$$\text{abs\_ackno} = \_reassembler.\text{stream\_out}().\text{bytes\_written}() + 1 \ (\text{SYN}) + (\text{input\_ended}() \ ? \ 1 : 0 \ (\text{FIN}))$$

$$\text{window\_size} = \_capacity - \_reassembler.\text{stream\_out}().\text{buffer\_size}()$$

### 4.2 Geometric Containment Testing

An inbound segment occupies $[\text{abs\_seqno}, \text{abs\_seqno} + \text{seg\_len})$. Testing for non-empty intersection requires decoupling zero-length segments:

```cpp
bool is_in_window = false;
if (seg_len == 0) {
    if (ws == 0) {
        // Zero-window probe check
        is_in_window = (abs_seqno == abs_ackno);
    } else {
        // Single-point boundary inclusion
        is_in_window = (abs_seqno >= abs_ackno && abs_seqno < abs_ackno + ws);
    }
} else {
    if (ws == 0) {
        is_in_window = false;
    } else {
        // Non-empty interval overlap
        is_in_window = (abs_seqno < abs_ackno + ws && abs_seqno + seg_len > abs_ackno);
    }
}
```

### 4.3 Zero-Window Probing Support

When receiver buffers saturate ($\text{window\_size} = 0$), the remote sender issues zero-length probe segments ($\text{seg\_len} = 0$). Admitting zero-length segments that align with `abs_ackno` allows the receiver to issue up-to-date ACK responses, preventing connection deadlocks.

### 4.4 Stream Index Translation

Upon admission, segment sequence numbers are shifted to stream indices:
- If `head.syn == true` or `abs_seqno == 0`, `stream_index = 0`.
- Otherwise, `stream_index = abs_seqno - 1` (accounting for the initial SYN).

$$\text{stream\_index} = (\text{head.syn} \lor \text{abs\_seqno} == 0) \ ? \ 0 : (\text{abs\_seqno} - 1)$$

---

## 5. Feedback Generation

- **`ackno()`**: Returns `std::nullopt` in `LISTEN` state. Otherwise, computes `abs_ackno` and invokes `wrap()` with `_isn.value()`.
- **`window_size()`**: Advertises unreserved capacity ($\_capacity - \text{buffer\_size}()$), mapped to the 16-bit window field in outgoing TCP headers.

---

## 6. Verification & Automated Test Suite

All 8 automated Lab 2 test units pass with complete compliance:
- `t_wrapping_ints_cmp` / `wrap` / `unwrap`: Validates arithmetic consistency across $2^{32}$ boundaries.
- `t_recv_connect`: Validates initial handshake handling and out-of-order pre-SYN rejection.
- `t_recv_transmit`: Verifies ordered in-window stream delivery and ACK monotonicity.
- `t_recv_window`: Validates dynamic window feedback under simulated reader consumption.
- `t_recv_reorder`: Confirms robust payload assembly under adversarial packet permutations.
- `t_recv_close`: Confirms correct teardown semantics and post-FIN ACK progression.

---

## 7. Conclusions

The `TCPReceiver` bridges unstructured IP packets and reliable stream reassembly. Its constant-time wrapping integer arithmetic and sliding-window validations establish the necessary guarantees for end-to-end full-duplex TCP transport.
