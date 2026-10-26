"""Real WebSocket upgrades, fragmented messages, and lifecycle checks on loopback."""
import base64
import hashlib
import http.server
import json
import socket
import struct
import subprocess
import sys
import threading

MESSAGE = b'{"event_type":"last_trade_price","asset_id":"18446744073709551616","price":"0.5","size":"1","timestamp":9223372036854775807}'


def frame(opcode, data, final=True):
    head = bytes([(0x80 if final else 0) | opcode])
    if len(data) < 126:
        return head + bytes([len(data)]) + data
    return head + b"\x7e" + struct.pack("!H", len(data)) + data


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    failures = []
    subscriptions = []
    requests = []

    def handle_error(self, request, client_address):
        self.failures.append(sys.exc_info()[1])


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        protocol = self.headers.get("Sec-WebSocket-Protocol")
        if protocol:
            self.send_header("Sec-WebSocket-Protocol", protocol.split(",")[0].strip())
        self.end_headers()
        self.server.requests.append((self.path, dict(self.headers)))
        self.connection.settimeout(3)
        try:
            if self.path == "/ws/user":
                self.connection.sendall(frame(1, b'{"event_type":"order","type":"PLACEMENT"}') + frame(1, b'{"event_type":"trade","type":"PLACEMENT"}'))
            else:
                if self.path == "/ws/market":
                    self.connection.sendall(frame(1, b'{"event_type":"price_change","price_changes":[{"asset_id":"1","price":"0.5"}]}') + frame(1, b"PONG") + frame(1, b'{"event_type":"tick_size_change","asset_id":"1","new_tick_size":"0.00001"}'))
                self.connection.sendall(frame(1, MESSAGE[:37], False) + frame(0, MESSAGE[37:]))
            while True:
                header = self.rfile.read(2)
                if len(header) != 2:
                    break
                size = header[1] & 127
                if size == 126:
                    size = struct.unpack("!H", self.rfile.read(2))[0]
                elif size == 127:
                    size = struct.unpack("!Q", self.rfile.read(8))[0]
                assert size <= 1024 * 1024
                mask = self.rfile.read(4) if header[1] & 128 else None
                data = self.rfile.read(size)
                if mask:
                    data = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
                if header[0] & 15 == 8:
                    break
                if data.startswith(b"{"):
                    self.server.subscriptions.append(json.loads(data))
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass
        self.close_connection = True


server = Server(("127.0.0.1", 0), Handler)
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
    for kind in ["us", "clob"]:
        for mode in ["destroy", "throw", "move", "reconnect", "subscribe"]:
            result = subprocess.run([sys.argv[1], kind, str(server.server_port), mode], capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, (kind, mode, result.returncode, result.stdout, result.stderr)
            assert not server.failures, server.failures

    for mode in ["user", "tick"]:
        result = subprocess.run([sys.argv[1], "clob", str(server.server_port), mode], capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
        assert not server.failures, server.failures
    assert any(item.get("type") == "market" and item.get("custom_feature_enabled") is True and item.get("assets_ids") == ["1"] for item in server.subscriptions), server.subscriptions
    assert any(item.get("operation") == "subscribe" and item.get("assets_ids") == ["2"] for item in server.subscriptions), server.subscriptions
    assert any(item.get("operation") == "unsubscribe" and item.get("assets_ids") == ["1"] for item in server.subscriptions), server.subscriptions
    assert any(item.get("auth") == {"apiKey": 'quoted"key', "secret": "AA==", "passphrase": "pass\\phrase"} and item.get("type") == "user" for item in server.subscriptions), server.subscriptions
    assert any(item.get("operation") == "subscribe" and item.get("markets") for item in server.subscriptions), server.subscriptions
    assert any(item.get("operation") == "unsubscribe" and item.get("markets") for item in server.subscriptions), server.subscriptions
    assert all(not any(name.lower().startswith("poly_") for name in headers) for path, headers in server.requests if path == "/ws/user")
    assert any(item.get("subscribe", {}).get("marketSlugs") == ['quoted"\\\nslug'] for item in server.subscriptions), server.subscriptions
finally:
    server.shutdown()
    server.server_close()
    worker.join()
print("US/CLOB lifecycle, fragments, channel/auth/subscription wire types, PONG, and invalid tick checks pass")
