Lab 4 Writeup
=============

My name: Vermosh

My SUNet ID: [your sunetid here]

This lab took me about 6 hours to do. I did not attend the lab session.

Program Structure and Design of the TCPConnection:
The `TCPConnection` acts as a coordinator between the `TCPSender` and `TCPReceiver`,
gluing them into a full-duplex TCP endpoint adhering to RFC 793.

Key design highlights:
1. Unified Outbound Segment Packaging (`_send_segments`):
   - Whenever `_sender` enqueues segments into `_sender.segments_out()`, `_send_segments()`
     drains the queue, attaching `_receiver`'s acknowledgment number (`ackno`) and advertised
     window size (`win`, clamped to `UINT16_MAX`) if `_receiver.ackno().has_value()`.
   - The fully populated segment is then pushed into `_segments_out`.

2. Segment Ingestion & Dispatching (`segment_received`):
   - In LISTEN: ignores non-SYN segments and RST packets. Upon receiving a valid SYN,
     hands it to `_receiver` and invokes `connect()` to dispatch a SYN+ACK.
   - In SYN_SENT: validates incoming ACKs and RSTs to prevent false resets.
   - In synchronized states:
     * Dispatches payload and sequence data to `_receiver.segment_received(seg)`.
     * Passes valid ACKs and window updates to `_sender.ack_received(...)`.
     * Triggers `_sender.fill_window()` to send pending outbound data.
     * Generates an empty ACK (`_sender.send_empty_segment()`) when required:
       a) Incoming segment consumed sequence space (SYN, data, or FIN);
       b) Out-of-window segments or zero-window probes arrived (`!recv_acceptable`);
       c) An unacceptable ACK was received (`!ack_acceptable`).

3. Teardown & Linger Logic (`tick` and clean shutdown):
   - Passive close: If the peer initiates termination (`_receiver.stream_out().input_ended()`
     before `_sender.stream_in().eof()`), `_linger_after_streams_finish` is set to `false`,
     enabling immediate connection teardown upon final acknowledgment.
   - Active close: If local initiates closure, the connection enters TIME_WAIT and lingers
     until `_time_since_last_segment_received >= 10 * _cfg.rt_timeout`.

4. Unclean Abort (`_abort_connection`):
   - Sets error on both `inbound_stream` and `outbound_stream`.
   - Dispatches a segment with `RST=true` if required, and deactivates the connection.

Implementation Challenges:
1. Avoiding infinite loops in `_send_segments`: Ensuring segments are immediately popped
   from `_sender.segments_out()` while reading `front()`.
2. Handling out-of-order packets and unacceptable ACKs: RFC 793 requires sending an ACK
   back to the sender upon receiving unacceptable ACKs or out-of-window segments. Capturing
   the return value of `_sender.ack_received` and `_receiver.segment_received` was essential
   to meet this requirement.
3. Clean shutdown conditions: Carefully distinguishing between active and passive close
   paths to determine when to deactivate without lingering versus when to wait out the 10*RTO.

Remaining Bugs:
None. All 12 TCP connection FSM tests (`fsm_connect`, `fsm_listen`, `fsm_active_close`,
`fsm_passive_close`, `fsm_ack_rst`, `fsm_ack_rst_win`, `fsm_winsize`, `fsm_retx`,
`fsm_retx_win`, `fsm_loopback`, `fsm_loopback_win`, `fsm_reorder`) and all 30 Sponge unit
tests pass with 100% success.
