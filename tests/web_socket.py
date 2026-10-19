"""Small WebSocket client shared by the isolated daemon web checks."""
import base64
import hashlib
import json
import os
import socket
import struct
import time


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(port, path, headers):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    req = f'GET {path} HTTP/1.1\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in headers.items()) + '\r\n'
    s.sendall(req.encode())
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    head, _, body = data.partition(b'\r\n\r\n')
    status = int(head.split(b' ')[1])
    return s, status, head.decode('latin-1'), body


class WebSocket:
    def __init__(self, port, token, origin=None, host=None, in_query=False):
        host = host or f'127.0.0.1:{port}'
        self.key = base64.b64encode(os.urandom(16)).decode()
        headers = {'Host': host, 'Origin': origin or f'http://{host}', 'Upgrade': 'websocket',
                   'Connection': 'Upgrade', 'Sec-WebSocket-Key': self.key, 'Sec-WebSocket-Version': '13'}
        if not in_query:
            headers['Sec-WebSocket-Protocol'] = f'mico, mico-token.{token}'
        path = f'/ws?token={token}' if in_query else '/ws'
        self.sock, self.status, self.head, rest = http(port, path, headers)
        self.buf = bytearray(rest)

    def accept_ok(self):
        want = base64.b64encode(hashlib.sha1((self.key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        return f'Sec-WebSocket-Accept: {want.decode()}' in self.head

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def messages(self, seconds):
        out = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while True:
                if len(self.buf) < 2:
                    break
                n, at = self.buf[1] & 0x7F, 2
                if n == 126:
                    n, at = struct.unpack_from('>H', self.buf, 2)[0], 4
                elif n == 127:
                    n, at = struct.unpack_from('>Q', self.buf, 2)[0], 10
                if len(self.buf) < at + n:
                    break
                if self.buf[0] & 0x0F == 1:
                    out.append(json.loads(self.buf[at:at + n].decode()))
                del self.buf[:at + n]
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                data = self.sock.recv(1 << 20)
            except socket.timeout:
                break
            if not data:
                break
            self.buf.extend(data)
        return out
