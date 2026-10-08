"""
Black-box tests for bserve and bcurl.

The frame code in this file is written from SPEC.md and shares nothing with the C
sources. Passing means each program agrees with the spec, not just with the other one.

    make test        (or: python3 -m unittest discover -s tests -v)
"""
import os
import random
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BSERVE = os.path.join(REPO, "bserve")
BCURL = os.path.join(REPO, "bcurl")

PREFACE = b"BHTP/1\r\n"
REQUEST, RESPONSE, DATA, GOAWAY = 1, 2, 3, 4
END_STREAM = 0x01
GET, HEAD, POST, PUT, DELETE = 1, 2, 3, 4, 5
NO_ERROR, PROTOCOL_ERROR, BAD_PREFACE = 0, 1, 2
TABLE = [None, "host", "user-agent", "accept", "if-none-match", "server", "date",
         "content-type", "content-length", "last-modified", "etag"]


# ---- a tiny codec, straight from the spec ----

def frame(ftype, stream, payload=b"", flags=0):
    return struct.pack(">HBBI", len(payload), ftype, flags, stream) + payload


def field(name, value):
    value = value.encode() if isinstance(value, str) else value
    if name in TABLE:
        head = bytes([0x80 | TABLE.index(name)])
    else:
        head = bytes([len(name)]) + name.encode()
    return head + struct.pack(">H", len(value)) + value


def request(stream, path, method=GET, headers=None, flags=END_STREAM):
    path = path.encode() if isinstance(path, str) else path
    body = struct.pack(">BH", method, len(path)) + path
    for name, value in (headers or [("host", "test")]):
        body += field(name, value)
    return frame(REQUEST, stream, body, flags)


def parse_fields(buf):
    out = {}
    while buf:
        first, buf = buf[0], buf[1:]
        if first & 0x80:
            name = TABLE[first & 0x7F] if (first & 0x7F) <= 10 else None
        else:
            name, buf = buf[:first].decode(), buf[first:]
        (n,) = struct.unpack(">H", buf[:2])
        if name:
            out[name] = buf[2:2 + n].decode()
        buf = buf[2 + n:]
    return out


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_bserve(root):
    """Starts bserve on a free port and waits until it accepts. Returns (proc, port)."""
    port = free_port()
    proc = subprocess.Popen([BSERVE, root, str(port)], stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("bserve did not start")


class Conn:
    def __init__(self, port, preface=PREFACE):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        if preface:
            self.sock.sendall(preface)

    def send(self, data):
        self.sock.sendall(data)

    def exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("connection closed")
            buf += chunk
        return buf

    def frame(self):
        length, ftype, flags, stream = struct.unpack(">HBBI", self.exact(8))
        return ftype, flags, stream, self.exact(length)

    def response(self):
        """Reads one whole response. Returns (stream, status, fields, body, data_frames)."""
        ftype, flags, stream, payload = self.frame()
        assert ftype == RESPONSE, "expected RESPONSE, got type %d" % ftype
        (status,) = struct.unpack(">H", payload[:2])
        fields = parse_fields(payload[2:])
        body, frames = b"", []
        while not flags & END_STREAM:
            ftype, flags, sid, payload = self.frame()
            assert ftype == DATA and sid == stream
            frames.append(len(payload))
            body += payload
        return stream, status, fields, body, frames

    def closed(self):
        try:
            return self.sock.recv(1) == b""
        except (ConnectionResetError, socket.timeout):
            return True

    def close(self):
        self.sock.close()


# ---- bserve ----

class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.root = os.path.join(cls.tmp, "www")
        os.makedirs(os.path.join(cls.root, "sub"))
        cls.files = {
            "index.html": b"<h1>hi</h1>\n",
            "hello.txt": b"hello, world\n",
            "empty.txt": b"",
            "with space.txt": b"spaced\n",
            "a#b.txt": b"hash\n",
            "sub/index.html": b"<p>sub</p>\n",
            "big.bin": random.Random(7).randbytes(200_000),
        }
        for name, data in cls.files.items():
            with open(os.path.join(cls.root, name), "wb") as f:
                f.write(data)
        with open(os.path.join(cls.tmp, "secret.txt"), "w") as f:
            f.write("outside the root\n")
        os.symlink(os.path.join(cls.tmp, "secret.txt"), os.path.join(cls.root, "escape.txt"))

        cls.proc, cls.port = start_bserve(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        shutil.rmtree(cls.tmp)

    def setUp(self):
        self.c = Conn(self.port)

    def tearDown(self):
        self.c.close()

    def get(self, path, stream=1, **kw):
        self.c.send(request(stream, path, **kw))
        return self.c.response()

    def test_get(self):
        stream, status, fields, body, _ = self.get("/hello.txt")
        self.assertEqual((stream, status, body), (1, 200, b"hello, world\n"))
        self.assertEqual(fields["content-length"], "13")
        self.assertEqual(fields["content-type"], "text/plain; charset=utf-8")
        for name in ("server", "date", "last-modified", "etag"):
            self.assertIn(name, fields)

    def test_root_and_directories_serve_index(self):
        self.assertEqual(self.get("/", 1)[3], self.files["index.html"])
        self.assertEqual(self.get("/sub", 2)[3], self.files["sub/index.html"])
        self.assertEqual(self.get("/sub/", 3)[3], self.files["sub/index.html"])

    def test_connection_stays_open_across_requests(self):
        for stream in range(1, 6):
            self.assertEqual(self.get("/hello.txt", stream)[:2], (stream, 200))

    def test_pipelined_requests_are_answered_in_order(self):
        self.c.send(request(1, "/hello.txt") + request(2, "/nope") + request(3, "/index.html"))
        got = [self.c.response()[:2] for _ in range(3)]
        self.assertEqual(got, [(1, 200), (2, 404), (3, 200)])

    def test_head_has_length_but_no_data(self):
        self.c.send(request(1, "/big.bin", method=HEAD))
        ftype, flags, stream, payload = self.c.frame()
        self.assertEqual((ftype, flags & END_STREAM), (RESPONSE, END_STREAM))
        self.assertEqual(parse_fields(payload[2:])["content-length"], "200000")
        # the next thing on the wire belongs to the next request
        self.assertEqual(self.get("/hello.txt", 2)[1], 200)

    def test_big_file_is_split_into_frames(self):
        _, status, fields, body, frames = self.get("/big.bin")
        self.assertEqual(status, 200)
        self.assertEqual(body, self.files["big.bin"])
        self.assertGreater(len(frames), 1)
        self.assertTrue(all(n <= 65535 for n in frames))

    def test_empty_file(self):
        self.c.send(request(1, "/empty.txt"))
        ftype, flags, _, payload = self.c.frame()
        self.assertEqual((ftype, flags & END_STREAM), (RESPONSE, END_STREAM))
        self.assertEqual(parse_fields(payload[2:])["content-length"], "0")

    def test_percent_decoding_and_query(self):
        self.assertEqual(self.get("/with%20space.txt?x=1")[3], b"spaced\n")

    def test_only_the_query_is_dropped(self):
        # SPEC §4 drops "?query" and nothing else, so '#' is part of the name
        self.assertEqual(self.get("/a#b.txt", 1)[3], b"hash\n")
        self.assertEqual(self.get("/a%23b.txt", 2)[3], b"hash\n")

    def test_trailing_slash_on_a_file_is_404(self):
        self.assertEqual(self.get("/hello.txt/", 1)[1], 404)
        self.assertEqual(self.get("/sub/index.html/", 2)[1], 404)

    def test_missing_file_is_404_and_connection_survives(self):
        self.assertEqual(self.get("/missing.html", 1)[1], 404)
        self.assertEqual(self.get("/hello.txt", 2)[1], 200)

    def test_traversal(self):
        for i, path in enumerate(["/../secret.txt", "/sub/../../secret.txt",
                                  "/%2e%2e/secret.txt", "/%2E%2E%2Fsecret.txt"], 1):
            with self.subTest(path=path):
                self.assertEqual(self.get(path, i)[1], 400)
        self.assertEqual(self.get("/escape.txt", 9)[1], 404)

    def test_bad_escapes(self):
        for i, path in enumerate(["/a%zz", "/a%2", "/a%00b"], 1):
            with self.subTest(path=path):
                self.assertEqual(self.get(path, i)[1], 400)

    def test_malformed_requests_get_400_and_connection_survives(self):
        def raw(*parts):
            return b"".join(parts)
        cases = {
            "too short": b"\x01\x00",
            "path overruns": raw(b"\x01\x00\x50", b"/x"),
            "empty path": b"\x01\x00\x00",
            "no leading slash": raw(b"\x01\x00\x01", b"x"),
            "index 0": raw(b"\x01\x00\x01/", b"\x80\x00\x00"),
            "empty literal name": raw(b"\x01\x00\x01/", b"\x00\x00\x00"),
            "uppercase name": raw(b"\x01\x00\x01/", b"\x01X\x00\x00"),
            "value overruns": raw(b"\x01\x00\x01/", b"\x81\x00\x09ab"),
            "missing value length": raw(b"\x01\x00\x01/", b"\x81\x00"),
            "CR in value": raw(b"\x01\x00\x01/", field("x-a", "a\rb")),
        }
        stream = 0
        for name, payload in cases.items():
            with self.subTest(name):
                stream += 1
                self.c.send(frame(REQUEST, stream, payload, END_STREAM))
                sid, status, _, body, _ = self.c.response()
                self.assertEqual((sid, status), (stream, 400))
                self.assertTrue(body.startswith(b"400 Bad Request: "))
        self.assertEqual(self.get("/hello.txt", stream + 1)[1], 200)

    def test_unknown_frame_types_are_skipped(self):
        self.c.send(frame(0x7F, 0, b"\xff" * 300))
        self.c.send(frame(0xEE, 1, b"looks like a request?", END_STREAM))
        self.c.send(frame(0x10, 0))
        self.assertEqual(self.get("/hello.txt")[1], 200)

    def test_unknown_flags_are_ignored(self):
        self.c.send(request(1, "/hello.txt", flags=0xFF))
        self.assertEqual(self.c.response()[1], 200)

    def test_unknown_table_index_is_skipped(self):
        payload = b"\x01\x00\x0a/hello.txt" + b"\x8b\x00\x03abc" + field("host", "x")
        self.c.send(frame(REQUEST, 1, payload, END_STREAM))
        self.assertEqual(self.c.response()[1], 200)

    def test_etag_gives_304(self):
        etag = self.get("/hello.txt", 1)[2]["etag"]
        self.c.send(request(2, "/hello.txt", headers=[("if-none-match", etag)]))
        ftype, flags, _, payload = self.c.frame()
        self.assertEqual(struct.unpack(">H", payload[:2])[0], 304)
        self.assertTrue(flags & END_STREAM)
        self.assertEqual(self.get("/hello.txt", 3, headers=[("if-none-match", '"other"')])[1], 200)

    def test_table_name_sent_as_literal(self):
        etag = self.get("/hello.txt", 1)[2]["etag"]
        path = b"/hello.txt"
        literal = bytes([13]) + b"if-none-match" + struct.pack(">H", len(etag)) + etag.encode()
        self.c.send(frame(REQUEST, 2, b"\x01" + struct.pack(">H", len(path)) + path + literal,
                          END_STREAM))
        self.assertEqual(self.c.response()[1], 304)

    def test_methods(self):
        _, status, fields, _, _ = self.get("/hello.txt", 1, method=POST)
        self.assertEqual((status, fields.get("allow")), (405, "GET, HEAD"))
        self.assertEqual(self.get("/hello.txt", 2, method=DELETE)[1], 405)
        self.assertEqual(self.get("/hello.txt", 3, method=0)[1], 501)
        self.assertEqual(self.get("/hello.txt", 4, method=200)[1], 501)

    def test_request_body_is_discarded(self):
        self.c.send(request(1, "/hello.txt", method=POST, flags=0))
        self.c.send(frame(DATA, 1, b"x" * 1000))
        self.c.send(frame(DATA, 1, b"y" * 10, END_STREAM))
        self.assertEqual(self.c.response()[1], 405)
        self.assertEqual(self.get("/hello.txt", 2)[1], 200)

    def expect_goaway(self, conn, code):
        ftype, _, stream, payload = conn.frame()
        self.assertEqual((ftype, stream), (GOAWAY, 0))
        self.assertEqual(struct.unpack(">H", payload[:2])[0], code)
        self.assertTrue(conn.closed())

    def test_bad_preface(self):
        c = Conn(self.port, preface=b"GET / HTTP/1.1\r\n\r\n")
        self.expect_goaway(c, BAD_PREFACE)
        c.close()

    def test_newer_version_preface_is_refused(self):
        c = Conn(self.port, preface=b"BHTP/2\r\n")
        self.expect_goaway(c, BAD_PREFACE)
        c.close()

    def test_stream_ids_must_increase(self):
        self.assertEqual(self.get("/hello.txt", 5)[1], 200)
        self.c.send(request(5, "/hello.txt"))
        self.expect_goaway(self.c, PROTOCOL_ERROR)

    def test_request_on_stream_zero(self):
        self.c.send(request(0, "/hello.txt"))
        self.expect_goaway(self.c, PROTOCOL_ERROR)

    def test_response_sent_to_server(self):
        self.c.send(frame(RESPONSE, 1, b"\x00\xc8", END_STREAM))
        self.expect_goaway(self.c, PROTOCOL_ERROR)


# ---- bcurl, against a scripted fake server ----

class FakeServer:
    """
    Accepts connections and answers each REQUEST with the bytes reply(stream, path)
    returns. A reply of None hangs up instead.
    """

    def __init__(self, reply):
        self.reply = reply
        self.accepts = 0
        self.paths = []
        self.requests = []      # raw REQUEST payloads
        self.goaways = []       # GOAWAY payloads bcurl sent us
        self.lsock = socket.socket()
        self.lsock.bind(("127.0.0.1", 0))
        self.lsock.listen()
        self.port = self.lsock.getsockname()[1]
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                sock, _ = self.lsock.accept()
            except OSError:
                return
            self.accepts += 1
            threading.Thread(target=self.serve, args=(sock,), daemon=True).start()

    def serve(self, sock):
        c = Conn.__new__(Conn)
        c.sock = sock
        try:
            assert c.exact(8) == PREFACE
            while True:
                ftype, _, stream, payload = c.frame()
                if ftype == GOAWAY:
                    self.goaways.append(payload)
                if ftype != REQUEST:
                    continue
                (plen,) = struct.unpack(">H", payload[1:3])
                path = payload[3:3 + plen].decode()
                self.paths.append(path)
                self.requests.append(payload)
                out = self.reply(stream, path)
                if out is None:
                    break
                sock.sendall(out)
        except (EOFError, OSError, AssertionError):
            pass
        finally:
            sock.close()

    def close(self):
        self.lsock.close()


def response(stream, status, body=b"", extra=b"", split=None):
    fields = field("content-length", str(len(body))) + extra
    out = frame(RESPONSE, stream, struct.pack(">H", status) + fields, 0 if body else END_STREAM)
    if body:
        chunks = [body[i:i + split] for i in range(0, len(body), split)] if split else [body]
        for i, chunk in enumerate(chunks):
            out += frame(DATA, stream, chunk, END_STREAM if i == len(chunks) - 1 else 0)
    return out


def bcurl(*args):
    return subprocess.run([BCURL, *args], capture_output=True, timeout=10)


class ClientTests(unittest.TestCase):
    def fake(self, reply):
        server = FakeServer(reply)
        self.addCleanup(server.close)
        return server

    def test_body_to_stdout(self):
        s = self.fake(lambda sid, path: response(sid, 200, b"body of " + path.encode()))
        r = bcurl("127.0.0.1:%d/a.txt" % s.port)
        self.assertEqual((r.returncode, r.stdout), (0, b"body of /a.txt"))

    def test_exit_codes(self):
        for status, code in ((200, 0), (304, 0), (404, 4), (400, 4), (500, 5), (503, 5)):
            with self.subTest(status=status):
                s = self.fake(lambda sid, path, st=status: response(sid, st, b"x" if st != 304 else b""))
                self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, code)

    def test_one_connection_for_many_paths(self):
        s = self.fake(lambda sid, path: response(sid, 200, path.encode()))
        r = bcurl("127.0.0.1:%d/a" % s.port, "/b", "127.0.0.1:%d/c" % s.port)
        self.assertEqual(r.stdout, b"/a/b/c")
        self.assertEqual((s.accepts, s.paths), (1, ["/a", "/b", "/c"]))

    def test_worst_status_decides_exit(self):
        s = self.fake(lambda sid, path: response(sid, 404 if path == "/b" else 200, b"x"))
        self.assertEqual(bcurl("127.0.0.1:%d/a" % s.port, "/b", "/c").returncode, 4)
        self.assertEqual(s.accepts, 1)

    def test_skips_unknown_frames_and_flags(self):
        def reply(sid, path):
            return (frame(0x42, 0, b"future stuff") +
                    frame(RESPONSE, sid, b"\x00\xc8" + field("content-length", "6") +
                          b"\x8b\x00\x02zz", 0xFE) +
                    frame(0x99, sid, b"also skipped", END_STREAM) +
                    frame(DATA, sid, b"abc", 0x80) +
                    frame(0x43, sid, b"") +
                    frame(DATA, sid, b"def", END_STREAM | 0x40))
        s = self.fake(reply)
        r = bcurl("127.0.0.1:%d/" % s.port)
        self.assertEqual((r.returncode, r.stdout), (0, b"abcdef"), r.stderr)

    def test_reassembles_many_data_frames(self):
        body = bytes(range(256)) * 300
        s = self.fake(lambda sid, path: response(sid, 200, body, split=1000))
        self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).stdout, body)

    def test_content_length_mismatch_is_protocol_error(self):
        def reply(sid, path):
            return (frame(RESPONSE, sid, b"\x00\xc8" + field("content-length", "10")) +
                    frame(DATA, sid, b"short", END_STREAM))
        s = self.fake(reply)
        self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, 3)

    def test_malformed_response_is_protocol_error(self):
        cases = {
            "index 0": b"\x80\x00\x00",
            "uppercase name": b"\x01X\x00\x00",
            "CR in value": field("x-a", "a\rb"),
            "value overruns": b"\x85\x00\x09ab",
            "status 999": b"\x03\xe7",
            "status 103": b"\x00\x67",
        }
        for name, fields in cases.items():
            with self.subTest(name):
                payload = fields if name.startswith("status") else b"\x00\xc8" + fields
                s = self.fake(lambda sid, path, p=payload: frame(RESPONSE, sid, p, END_STREAM))
                self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, 3)

    def test_data_in_a_bodiless_response(self):
        s = self.fake(lambda sid, path: response(sid, 200, b"abcde"))
        self.assertEqual(bcurl("-I", "127.0.0.1:%d/" % s.port).returncode, 3)
        s = self.fake(lambda sid, path: frame(RESPONSE, sid, b"\x01\x30") + frame(DATA, sid, b"x", END_STREAM))
        self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, 3)

    def test_protocol_error_is_explained_with_goaway(self):
        s = self.fake(lambda sid, path: frame(RESPONSE, sid + 1, b"\x00\xc8", END_STREAM))
        self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, 3)
        for _ in range(20):
            if s.goaways:
                break
            time.sleep(0.05)
        self.assertEqual(s.goaways[0][:2], b"\x00\x01")

    def test_data_before_response_is_protocol_error(self):
        s = self.fake(lambda sid, path: frame(DATA, sid, b"x", END_STREAM))
        self.assertEqual(bcurl("127.0.0.1:%d/" % s.port).returncode, 3)

    def test_goaway(self):
        s = self.fake(lambda sid, path: frame(GOAWAY, 0, b"\x00\x01nope"))
        r = bcurl("127.0.0.1:%d/" % s.port)
        self.assertEqual(r.returncode, 3)
        self.assertIn(b"PROTOCOL_ERROR", r.stderr)

    def test_server_hangup_is_not_retried(self):
        # /b gets no answer, just a closed socket. bcurl must give up, not reconnect.
        s = self.fake(lambda sid, path: response(sid, 200, b"ok") if path == "/a" else None)
        r = bcurl("127.0.0.1:%d/a" % s.port, "/b")
        self.assertEqual((r.returncode, r.stdout), (2, b"ok"))
        self.assertEqual(s.accepts, 1)

    def test_request_encoding(self):
        s = self.fake(lambda sid, path: response(sid, 200))
        bcurl("-H", "X-Trace: 42", "-H", "accept: text/html", "127.0.0.1:%d/p?q=1" % s.port)
        payload = s.requests[0]
        self.assertEqual(payload[:3], b"\x01\x00\x06")
        self.assertEqual(payload[3:9], b"/p?q=1")
        fields = parse_fields(payload[9:])
        self.assertEqual(fields["accept"], "text/html")
        self.assertEqual(fields["x-trace"], "42")
        self.assertIn("host", fields)

    def test_verbose_hexdumps_frames(self):
        s = self.fake(lambda sid, path: response(sid, 200, b"hi"))
        r = bcurl("-v", "127.0.0.1:%d/" % s.port)
        self.assertIn(b"42 48 54 50 2f 31 0d 0a", r.stderr)
        self.assertIn(b"> REQUEST", r.stderr)
        self.assertIn(b"< RESPONSE", r.stderr)
        self.assertIn(b"< DATA", r.stderr)
        self.assertEqual(r.stdout, b"hi")

    def test_refuses_a_second_server(self):
        r = bcurl("127.0.0.1:1/a", "127.0.0.2:1/b")
        self.assertEqual(r.returncode, 1)


# ---- the real pair ----

class EndToEnd(unittest.TestCase):
    def test_bcurl_against_bserve(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        data = os.urandom(150_000)
        with open(os.path.join(tmp, "f.bin"), "wb") as f:
            f.write(data)
        proc, port = start_bserve(tmp)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.terminate)
        r = bcurl("localhost:%d/f.bin" % port, "/f.bin", "/missing")
        self.assertEqual(r.stdout[:len(data) * 2], data * 2)
        self.assertEqual(r.returncode, 4)


if __name__ == "__main__":
    unittest.main()
