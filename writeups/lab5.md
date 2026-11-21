Lab 5 Writeup
=============

My name: Vermosh

My SUNet ID: [your sunetid here]

This lab took me about 3 hours to do. I did not attend the lab session.

Program Structure and Design of the NetworkInterface:
The `NetworkInterface` acts as the bridge connecting the Internet layer (IPv4)
and the network access layer (Ethernet). It encapsulates IP datagrams into Ethernet
frames for transmission and decapsulates incoming frames according to the Address
Resolution Protocol (ARP, RFC 826).

Key design highlights:
1. Data Structures & State Management:
   - `_arp_table`: Maps 32-bit IPv4 addresses to their corresponding EthernetAddress
     and remaining TTL (initialized to 30 seconds / 30,000 ms).
   - `_waiting_dgrams`: Maps destination IPv4 addresses to queues (`std::vector<InternetDatagram>`)
     of outgoing datagrams waiting for their link-layer address to be resolved.
   - `_arp_request_timer`: Maps target IPv4 addresses to remaining rate-limit countdowns
     (initialized to 5 seconds / 5,000 ms) to suppress duplicate ARP broadcast floods.

2. Outbound Datagram Transmission (`send_datagram`):
   - Hits ARP cache: If the next hop's Ethernet address is already cached, it wraps
     the serialized datagram into an Ethernet frame (`TYPE_IPv4`) and pushes it
     directly to `_frames_out`.
   - Misses ARP cache: Queues the datagram in `_waiting_dgrams[next_hop_ip]`. If no
     ARP request has been broadcast for this IP within the last 5 seconds, broadcasts
     an ARP request (`OPCODE_REQUEST` to `ETHERNET_BROADCAST`) and starts the 5s timer.

3. Inbound Frame Reception & Dispatching (`recv_frame`):
   - Filtering: Drops frames whose destination MAC is neither the broadcast address
     nor the interface's own address.
   - IPv4 Frame: Parses payload as `InternetDatagram` and returns it directly if valid.
   - ARP Frame: Parses payload as `ARPMessage` and verifies `supported()`.
     * Snooping / Cache Update: Unconditionally records/refreshes the mapping between
       `sender_ip_address` and `sender_ethernet_address` with a 30-second TTL.
     * Pending Queue Flushing: Dispatches all queued datagrams for `sender_ip_address`
       to `_frames_out` as IPv4 frames.
     * Request Answering: If the frame is an `OPCODE_REQUEST` querying the interface's
       own IP, sends a unicast `OPCODE_REPLY` back to the requester.

4. Soft State Expiration (`tick`):
   - Decrements TTLs for entries in `_arp_table` and `_arp_request_timer`.
   - Safely erases expired entries using `it = map.erase(it)` to prevent iterator invalidation.

Implementation Challenges:
1. Iterator Invalidation in `tick`: Erasing expired entries from `std::unordered_map`
   during traversal requires using iterator reassignment (`it = map.erase(it)`) instead
   of range-based for loops to avoid undefined behavior.
2. Rate-Limiting Semantics: Ensuring the 5-second suppression timer triggers only on
   cache misses when no timer exists, and maintaining independence across different
   destination IPs.
3. Queue Flushing Order: Ensuring all datagrams waiting for a resolved MAC address
   are sent out sequentially in FIFO order without losing packets.

Remaining Bugs:
None. All 6 ARP network interface test scenarios (`typical ARP workflow`, `reply to ARP request`,
`learn from ARP request`, `pending mappings last five seconds`, `active mappings last 30 seconds`,
`different ARP mappings are independent`) pass with 100% success.
