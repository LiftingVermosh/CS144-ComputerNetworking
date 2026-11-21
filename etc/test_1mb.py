#!/usr/bin/env python3
import os
import subprocess
import time
import hashlib

def main():
    os.chdir('/home/vermosh/projects/CS144_ComputerNetwork')

    # Create 1MB test file if not exists
    test_data = os.urandom(1024 * 1024) # 1MB
    input_file = "/tmp/big.txt"
    output_file = "/tmp/big-received.txt"
    with open(input_file, "wb") as f:
        f.write(test_data)
    expected_hash = hashlib.sha256(test_data).hexdigest()
    print(f"Generated 1MB random file, SHA-256: {expected_hash}")

    # 1. Start relay
    relay_proc = subprocess.Popen(['python3', 'etc/relay.py', '4000'])
    time.sleep(0.3)

    # 2. Start server sending the file
    with open(input_file, "rb") as fin:
        server_proc = subprocess.Popen(
            ['./build/apps/endtoend', 'server', '127.0.0.1', '4000'],
            stdin=fin,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE
        )
    time.sleep(0.3)

    # 3. Start client receiving the file
    with open(output_file, "wb") as fout, open("/dev/null", "rb") as fnull:
        client_proc = subprocess.Popen(
            ['./build/apps/endtoend', 'client', '127.0.0.1', '4001'],
            stdin=fnull,
            stdout=fout,
            stderr=subprocess.PIPE
        )

    print("Transferring 1MB file across the network stack...")
    start_time = time.time()
    # Wait for completion (up to 60 seconds)
    while time.time() - start_time < 60:
        if server_proc.poll() is not None and client_proc.poll() is not None:
            break
        time.sleep(0.5)

    duration = time.time() - start_time
    print(f"Transfer finished in {duration:.2f} seconds. Server exit: {server_proc.poll()}, Client exit: {client_proc.poll()}")

    relay_proc.terminate()
    if server_proc.poll() is None:
        server_proc.terminate()
    if client_proc.poll() is None:
        client_proc.terminate()

    # Verify received file
    if os.path.exists(output_file):
        with open(output_file, "rb") as f:
            received_data = f.read()
        received_hash = hashlib.sha256(received_data).hexdigest()
        print(f"Received size: {len(received_data)} bytes")
        print(f"Received SHA-256: {received_hash}")
        if received_hash == expected_hash:
            print("SUCCESS! SHA-256 hashes match perfectly!")
        else:
            print("FAILURE! Hash mismatch!")
    else:
        print("Output file does not exist!")

if __name__ == "__main__":
    main()
