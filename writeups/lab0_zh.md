# CS144 Lab 0: 实验归档报告 (Networking Warmup)

## 1. 实验范围与设计目标 (Scope & Objectives)

本实验涵盖网络通信协议栈的基础支撑构建，主要包含两个核心模块的工程实现：

1. **`webget` 客户端**：基于操作系统原生套接字接口（POSIX/BSD Socket 封装），实现遵循 HTTP/1.1 规范的最小化 Web 客户端，完成与远程 Web 服务器的请求投递与完整响应拉取。
2. **`ByteStream` 内存流控字节流**：设计并实现位于内存中的有序字节流（In-order Byte Stream）抽象，提供严格受容量限制（Flow-controlled / Fixed Capacity）的双端读写接口，为后续传输层协议栈（Sponge）提供核心数据暂存与滑动窗口流控基底。

---

## 2. 核心数据结构与实现机制 (ByteStream Design)

### 2.1 内存模型与惰性紧凑策略 (Memory Layout & Compaction)

`ByteStream` 的底层存储采用连续内存容器 `std::string`，配合头部逻辑偏移量 `_buffer_start` 记录尚未被读取弹出的第一个有效字节索引。

```
内存物理布局：
[  已消费前缀 (Dead Prefix)  ][       有效未读数据 (Active Buffer)       ]
0                       _buffer_start                                _buffer.size()
|<--- 待惰性回收空间 --->|<------------ buffer_size() ------------->|
```

针对读者逐字节或小粒度读取（如单字节循环读取长流）场景，若每次调用 `pop_output(len)` 均直接执行 `std::string::erase(0, len)`，会导致后续全部未读字节的前移拷贝，引发总体时间复杂度退化为二次方 $O(N^2)$。

为此，系统引入**阈值化惰性紧凑策略（Lazy Compaction）**：

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

- **均摊复杂度证明**：仅当失效前缀超过 $4096$ 字节且占据当前物理存储的一半以上时，才执行一次线性内存移动。在发生一次 $O(M)$ 的前缀擦除前，系统必然经历了至少 $\frac{M}{2}$ 次弹出操作，因此单次 `pop_output` 的均摊时间开销严格保证为 $O(1)$。
- **排空优化**：当缓冲区全部数据被读取完毕（`_buffer_start == _buffer.size()`）时，直接调用 `clear()` 将存储复位，重置逻辑偏移至原点。

### 2.2 流控与双向计数器 (Flow Control & Invariant Accounting)

`ByteStream` 的容量语义要求内部缓冲的有效字节数在任意时刻均不能超过预设上限 `_capacity`：

$$\text{buffer\_size}() = \_buffer.\text{size}() - \_buffer\_start \le \_capacity$$

$$\text{remaining\_capacity}() = \_capacity - \text{buffer\_size}()$$

在写入端，`write(data)` 仅接纳不超过当前剩余可用容量的数据：

$$\text{accepted\_bytes} = \min(\text{data.size}(), \text{remaining\_capacity}())$$

为了避免历史数据被弹出擦除后丢失全局审计信息，类中维护了两个独立累加的 64 位单调递增计数器：
- `_bytes_written`：记录自流初始化以来写入端累计成功写入的字节总量。
- `_bytes_read`：记录自流初始化以来读取端累计读取弹出的字节总量。

### 2.3 状态机语义：写端关闭与读端 EOF (Lifecycle & State Machine)

字节流的终结过程遵循两阶段解耦状态机：

```
+------------------------------------+
| 初始状态:                           |
| input_ended: false,  eof: false    |
+-----------------+------------------+
                  | 写端调用 end_input()
                  v
+------------------------------------+
| 写端关闭 (半关闭):                  |
| input_ended: true,   eof: false    |
+-----------------+------------------+
                  | 读端排空缓冲区 (buffer_empty == true)
                  v
+------------------------------------+
| 彻底终结:                          |
| input_ended: true,   eof: true     |
+------------------------------------+
```

- `input_ended()` 反映写入端的行为：写入端显式调用 `end_input()` 后即变为 `true`，标志不再有新字节产生。
- `eof()` 反映读取端的消费状态：必须同时满足 **写端已结束输入** 且 **缓冲区内无残存字节** 时才为 `true`。
  $$\text{eof}() \iff \text{input\_ended}() \land \text{buffer\_empty}()$$

---

## 3. 网络客户端实现 (webget & HTTP/1.1)

### 3.1 报文格式与协议边界

在 `apps/webget.cc` 中，客户端通过 `TCPSocket` 与远程服务器建立 TCP 连接，并严格遵循 HTTP/1.1 规范构造请求：
- **行定界符**：每一行请求头部必须严格以 CRLF（`\r\n`）结束，不能使用仅含 LF（`\n`）的换行符。
- **头部终结标识**：请求必须以一个独立的空行（即连续的 `\r\n\r\n`）标识头部结束，通知服务端解析器开始处理请求。
- **连接控制字段**：必须显式携带 `Connection: close\r\n`，明确要求服务端在返回响应载荷后主动关闭连接写半段，以生成可被客户端检测的 EOF 信号。

### 3.2 套接字读取循环与连接终结

TCP 传输层只保证字节流的有序可靠传输，不保留任何应用层消息边界。服务器发送的响应报文可能在传输层被划分为任意数量、任意尺寸的 TCP 数据段到达接收端。

因此，客户端不得假设一次 `read()` 调用即可获取全部响应，而必须采用轮询循环持续读取，直到套接字底层到达文件结束符（EOF）：

```cpp
while (not socket.eof()) {
    std::cout << socket.read();
}
```

---

## 4.  测试与正确性验证 (Verification & Results)

经由自动化测试套件执行全量回归，所有功能与性能断言均通过：
- `t_byte_stream_construction`：初始化与零容量边界校验通过。
- `t_byte_stream_one_write` / `two_writes`：单次及分批写入流程与容量截断校验通过。
- `t_byte_stream_capacity`：高频满载溢出测试与有效性校验通过。
- `t_byte_stream_many_writes`：长序列随机读写与均摊复杂度压力测试通过。
- `t_address_dt` / `t_parser_dt` / `t_socket_dt`：套接字与协议解析自检测试通过。
