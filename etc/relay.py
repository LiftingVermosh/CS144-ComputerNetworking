#!/usr/bin/env python3
"""
Simple UDP Relay Bouncer for CS144 Lab 7 / Checkpoint 7.
Simulates the relay server (e.g. cs144.keithw.org).
Pairs port N (Server) and port N+1 (Client) on localhost or LAN.
"""

import socket
import select
import sys

def main():
    base_port = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    host = "0.0.0.0"

    even_port = base_port
    odd_port = base_port + 1

    s_even = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_even.bind((host, even_port))

    s_odd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_odd.bind((host, odd_port))

    print(f"[Relay] Listening on UDP {host}:{even_port} (Server) and {host}:{odd_port} (Client)...")
    print("[Relay] Ready to forward packets between peers. Press Ctrl+C to stop.")

    even_peer = None
    odd_peer = None

    try:
        while True:
            rlist, _, _ = select.select([s_even, s_odd], [], [])
            if s_even in rlist:
                data, addr = s_even.recvfrom(65535)
                if addr != even_peer:
                    even_peer = addr
                    print(f"[Relay] Server registered from {addr}")
                # Forward to client if we know client's address and data is non-empty
                if data and odd_peer:
                    s_odd.sendto(data, odd_peer)

            if s_odd in rlist:
                data, addr = s_odd.recvfrom(65535)
                if addr != odd_peer:
                    odd_peer = addr
                    print(f"[Relay] Client registered from {addr}")
                # Forward to server if we know server's address and data is non-empty
                if data and even_peer:
                    s_even.sendto(data, even_peer)

    except KeyboardInterrupt:
        print("\n[Relay] Stopping...")
    finally:
        s_even.close()
        s_odd.close()

if __name__ == "__main__":
    main()
