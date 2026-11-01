# CS144 Lab 4: 实验归档报告 (The TCP Connection)

## 1. 实验范围与设计目标 (Scope & Objectives)

本实验构建 TCP 传输层协议栈的顶层控制核心 —— **`TCPConnection`**。作为发送半段 `TCPSender` 与接收半段 `TCPReceiver` 的协调者（Coordinator），`TCPConnection` 将两端的局部逻辑粘合为一个功能完整、严格符合 RFC 793 规范的全双工 TCP 连接端点。

主要任务包括：
1. **全双工报文协调与捎带应答（Segment Coordination & Piggybacking）**：
   - 提取 `TCPSender` 待发队列中的报文段，动态向其注入 `TCPReceiver` 最新的累计确认号（`ackno`）与接收窗口大小（`win`，截断至 16 位），并压入连接的对外发送队列 `_segments_out`。
   - 在对端报文需要被确认但本地发送端无数据待发时，驱动 `TCPSender` 构造空报文段（`send_empty_segment`）完成纯 ACK 回复。
2. **连接生命周期管理与 FSM 隐式驱动**：
   - 支持客户端主动连接发起（`connect` 发送 SYN）、服务端监听转建连（`LISTEN` 接收 SYN 并回复 SYN+ACK）。
   - 协调数据传输中的窗口更新与滑动驱动，处理乱序报文和零窗口探测（Zero-Window Probing）。
3. **连接终结与 TIME_WAIT 逗留裁决（Connection Teardown & Linger Logic）**：
   - 区分并处理**主动关闭（Active Close）**与**被动关闭（Passive Close）**。
   - 实现符合规范的 TIME_WAIT 状态逗留机制：主动关闭方在双向流全部结束且在途数据清空后，必须保持存活并逗留 $10 \times \text{rt\_timeout}$ 毫秒；被动关闭方在双向流结束后可立即安全终止连接。
4. **异常中止与重置状态控制（RST / Abort Handling）**：
   - 监测重传失败次数，当 `TCPSender` 连续重传次数超过阈值 `TCPConfig::MAX_RETX_ATTEMPTS` 时，主动发送 RST 报文中止连接。
   - 处理进站 RST 报文与析构函数中的未决连接兜底清理，同步标记双向字节流的 `error` 状态并终止活动。

---

## 2. 状态机模型与整体架构 (FSM Architecture & Lifecycle)

Sponge 框架未采用集中式的复杂状态枚举，而是通过 `TCPSender`、`TCPReceiver` 以及连接控制标志的动态组合来表征 RFC 793 规定的标准连接状态（详见 `libsponge/tcp_helpers/tcp_state.cc`）。

```
                                  +------------------------------------+
                                  |           TCPConnection            |
                                  |                                    |
   write() ---------------------> |  +--------------+  +-------------+ |
   end_input_stream()             |  |  TCPSender   |  | TCPReceiver | | <---- segment_received()
                                  |  +--------------+  +-------------+ |
                                  |         |                 |        |
                                  |         v (Data/SYN/FIN)  v (ACK/W)|
                                  |     +-------------------------+    |
                                  |     | Header Assembly & Merge |    |
                                  |     +-------------------------+    |
                                  |                  |                 |
                                  |                  v                 |
                                  |           _segments_out            |
                                  +------------------------------------+
```

### 状态映射矩阵 (RFC 793 State Mapping)

| RFC 793 状态 | `TCPSender` 状态 | `TCPReceiver` 状态 | `_linger_after_streams_finish` | `active()` |
| :--- | :--- | :--- | :--- | :--- |
| **LISTEN** | `CLOSED` | `LISTEN` | `true` | `true` |
| **SYN_SENT** | `SYN_SENT` | `LISTEN` | `true` | `true` |
| **SYN_RCVD** | `SYN_SENT` | `SYN_RECV` | `true` | `true` |
| **ESTABLISHED** | `SYN_ACKED` | `SYN_RECV` | `true` | `true` |
| **CLOSE_WAIT** | `SYN_ACKED` | `FIN_RECV` | `false` | `true` |
| **LAST_ACK** | `FIN_SENT` | `FIN_RECV` | `false` | `true` |
| **FIN_WAIT_1** | `FIN_SENT` | `SYN_RECV` | `true` | `true` |
| **FIN_WAIT_2** | `FIN_ACKED` | `SYN_RECV` | `true` | `true` |
| **CLOSING** | `FIN_SENT` | `FIN_RECV` | `true` | `true` |
| **TIME_WAIT** | `FIN_ACKED` | `FIN_RECV` | `true` | `true` (等待 10*RTO) |
| **CLOSED** | `FIN_ACKED` | `FIN_RECV` | `false` | `false` |
| **RESET** | `ERROR` | `ERROR` | `false` | `false` |

---

## 3. 核心机制设计与实现细节 (Key Implementation Mechanisms)

### 3.1 统一报文分发与组装机制 (`_send_segments`)

所有经由 `TCPSender` 产生的报文在流出前，均需在此统一注入接收端的确认与流控上下文：
```cpp
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
```
- **窗口大小安全截断**：`TCPHeader::win` 为 16 位整型，`_receiver.window_size()` 必须被 `std::min(UINT16_MAX, ...)` 钳位，防止无符号溢出。
- **ACK 标志位保护**：仅在 `_receiver.ackno().has_value()`（即已建立或接收到有效 SYN）时置位 `ack`，避免三次握手初期的握手标志紊乱。

### 3.2 进站报文处理流程 (`segment_received`)

进站报文处理是连接状态机推进的核心中枢，需按严格顺序执行：
1. **存活性短路**：若连接非活动（`!_active`），无条件忽略。
2. **时钟刷新**：收到任意报文立即重置 `_time_since_last_segment_received = 0`。
3. **LISTEN 状态判定**：
   - 忽略所有带有 `RST` 标志的报文。
   - 忽略所有不含 `SYN` 的报文（如游离的数据或纯 ACK）。
   - 收到合法 `SYN`：交付 `_receiver` 解析，并触发本地 `connect()` 发送 `SYN+ACK`。
4. **SYN_SENT 状态保护**：
   - 若收到 RST，仅在 ACK 合法（`ackno == _sender.next_seqno()`）时重置连接。
   - 若收到非法 ACK（`ackno != _sender.next_seqno()`），直接忽略。
5. **已建连状态 RST 判定**：若收到 RST，调用 `_abort_connection(false)` 无条件切断连接并置位双向流错误。
6. **Receiver 与 Sender 分发推进**：
   - 调用 `_receiver.segment_received(seg)` 解析载荷与窗口。
   - 若报文携带 ACK，调用 `_sender.ack_received(...)`。
7. **被动关闭裁决 (Linger 判定)**：
   - 若对端 FIN 到达使入站流结束（`input_ended()`），而本地上层尚未结束输入流（`!_sender.stream_in().eof()`），判定为被动关闭，置 `_linger_after_streams_finish = false`。
8. **ACK 必回场景判定**：
   - 报文占用序号空间（`seg.length_in_sequence_space() > 0`）。
   - 报文落在窗口之外（`!recv_acceptable`）或者为零窗口探测报文（`seqno == ackno - 1`）。
   - 收到不可接受的超前 ACK（`!ack_acceptable`）。
   - 若满足上述条件且 `_sender.segments_out()` 未生成数据，调用 `_sender.send_empty_segment()` 补发空 ACK。
9. **干净关闭判定**：检查四项关闭前置条件。

### 3.3 定时驱动与重传监控 (`tick`)

```cpp
void TCPConnection::tick(const size_t ms_since_last_tick) {
    if (!_active) return;

    _time_since_last_segment_received += ms_since_last_tick;
    _sender.tick(ms_since_last_tick);

    // 1. 连续重传次数超限检测
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        _abort_connection(true);
        return;
    }

    _send_segments();

    // 2. TIME_WAIT 状态逗留超时检测
    if (_receiver.stream_out().input_ended() && 
        _sender.stream_in().eof() && 
        _sender.bytes_in_flight() == 0) {
        if (!_linger_after_streams_finish || _time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
            _active = false;
            _linger_after_streams_finish = false;
        }
    }
}
```

### 3.4 干净关闭（Clean Shutdown）的四项判定准则

```
               [ 条件 #1: _receiver.stream_out().input_ended() == true ]
               [ 条件 #2: _sender.stream_in().eof() == true            ]
               [ 条件 #3: _sender.bytes_in_flight() == 0               ]
                                      |
                                      v
                      +--------------------------------+
                      | _linger_after_streams_finish ? |
                      +--------------------------------+
                               /              \
                     (false)  /                \ (true)
                             v                  v
                    [ 被动关闭完成 ]        [ TIME_WAIT 逗留中 ]
                    立即置 _active = false  时间 >= 10*RTO 后置 _active = false
```

---

## 4. 关键问题与缺陷排查 (Debugging & Challenges)

在实现与调优过程中，攻克了以下几个具有典型性的隐蔽缺陷：

1. **出站队列死循环与弹出时机问题**：
   - **现象**：初始代码中 `_send_segments()` 内遍历 `_sender.segments_out()` 但未执行 `pop()`，导致 `while (!empty())` 死循环挂起。
   - **解决**：在取用 `front()` 后立即将其安全 `pop()`，杜绝外层方法误弹或漏弹。

2. **LISTEN 状态下非 SYN 报文误发 SYN 的缺陷**：
   - **现象**：在服务端 LISTEN 状态收到未带 SYN 的纯数据或非法报文时，直接调用 `_sender.fill_window()` 导致服务端错误进入 `SYN_SENT` 状态。
   - **解决**：在 `segment_received` 开头进行严格的前置检查，若处于未建连且无 ACK 状态，非 SYN 报文直接静默丢弃（Relaxed 规范）。

3. **未来超前 ACK（Bad ACK in the Future）的回复响应**：
   - **现象**：对端发送了大于 `_next_seqno` 的非法 ACK 序号时，原先只单纯调用 `_sender.ack_received`，而未触发 ACK 回发，导致测试提示 `test 1 failed: bad ACK`。
   - **解决**：捕获 `_sender.ack_received(...)` 的返回值，若返回 `false` 表明 ACK 非法，强制置位 `send_empty_ack = true`，回发当前正确的 ACK 状态使对端同步。

4. **窗口外报文与零窗口探测处理**：
   - **现象**：当接收到落在窗口之外的 0 载荷报文时未做回应，测试在窗口探测时断言超时。
   - **解决**：利用 `_receiver.segment_received(seg)` 返回的 `bool` 值（标识报文是否落在接收窗口内）。一旦被 receiver 判定为超出窗口但本地已建连，立即向对端回送最新 ACK 修正对端视图。

---

## 5. 测试验证与结果 (Verification Results)

在本地 Ubuntu 24.04 (WSL) 环境下，运行 CTest 进行完整回归测试：

```bash
ctest -V -R "^t_(active_close|passive_close|ack_rst|ack_rst_win|connect|listen|winsize|retx|retx_win|loopback|loopback_win|reorder)$"
```

### 测试执行结果摘要：

```
Test project /home/vermosh/projects/CS144_ComputerNetwork/build
      Start 33: t_active_close ...................   Passed    0.00 sec
      Start 34: t_passive_close ..................   Passed    0.00 sec
      Start 35: t_ack_rst ........................   Passed    0.00 sec
      Start 36: t_ack_rst_win ....................   Passed    0.00 sec
      Start 37: t_connect ........................   Passed    0.00 sec
      Start 38: t_listen .........................   Passed    0.00 sec
      Start 39: t_winsize ........................   Passed    0.02 sec
      Start 40: t_retx ...........................   Passed    0.00 sec
      Start 41: t_retx_win .......................   Passed    0.00 sec
      Start 42: t_loopback .......................   Passed    0.06 sec
      Start 43: t_loopback_win ...................   Passed    0.04 sec
      Start 44: t_reorder ........................   Passed    0.06 sec

100% tests passed, 0 tests failed out of 12
Total Test time (real) = 0.23 sec
```

此外，运行 Lab 0 ~ Lab 3 全部回归测试（`t_wrapping_*`, `t_byte_stream_*`, `t_strm_reassem_*`, `t_recv_*`, `t_send_*`）共 30 项单元测试，**全部 100% 通过**，无任何功能回退。
