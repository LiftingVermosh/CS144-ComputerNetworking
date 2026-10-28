# CS144 Lab 2: 实验归档报告 (The TCP Receiver)

## 1. 实验范围与设计目标 (Scope & Objectives)

本实验构建 TCP 传输层接收半段的顶层控制核心 —— **`TCPReceiver`** 与 32 位循环序号转换抽象 **`WrappingInt32`**。

主要任务包括：
1. **32 位循环序号算术抽象**：建立 32 位有线序列号（`seqno`）与 64 位内部绝对序列号（`absolute seqno`）之间的双向映射转换（`wrap` 与 `unwrap`），在常数时间 $O(1)$ 内解决循环序号的周期歧义问题。
2. **TCP 接收端状态机与滑动窗口管理**：实现对进站 TCP 报文段（`TCPSegment`）的解析、连接建立（`SYN`）与终结（`FIN`）状态跟踪、接收窗口边界判定（Window Boundary Checks），并将有效载荷安全传递至下层 `StreamReassembler`。
3. **反馈信息生成**：动态计算并向对端发送方回传累计确认号（`ackno`）与可用接收窗口大小（`window_size`），驱动 TCP 全双工流量控制。

---

## 2. 序号空间体系与转换机制 (Sequence Number Systems)

### 2.1 三大序号坐标系定义与映射

TCP 协议栈内部共存三种具有不同语义与生命周期的序列号体系：

| 维度 | 序列号 (Seqno) | 绝对序列号 (Absolute Seqno) | 流索引 (Stream Index) |
| :--- | :--- | :--- | :--- |
| **数据类型** | `WrappingInt32` (32 位无符号) | `uint64_t` (64 位无符号) | `uint64_t` (64 位无符号) |
| **起始基准** | 随机选取的 `ISN` | `0`（固定对应 SYN） | `0`（固定对应首字节有效载荷） |
| **回绕特性** | 模 $2^{32}$ 回绕（约 4 GiB 周期） | 严格单调递增，不回绕 | 严格单调递增，不回绕 |
| **控制标志计数** | 计入 SYN 与 FIN（各占 1 个序号） | 计入 SYN 与 FIN（各占 1 个序号） | **不计入** SYN/FIN（仅度量有效载荷） |
| **应用层级** | 有线网络报头字段（Wire Format） | 传输层协议状态判定、滑动窗口 | `StreamReassembler` 与 `ByteStream` |

```
序列号映射关系：
Seqno:           [  ISN  ]   [ ISN + 1 ]   [ ISN + 2 ] ... [ ISN + N ]   [ ISN + N + 1 ]
                     |             |             |               |               |
Absolute Seqno:      0             1             2               N              N + 1
                     |             |             |               |               |
Payload Content:    SYN           'A'           'B'             'Z'             FIN
                                   |             |               |
Stream Index:     (无对应)          0             1              N - 1          (无对应)
```

### 2.2 循环序号正向投影 (`wrap`)

从 64 位绝对序列号转换为 32 位相对序列号的过程为正向单射映射。利用 C++ 无符号 32 位整型加法溢出自动截断高位的特性，直接实现模 $2^{32}$ 投影：

```cpp
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    return WrappingInt32(isn.raw_value() + static_cast<uint32_t>(n));
}
```

计算时间复杂度为 $O(1)$，无分支判断。

### 2.3 基于先验的最近邻反演 (`unwrap`)

将 32 位循环序列号还原为 64 位绝对序列号是一个一对多的满射逆问题。算法引入先验信息 `checkpoint`（最近已知处理的绝对序号），目标是求取与 `checkpoint` 欧几里得距离最近的候选绝对序号。

算法利用模 $2^{32}$ 圆环上补码有符号整数运算的几何特性实现 $O(1)$ 常数时间求解：

```cpp
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    // 1. 计算在 32 位模圆环上从 checkpoint 指向 n 的最短有符号位移量
    int32_t offset = n - wrap(checkpoint, isn);

    // 2. 将位移量累加至 64 位基准点，获得最近邻候选绝对序号
    int64_t candidate = static_cast<int64_t>(checkpoint) + offset;

    // 3. 边界纠偏：若结果下溢至负数空间，平移一个周期 (2^32) 恢复至非负空间
    if (candidate < 0) {
        candidate += (1ULL << 32);
    }
    return static_cast<uint64_t>(candidate);
}
```

- **数学完备性**：`n - wrap(checkpoint, isn)` 返回的无符号差值经转换为 `int32_t` 后，天然映射到区间 $[-2^{31}, 2^{31}-1]$ 内，精确表达了在循环模空间中的最短测地线位移。算法无需多周期迭代循环，完全杜绝了性能波动。

---

## 3. TCPReceiver 架构与状态机 (TCPReceiver Design & FSM)

### 3.1 接收端状态转移模型

`TCPReceiver` 维护显式的三阶段生命周期：

```
       未收到 SYN                   收到 SYN 报文                 收到 FIN 且重组完毕
  +------------------+         +--------------------+         +------------------------+
  |      LISTEN      | ======> |      SYN_RECV      | ======> |        FIN_RECV        |
  |  (!_isn.has_val) |  SYN包  |   (_isn.has_val)   |  FIN包  |  (stream.input_ended)  |
  +------------------+         +--------------------+         +------------------------+
       |                                |                                  |
       v                                v                                  v
  ackno: nullopt              ackno: written + 1                 ackno: written + 2
```

1. **`LISTEN`（监听态）**：`_isn` 为空。此状态下拒绝任何未携带 `SYN` 标志的数据段。接收到首个 `SYN` 后，记录 `_isn = head.seqno`，并将载荷交付至流索引 0 处，状态转移至 `SYN_RECV`。
2. **`SYN_RECV`（同步接收态）**：`_isn` 有效。利用已重组字节数作为 `checkpoint` 反演序号，执行滑动窗口包含性检验并推入重组器。
3. **`FIN_RECV`（终止态）**：接收到 `FIN` 且所有前序数据全部由 `StreamReassembler` 按序装配推入 `ByteStream`（即 `stream_out().input_ended() == true`）。确认号在此阶段为 `FIN` 标志计入额外的 1 个序号。

### 3.2 构造约束与资源依赖

`StreamReassembler` 缺乏无参默认构造函数，必须依赖显式传入的 `capacity` 参数进行初始化。为避免在构造函数体执行前引发编译器试图默认构造成员的错误，`TCPReceiver` 必须在**成员初始化列表（Member Initializer List）**中完成构造：

```cpp
TCPReceiver(const size_t capacity) : _reassembler(capacity), _capacity(capacity) {}
```

---

## 4. 滑动窗口与报文合法性判定 (Sliding Window & Segment Processing)

### 4.1 窗口边界与容量计算

接收端当前的接收窗口在绝对序号空间中表示为半开区间：

$$[\text{abs\_ackno}, \text{abs\_ackno} + \text{window\_size})$$

其中：
- 左边界为当前期待接收的首个字节绝对序号：
  $$\text{abs\_ackno} = \_reassembler.\text{stream\_out}().\text{bytes\_written}() + 1 \ (\text{SYN}) + (\text{input\_ended}() \ ? \ 1 : 0 \ (\text{FIN}))$$
- 窗口大小为缓冲区剩余可用容量：
  $$\text{window\_size} = \_capacity - \_reassembler.\text{stream\_out}().\text{buffer\_size}()$$

### 4.2 报文包含性相交检测 (Segment Containment)

进站报文段占据的绝对序号区间为 $[\text{abs\_seqno}, \text{abs\_seqno} + \text{seg\_len})$。判定报文是否与当前接收窗口存在重叠交集时，必须解耦报文长度是否为 0 的边界情形：

```cpp
bool is_in_window = false;
if (seg_len == 0) {
    if (ws == 0) {
        // 零窗口探测特例：仅当绝对序号精确对齐左边界时允许接收
        is_in_window = (abs_seqno == abs_ackno);
    } else {
        // 单点序列号落在半开区间 [abs_ackno, abs_ackno + ws) 内
        is_in_window = (abs_seqno >= abs_ackno && abs_seqno < abs_ackno + ws);
    }
} else {
    if (ws == 0) {
        // 窗口为 0 时拒绝任何带载荷的非空报文段
        is_in_window = false;
    } else {
        // 两非空区间 [abs_seqno, abs_seqno + seg_len) 与 [abs_ackno, abs_ackno + ws) 存在交集
        is_in_window = (abs_seqno < abs_ackno + ws && abs_seqno + seg_len > abs_ackno);
    }
}
```

### 4.3 零窗口探测与边界处理 (Zero-Window Probing)

当接收端上层应用未及时读取数据导致缓冲区饱和（$\text{window\_size} = 0$）时，对端发送方将暂停数据传输并启动定时器发送零窗口探测段（Zero-Window Probe，$\text{seg\_len} = 0$）。

上述判定逻辑严格保证：当 $\text{ws} == 0$ 且 $\text{abs\_seqno} == \text{abs\_ackno}$ 时，报文判定返回 `true`，从而触发接收端向对端刷新当前 ACK 和窗口状态，避免连接陷入死锁。

### 4.4 流索引转换与载荷交付

确认数据落入窗口后，需将绝对序号转换为 `StreamReassembler` 的流索引：
- 若报文携带 `SYN` 标志或绝对序号为 0，有效载荷从流索引 0 开始；
- 否则，有效载荷的流索引为 $\text{abs\_seqno} - 1$（扣除 `SYN` 所占用的 1 个序号）。

$$\text{stream\_index} = (\text{head.syn} \lor \text{abs\_seqno} == 0) \ ? \ 0 : (\text{abs\_seqno} - 1)$$

---

## 5. 反馈接口与确认生成 (Feedback Generation)

- **`ackno()`**：
  若连接尚未建立（未收到 SYN），返回 `std::nullopt`。建立后，计算当前的 `abs_ackno` 并调用 `wrap` 投影回 32 位循环序列号。
- **`window_size()`**：
  返回接收端在当前时序下能够容纳的最大字节量（$\_capacity - \text{buffer\_size}()$），直接映射至 TCP 报头中的 16 位通告窗口字段。

---

## 6. 测试与正确性验证 (Verification & Results)

Lab 2 关联的全部 8 个自动化测试单元均以 100% 通过率执行完毕：
- `t_wrapping_ints_cmp` / `wrap` / `unwrap`：验证了 32 位整数与 64 位绝对整数跨越 $2^{32}$ 边界时的算术正确性。
- `t_recv_connect`：验证了首包 SYN 建立与异常前置非 SYN 包的正确丢弃。
- `t_recv_transmit`：验证了按序数据流交付与 ACK 累进。
- `t_recv_window`：验证了应用层读取状态对窗口通告值的动态反馈调节。
- `t_recv_reorder`：验证了乱序报文与重叠数据在接收端的稳定组装。
- `t_recv_close`：验证了带有 FIN 标志的报文在完全重组后的正常断开。

---

## 7. 实验结论 (Conclusions)

`TCPReceiver` 实现了底层无状态数据包与上层有序字节流之间的协议转换闭环。通过完备的 32 位整数圆环补码算术与严格的滑动窗口相交检测，为后续构建全双工 `TCPConnection` 奠定了稳定可靠的接收端基石。
