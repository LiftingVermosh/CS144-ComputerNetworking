#!/usr/bin/env python3
import os
import subprocess
import time
import sys

def main():
    os.chdir('/home/vermosh/projects/CS144_ComputerNetwork')

    # 1. Start relay
    relay_proc = subprocess.Popen(['python3', 'etc/relay.py', '3000'])
    time.sleep(0.3)

    # 2. Start server
    server_proc = subprocess.Popen(
        ['./build/apps/endtoend', 'server', '127.0.0.1', '3000'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    time.sleep(0.3)

    # 3. Start client
    client_proc = subprocess.Popen(
        ['./build/apps/endtoend', 'client', '127.0.0.1', '3001'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    time.sleep(2.0)
    print(f"Status check - Server poll: {server_proc.poll()}, Client poll: {client_proc.poll()}")

    msg = b"Hello CS144 Lab 7!\n"
    print(f"Sending message from client: {msg}")
    client_proc.stdin.write(msg)
    client_proc.stdin.flush()

    time.sleep(2.0)

    # Close input
    client_proc.stdin.close()
    server_proc.stdin.close()
    client_proc.stdin = None
    server_proc.stdin = None

    time.sleep(1.0)
    client_proc.terminate()
    server_proc.terminate()
    relay_proc.terminate()

    s_out, s_err = server_proc.communicate()
    c_out, c_err = client_proc.communicate()

    print("\n--- Server Stdout ---")
    print(s_out.decode('utf-8', errors='ignore'))

    print("--- Server Stderr (tail) ---")
    print("\n".join(s_err.decode('utf-8', errors='ignore').splitlines()[-10:]))

    print("\n--- Client Stderr (tail) ---")
    print("\n".join(c_err.decode('utf-8', errors='ignore').splitlines()[-10:]))

if __name__ == "__main__":
    main()
