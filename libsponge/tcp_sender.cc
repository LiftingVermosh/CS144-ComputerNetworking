#include "tcp_sender.hh"

#include "tcp_config.hh"
#include "wrapping_integers.hh"

#include <random>
#include <string>

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const {
    return _bytes_in_flight;
}

void TCPSender::fill_window() {
    // Your code here.    
    // 计算对端通告的有效假定窗口
    uint16_t effective_window = (_window_size == 0) ? 1 : _window_size;

    // 循环填充窗口
    while (effective_window > _bytes_in_flight) {
        TCPSegment seg;

        // 若尚未发送 SYN，优先填入 SYN
        if (!_syn_sent) {
            seg.header().syn = true;
            _syn_sent = true;
        }

        // 计算最多能装多少数据载荷
        size_t win_free_space = effective_window - _bytes_in_flight;
        size_t syn_cost = seg.header().syn ? 1 : 0;
        
        if (win_free_space < syn_cost) {
            break; // 连 SYN 都装不下，直接退出
        }

        size_t max_payload = std::min(TCPConfig::MAX_PAYLOAD_SIZE, win_free_space - syn_cost);
        string payload = _stream.read(max_payload);
        seg.payload() = Buffer{std::move(payload)};

        // 判定是否能够捎带 FIN
        if (!_fin_sent && _stream.eof() && (seg.length_in_sequence_space() < win_free_space)) {
            seg.header().fin = true;
            _fin_sent = true;
        }
        
        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        // 设定序列号
        seg.header().seqno = wrap(_next_seqno, _isn);
        // 发送报文并记录在途
        _segments_out.push(seg);
        _outstanding_segments.push(seg);

        // 递增序号与在途计数
        _next_seqno += seg.length_in_sequence_space();
        _bytes_in_flight += seg.length_in_sequence_space();
        
        if (!_timer.is_running()) {
            _timer.start();
        }
        
        // 如果已经发送了 FIN，后续不可能再有新数据，直接跳出
        if (seg.header().fin) {
            break;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
//! \returns `false` if the ackno appears invalid (acknowledges something the TCPSender hasn't sent yet)
bool TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_ackno = unwrap(ackno, _isn, _next_seqno);
    // 如果 ackno 超前于已发送的最大序号，是非法 ACK
    if (abs_ackno > _next_seqno) {
        return false;
    }

    // 更新通告窗口大小
    _window_size = window_size;

    // 如果是旧的 ACK，直接返回
    if (abs_ackno <= _last_acked_seqno) {
        return true;
    }

    // 确认新数据，更新最新确认号
    _last_acked_seqno = abs_ackno;
    // 弹出在途队列中所有完全被确认的报文
    bool has_new_data_acked = false;
    while (!_outstanding_segments.empty()) {
        const TCPSegment &seg = _outstanding_segments.front();
        uint64_t seg_start_abs = unwrap(seg.header().seqno, _isn, _next_seqno);
        uint64_t seg_end_abs   = seg_start_abs + seg.length_in_sequence_space();

        if (seg_end_abs <= abs_ackno) {
            _bytes_in_flight -= seg.length_in_sequence_space();
            _outstanding_segments.pop();
            has_new_data_acked = true;
        } else {
            break;
        }
    }

    // 如果确认了新数据，重置超时时间与重传次数，并管理计时器
    if (has_new_data_acked) {
        _timer.reset_rto(_initial_retransmission_timeout);
        _consecutive_retx_count = 0;

        if (!_outstanding_segments.empty()) {
            _timer.start();
        } else {
            _timer.stop();
        }
    }
    
    return true;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    _timer.tick(ms_since_last_tick);

    // 计时器超时，处理重传与退避
    if (_timer.is_expired()) {
        // 防御性检查：确保在途队列有报文可重传
        if (!_outstanding_segments.empty()) {
            _segments_out.push(_outstanding_segments.front());

            // 只有在窗口非 0 时才递增重传计数与翻倍 RTO
            if (_window_size > 0) {
                _consecutive_retx_count++;
                _timer.double_rto();
            }

            // 重启计时器
            _timer.start();
        } else {
            _timer.stop();
        }
    }
}

unsigned int TCPSender::consecutive_retransmissions() const {
    return _consecutive_retx_count;
}

void TCPSender::send_empty_segment() {
    // Your code here.
    TCPSegment seg;
    seg.header().seqno = next_seqno(); // 设置当前绝对序号 wrap 后的 32 位 seqno
    _segments_out.push(seg);
}
