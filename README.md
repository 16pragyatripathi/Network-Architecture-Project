# BHTP/1: HTTP, in binary

A binary framing for HTTP, a file server that speaks it (`bserve`), and a client
(`bcurl`). Course project, Network Architecture.

**Pragya Tripathi** · 24BCS10032 · pragya.24bcs10032@sst.scaler.com

| What the brief asks for                   | Where it is                                |
|-------------------------------------------|--------------------------------------------|
| 1. The spec, two pages                    | [SPEC.md](SPEC.md) ([PDF](SPEC.pdf), exactly two A4 pages) |
| 2. The program                            | [src/bserve.c](src/bserve.c), [src/bcurl.c](src/bcurl.c) |
| 3. Annotated hexdump of a request/response | [HEXDUMP.md](HEXDUMP.md)                  |
| Why the fields are the widths they are    | [DESIGN.md](DESIGN.md), and SPEC.md §6     |

## Build

Needs a C compiler and make. Tested on macOS (clang) and should build on any Linux with gcc.

```
make
```

## Run

```
./bserve ./www 9000                                   # terminal 1
./bcurl localhost:9000/index.html                     # terminal 2
```

`bserve <root> <port>` serves files under `<root>`. It logs one line per request and one
per closed connection to stderr, which makes keep-alive easy to see:

```
::1:55174 #1 GET / 200 450
::1:55174 #2 GET /notes/ 200 238
::1:55174 #3 GET /missing.html 404 14
::1:55174 #4 GET /style.css 200 167
::1:55174 closed after 4 requests
```

That's from `./bcurl localhost:9000/ /notes/ /missing.html /style.css`. Four requests,
one connection.

## bcurl

```
bcurl [-v] [-I] [-X method] [-H 'name: value']... host[:port]/path [/path | url]...
```

| Option | |
|---|---|
| `-v` | hexdump every frame, sent (`>`) and received (`<`), to stderr, plus decoded fields |
| `-I` | send HEAD and print the status and fields instead of a body |
| `-X` | `GET` `HEAD` `POST` `PUT` `DELETE`, or a raw method code (`-X 9` → 501) |
| `-H` | add a header field; repeatable. Names in the static table are indexed, others go literal |

Bodies go to stdout. Any extra paths (or URLs on the same host and port) are fetched over
the same connection. bcurl never opens a second one, not even to retry. The port defaults
to 9000, and a leading `bhtp://` is accepted.

Exit status: `0` success · `1` usage · `2` network error / server hung up · `3` protocol
error · `4` a 4xx response · `5` a 5xx response. With several paths, the worst one wins.

Some things to try:

```
./bcurl -v localhost:9000/hello.txt                        # every byte, both directions
./bcurl -I localhost:9000/index.html                       # fields only
./bcurl -H 'if-none-match: "…etag from -I…"' -I localhost:9000/index.html   # 304
./bcurl -X POST localhost:9000/                            # 405, with a literal `allow`
./bcurl localhost:9000/%2e%2e/etc/passwd                   # 400, '..' in path
```

## Tests

```
make test
```

43 black-box tests in [tests/test_bhtp.py](tests/test_bhtp.py) (Python 3, standard library
only). They have their own encoder and decoder, written from the spec, and they cover:

- **bserve against raw frames:** keep-alive, pipelining, HEAD, 64 KiB+ files split into frames,
  each malformed-request case getting 400 *and the connection surviving it*, unknown frame
  types / flags / table indexes being skipped, traversal (`..`, `%2e%2e`, symlinks out of the
  root), 304/405/501, and every GOAWAY case.
- **bcurl against a scripted fake server:** exit codes, one connection for many paths, skipping
  unknown frames and flags mid-response, malformed responses, content-length mismatches,
  GOAWAY in both directions, and never reconnecting after a hang-up.
- **bcurl against bserve** end to end.

The suite also passes with both programs built under AddressSanitizer and UBSan.

## Layout

```
SPEC.md  SPEC.pdf    the protocol
DESIGN.md            why it looks like that
HEXDUMP.md           one exchange, annotated byte by byte
src/bserve.c         the server
src/bcurl.c          the client
tests/test_bhtp.py   black-box tests
www/                 a small site to serve
```

`bserve.c` and `bcurl.c` deliberately share no code. The brief says the only thing that
crosses between the server and the client is the spec, so each one implements the spec on its
own.
