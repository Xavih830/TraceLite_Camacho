#!/usr/bin/env python3
import hashlib
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MOSTRAR_CABECERAS = "--mostrar-cabeceras" in sys.argv


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):
        return

    def do_GET(self):
        if MOSTRAR_CABECERAS:
            tp = self.headers.get("traceparent", "")
            print(f"[backend_demo] traceparent={tp}", file=sys.stderr, flush=True)

        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        if parsed.path == "/hash":
            try:
                mb = max(1, int(qs.get("mb", ["1"])[0]))
            except ValueError:
                mb = 1
            bloque = b"tracelite_sha256_" * 64
            rep = (mb * 1024 * 1024) // len(bloque) + 1
            datos = (bloque * rep)[: mb * 1024 * 1024]
            digest = hashlib.sha256(datos).hexdigest().encode("ascii") + b"\n"
            self.send_response(200)
            self.send_header("Content-Length", str(len(digest)))
            self.end_headers()
            self.wfile.write(digest)
        elif parsed.path == "/espera":
            try:
                ms = max(0, int(qs.get("ms", ["100"])[0]))
            except ValueError:
                ms = 100
            time.sleep(ms / 1000.0)
            cuerpo = f"ok {ms} ms\n".encode("ascii")
            self.send_response(200)
            self.send_header("Content-Length", str(len(cuerpo)))
            self.end_headers()
            self.wfile.write(cuerpo)
        elif parsed.path == "/error":
            cuerpo = b"error interno simulado\n"
            self.send_response(500)
            self.send_header("Content-Length", str(len(cuerpo)))
            self.end_headers()
            self.wfile.write(cuerpo)
        else:
            cuerpo = b"no encontrado\n"
            self.send_response(404)
            self.send_header("Content-Length", str(len(cuerpo)))
            self.end_headers()
            self.wfile.write(cuerpo)


def main():
    puerto = 8000
    for arg in sys.argv[1:]:
        if arg.isdigit():
            puerto = int(arg)
            break
    srv = ThreadingHTTPServer(("127.0.0.1", puerto), Handler)
    srv.serve_forever()


if __name__ == "__main__":
    main()
