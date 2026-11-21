#include "router.hh"

#include <iostream>

using namespace std;

// Dummy implementation of an IP router

// Given an incoming Internet datagram, the router decides
// (1) which interface to send it out on, and
// (2) what next hop address to send it to.

// For Lab 6, please replace with a real implementation that passes the
// automated checks run by `make check_lab6`.

// You will need to add private members to the class declaration in `router.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

//! \param[in] route_prefix The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
//! \param[in] prefix_length For this route to be applicable, how many high-order (most-significant) bits of the route_prefix will need to match the corresponding bits of the datagram's destination address?
//! \param[in] next_hop The IP address of the next hop. Will be empty if the network is directly attached to the router (in which case, the next hop address should be the datagram's final destination).
//! \param[in] interface_num The index of the interface to send the datagram out on.
void Router::add_route(const uint32_t route_prefix,
                       const uint8_t prefix_length,
                       const optional<Address> next_hop,
                       const size_t interface_num) {
    RouteEntry route;
    route.route_prefix  = route_prefix;
    route.prefix_length = prefix_length;
    route.interface_num = interface_num;
    route.next_hop      = next_hop;

    _routing_table.push_back(route);
}

//! \param[in] dgram The datagram to be routed
void Router::route_one_datagram(InternetDatagram &dgram) {
    const RouteEntry *best_route = nullptr;
    int max_prefix_len = -1;

    uint32_t dst_ip = dgram.header().dst;

    // LPM
    for (const auto &entry : _routing_table) {
        bool is_match = (entry.prefix_length == 0) ||
                        ((dst_ip >> (32 - entry.prefix_length)) == (entry.route_prefix >> (32 - entry.prefix_length)));

        // 当前缀长度大于之前找到的最大长度时，更新最佳匹配
        if (is_match && static_cast<int>(entry.prefix_length) > max_prefix_len) {
            max_prefix_len = entry.prefix_length;
            best_route = &entry;
        }
    }

    // Hit Check
    if (best_route == nullptr) {
        return; // 丢弃数据报
    }

    // Ttl Check
    if(dgram.header().ttl <= 1) {
        return;
    } else {
        dgram.header().ttl--;
    }

    // next hop
    const Address next_hop = best_route->next_hop.has_value() ? best_route->next_hop.value() : Address::from_ipv4_numeric(dst_ip);
    
    // send 2 interface
    interface(best_route->interface_num).send_datagram(dgram, next_hop);
}

void Router::route() {
    // Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
    for (auto &interface : _interfaces) {
        auto &queue = interface.datagrams_out();
        while (not queue.empty()) {
            route_one_datagram(queue.front());
            queue.pop();
        }
    }
}
