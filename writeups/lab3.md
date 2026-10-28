# CS144 Lab 3: Technical Archive & Writeup (The TCP Sender)

## 1. Scope & Objectives

This lab implements the sending core of the TCP transport protocol, encapsulated within **`TCPSender`**.

The primary responsibilities of `TCPSender` include:
1. **Outbound Stream Chunking & Flow Control (`fill_window`)**: Ingesting bytes from the outgoing `ByteStream`, packaging them into `TCPSegment` structures under `TCPConfig::MAX_PAYLOAD_SIZE` (1452 bytes) constraints, strictly conforming to the sliding window advertised by the remote receiver, and performing zero-window probing when receiver capacity drops to zero.
2. **Flight Tracking & Sequence Consumption**: Tracking unacknowledged segments in an internal outstanding queue, accounting for the sequence space occupied by payload bytes as well as `SYN` and `FIN` control flags (`bytes_in_flight`).
3. **Retransmission Timer & ARQ (`tick` & `Timer`)**: Maintaining an event-driven retransmission timer based on relative millisecond ticks (`ms_since_last_tick`), managing retransmission timeouts (RTO) with exponential backoff under network congestion, and resending the earliest outstanding segment upon timer expiration.
4. **Cumulative Acknowledgment & Window Tracking (`ack_received`)**: Validating incoming `ackno` and `window_size` values, popping fully-acknowledged segments from the outstanding queue, resetting the RTO and consecutive retransmissions counter, and adjusting the timer accordingly.

---

## 2. State Machine & Protocol Lifecycle

`TCPSender` maintains standard TCP sender states governed implicitly by `_next_seqno`, `_bytes_in_flight`, `_syn_sent`, `_fin_sent`, and the outbound `_stream`:

```
          Unconnected                 fill_window() emits SYN           First valid ACK received
     +-------------------+         +---------------------------+      +---------------------------+
     |      CLOSED       | ======> |         SYN_SENT          | ===> |         SYN_ACKED         |
     | (next_seqno == 0) |  SYN    | (next == bytes_in_flight) | ACK  | (Stream active & sliding) |
     +-------------------+         +---------------------------+      +---------------------------+
                                                                                    |
                                                                                    | Stream EOF & window fits FIN
                                                                                    v
          Terminated                   Final ACK acknowledging FIN       Segment with FIN emitted
     +-------------------+         +---------------------------+      +---------------------------+
     |     FIN_ACKED     | <====== |         FIN_SENT          | <=== |         FIN_SENT          |
     | (in_flight == 0)  |   ACK   |   (bytes_in_flight > 0)   | FIN  | (Waiting for FIN ACK)     |
     +-------------------+         +---------------------------+      +---------------------------+
```

These states map directly to `TCPState::state_summary(sender)` defined in `libsponge/tcp_helpers/tcp_state.cc`:
- **CLOSED**: `next_seqno_absolute() == 0`, no SYN has been sent.
- **SYN_SENT**: `next_seqno_absolute() == bytes_in_flight()`, SYN in flight, awaiting acknowledgment.
- **SYN_ACKED**: SYN acknowledged, outbound stream actively transmitting or finished without FIN dispatched.
- **FIN_SENT**: FIN transmitted, awaiting remote acknowledgment (`bytes_in_flight() > 0`).
- **FIN_ACKED**: FIN transmitted and completely acknowledged (`bytes_in_flight() == 0`).

---

## 3. Sliding Window Management & Segment Assembly

### 3.1 Sequence Space Accounting
In TCP sequence space:
- **`SYN`**: Consumes 1 sequence number (first segment sent).
- **Payload**: Consumes $N$ sequence numbers ($N = \text{payload.size()}$).
- **`FIN`**: Consumes 1 sequence number (last segment sent).

Segment sequence footprint is calculated uniformly via `TCPSegment::length_in_sequence_space()`.

### 3.2 Segment Assembly Loop (`fill_window`)

```cpp
void TCPSender::fill_window() {
    uint16_t effective_window = (_window_size == 0) ? 1 : _window_size;

    while (effective_window > _bytes_in_flight) {
        TCPSegment seg;

        if (!_syn_sent) {
            seg.header().syn = true;
            _syn_sent = true;
        }

        size_t win_free_space = effective_window - _bytes_in_flight;
        size_t syn_cost = seg.header().syn ? 1 : 0;
        if (win_free_space < syn_cost) {
            break;
        }

        size_t max_payload = std::min(TCPConfig::MAX_PAYLOAD_SIZE, win_free_space - syn_cost);
        string payload = _stream.read(max_payload);
        seg.payload() = Buffer{std::move(payload)};

        if (!_fin_sent && _stream.eof() && (seg.length_in_sequence_space() < win_free_space)) {
            seg.header().fin = true;
            _fin_sent = true;
        }

        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        seg.header().seqno = wrap(_next_seqno, _isn);
        _segments_out.push(seg);
        _outstanding_segments.push(seg);

        _next_seqno += seg.length_in_sequence_space();
        _bytes_in_flight += seg.length_in_sequence_space();

        if (!_timer.is_running()) {
            _timer.start();
        }

        if (seg.header().fin) {
            break;
        }
    }
}
```

### 3.3 Zero-Window Probing & FIN Deferral
- **Zero-Window Probing**: When the advertised window is 0, `fill_window()` locally treats the available window as 1 to emit a 1-byte probe segment. This elicits a window update from the receiver, preventing connection deadlock. Importantly, `_window_size` is left unmodified.
- **FIN Deferral**: When `_stream.eof()` is reached but remaining window capacity is insufficient to carry both payload and FIN, the FIN flag is deferred until subsequent ACKs open additional window space.

---

## 4. Retransmission Timer & ARQ (`Timer` & `tick`)

### 4.1 Timer Encapsulation
An internal `Timer` struct manages RTO tracking and expiration:
```cpp
class Timer {
  private:
    bool _running{false};
    size_t _time_elapsed{0};
    unsigned int _rto{0};

  public:
    Timer(unsigned int rto) : _rto(rto) {}
    void start() { _running = true; _time_elapsed = 0; }
    void stop() { _running = false; _time_elapsed = 0; }
    bool is_running() const { return _running; }
    void tick(size_t ms) { if (_running) _time_elapsed += ms; }
    bool is_expired() const { return _running && (_time_elapsed >= _rto); }
    void double_rto() { _rto *= 2; }
    void reset_rto(unsigned int initial_rto) { _rto = initial_rto; }
};
```

### 4.2 Expiration & Differential Backoff
When `_timer.is_expired()` evaluates to true in `tick()`:
1. The earliest unacknowledged segment (`_outstanding_segments.front()`) is retransmitted to `_segments_out`.
2. **Differential Exponential Backoff**:
   - If `_window_size > 0`: `_consecutive_retx_count` increments, and RTO doubles (`_timer.double_rto()`).
   - If `_window_size == 0`: RTO is **not** doubled, and `_consecutive_retx_count` is **not** incremented, as zero-window probe drops reflect receiver buffer saturation rather than network congestion.
3. The timer restarts using the active RTO.

---

## 5. Acknowledgment Processing (`ack_received`)

1. **Validity Check**: Convert incoming `ackno` to absolute coordinates via `unwrap(ackno, _isn, _next_seqno)`. If `abs_ackno > _next_seqno`, the segment acknowledges unsent bytes and is rejected with `false`.
2. **Old ACK Ingestion**: If `abs_ackno <= _last_acked_seqno`, the ACK does not confirm new data. Update `_window_size` and return `true` without resetting the timer.
3. **Cumulative Buffer Clearance**:
   - Update `_last_acked_seqno = abs_ackno` and `_window_size = window_size`.
   - Pop segments from `_outstanding_segments` whose sequence footprint satisfies `seg_end_abs <= abs_ackno`, decrementing `_bytes_in_flight`.
   - Reset RTO to `_initial_retransmission_timeout` and reset `_consecutive_retx_count = 0`.
   - If outstanding segments remain, restart the timer; otherwise, stop the timer.

---

## 6. Bare ACK Generation (`send_empty_segment`)

```cpp
void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    _segments_out.push(seg);
}
```
Generates a zero-length segment carrying `_next_seqno`. Since it consumes 0 sequence numbers, it is neither buffered into `_outstanding_segments` nor tracked by the retransmission timer.

---

## 7. Verification & Experimental Results

Executing the full regression test suite:
```bash
cmake --build build --target check_lab3
```

All 6 `TCPSender` unit tests executed with 100% pass rate:
- `t_send_connect`: Verified SYN generation, sequence consumption, and `SYN_SENT` transition.
- `t_send_transmit`: Verified payload segmentation, `MAX_PAYLOAD_SIZE` compliance, and sequence incrementation.
- `t_send_retx`: Verified timer ticks, exponential backoff, and retransmission thresholds.
- `t_send_window`: Verified window saturation, zero-window probe semantics, and FIN piggybacking.
- `t_send_ack`: Verified redundant ACK handling, impossible ACK rejection, and flight queue drainage.
- `t_send_close`: Verified FIN generation and termination transitions (`FIN_SENT`, `FIN_ACKED`).

*(Note: `t_webget` returned HTTP 404 due to upstream remote server route removal on `cs144.keithw.org`; this test exercises standard Linux kernel sockets from Lab 0 and bears no dependency on `TCPSender`.)*

---

## 8. Conclusions

`TCPSender` completes the egress half of the TCP transport pipeline. By orchestrating packet chunking, flight queue tracking, flow-controlled transmission, and adaptive timeout retransmission, it ensures robust, reliable delivery over unreliable lower layers.
