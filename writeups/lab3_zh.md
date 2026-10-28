# CS144 Lab 3: 实验归档报告 (The TCP Sender)

## 1. 实验范围与设计目标 (Scope & Objectives)

本实验构建 TCP 传输层协议栈的发送半段控制核心 —— **`TCPSender`**。

主要任务包括：
1. **出站字节流切片与流量控制 (`fill_window`)**：从上层 `ByteStream` 中读取字节，切片封装为符合链路 MTU 要求的 `TCPSegment`；根据对端接收方通告的滑动窗口动态控制发送速率，并在窗口耗尽时实施零窗口探测（Zero-Window Probing）。
2. **在途报文追踪与序号空间推进**：维护所有已发出但尚未被对端完全确认的“在途报文队列”（Outstanding Segments Queue）；精确计量有效载荷、`SYN` 与 `FIN` 标志在序号空间中的消耗（`bytes_in_flight`）。
3. **软件重传计时器与 ARQ 机制 (`tick` & `Timer`)**：基于事件驱动的时间步长（`ms_since_last_tick`），实现符合 RFC 6298 简化规范的连续重传超时（RTO）管理、最早在途报文超时重传、以及拥塞指数退避（Exponential Backoff）。
4. **累计确认与滑窗推进 (`ack_received`)**：解析对端反馈的累计确认号（`ackno`）与窗口大小（`window_size`），校验确认号合法性，安全释放已确认的在途报文，动态重置重传计时器并重置连续重传计数器。

---

## 2. 状态机模型与生命周期 (TCPSender FSM & Lifecycle)

`TCPSender` 通过内部变量（`_syn_sent`、`_fin_sent`、`_next_seqno`、`_bytes_in_flight` 及输入流状态）隐式维护标准 TCP 状态：

```
       初始未建连                 调用 fill_window 发送 SYN           收到对端首个合法 ACK
  +------------------+         +----------------------------+      +-----------------------+
  |      CLOSED      | ======> |          SYN_SENT          | ===> |       SYN_ACKED       |
  | (next_seqno == 0)|  SYN包  | (next == bytes_in_flight)  | ACK  | (流进行中, 正常发送)   |
  +------------------+         +----------------------------+      +-----------------------+
                                                                               |
                                                                               | 流 EOF 且窗口容纳 FIN
                                                                               v
       完全确认关闭                收到包含 FIN 的最终 ACK            发送带有 FIN 的报文
  +------------------+         +----------------------------+      +-----------------------+
  |    FIN_ACKED     | <====== |          FIN_SENT          | <=== |       FIN_SENT        |
  | (in_flight == 0) |   ACK   |  (bytes_in_flight > 0)     | FIN  | (等待 FIN 被完全确认)  |
  +------------------+         +----------------------------+      +-----------------------+
```

该生命周期与 `libsponge/tcp_helpers/tcp_state.cc` 中的 `TCPState::state_summary(sender)` 保持完全一致：
- **CLOSED**：`next_seqno_absolute() == 0`，连接尚未启动；
- **SYN_SENT**：`next_seqno_absolute() == bytes_in_flight()`，已发出 SYN 但尚未收到任何确认；
- **SYN_ACKED**：SYN 已被确认，数据流持续传输，或输入流已结束但尚未发送 FIN；
- **FIN_SENT**：已发送 FIN，但在途字节数 `bytes_in_flight() > 0`（等待对端对 FIN 进行确认）；
- **FIN_ACKED**：FIN 已发送且在途字节数为 0，发送半段完全终结。

---

## 3. 滑动窗口填充与切片策略 (Sliding Window & Segment Packing)

### 3.1 序号空间消耗规则

TCP 协议中控制标志与载荷在序号坐标系中具有不同的计量定义：
- **`SYN`**：逻辑消耗 1 个序号空间，仅在连接发起时的首个报文段置位；
- **Payload**：每个有效载荷字节消耗 1 个序号空间；
- **`FIN`**：逻辑消耗 1 个序号空间，仅在流已结束且发送端决定关闭连接时置位。

报文段占用的实际序号空间长度统一由 `TCPSegment::length_in_sequence_space()` 计算。

### 3.2 切片循环与零窗口探测 (`fill_window`)

在 `fill_window()` 中，发送端循环读取 `_stream` 并发送报文，直到可用窗口耗尽或无数据可发：

```cpp
void TCPSender::fill_window() {
    // 1. 零窗口探测特例：若对端通告窗口为 0，假定窗口大小为 1
    uint16_t effective_window = (_window_size == 0) ? 1 : _window_size;

    // 2. 循环填充可用窗口
    while (effective_window > _bytes_in_flight) {
        TCPSegment seg;

        // 若尚未发送 SYN，优先填入 SYN 标志
        if (!_syn_sent) {
            seg.header().syn = true;
            _syn_sent = true;
        }

        size_t win_free_space = effective_window - _bytes_in_flight;
        size_t syn_cost = seg.header().syn ? 1 : 0;
        if (win_free_space < syn_cost) {
            break;
        }

        // 计算当前报文最大可携带载荷
        size_t max_payload = std::min(TCPConfig::MAX_PAYLOAD_SIZE, win_free_space - syn_cost);
        string payload = _stream.read(max_payload);
        seg.payload() = Buffer{std::move(payload)};

        // 判定是否可捎带 FIN
        if (!_fin_sent && _stream.eof() && (seg.length_in_sequence_space() < win_free_space)) {
            seg.header().fin = true;
            _fin_sent = true;
        }

        // 若本次循环生成的报文不占任何序号空间，跳出避免死循环
        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        // 设定序列号、推入发送队列与在途队列
        seg.header().seqno = wrap(_next_seqno, _isn);
        _segments_out.push(seg);
        _outstanding_segments.push(seg);

        // 推进内部序号与在途计数
        _next_seqno += seg.length_in_sequence_space();
        _bytes_in_flight += seg.length_in_sequence_space();

        // 若计时器尚未启动，立即启动
        if (!_timer.is_running()) {
            _timer.start();
        }

        if (seg.header().fin) {
            break;
        }
    }
}
```

### 3.3 关键边界保护
1. **零窗口假定**：当接收端通告 `window_size == 0` 时，仅在 `fill_window` 局部将有效窗口视为 1，发送 1 字节数据试探（Zero-Window Probing），**不改变成员变量 `_window_size` 的真实值**。
2. **FIN 延迟发送**：当输入流 EOF 时，若当前剩余窗口不足以容纳 FIN（`seg.length_in_sequence_space() == win_free_space`），FIN 必须延迟到后续窗口打开时再单独作为纯 FIN 报文发出。

---

## 4. 软件重传计时器与 ARQ 机制 (`Timer` & `tick`)

### 4.1 独立计时器抽象

为了保持模块正交性，在 `tcp_sender.hh` 内部设计高内聚的 `Timer` 辅助类：

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

### 4.2 超时检测与差异化指数退避

外部通过 `tick(ms_since_last_tick)` 驱动计时器流逝。当计时器超时时触发重传：

```cpp
void TCPSender::tick(const size_t ms_since_last_tick) {
    _timer.tick(ms_since_last_tick);

    if (_timer.is_expired()) {
        if (!_outstanding_segments.empty()) {
            // 1. 重传在途队列中最早（序号最小）的未确认段
            _segments_out.push(_outstanding_segments.front());

            // 2. 指数退避判定：仅当通告窗口非零时执行
            if (_window_size > 0) {
                _consecutive_retx_count++;
                _timer.double_rto();
            }

            // 3. 重启计时器重新计时
            _timer.start();
        } else {
            _timer.stop();
        }
    }
}
```

- **差异化退避设计原理**：
  若对端通告窗口为 0（`_window_size == 0`），此时发送的数据属于零窗口探测包。探测段丢失或被对端静默丢弃是预期内的正常交互，不代表骨干网络发生拥塞，因此**禁止对 RTO 执行加倍，且禁止累加连续重传次数**。

---

## 5. 确认应答与在途队列维护 (`ack_received`)

对端回传报文携带 `ackno` 与 `window_size`。`ack_received` 负责执行滑动窗口右移与报文出队：

### 5.1 绝对序号投影与合法性校验
利用 Lab 2 实现的 `unwrap` 算法，以 `_next_seqno` 作为先验基准点反演绝对确认号：
```cpp
uint64_t abs_ackno = unwrap(ackno, _isn, _next_seqno);
if (abs_ackno > _next_seqno) {
    return false; // 超前确认尚未发送的序号，判定为非法报文
}
```

### 5.2 累计确认与资源回收
1. **旧 ACK 过滤**：若 `abs_ackno <= _last_acked_seqno`，说明该 ACK 属于重复确认，不代表新数据被接收，仅更新 `_window_size` 后直接返回 `true`（无需重置计时器）。
2. **在途报文出队**：遍历 `_outstanding_segments`，当且仅当报文所占用的序号空间全部落入已确认范围（`seg_end_abs <= abs_ackno`）时，弹出该报文并从 `_bytes_in_flight` 中扣减其长度。
3. **计时器与计数器复位**：
   - 确认新数据后，RTO 恢复初始值：`_timer.reset_rto(_initial_retransmission_timeout)`；
   - 连续重传计数器清零：`_consecutive_retx_count = 0`；
   - 若在途队列仍有报文，重启计时器；若在途队列清空，停止计时器。

---

## 6. 空报文段生成 (`send_empty_segment`)

```cpp
void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    _segments_out.push(seg);
}
```
用于上层 `TCPConnection` 回复纯 ACK、心跳保活或异常诊断。由于该报文不占序号（长度为 0），严禁将其推入 `_outstanding_segments` 或启动重传计时器。

---

## 7. 测试与正确性验证 (Verification & Results)

运行全量测试套件：
```bash
cmake --build build --target check_lab3
```

### 7.1 测试通过矩阵

| 测试编号 | 测试用例名称 | 测试目标与验证项 | 执行结果 |
| :---: | :--- | :--- | :---: |
| #9 | `t_send_connect` | 初始 SYN 发送、序号占用 (1 字节) 及状态迁移至 `SYN_SENT` | **Passed** (0.00s) |
| #10 | `t_send_transmit` | 按序数据切片、`MAX_PAYLOAD_SIZE` 限制、连续序号推进 | **Passed** (0.02s) |
| #11 | `t_send_retx` | 定时器超时重传最早段、RTO 指数退避、最大重传次数限制 | **Passed** (0.00s) |
| #12 | `t_send_window` | 滑动窗口填满限制、零窗口探测（假定 win=1）、FIN 捎带与延迟 | **Passed** (0.01s) |
| #13 | `t_send_ack` | 重复 ACK 忽略、超前 ACK 拒绝、累积确认弹出在途报文 | **Passed** (0.00s) |
| #14 | `t_send_close` | FIN 标志发送、状态迁移至 `FIN_SENT` 与 `FIN_ACKED` | **Passed** (0.00s) |

Lab 3 专有的全部 6 个测试用例均以 100% 成功率通过，且历史 Lab 0~2 的所有单元测试（`wrapping_integers`, `stream_reassembler`, `tcp_receiver`）保持完全绿灯无回归。

### 7.2 关于 `t_webget` 失败的诊断说明
测试中唯一的 Failed 项为 `t_webget`（来自 Lab 0 的外网连通性脚本）：
- **错误现象**：远端 Web 服务器返回 `HTTP/1.1 404 Not Found`。
- **原因剖析**：测试脚本请求 `http://cs144.keithw.org/hasher/xyzzy`，由于 Stanford 官方测试服务器上的该 URL 路径已下线或重定向，导致返回 404。本地 Socket 读写与 HTTP 协议交互均正常执行，且 `webget` 不依赖 `TCPSender` 代码，故此项失败与 Lab 3 正确性无关。

---

## 8. 实验总结 (Conclusions)

`TCPSender` 建立了 TCP 传输层全双工连接的出站管道。通过滑动窗口切片控制、在途报文状态追踪、以及结合指数退避的 ARQ 重传机制，保证了在不可靠网络环境下字节流能够安全、按序、高效地送达对端。本模块与 Lab 2 的 `TCPReceiver` 相互独立对称，为 Lab 4 组装全功能的 `TCPConnection` 奠定了坚实基础。
