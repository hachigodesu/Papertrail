#!/usr/bin/env python3
"""Minimal local DNS resolver that forwards queries over HTTPS (RFC 8484).

For WSL distros on a network where port 53 is blocked but HTTPS works.
Usage: papertrail-doh.py LISTEN_ADDRESS
"""
import socketserver
import ssl
import struct
import sys
import urllib.request

URL = "https://1.1.1.1/dns-query"
HEADERS = {
    "content-type": "application/dns-message",
    "accept": "application/dns-message",
}
TLS = ssl.create_default_context()


def resolve(query):
    try:
        request = urllib.request.Request(URL, data=query, headers=HEADERS, method="POST")
        with urllib.request.urlopen(request, timeout=10, context=TLS) as response:
            return response.read()
    except Exception as error:  # Any failure becomes a DNS error, not a dead thread.
        print(f"lookup failed: {error}", file=sys.stderr, flush=True)
        return query[:2] + b"\x81\x82" + query[4:]  # SERVFAIL, echoing the question.


class UdpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        query, sock = self.request
        sock.sendto(resolve(query), self.client_address)


class TcpHandler(socketserver.StreamRequestHandler):
    def handle(self):
        header = self.rfile.read(2)
        if len(header) == 2:
            reply = resolve(self.rfile.read(struct.unpack(">H", header)[0]))
            self.wfile.write(struct.pack(">H", len(reply)) + reply)


class UdpServer(socketserver.ThreadingUDPServer):
    allow_reuse_address = True
    daemon_threads = True


class TcpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    address = (sys.argv[1], 53)
    tcp = TcpServer(address, TcpHandler)
    import threading
    threading.Thread(target=tcp.serve_forever, daemon=True).start()
    print(f"listening on {address[0]}:53", flush=True)
    UdpServer(address, UdpHandler).serve_forever()


if __name__ == "__main__":
    main()
