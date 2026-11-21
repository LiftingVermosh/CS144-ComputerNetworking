Lab 6 Writeup
=============

My name: Vermosh

My SUNet ID: [your sunetid here]

This lab took me about 2 hours to do. I did not attend the lab session.

Program Structure and Design of the Router:
The `Router` coordinates multiple network interfaces (`AsyncNetworkInterface`)
and forwards incoming Internet datagrams to the appropriate next hop and outbound
interface based on a routing table implementing the Longest Prefix Match (LPM) algorithm.

Key design highlights:
1. Route Table Representation (`RouteEntry`):
   - `route_prefix`: 32-bit numeric IP prefix.
   - `prefix_length`: Number of significant prefix bits (0 to 32).
   - `next_hop`: Optional next-hop router IP address (`std::optional<Address>`).
   - `interface_num`: Outgoing interface index.
   - `_routing_table`: Stored as a `std::vector<RouteEntry>`, enabling straightforward
     O(N) sequential search that is optimal for small-to-medium routing tables.

2. Longest Prefix Match Forwarding (`route_one_datagram`):
   - Matching: For each rule in `_routing_table`, checks whether the most-significant
     `prefix_length` bits of the datagram's destination IP match `route_prefix`.
     Special-cases `prefix_length == 0` (default route) to avoid C++ undefined
     behavior when shifting a 32-bit integer by 32 bits.
   - Best Match Selection: Selects the matching rule with the strictly largest
     `prefix_length`.
   - Miss Handling: Drops the datagram immediately if no matching route is found.
   - TTL Expiration: Drops the datagram if `ttl <= 1` (expired or drops to zero).
     Otherwise, decrements `ttl` by 1.
   - Next-Hop Resolution:
     * Indirect Hop: If `next_hop` has a value, forwards to `next_hop.value()`.
     * Direct Hop: If `next_hop` is nullopt (directly attached network), forwards
       to `Address::from_ipv4_numeric(dst_ip)`.
   - Transmission: Forwards the updated datagram out on `interface(interface_num)`.

3. Router Polling Loop (`route`):
   - Iterates through all registered network interfaces, drains all queued incoming
     datagrams from each interface's `datagrams_out()` queue, and dispatches them
     via `route_one_datagram`.

Implementation Challenges:
1. Undefined Behavior in Bit-Shifting: Shifting a `uint32_t` by 32 bits is undefined
   in C/C++. For default routes (`prefix_length == 0`), `32 - prefix_length == 32`,
   which causes sanitizers to abort. Resolved by short-circuiting `prefix_length == 0`
   to unconditionally evaluate to `true`.
2. Direct vs. Indirect Next-Hop: Handled the distinction between networks with a gateway
   router (`next_hop.has_value()`) versus directly attached networks where the datagram's
   own destination IP must serve as the link-layer next hop.
3. Variable Scope Typo: Fixed an undeclared identifier typo (`match_route` vs `best_route`)
   during outbound interface transmission.

Remaining Bugs:
None. All automated tests in `network_simulator` and `check_lab6` pass cleanly.
