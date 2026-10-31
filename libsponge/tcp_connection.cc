#include "tcp_connection.hh"

#include <iostream>

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const {
    return _sender.stream_in().remaining_capacity();
}

size_t TCPConnection::bytes_in_flight() const {
    return _sender.bytes_in_flight();
}

size_t TCPConnection::unassembled_bytes() const {
    return _receiver.unassembled_bytes();
}

size_t TCPConnection::time_since_last_segment_received() const {
    return _time_since_last_segment_received;
}

bool TCPConnection::active() const {
    return _active;
}

size_t TCPConnection::write(const string &data) {
    if (!_active || data.empty()) {
        return 0;
    }

    size_t bytes_written = _sender.stream_in().write(data);
    _sender.fill_window();
    _send_segments();

    return bytes_written;
}

void TCPConnection::end_input_stream() {
    if (!_active) {
        return;
    }

    _sender.stream_in().end_input();
    _sender.fill_window();
    _send_segments();
}

void TCPConnection::connect() {
    if (!_active) {
        return;
    }

    _sender.fill_window();
    _send_segments();
}

void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!_active) {
        return;
    }

    _time_since_last_segment_received = 0;

    // LISTEN 状态特判
    if (!_receiver.ackno().has_value() && _sender.next_seqno_absolute() == 0) {
        // 在 LISTEN 状态下，所有的 RST 报文直接忽略
        if (seg.header().rst) {
            return;
        }
        // 在 LISTEN 状态下，非 SYN 报文直接丢弃（如单纯的 ACK 或非 SYN 数据）
        if (!seg.header().syn) {
            return;
        }
        // 收到有效 SYN，交由 receiver 处理并作为服务端发起连接（产生 SYN+ACK）
        _receiver.segment_received(seg);
        connect();
        return;
    }

    // SYN_SENT 状态下的 RST 与 ACK 处理
    bool in_syn_sent = _sender.next_seqno_absolute() > 0 &&
                       _sender.bytes_in_flight() == _sender.next_seqno_absolute() &&
                       !_receiver.ackno().has_value();
    if (in_syn_sent) {
        if (seg.header().rst) {
            // 当 ACK 合法时才 RESET，否则忽略
            if (seg.header().ack && seg.header().ackno == _sender.next_seqno()) {
                _abort_connection(false);
            }
            return;
        }
        // SYN_SENT 状态下如果收到非法的 ACK，直接忽略
        if (seg.header().ack && seg.header().ackno != _sender.next_seqno()) {
            return;
        }
    } else {
        // 同步状态下的 RST 处理
        if (seg.header().rst) {
            _abort_connection(false);
            return;
        }
    }

    // 将报文传递给 Receiver，获取其是否落入接收窗口
    bool recv_acceptable = _receiver.segment_received(seg);

    // 检查被动关闭：对端 FIN 已到达且输入流结束，但本地发送流尚未结束
    if (_receiver.stream_out().input_ended() && !_sender.stream_in().eof()) {
        _linger_after_streams_finish = false;
    }

    // 处理 ACK
    bool send_empty_ack = false;
    if (seg.header().ack) {
        const bool ack_acceptable = _sender.ack_received(seg.header().ackno, seg.header().win);
        // 如果 ACK 的序号超出已发送范围（bad ACK in the future），需要回发当前 ACK 纠正对端
        if (!ack_acceptable) {
            send_empty_ack = true;
        }
    }

    // 若报文占用了序号空间（SYN / FIN / 数据），必须回发 ACK
    if (seg.length_in_sequence_space() > 0) {
        send_empty_ack = true;
    }

    // 报文落在接收窗口之外必须回发 ACK 告知当前最新状态
    if (!recv_acceptable && _receiver.ackno().has_value()) {
        send_empty_ack = true;
    }

    // 尝试填充窗口发送可能产生的新数据
    _sender.fill_window();

    // 需要回送 ACK 但没有伴随数据发出时，发送空 ACK
    if (send_empty_ack && _sender.segments_out().empty() && _receiver.ackno().has_value()) {
        _sender.send_empty_segment();
    }

    // 推送所有报文
    _send_segments();

    // 干净关闭检测
    if (_receiver.stream_out().input_ended() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0) {
        if (!_linger_after_streams_finish) {
            _active = false;
        }
    }
}

void TCPConnection::tick(const size_t ms_since_last_tick) {
    if (!_active) {
        return;
    }

    _time_since_last_segment_received += ms_since_last_tick;
    _sender.tick(ms_since_last_tick);

    // 连续重传超限，异常中止
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        _abort_connection(true);
        return;
    }

    _send_segments();

    // 检查 TIME_WAIT 状态是否超时或被动关闭达成
    if (_receiver.stream_out().input_ended() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0) {
        if (!_linger_after_streams_finish || _time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
            _active = false;
            _linger_after_streams_finish = false;
        }
    }
}

void TCPConnection::_send_segments() {
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
            seg.header().win = min(static_cast<size_t>(UINT16_MAX), _receiver.window_size());
        }

        _segments_out.push(seg);
    }
}

void TCPConnection::_abort_connection(bool send_rst) {
    _receiver.stream_out().set_error();
    _sender.stream_in().set_error();

    if (send_rst) {
        TCPSegment rst_seg;
        if (_sender.segments_out().empty()) {
            _sender.send_empty_segment();
        }
        rst_seg = _sender.segments_out().front();
        while (!_sender.segments_out().empty()) {
            _sender.segments_out().pop();
        }

        rst_seg.header().rst = true;
        if (_receiver.ackno().has_value()) {
            rst_seg.header().ack = true;
            rst_seg.header().ackno = _receiver.ackno().value();
            rst_seg.header().win = min(static_cast<size_t>(UINT16_MAX), _receiver.window_size());
        }
        _segments_out.push(rst_seg);
    }

    _active = false;
    _linger_after_streams_finish = false;
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            _abort_connection(true);
            cerr << "Warning: Unclean shutdown of TCPConnection\n";
        }
    } catch (const exception &e) {
        cerr << "Exception destructing TCP FSM: " << e.what() << endl;
    }
}
