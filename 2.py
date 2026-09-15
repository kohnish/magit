#!/usr/bin/env python3
import subprocess
import struct
import sys
import msgpack

SERVER = "/home/kohei/.emacs.d/elpa/magit/cmake-build-debug/src/magit-server"


def encode_str(s: str) -> bytes:
    b = s.encode("utf-8")
    n = len(b)

    if n <= 31:
        return bytes([0xa0 + n]) + b
    elif n <= 255:
        return bytes([0xd9, n]) + b
    elif n <= 65535:
        return bytes([0xda]) + struct.pack(">H", n) + b
    else:
        return bytes([0xdb]) + struct.pack(">I", n) + b


def encode_uint(n: int) -> bytes:
    if 0 <= n <= 127:
        return bytes([n])
    elif n <= 255:
        return bytes([0xcc, n])
    elif n <= 65535:
        return bytes([0xcd]) + struct.pack(">H", n)
    else:
        return bytes([0xce]) + struct.pack(">I", n)


def encode_status_msg(request_id: int, cmd_id: int, default_dir: str) -> bytes:
    body = (
        b"\x83"
        + b"\x00" + encode_uint(request_id)
        + b"\x01" + encode_uint(cmd_id)
        + b"\x02" + encode_str(default_dir)
    )
    return struct.pack(">I", len(body)) + body


def read_exact(f, n):
    data = b""

    while len(data) < n:
        chunk = f.read(n - len(data))
        if not chunk:
            raise EOFError("server closed connection")
        data += chunk

    return data


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    default_dir = root.rstrip("/") + "/"

    proc = subprocess.Popen(
        [SERVER, root],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        bufsize=0,
    )

    try:
        msg = encode_status_msg(
            request_id=1,
            cmd_id=1,
            default_dir=default_dir,
        )

        proc.stdin.write(msg)
        proc.stdin.flush()

        len_bytes = read_exact(proc.stdout, 4)
        (body_len,) = struct.unpack(">I", len_bytes)

        payload = read_exact(proc.stdout, body_len)

        print(msgpack.unpackb(payload, strict_map_key=False))

    finally:
        proc.terminate()
        proc.wait()


if __name__ == "__main__":
    main()
