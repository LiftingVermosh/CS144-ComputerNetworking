#include "tcp_receiver.hh"

using namespace std;

bool TCPReceiver::segment_received(const TCPSegment &seg) {
    const TCPHeader &head = seg.header();

    // 尚未收到过 SYN
    if (!_isn.has_value()) {
        if (!head.syn) {
            return false;
        }
        _isn = head.seqno;
        // 首个 SYN 报文必然是有效连接建立，其 stream_index 从 0 开始
        _reassembler.push_substring(seg.payload().copy(), 0, head.fin);
        return true;
    }

    // 已经收到过 SYN 
    uint64_t ckpt = _reassembler.stream_out().bytes_written();
    uint64_t abs_seqno = unwrap(head.seqno, _isn.value(), ckpt);

    // 接收窗口左边界
    uint64_t abs_ackno = _reassembler.stream_out().bytes_written() + 1
                         + (_reassembler.stream_out().input_ended() ? 1 : 0);
    size_t ws = window_size();
    size_t seg_len = seg.length_in_sequence_space();

    // 窗口判断
    bool is_in_window = false;
    if (seg_len == 0) {
        if (ws == 0) {
            is_in_window = (abs_seqno == abs_ackno);
        } else {
            is_in_window = (abs_seqno >= abs_ackno && abs_seqno < abs_ackno + ws);
        }
    } else {
        if (ws == 0) {
            is_in_window = false;
        } else {
            is_in_window = (abs_seqno < abs_ackno + ws && abs_seqno + seg_len > abs_ackno);
        }
    }

    // 若报文落在窗口内，将有效载荷及 FIN 推入重组器
    if (is_in_window) {
        uint64_t stream_index = (head.syn || abs_seqno == 0) ? 0 : (abs_seqno - 1);
        _reassembler.push_substring(seg.payload().copy(), stream_index, head.fin);
    }

    return is_in_window;
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (!_isn.has_value()) {
        return nullopt;
    }

    uint64_t abs_ackno = _reassembler.stream_out().bytes_written() + 1;
    if (_reassembler.stream_out().input_ended()) {
        abs_ackno += 1;
    }
    return wrap(abs_ackno, _isn.value());
}

size_t TCPReceiver::window_size() const {
    return _capacity - _reassembler.stream_out().buffer_size();
}
