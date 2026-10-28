# CS144 Lab 1: 实验归档报告 (Stitching Substrings into a Byte Stream)

## 1. 实验范围与设计目标 (Scope & Objectives)

本实验针对不可靠网络中由于丢包、乱序、网络链路抖动所导致的离散子串问题，设计并实现传输层接收端的核心重组组件 —— **`StreamReassembler`（流重组器）**。

核心目标是将接收到的任意乱序、重叠、重复到达的数据段，按照全局流索引在严格受限的内存容量护栏内进行去重、缝合与缓冲，并在出现连续前缀时按序写入下层定容字节流 `ByteStream`。

---

## 2. 系统架构与容量约束 (Architecture & Capacity Constraints)

### 2.1 接收端内存契约与不变式

在网络协议栈设计中，接收端的可用物理内存上限必须是全局刚性受限的，以防范恶意构造乱序高位数据引发的内存耗尽攻击（OOM DoS）。

系统定义 `_capacity` 为接收端整体内存占用的物理上限，其生命周期内必须无条件满足以下内存不变式：

$$\text{buffer\_size}() + \text{unassembled\_bytes}() \le \_capacity$$

```
接收端全局内存视图：
[ 已消费数据 (已 pop) ] | [ 已重组未读取 (ByteStream) ] | [ 未重组暂存数据 (Reassembler) ] | [ 不可接受越界区域 ]
                        |<-------- buffer_size() ------->|<------ unassembled_bytes() ------>|
                        |<--------------------------- _capacity --------------------------->|
                        first_unread                     first_unassembled                  limit
```

- **`first_unassembled`**：接收端当前期待写入 `ByteStream` 的首个绝对字节索引。
- **`limit`**：当前允许被重组器暂存的最大绝对索引边界。任何索引 $\ge \text{limit}$ 的数据段均被视作越界并丢弃。

$$\text{limit} = \text{first\_unassembled} + (\_capacity - \_output.\text{buffer\_size}())$$

### 2.2 索引记账与零冗余状态设计

字节流仅以严格递增的顺序从索引 0 开始推入 `_output`。因此：
$$\text{first\_unassembled} \equiv \_output.\text{bytes\_written}()$$

该恒等关系消除了维护额外游标变量的冗余，确保了系统状态的单一定义源（Single Source of Truth）。同时，`_stored_bytes` 变量通过增量维护的方式记录重组器中已暂存的独立字节数，使得 `unassembled_bytes()` 的查询开销降为 $O(1)$。

---

## 3. 数据结构选型与算法设计 (Data Structures & Algorithm)

### 3.1 数据结构选型分析：逐字节映射 vs 区间映射

为实现乱序数据的存储与去重，实验对两种设计方案进行了理论评估与实测比对：

1. **方案 A：逐字节映射 (`std::map<uint64_t, char>`)**
   - *机制*：以单字节为单位存入红黑树，利用 `emplace` 天然去重。
   - *缺陷*：红黑树每个节点包含 3 个 64 位指针、颜色标记以及对齐填充，存储 1 个字节需消耗 32~48 字节的物理内存。高频节点分配导致严重的内存碎片，且破坏了 CPU 空间局部性（Spatial Locality）。
2. **方案 B：区间合并映射 (`std::map<uint64_t, std::string>`)**
   - *机制*：以连续子串为区间存储在红黑树中，键为区间的起始绝对索引。
   - *优势*：维持“区间两两不相交、两两不相邻”的不变式，将离散字符压缩为连续字符数组，极大提升缓存利用率并降低节点数量。

### 3.2 区间合并算法设计 (Coalescing Algorithm)

为保证最小内存占用，`_stored` 内部时刻满足严格不变式：**任意相邻的两个区间之间至少存在一个未填充的空洞（Hole）**。

当新数据段到达并被裁剪至可用范围 $[start, end)$ 后，合并算法的核心执行步骤如下：

```cpp
void StreamReassembler::merge_pending_bytes(const string &data,
                                            const uint64_t index,
                                            const uint64_t start,
                                            const uint64_t end) {
    // 1. 查找首个可能与 [start, end) 相交或相邻的区间
    auto first = _stored.lower_bound(start);
    if (first != _stored.begin()) {
        const auto previous = prev(first);
        if (previous->first + previous->second.size() >= start) {
            first = previous;
        }
    }

    // 2. 快速判断：当前已有区间已完全覆盖新数据
    if (first != _stored.end() and first->first <= start and end <= first->first + first->second.size()) {
        return;
    }

    // 3. 扫描并合并所有相交及相接的后续区间
    auto last = first;
    uint64_t merged_start = start;
    uint64_t merged_end = end;
    while (last != _stored.end() and last->first <= end) {
        merged_start = min(merged_start, last->first);
        merged_end = max(merged_end, last->first + last->second.size());
        ++last;
    }

    // 4. 无空隙内存覆盖构造
    string merged(merged_end - merged_start, '\0');
    for (auto known = first; known != last; ++known) {
        merged.replace(known->first - merged_start, known->second.size(), known->second);
        _stored_bytes -= known->second.size();
    }
    merged.replace(start - merged_start, end - start, data, start - index, end - start);

    // 5. 替换节点
    _stored.erase(first, last);
    _stored_bytes += merged.size();
    _stored.emplace(merged_start, move(merged));
}
```

- **左侧穿透检测 (`prev(first)`)**：`lower_bound(start)` 仅返回键值 $\ge start$ 的首个节点。若存在起始位置 $< start$ 但尾部延伸至 $start$ 之内的区间，必须通过 `prev(first)` 向前回退获取，避免合并遗漏。
- **相邻区间吸收 (`last->first <= end`)**：采用带等号的比较条件，使得与新数据恰好首尾相接的相邻条目（如 $[0, 5)$ 与 $[5, 10)$）强制合并为大单体区间，杜绝碎片累积。
- **连续内存直接填充**：基于连续相交保证，直接开辟 `merged_end - merged_start` 字节的单一缓冲区，依次覆盖旧数据与新数据，消除频繁动态重分配。

### 3.3 数据排空与流终结判定 (Drain & EOF Invariant)

- **连续前缀排空**：每次插入后，检查 `_stored.begin()->first` 是否与 `first_unassembled` 对齐。只要存在连续前缀，即以连续批次循环写入 `_output`，并同步更新 `_stored_bytes`。
- **EOF 语义保护**：由于网络可能对载荷进行截断，**仅当携带 EOF 标记的数据段完整被接收端容量容纳时，其 EOF 对应的终止索引才被确认为有效**。若数据段被尾部截断，则忽略本次 EOF 标记，防止因数据缺失导致流被错误地提前终结。

---

## 4. 性能评测与分析 (Performance Evaluation)

在标准 Linux 测试环境下，对 64 MiB 数据规模进行高负载基准吞吐率评测：

| 测试场景（64 MiB 吞吐测试） | 方案 A：逐字节映射 | 方案 B：区间合并映射 | 性能对比 |
| :--- | :--- | :--- | :--- |
| **严格按序 (1460 B 报文段)** | 0.216 Gbit/s | **8.742 Gbit/s** | **提升 40.5 倍** |
| **乱序到达 (1460 B 报文段)** | 0.068 Gbit/s | **1.632 Gbit/s** | **提升 24.0 倍** |
| **乱序到达 (64 B 极端细粒度)** | 0.039 Gbit/s | **0.422 Gbit/s** | **提升 10.8 倍** |

评测结论表明：区间合并映射在乱序吞吐率指标上完全超越了实验规范设定的及格阈值（0.1 Gbit/s），验证了减少动态内存分配与维持物理连续性对于高性能网络数据平面的决定性意义。

---

## 5. 正确性验证与差分测试 (Testing & Verification)

除通过官方全部 8 项功能单元测试之外，实验构建了基于蒙特卡洛随机状态的**差分模糊测试框架（Differential Fuzzer）**：
- **测试模型**：以数学上天然自洽的逐字节映射作为参考预言机（Oracle），与区间合并实现进行随机并发对比。
- **混沌输入注入**：在随机容量（$1 \sim 65536$ 字节）、随机切片跨度、重叠区间、乱序索引与随机 EOF 触发条件下，持续驱动双端状态演进。
- **验证规模**：在 7 组独立随机参数集下累计执行 **140,000 次** 连续操作，在每一步原子操作后全量比对内部缓冲内容、写入计数、未重组字节统计及终止状态标志。测试结果表明两套实现状态转移完全一致，无状态漂移。

---

## 6. 实验结论 (Conclusions)

1. `StreamReassembler` 通过严谨的区间合并逻辑，在乱序、重叠不可靠信道与有序可靠流控管道之间构筑了确定的转换屏障。
2. 保持全局容量不变式约束，有效防御了潜在的内存耗尽与越界攻击。
3. 单元测试与差分模糊测试全面验证了算法的确定性与工程完备性。
