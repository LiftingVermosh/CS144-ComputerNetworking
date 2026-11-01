#ifndef SPONGE_LIBSPONGE_NETWORK_INTERFACE_HH
#define SPONGE_LIBSPONGE_NETWORK_INTERFACE_HH

#include "ethernet_frame.hh"
#include "tcp_over_ip.hh"
#include "tun.hh"

#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

//! \brief A "network interface" that connects IP (the internet layer, or network layer)
//! with Ethernet (the network access layer, or link layer).

//! This module is the lowest layer of a TCP/IP stack
//! (connecting IP with the lower-layer network protocol,
//! e.g. Ethernet). But the same module is also used repeatedly
//! as part of a router: a router generally has many network
//! interfaces, and the router's job is to route Internet datagrams
//! between the different interfaces.

//! The network interface translates datagrams (coming from the
//! "customer," e.g. a TCP/IP stack or router) into Ethernet
//! frames. To fill in the Ethernet destination address, it looks up
//! the Ethernet address of the next IP hop of each datagram, making
//! requests with the [Address Resolution Protocol](\ref rfc::rfc826).
//! In the opposite direction, the network interface accepts Ethernet
//! frames, checks if they are intended for it, and if so, processes
//! the the payload depending on its type. If it's an IPv4 datagram,
//! the network interface passes it up the stack. If it's an ARP
//! request or reply, the network interface processes the frame
//! and learns or replies as necessary.
class NetworkInterface {
  private:
    //! Ethernet (known as hardware, network-access-layer, or link-layer) address of the interface
    EthernetAddress _ethernet_address;

    //! IP (known as internet-layer or network-layer) address of the interface
    Address _ip_address;

    //! outbound queue of Ethernet frames that the NetworkInterface wants sent
    std::queue<EthernetFrame> _frames_out{};

    static constexpr size_t ARP_ENTRY_TTL = 30000;      //!< ARP 映射条目缓存有效期 30 秒 (30,000 ms)
    static constexpr size_t ARP_REQUEST_TTL = 5000;     //!< 同一 IP 重复发送 ARP 请求的抑制窗口 5 秒 (5,000 ms)

    struct ArpEntry {
        EthernetAddress mac{};
        size_t ttl{ARP_ENTRY_TTL};
    };

    std::unordered_map<uint32_t, ArpEntry> _arp_table{};                           //!< ARP 缓存条目映射表 (IP -> {MAC, TTL})
    std::unordered_map<uint32_t, std::vector<InternetDatagram>> _waiting_dgrams{}; //!< 未决 IP 数据报等待队列 (IP -> 数据报列表)
    std::unordered_map<uint32_t, size_t> _arp_request_timer{};                     //!< ARP 请求限频计时器 (IP -> 剩余静默时间)

    //! 构造并广播一个 ARP 请求帧
    void _send_arp_request(const uint32_t ip);

    //! 构造并单播回复一个 ARP 应答帧
    void _send_arp_reply(const EthernetAddress &target_eth, const uint32_t target_ip);

    //! 将 IPv4 数据报封装为以太网帧并发入 _frames_out
    void send_ipv4_frame(const EthernetAddress &dst_eth, const InternetDatagram &dgram);

  public:
    //! \brief Construct a network interface with given Ethernet (network-access-layer) and IP (internet-layer) addresses
    NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address);

    //! \brief Access queue of Ethernet frames awaiting transmission
    std::queue<EthernetFrame> &frames_out() { return _frames_out; }

    //! \brief Sends an IPv4 datagram, encapsulated in an Ethernet frame (if it knows the Ethernet destination address).

    //! Will need to use [ARP](\ref rfc::rfc826) to look up the Ethernet destination address for the next hop
    //! ("Sending" is accomplished by pushing the frame onto the frames_out queue.)
    void send_datagram(const InternetDatagram &dgram, const Address &next_hop);

    //! \brief Receives an Ethernet frame and responds appropriately.

    //! If type is IPv4, returns the datagram.
    //! If type is ARP request, learn a mapping from the "sender" fields, and send an ARP reply.
    //! If type is ARP reply, learn a mapping from the "target" fields.
    std::optional<InternetDatagram> recv_frame(const EthernetFrame &frame);

    //! \brief Called periodically when time elapses
    void tick(const size_t ms_since_last_tick);
};

#endif  // SPONGE_LIBSPONGE_NETWORK_INTERFACE_HH
