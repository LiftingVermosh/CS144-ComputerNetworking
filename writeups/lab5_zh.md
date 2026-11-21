# CS144 Lab 5: 实验归档报告 (The Network Interface)

## 1. 实验范围与设计目标 (Scope & Objectives)

在完成了传输层协议栈核心（Lab 0~4：`ByteStream`、`StreamReassembler`、`TCPReceiver`、`TCPSender` 及协调器 `TCPConnection`）后，本实验将视角从 L4（传输层）**下沉至网络接口层（Network Interface）**，构建端系统与物理链路之间的核心桥梁。

网络接口组件在整体网络体系中具有双重关键地位：
1. **端系统协议栈底部（Host Stack Bottom）**：将上层 TCP/IP 数据报转换为链路层以太网帧（Ethernet Frame），交由真实物理网卡或 Linux 虚拟 TAP 设备发往物理信道；
2. **路由器转发核心（Router Switching Fabric，Lab 6 基础）**：路由器本质上拥有多个独立的 `NetworkInterface`，负责在不同网络接口之间转发 IP 数据报。

### 核心任务梳理
1. **数据帧封装与解封装（Encapsulation & Decapsulation）**：
   - 出站方向：接收上层传递的 `InternetDatagram` 及下一跳地址 `Address next_hop`，依据链路层地址将其封装为 `EthernetFrame` 并推入待发队列 `_frames_out`；
   - 进站方向：接收物理信道传入的原始 `EthernetFrame`，执行 MAC 地址过滤，若为 IPv4 数据报则解封装并上送网络层，若为 ARP 报文则转入地址解析逻辑。
2. **RFC 826 ARP 协议全流程实现**：
   - **ARP 广播请求（ARP Request）**：当下一跳 MAC 地址未知时，暂存出站数据报，并构造以太网广播帧向局域网广播查询该 IP 的硬件地址；
   - **ARP 单播应答（ARP Reply）**：当收到针对本机 IP 的 ARP 查询时，构造并单播回复包含本机 MAC 与 IP 的应答帧；
   - **被动无条件学习（Snooping / Cache Update）**：无论是进站的 ARP 请求还是 ARP 应答，只要包含发送方的 IP 与 MAC 映射，均更新本地缓存。
3. **软状态生命周期管理（Soft State Lifecycle & Rate Limiting）**：
   - **缓存老化机制**：ARP 映射条目默认生存时间（TTL）为 **30 秒**（30,000 ms），过期后自动失效；
   - **ARP 请求防泛洪抑制**：针对同一下一跳 IP，在 **5 秒**（5,000 ms）内最多发送一次 ARP 请求广播，避免瞬时网络风暴；
4. **未决数据报等待队列（Pending Datagrams Queuing & Flushing）**：
   - 对尚未获取到 MAC 地址的目标 IP，必须建立先进先出队列暂存其 IP 数据报；一旦获知其 MAC 地址，立即批量封装并一次性冲刷发送。

---

## 2. 架构模型与数据流图 (Architecture & Data Flow)

`NetworkInterface` 连接了以 32 位整型为标识的 IP 逻辑世界与以 48 位（6 字节）MAC 地址为标识的以太网物理世界：

```
       +--------------------------------------------------------------+
       |           Upper Layer (TCP / Lab 6 Router Routing)           |
       +--------------------------------------------------------------+
                                      |
                                      | send_datagram(dgram, next_hop)
                                      v
       +--------------------------------------------------------------+
       |                      NetworkInterface                        |
       |                                                              |
       |   +------------------------------------------------------+   |
       |   |                     ARP Cache                        |   |
       |   |      [ IP (uint32_t)  ->  { MAC, Remaining TTL } ]   |   |
       |   +------------------------------------------------------+   |
       |                              |                               |
       |            +-----------------+-----------------+             |
       |            | Hit                               | Miss        |
       |            v                                   v             |
       |   [ Wrap TYPE_IPv4 Frame ]          [ Enqueue _waiting_dgrams ]
       |            |                                   |             |
       |            |                        [ Check Rate Limiter (5s)]
       |            |                                   | Not sent    |
       |            |                                   v             |
       |            |                       [ Broadcast ARP Request ] |
       |            |                                   |             |
       |            +-----------------+-----------------+             |
       |                              |                               |
       |                              v                               |
       |                        _frames_out                           |
       +--------------------------------------------------------------+
                                      |
                                      v
       +--------------------------------------------------------------+
       |             Link Layer / TAP Device / Network                |
       +--------------------------------------------------------------+
```

### 进站数据分发流向图

```
                   Incoming EthernetFrame (recv_frame)
                                  |
                                  v
              +---------------------------------------+
              | Destination MAC Filter                |
              | dst == _ethernet_address OR Broadcast?|
              +---------------------------------------+
                        | Yes                   | No
                        v                       v
               Frame Type Dispatch          [ Drop / Return nullopt ]
               /                 \
              /                   \
   TYPE_IPv4 /                     \ TYPE_ARP
            v                       v
   [ Parse IPv4 Header ]    [ Parse & Verify ARPMessage ]
            |                       |
            v                       +---> 1. Learn Sender Mapping (TTL: 30s)
   [ Return Datagram ]              +---> 2. Flush _waiting_dgrams for Sender IP
                                    +---> 3. Target == Self IP & Opcode == REQUEST?
                                                  | Yes
                                                  v
                                         [ Send Unicast ARP Reply ]
```

---

## 3. 核心机制设计与实现细节 (Key Implementation Mechanisms)

### 3.1 状态存储与数据结构设计

在 [`libsponge/network_interface.hh`](file:///z:/home/vermosh/projects/CS144_ComputerNetwork/libsponge/network_interface.hh) 中设计如下私有状态：

```cpp
class NetworkInterface {
  private:
    static constexpr size_t ARP_ENTRY_TTL = 30000;   //!< ARP 条目缓存期 30s
    static constexpr size_t ARP_REQUEST_TTL = 5000;  //!< 同一 IP 重复请求抑制窗口 5s

    struct ArpEntry {
        EthernetAddress mac{};
        size_t ttl{ARP_ENTRY_TTL};
    };

    // 1. IP -> {MAC, TTL} 缓存映射表
    std::unordered_map<uint32_t, ArpEntry> _arp_table{};

    // 2. 未决 IP 数据报等待队列 (IP -> vector<InternetDatagram>)
    std::unordered_map<uint32_t, std::vector<InternetDatagram>> _waiting_dgrams{};

    // 3. ARP 请求限频计时器 (IP -> 剩余抑制时间)
    std::unordered_map<uint32_t, size_t> _arp_request_timer{};

    // 内部私有辅助方法
    void _send_arp_request(const uint32_t ip);
    void _send_arp_reply(const EthernetAddress &target_eth, const uint32_t target_ip);
    void send_ipv4_frame(const EthernetAddress &dst_eth, const InternetDatagram &dgram);
...
}
```

- **类型规范统一**：IP 统一采用主机字节序的 `uint32_t`（由 `Address::ipv4_numeric()` 导出），便于在哈希表中作高速查找；
- **结构体防御性初始化**：`ArpEntry` 内 `EthernetAddress mac{}` 显示进行值初始化，严格规避 `-Weffc++` 关于未初始化成员变量的警告。

---

### 3.2 出站数据报发送逻辑 (`send_datagram`)

出站逻辑清晰分离了**缓存直发**与**排队等待 + 限频广播**两个路径：

```cpp
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    // 1. 检查 ARP 缓存表：命中则立即发送
    const auto arp_it = _arp_table.find(next_hop_ip);
    if (arp_it != _arp_table.end()) {
        send_ipv4_frame(arp_it->second.mac, dgram);
        return;
    }

    // 2. 缓存未命中：排队进入等待队列（支持同一下一跳连续到达多个数据报）
    _waiting_dgrams[next_hop_ip].push_back(dgram);

    // 3. ARP 请求限频检查：5 秒内若未发送过请求，则广播并发起 5 秒静默倒计时
    if (_arp_request_timer.find(next_hop_ip) == _arp_request_timer.end()) {
        _send_arp_request(next_hop_ip);
        _arp_request_timer[next_hop_ip] = ARP_REQUEST_TTL;
    }
}
```

---

### 3.3 进站帧过滤、解封装与 ARP 状态机 (`recv_frame`)

`recv_frame` 是整个协议栈与以太网底层交汇的核心枢纽：

```cpp
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // 1. 物理层 MAC 过滤：只接收发往本机 MAC 或以太网广播地址的帧
    const EthernetAddress &dst_mac = frame.header().dst;
    if (dst_mac != _ethernet_address && dst_mac != ETHERNET_BROADCAST) {
        return std::nullopt;
    }

    // 2. IPv4 数据报处理：直接上送
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
        return std::nullopt;
    }

    // 3. ARP 报文处理
    if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp_msg;
        if (arp_msg.parse(frame.payload()) != ParseResult::NoError || !arp_msg.supported()) {
            return std::nullopt;
        }

        // 无条件学习发送方映射 (Snooping)，有效期重置为 30 秒
        _arp_table[arp_msg.sender_ip_address] = {arp_msg.sender_ethernet_address, ARP_ENTRY_TTL};

        // 冲刷该 IP 的所有在途未决数据报 (FIFO 发送)
        const auto wait_it = _waiting_dgrams.find(arp_msg.sender_ip_address);
        if (wait_it != _waiting_dgrams.end()) {
            for (const auto &dgram : wait_it->second) {
                send_ipv4_frame(arp_msg.sender_ethernet_address, dgram);
            }
            _waiting_dgrams.erase(wait_it);
        }

        // 若为询问本机 MAC 的 ARP 请求，则予以单播应答
        if (arp_msg.opcode == ARPMessage::OPCODE_REQUEST &&
            arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
            _send_arp_reply(arp_msg.sender_ethernet_address, arp_msg.sender_ip_address);
        }
    }

    return std::nullopt;
}
```

- **无条件泛听学习（Unconditional Learning）**：只要收到合法的 ARP 报文（不管是 Request 还是 Reply），都可以合法学习 `sender_ip_address -> sender_ethernet_address`。这保证了即使本端尚未发送过查询，也能提前预热缓存；
- **全格式校验（`supported()`）**：调用 `arp_msg.supported()` 检验硬件类型（以太网）、协议类型（IPv4）及硬件/协议地址长度，确保非法或未知变体不会污染内部映射；
- **单播回包精准寻址**：回复 ARP Reply 时，必须以请求方的 `sender_ethernet_address` 为目标 MAC，`sender_ip_address` 为目标 IP，避免错填自身 IP。

---

### 3.4 软状态计时与安全迭代器删除 (`tick`)

网络协议栈的软状态更新必须规避 C++ 标准容器迭代器失效的经典陷阱：

```cpp
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    // 1. ARP 映射缓存条目过期判定 (30s 倒计时)
    for (auto it = _arp_table.begin(); it != _arp_table.end();) {
        if (it->second.ttl <= ms_since_last_tick) {
            it = _arp_table.erase(it);
        } else {
            it->second.ttl -= ms_since_last_tick;
            ++it;
        }
    }

    // 2. ARP 请求限频计时器递减 (5s 倒计时)
    for (auto it = _arp_request_timer.begin(); it != _arp_request_timer.end();) {
        if (it->second <= ms_since_last_tick) {
            it = _arp_request_timer.erase(it);
        } else {
            it->second -= ms_since_last_tick;
            ++it;
        }
    }
}
```

- **迭代器安全保证**：在 `std::unordered_map` 中，`erase` 会导致指向被删元素的迭代器立即失效。若使用基于范围的 for 循环并在体内 erase，下一次 `++` 将引发未定义行为（UB）。显式使用 `it = map.erase(it)` 能够原子地将迭代器推进到下一个有效位置。

---

## 4. 关键缺陷排查与调试复盘 (Debugging & Challenges Retrospective)

在最初的编码过程中，定位并逐一攻克了以下几个深层次细节陷阱：

| 陷阱与缺陷表现 | 产生根因 | 修正方案与防御措施 |
| :--- | :--- | :--- |
| **编译期 `-Weffc++` 报错** | `ArpEntry` 中的 `EthernetAddress mac` 未经初始化列表赋值 | 在结构体内声明时显式赋予 `{}` 默认零值初始化 |
| **`recv_frame` 缺少尾部 return** | C++ 要求所有带返回值的非 void 函数所有控制路径必须返回值 | 在函数末尾显式 `return std::nullopt;` |
| **缓存命中逻辑反转** | `_arp_table.find != end()` 命中分支中误调用了 `_send_arp_request` | 纠正为命中时调用 `send_ipv4_frame` 直接发送 IP 数据报 |
| **未决报文静默丢失** | `vector<InternetDatagram> curDgram = _waiting_dgrams[ipv4];` 发生值拷贝 | 移除冗余局部变量，使用引用或直接 `_waiting_dgrams[ipv4].push_back(...)` |
| **ARP 广播请求完全不发送** | 限频检查写成 `if (timer.find != end())`，即计时器存在时才发包 | 纠正为 `timer.find == end()`，只有未在限频倒计时内才发包 |
| **冲刷等待队列发送了 ARP 包** | 收到 ARP 回包后误调用 `_send_arp_reply` 代替发送 IPv4 报文 | 纠正为遍历 `_waiting_dgrams` 逐个调用 `send_ipv4_frame` |
| **ARP Reply 目标 IP 错填** | `_send_arp_reply` 传入了 `arp_msg.target_ip_address`（本机 IP） | 纠正为 `arp_msg.sender_ip_address`（对端 IP） |
| **范围 for 遍历中 erase 导致 UB** | `for (auto &pair : map) { map.erase(pair.first); }` 破坏哈希表链表指针 | 改为标准安全的 `it = map.erase(it)` 迭代器递增模型 |

---

## 5. 边界考量与测试验证 (Boundary Considerations & Test Verification)

针对 `tests/net_interface.cc` 中的全套测试场景进行了详尽验证：

### 5.1 覆盖测试用例验证
1. **`typical ARP workflow`**：
   - 验证无 ARP 缓存时，IP 数据报被暂存并广播 ARP 请求（目的 MAC `ff:ff:ff:ff:ff:ff`）；
   - 收到 ARP 应答后，在途等待的数据报被正确取出并封装为单播以太网帧发送；
   - 收到目的为本机的 IPv4 帧成功上送，目的为第三方的帧被丢弃。
2. **`reply to ARP request`**：
   - 接收目的为广播且询问本机的 ARP 请求，单播回复符合 RFC 826 的 `OPCODE_REPLY`；
   - 忽略询问其他主机的 ARP 请求（不予回复）。
3. **`learn from ARP request`**：
   - 验证无条件学习：收到对端发送的 ARP 请求后，即使不发向本机，也缓存其 MAC；
   - 后续向该对端发送 IP 数据报时直接命中缓存，无需再次发送 ARP 请求。
4. **`pending mappings last five seconds`**：
   - 首次发送未命中后广播一次 ARP 请求；
   - 在 4,990 ms 处再次发送同一 IP，不产生新的 ARP 广播（限频生效）；
   - 跨越 5,000 ms（到达 5,010 ms）后，再次发送该 IP，成功触发第二次 ARP 广播请求。
5. **`active mappings last 30 seconds`**：
   - 验证缓存有效时长精准保持 30 秒（在 10s、20s 时发包均命中缓存直接发送）；
   - 在 31 秒时失效，下一次发包重新发起 ARP 广播查询。
6. **`different ARP mappings are independent`**：
   - 验证多目标 IP（如 `10.0.0.5` 与 `10.0.0.19`）的 ARP 缓存与过期计时器彼此完全隔离、互不干扰。

### 5.2 运行评测结果

在项目构建目录下运行：
```bash
$ ctest -R arp_network_interface -V
```

测试输出：
```text
Test project /home/vermosh/projects/CS144_ComputerNetwork/build
Constructing a list of tests
Done constructing a list of tests
Updating test list for fixtures
Added 0 tests to meet fixture requirements
Checking test dependency graph...
Checking test dependency graph end
test 29
    Start 29: arp_network_interface

29: Test command: /home/vermosh/projects/CS144_ComputerNetwork/build/tests/net_interface
29: Working Directory: /home/vermosh/projects/CS144_ComputerNetwork/build
29: Test timeout computed to be: 10000000
29: DEBUG: Network interface has Ethernet address 02:aa:6b:8a:c3:bc and IP address 4.3.2.1
29: DEBUG: Network interface has Ethernet address 0e:0e:2e:de:60:99 and IP address 5.5.5.5
29: DEBUG: Network interface has Ethernet address 7e:ec:eb:b6:da:50 and IP address 5.5.5.5
29: DEBUG: Network interface has Ethernet address 96:1a:03:25:fb:a0 and IP address 1.2.3.4
29: DEBUG: Network interface has Ethernet address b6:1b:51:2f:d3:c4 and IP address 4.3.2.1
29: DEBUG: Network interface has Ethernet address 9a:9c:eb:f1:1b:6f and IP address 10.0.0.1
1/1 Test #29: arp_network_interface ............   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.01 sec
```

全部测试 100% 通过，且内存与异常状态机逻辑严谨无告警。本模块为后续 Lab 6 构建多接口 IP 路由器（Router）奠定了坚实的基础。
