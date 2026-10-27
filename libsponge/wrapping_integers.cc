#include "wrapping_integers.hh"

using namespace std;

//! Transform an "absolute" 64-bit sequence number (zero-indexed) into a WrappingInt32
//! \param n The input absolute 64-bit sequence number
//! \param isn The initial sequence number
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    
    // 直接利用模溢出特性即可
    uint32_t seqno = isn.raw_value() + static_cast<uint32_t>(n);
    
    return WrappingInt32(seqno);
}

//! Transform a WrappingInt32 into an "absolute" 64-bit sequence number (zero-indexed)
//! \param n The relative sequence number
//! \param isn The initial sequence number
//! \param checkpoint A recent absolute 64-bit sequence number
//! \returns the 64-bit sequence number that wraps to `n` and is closest to `checkpoint`
//!
//! \note Each of the two streams of the TCP connection has its own ISN. One stream
//! runs from the local TCPSender to the remote TCPReceiver and has one ISN,
//! and the other stream runs from the remote TCPSender to the local TCPReceiver and
//! has a different ISN.
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    /* wrap 64 \to 32 是有损压缩，所以强制要求第三个参数 checkpoint 作为 Prior */ 

    // 计算在 32 位圆环上距离 checkpoint 对应的 WrappingInt32 的带符号最短距离
    int32_t offset = n - wrap(checkpoint, isn);
    
    // 加到 checkpoint 上得到最接近的候选绝对序号
    int64_t candidate = static_cast<int64_t>(checkpoint) + offset;
    
    // 若下溢到负数，向右平移一个周期 (2^32) 恢复为非负数
    if (candidate < 0) {
        candidate += (1ULL << 32);
    }
    return static_cast<uint64_t>(candidate);
}
