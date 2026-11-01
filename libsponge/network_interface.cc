#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    // 查询 ARP 缓存
    const auto arp_it = _arp_table.find(next_hop_ip);
    if (arp_it != _arp_table.end()) {
        send_ipv4_frame(arp_it->second.mac, dgram);
        return;
    }

    // 未命中：加入未决数据报等待队列
    _waiting_dgrams[next_hop_ip].push_back(dgram);

    // ARP 请求限频检查
    if (_arp_request_timer.find(next_hop_ip) == _arp_request_timer.end()) {
        _send_arp_request(next_hop_ip);
        _arp_request_timer[next_hop_ip] = ARP_REQUEST_TTL;
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // 过滤非发往本接口的帧
    const EthernetAddress &dst_mac = frame.header().dst;
    if (dst_mac != _ethernet_address && dst_mac != ETHERNET_BROADCAST) {
        return std::nullopt;
    }

    // IPv4 数据报帧
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
        return std::nullopt;
    }

    // ARP 报文帧
    if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp_msg;
        if (arp_msg.parse(frame.payload()) != ParseResult::NoError || !arp_msg.supported()) {
            return std::nullopt;
        }

        // 无条件从 ARP 报文中学习发送方映射 (IP -> MAC)，重置存活期 30 秒
        _arp_table[arp_msg.sender_ip_address] = {arp_msg.sender_ethernet_address, ARP_ENTRY_TTL};

        // 冲刷所有等待该 IP 的未决数据报
        const auto wait_it = _waiting_dgrams.find(arp_msg.sender_ip_address);
        if (wait_it != _waiting_dgrams.end()) {
            for (const auto &dgram : wait_it->second) {
                send_ipv4_frame(arp_msg.sender_ethernet_address, dgram);
            }
            _waiting_dgrams.erase(wait_it);
        }

        // 若收到了 ARP 请求且询问的目标正是本机 IP，则构造并单播回复 ARP Reply
        if (arp_msg.opcode == ARPMessage::OPCODE_REQUEST &&
            arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
            _send_arp_reply(arp_msg.sender_ethernet_address, arp_msg.sender_ip_address);
        }
    }

    return std::nullopt;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    // ARP 映射缓存条目判定
    for (auto it = _arp_table.begin(); it != _arp_table.end();) {
        if (it->second.ttl <= ms_since_last_tick) {
            it = _arp_table.erase(it);
        } else {
            it->second.ttl -= ms_since_last_tick;
            ++it;
        }
    }

    // ARP 请求限频递减
    for (auto it = _arp_request_timer.begin(); it != _arp_request_timer.end();) {
        if (it->second <= ms_since_last_tick) {
            it = _arp_request_timer.erase(it);
        } else {
            it->second -= ms_since_last_tick;
            ++it;
        }
    }
}

void NetworkInterface::_send_arp_request(const uint32_t ip) {
    ARPMessage arp_msg;
    arp_msg.opcode                  = ARPMessage::OPCODE_REQUEST;
    arp_msg.sender_ethernet_address = _ethernet_address;
    arp_msg.sender_ip_address       = _ip_address.ipv4_numeric();
    arp_msg.target_ethernet_address = {};
    arp_msg.target_ip_address       = ip;

    EthernetFrame ef;
    ef.header().src  = _ethernet_address;
    ef.header().dst  = ETHERNET_BROADCAST;
    ef.header().type = EthernetHeader::TYPE_ARP;
    ef.payload()     = arp_msg.serialize();

    _frames_out.push(ef);
}

void NetworkInterface::_send_arp_reply(const EthernetAddress &target_eth, const uint32_t target_ip) {
    ARPMessage arp_msg;
    arp_msg.opcode                  = ARPMessage::OPCODE_REPLY;
    arp_msg.sender_ethernet_address = _ethernet_address;
    arp_msg.sender_ip_address       = _ip_address.ipv4_numeric();
    arp_msg.target_ethernet_address = target_eth;
    arp_msg.target_ip_address       = target_ip;

    EthernetFrame ef;
    ef.header().src  = _ethernet_address;
    ef.header().dst  = target_eth;
    ef.header().type = EthernetHeader::TYPE_ARP;
    ef.payload()     = arp_msg.serialize();

    _frames_out.push(ef);
}

void NetworkInterface::send_ipv4_frame(const EthernetAddress &dst_eth, const InternetDatagram &dgram) {
    EthernetFrame ef;
    ef.header().src  = _ethernet_address;
    ef.header().dst  = dst_eth;
    ef.header().type = EthernetHeader::TYPE_IPv4;
    ef.payload()     = dgram.serialize();

    _frames_out.push(ef);
}
