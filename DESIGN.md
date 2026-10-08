# Design notes

SPEC.md says *what* BHTP/1 is. This file says why it looks that way, and what each choice
costs. It's the long form of SPEC.md §6.

## The frame header: 16 / 8 / 8 / 32

### What HTTP/2 chose, and why

HTTP/2's header is 9 bytes: Length 24, Type 8, Flags 8, a reserved bit, Stream ID 31.

- **Length 24.** HTTP/2 multiplexes many streams over one connection, so a frame is also the
  unit of fairness. While a 1 MB frame for one stream is going out, every other stream waits
  behind it. So the *default* maximum is small (16 KiB), and a receiver that wants bigger
  frames for bulk transfer raises it with `SETTINGS_MAX_FRAME_SIZE`, up to 2^24−1. The field
  is 24 bits so it can hold any limit a peer might negotiate, not because frames are normally
  that big.
- **It wasn't always 24.** Up to draft-13 (2014) the HTTP/2 header was 8 bytes with a 14-bit
  length, very close to ours. Draft-14 grew the length to 24 bits and the header to 9 bytes,
  together with the new setting, so that large frames could be negotiated.
- **Type 8.** Room for extension frame types, plus the rule that unknown types are ignored.
  That is how HTTP/2 grew ALTSVC, ORIGIN and PRIORITY_UPDATE without a new version.
- **Flags 8.** Per-type booleans: END_STREAM, END_HEADERS, PADDED, PRIORITY.
- **R + 31.** Stream IDs are odd for client-initiated streams and even for server push, and
  can't be reused, so 2^31 is how many streams a connection can ever carry. The reserved bit
  has no defined meaning. The usual explanation is that a 31-bit ID fits a *signed* 32-bit
  integer, which mattered for Java.

### What we chose instead

```
Length 16 | Type 8 | Flags 8 | Stream ID 32       8 bytes
```

**Length 16.** BHTP has no SETTINGS frame and no negotiation, so the largest value the field
can hold is the largest frame every receiver must be ready to buffer. With 24 bits that would
be 16 MiB per frame, and a peer could make you allocate it. With 16 bits it's 64 KiB, which is
a fine worst case. Bodies are split into DATA frames anyway, so all a larger field would buy is
fewer headers. At 64 KiB per frame the header is 0.012% of the bytes. The other gain is that
*every* value of the field is legal. There is no "frame too large" error, so a receiver can
always find the next frame. That is what makes the error model below work.

What we give up: a single header block can't exceed 64 KiB. For a file server that sends six
short headers, that is fine. Real browsers can send cookies that size, and a BHTP/2 would need
to deal with it.

**Type 8, Flags 8.** We use four types and one flag. Making either field smaller to save a
few bits would leave us no room to grow, and then the skip rule would have nothing to protect.

**Stream ID 32, no reserved bit.** v1 answers requests strictly in order, so today the ID is a
consistency check. A response that names the wrong stream is caught immediately instead of
being handed to the wrong caller. But the ID is the one field multiplexing can't be added
without, and adding a field later means a new header layout. Four bytes now is cheaper. We
dropped HTTP/2's reserved bit because C, Python, Go and Rust all have unsigned 32-bit integers,
and because "reserved, meaning undefined" is a bit nobody can ever use.

**8 bytes, every field byte-aligned.** This matters more than it sounds. In a hexdump the
header always reads `LL LL TT FF SS SS SS SS`. Parsing it is two shifts and two byte reads, with
no masking. Annotating our own bytes for HEXDUMP.md was easy because of this.

## The preface

The client's first 8 bytes are `BHTP/1\r\n`. Without it, a confused peer is silent and
expensive. Point a browser at bserve and `GET / HT` would be read as a frame header with
Length 0x4745 (18245) and type `T`. That is an unknown type, so the server would skip 18 KB
that are never coming and wait for 60 seconds. With the preface, the server sees the mistake
in the first read and sends GOAWAY `BAD_PREFACE`.

The `1` is the version of the *frame header layout*, not of the protocol's features. That
distinction is the whole versioning story:

- New frame types, new flags and new table indexes **don't** change the preface. Old peers
  skip them, so a newer peer can just try them.
- Only a change to the 8 header bytes would bump it, because that is the one change an old
  peer can't skip past. It wouldn't know where the next frame starts.

## REQUEST and RESPONSE as separate types

HTTP/2 uses one HEADERS type and puts method, path and status in pseudo-headers (`:method`,
`:path`, `:status`). We made them fixed fields at the start of two different frame types:

- A method is one byte and a status is two. As pseudo-headers they'd need a name, a length
  and a value each, and rules about ordering and duplicates.
- The type alone tells you the direction, so "a server received a RESPONSE" is a check on one
  byte.
- They're the first bytes of the payload, which makes them easy to find in a hexdump.

v1 has no 1xx responses, so a RESPONSE is always the final one. If a later version wants
interim responses like 103 Early Hints, it can add them as a new frame type, and v1 clients
will skip it.

## Header fields: HPACK's first two mechanisms, and nothing else

HPACK (RFC 7541) has four parts: a static table, length-prefixed literals, Huffman coding,
and a dynamic table. We took the first two.

**The static table is the ten names we actually send**, ordered by who sends them: the
client's (`host`, `user-agent`, `accept`, `if-none-match`), then the server's (`server`,
`date`, `content-type`, `content-length`, `last-modified`, `etag`). Each costs one byte instead
of its spelling. `allow` *is* something bserve sends, but only on a 405, so it stays a literal.
A table is for the common case. HPACK picked its 61 entries the same way, from the fields
seen most often on popular sites (RFC 7541, Appendix A).

**Encoding.** The first byte's top bit picks the form. The other 7 bits are either the table
index or the literal name's length, so one byte does both jobs. Names are capped at 127 bytes
(the longest registered HTTP field names are around 40). Values get a fixed 16-bit length
rather than HPACK's variable-length integers. That is one extra byte per field, in exchange
for a parser with no varint loop to get wrong.

**No Huffman, no dynamic table.** The dynamic table is where HPACK's complexity and its
security story live. Both sides must keep identical state across every frame, and compressing
secrets together with attacker-chosen data is what CRIME exploited against SPDY's zlib header
compression. With a static table and literals, every frame decodes on its own, with no state
carried from earlier frames.

**No CONTINUATION.** A header block must fit in one frame. HTTP/2's CONTINUATION frames let a
block span frames, and in 2024 a family of "CONTINUATION flood" bugs let attackers make servers
buffer endless header data. We don't have that frame, so we can't have that bug.

**Unknown index → skip the field.** Indexes 11–127 are reserved. A receiver that sees one
skips the field, which it can always do because the value is length-prefixed. This is the frame
rule again, one level down. It means a later version can grow the table, and senders are told
not to use new indexes until they know the peer understands them.

## Three skip rules, one idea

| You meet...              | You do...                                         |
|--------------------------|---------------------------------------------------|
| an unknown frame type    | read `Length` bytes, discard, carry on            |
| an unknown flag bit      | ignore it                                         |
| an unknown table index   | read the value's length, skip the value, carry on |

Each one works because a length comes before everything that can be skipped. This is the
"one line you may not skip" from the brief, applied everywhere it can be.

## Errors: almost everything is recoverable

Because no 16-bit length is ever illegal, a broken *payload* never loses the frame boundary.
So we split errors in two:

- **Stream errors.** The REQUEST payload doesn't parse (truncated, overlong lengths, index 0,
  bad name, CR/LF in a value), or the path is bad. The server answers **400** on that stream,
  with a body saying exactly what was wrong, and keeps the connection. The body text matters
  more than it seems. When your partner's client gets a 400, the response tells them which
  rule they broke.
- **Connection errors.** The frame *sequence* is wrong: bad preface, stream IDs going
  backwards, a frame type sent in the wrong direction. Matching requests to responses can no
  longer be trusted, so GOAWAY and close.

A header field value with CR or LF is rejected even though our own framing wouldn't care.
Someone will eventually put a BHTP-to-HTTP/1.1 gateway in front of this, and that is exactly
the input that becomes header injection there.

## END_STREAM vs content-length

END_STREAM is what ends a message. `content-length` is a cross-check. If they disagree, the
message is broken (§5), and bcurl exits with status 3 instead of quietly writing a truncated
file. HEAD and 304 are the exceptions, because there the header describes a body that isn't
sent. bserve uses this when a file shrinks while it's being sent. content-length has already
gone out, so the server ends the stream early and the client is guaranteed to notice.

bcurl writes DATA to stdout as it arrives instead of holding the whole body in memory. So when
a body comes up short, the bytes that did arrive are already written, and the exit status is
what tells you not to trust them. curl makes the same trade.

## Implementation choices (not protocol)

- **bserve forks a process per connection.** Persistent connections mean a client can hold
  one open for a long time, so a single-threaded loop would let one idle client block
  everyone. Fork is the simplest model that doesn't. Each connection has a 60 s idle timeout,
  and the server says GOAWAY NO_ERROR before hanging up.
- **TCP_NODELAY on both sides.** A response is a small RESPONSE frame followed by DATA. With
  Nagle's algorithm on, the DATA sits waiting for the ACK of the RESPONSE, and the client delays
  that ACK, so small files took tens of milliseconds for no reason.
- **Path safety in two layers.** `..` segments are refused after percent-decoding, so
  `%2e%2e` doesn't sneak through. The final file is also checked with `realpath()` to be under
  the root, which catches symlinks that point out of it.
- **Stream IDs run out at 2^32−1.** That is four billion requests on one connection, about
  13 years at ten requests a second. A client that gets there just opens a new connection.
  bcurl, which never opens a second one, won't come anywhere near it.
- **bcurl opens exactly one connection.** Extra paths reuse it. A URL on another host is
  refused up front, before anything is sent. If the server hangs up, bcurl reports it and stops
  rather than reconnecting.
- **No shared code between bserve.c and bcurl.c.** The brief pairs a server writer with a
  client writer and lets only the spec cross between them. A shared codec would make the two
  programs agree with each other even where they disagree with the spec. tests/ has a third,
  independent implementation in Python for the same reason.
