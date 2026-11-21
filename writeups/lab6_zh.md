# CS144 Lab 6: 实验归档报告 (The IP Router)

## 1. 实验范围与设计目标 (Scope & Objectives)

在完成了端到端传输层（Lab 0~4：`ByteStream`、`StreamReassembler`、`TCPReceiver`、`TCPSender`、`TCPConnection`）以及网络访问接口层（Lab 5：`NetworkInterface` 与 ARP 协议）之后，本实验将视角上升至**网络层核心设备——IP 路由器（IP Router）**。

在真实的互联网络拓扑中，路由器拥有多个网络接口（每个接口对应一个独立的物理或虚拟链路），其核心使命是：**依据路由表将从任意接口接收到的 IPv4 数据报正确路由并转发到下一个网络跳步**。

### 核心任务梳理
1. **路由规则存储（Routing Table Maintenance）**：
   - 实现 `Router::add_route` 方法，存储包含目标网络前缀（`route_prefix`）、前缀长度（`prefix_length`）、下一跳地址（`next_hop`）及出接口索引（`interface_num`）的转发表项；
2. **最长前缀匹配转发（Longest Prefix Match, LPM）**：
   - 提取进入路由器的数据报的目的 IPv4 地址，在路由表中检索所有匹配项，并选出前缀长度（掩码位数）最长、最具体的一条规则作为最佳转发决策；
3. **数据报生命周期与 TTL 管理**：
   - 检查 IP 报头的生存时间字段（Time to Live, TTL）。若 `ttl <= 1`（原 TTL 已为 0 或转发减 1 后归零），则必须安全丢弃该数据报，阻断网络环路导致的广播风暴；
   - 若 `ttl > 1`，将 `ttl` 递减 1；
4. **直连 vs 间接下一跳解析（Next-Hop Resolution）**：
   - 若命中规则的 `next_hop` 为空（`std::nullopt`），代表目的网络与该接口**直接相连（Directly Attached）**，此时物理帧的下一跳即为数据报的**最终目的 IP**；
   - 若 `next_hop` 有值，代表该网段需经过**路径上的下一台路由器**中继，下一跳为规则指定的网关 IP；
5. **接口出队与多接口协调（Polling Loop）**：
   - 协调驱动底层所有 `AsyncNetworkInterface` 实例，将各网卡接收到的 IPv4 数据报批量转发至目的网卡。

---

## 2. 架构模型与数据流图 (Architecture & Data Flow)

路由器内部包含若干个 `AsyncNetworkInterface`，它在 `NetworkInterface` 的基础上封装了异步队列，使得接收与转发解耦。整个转发数据流如下：

```
       +--------------------------------------------------------------------------+
       |                               Router                                     |
       |                                                                          |
       |   +------------------------------------------------------------------+   |
       |   |                     Routing Table (_routing_table)               |   |
       |   |  [ Prefix / Len ]  ->  { next_hop (optional), interface_num }    |   |
       |   +------------------------------------------------------------------+   |
       |                                                                          |
       |   AsyncInterface 0       AsyncInterface 1               AsyncInterface N |
       |     [recv_frame]           [recv_frame]                   [recv_frame]   |
       |          |                      |                              |         |
       |          v                      v                              v         |
       |   datagrams_out()        datagrams_out()                datagrams_out()  |
       |          \                      |                             /          |
       |           \                     |                            /           |
       |            +--------------------+---------------------------+            |
       |                                 |                                        |
       |                                 v  Router::route()                       |
       |                    +--------------------------+                          |
       |                    |  route_one_datagram()    |                          |
       |                    +--------------------------+                          |
       |                                 |                                        |
       |                     [ 1. Longest Prefix Match ]                          |
       |                                 |                                        |
       |                       +---------+---------+                              |
       |                       | Hit               | Miss                         |
       |                       v                   v                              |
       |              [ 2. TTL <= 1 ? ]         [ Drop ]                          |
       |                 /          \                                             |
       |               Yes           No                                           |
       |               /              \                                           |
       |           [ Drop ]      [ TTL-- & Resolve Next-Hop ]                     |
       |                               |                                          |
       |                               v                                          |
       |                 interface(interface_num)                                |
       |                      .send_datagram(dgram, actual_next_hop)              |
       |                               |                                          |
       |                               v                                          |
       |                       [ Lab 5 ARP / Eth ]                                |
       +--------------------------------------------------------------------------+
```

---

## 3. 核心设计与数据结构 (Data Structures & Key Components)

### 3.1 路由条目结构体 `RouteEntry`
在 `libsponge/router.hh` 中定义轻量级结构体表示单条转发表项：

```cpp
struct RouteEntry {
    uint32_t route_prefix{};          // 目标网络前缀（32位无符号整数）
    uint8_t prefix_length{};          // 匹配前缀长度（0 ~ 32 位）
    std::optional<Address> next_hop{};// 下一跳 IP（若为空则为直连）
    size_t interface_num{};           // 转发出的网卡编号
};
```

### 3.2 路由表容器选型
```cpp
std::vector<RouteEntry> _routing_table{};
```
- **选型分析**：
  在实际骨干路由器中，路由表可能包含数十万条前缀，通常采用 Trie 树（如 1-bit Trie、Multi-bit Trie 或 LC-Trie）实现 $O(W)$ 检索。
  但对于本实验环境（测试网络通常只有数条到十几条规则），使用 `std::vector` 进行线性遍历的时间仅需数十纳秒，逻辑极其直观且无任何动态内存泄漏风险。实验官方文档亦明确推荐优先采用 $O(N)$ 线性表。

---

## 4. 关键算法与实现细节 (Algorithmic Implementations & Edge Cases)

### 4.1 最长前缀匹配（Longest Prefix Match）与移位 UB 防护
在比较目的 IP 是否匹配前缀时，需要比较其最高 `prefix_length` 位。
如果在 C/C++ 中直接编写 `dst_ip >> (32 - prefix_length)`，当配置默认路由（`0.0.0.0/0`，`prefix_length == 0`）时，移位量为 `32`。**在 C/C++ 语言规范中，对 32 位整型位移 >= 32 位属于未定义行为（Undefined Behavior, UB）**，会导致 AddressSanitizer 拦截或编译器异常优化。

**严谨且优雅的实现方案**：
```cpp
for (const auto &entry : _routing_table) {
    // prefix_length == 0 时，无需移位，任何 IP 均无条件匹配
    bool is_match = (entry.prefix_length == 0) ||
                    ((dst_ip >> (32 - entry.prefix_length)) == (entry.route_prefix >> (32 - entry.prefix_length)));

    // 寻找满足匹配条件的规则中 prefix_length 最大者
    if (is_match && static_cast<int>(entry.prefix_length) > max_prefix_len) {
        max_prefix_len = entry.prefix_length;
        best_route = &entry;
    }
}
```
* **特性**：
  1. `entry.prefix_length == 0` 短路求值，规避了 `32 - 0 = 32` 的移位 UB；
  2. 当 `prefix_length == 32`（主机路由）时，右移 0 位，直接进行整型全值对比，语法完全合规；
  3. 利用 `uint32_t` 的逻辑右移（高位自动补 0），保证前缀比对准确无误。

### 4.2 TTL 边界判定与丢包处理
IP 协议规定每个路由器必须将经过数据报的 TTL 减 1。若 TTL 到达 0，数据报必须被丢弃。
* **边界条件**：
  - 若输入数据报的 `ttl == 0`，已被上一跳耗尽，必须丢弃；
  - 若输入数据报的 `ttl == 1`，减 1 后在出网卡前即变为 0，不得发往信道，必须丢弃。
* **代码逻辑**：
  ```cpp
  if (dgram.header().ttl <= 1) {
      return; // 直接丢弃数据报
  } else {
      dgram.header().ttl--;
  }
  ```

### 4.3 直连与非直连下一跳地址的动态决策
```cpp
const Address next_hop = best_route->next_hop.has_value() 
                       ? best_route->next_hop.value() 
                       : Address::from_ipv4_numeric(dst_ip);

interface(best_route->interface_num).send_datagram(dgram, next_hop);
```
- 当 `next_hop` 缺省（直连）时，通过 `Address::from_ipv4_numeric(dst_ip)` 从网络字节序整型快速构建目标 `Address` 对象，直接委托给底层 `NetworkInterface` 的 ARP 模块查询其 MAC 地址。

---

## 5. 编译、验证与测试 (Verification & Test Results)

### 5.1 模拟网络测试（`network_simulator`）
运行端到端网络模拟器 `./build/apps/network_simulator`：
该模拟器构建了包含 7 个网络接口与多个终端（`applesauce`, `cherrypie`, `dm42`, `dm43`, `hs_router`, `default_router` 等）的复杂网络，测试覆盖：
- 两台普通主机跨网段通信（`applesauce` -> `cherrypie`）；
- 反向跨网段通信（`cherrypie` -> `applesauce`）；
- 默认路由匹配发往外网（`applesauce` -> `1.2.3.4`）；
- 多级子网掩码匹配（`/17`、`/18`、`/19`）发往 HS 局域网及回退；
- 同子网两台主机本地通信（`dm42` -> `dm43`）；
- TTL 耗尽边界测试（分别以 `TTL = 1` 与 `TTL = 0` 发送，验证路由器不漏发过期报文）。

**测试输出结果**：
```text
Constructing network.
Testing traffic between two ordinary hosts (applesauce to cherrypie)...
Testing traffic between two ordinary hosts (cherrypie to applesauce)...
Success! Testing applesauce sending to the Internet.
Success! Testing sending to the HS network and Internet.
Success! Testing two hosts on the same network (dm42 to dm43)...
Success! Testing TTL expiration...
Congratulations! All datagrams were routed successfully.
```

### 5.2 自动化测试集（`check_lab6`）
执行 CTest 自动化测试用例集：
```bash
ctest --test-dir build --output-on-failure -R '^arp_|^router_'
```
**运行结果**：
```text
Test project /home/vermosh/projects/CS144_ComputerNetwork/build
    Start 29: arp_network_interface
1/2 Test #29: arp_network_interface ............   Passed    0.00 sec
    Start 30: router_test
2/2 Test #30: router_test ......................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 2
Total Test time (real) =   0.02 sec
```

---

## 6. 疑难问题与解决复盘 (Troubleshooting & Lessons Learned)

### 6.1 符号拼写错误导致的编译故障
- **现象**：初次编译时出现编译报错 `‘match_route’ was not declared in this scope; did you mean ‘add_route’`；
- **排查**：在 `Router::route_one_datagram` 末尾转发时，指针变量命名为 `best_route`，但在调用 `interface(...)` 时误写成了 `match_route.interface_num`；
- **修正**：更正为 `interface(best_route->interface_num).send_datagram(dgram, next_hop)`，静态类型检查通过。

### 6.2 32 位无符号整数移位 UB 的安全防御
- **分析**：若使用 `(0xFFFFFFFFU << (32 - prefix_length))` 构造子网掩码，在 `prefix_length == 0` 时会产生 `<< 32`。在 x86 架构中，移位指令只会取低 5 位（即 `32 & 0x1F = 0`），导致实际移位 0 位并产生错误掩码，同时在开启 Sanitizer 时会触发 Undefined Behavior 崩溃。
- **总结**：在处理 CIDR 前缀与掩码计算时，必须对长度为 0 和 32 的极端边界进行特判或短路求值。

---

## 7. 总结与前瞻 (Summary & Future Outlook)

在 Lab 6 完成后，CS144 的核心网络栈已经全部闭环：
- **L4 传输层**：由 `TCPSender`、`TCPReceiver`、`StreamReassembler` 组成的可靠全双工流协议；
- **L3 网络层**：由 `Router` 驱动的最长前缀匹配路由与转发引擎；
- **L2 链路/网络访问层**：由 `NetworkInterface` 与 ARP 软状态缓存构成的数据封装与硬件寻址通道。

整个协议栈的各层职责高度解耦：上层 TCP 不需要关心 IP 路由是如何选路或跳步的，网络层 Router 不需要理解以太网帧结构或 ARP 查询报文，真正体现了分层网络架构（Layered Architecture）的设计美感。
