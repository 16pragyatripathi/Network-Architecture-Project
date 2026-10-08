# One request and response, byte by byte

This is a real capture, not a hand-made example. With `bserve` running on `./www`:

```
$ ./bcurl -v -H 'accept-language: en' localhost:9000/hello.txt
```

`accept-language` is there on purpose. It isn't in the static table, so the request shows
both ways of encoding a header name: three indexed names and one literal.

## What `bcurl -v` printed

`>` is what bcurl sent and `<` is what it received. The decoded lines under each frame are bcurl
reading its own bytes back. The body itself (`hello, world`) went to stdout.

```
> preface
> 0000  42 48 54 50 2f 31 0d 0a                           |BHTP/1..|
> REQUEST  stream=1 flags=0x01 END_STREAM length=68
> 0000  00 44 01 01 00 00 00 01  01 00 0a 2f 68 65 6c 6c  |.D........./hell|
> 0010  6f 2e 74 78 74 81 00 0e  6c 6f 63 61 6c 68 6f 73  |o.txt...localhos|
> 0020  74 3a 39 30 30 30 82 00  09 62 63 75 72 6c 2f 31  |t:9000...bcurl/1|
> 0030  2e 30 83 00 03 2a 2f 2a  0f 61 63 63 65 70 74 2d  |.0...*/*.accept-|
> 0040  6c 61 6e 67 75 61 67 65  00 02 65 6e              |language..en|
> GET /hello.txt
>   host: localhost:9000
>   user-agent: bcurl/1.0
>   accept: */*
>   accept-language: en
< RESPONSE  stream=1 flags=0x00 length=127
< 0000  00 7f 02 00 00 00 00 01  00 c8 85 00 0a 62 73 65  |.............bse|
< 0010  72 76 65 2f 31 2e 30 86  00 1d 54 68 75 2c 20 30  |rve/1.0...Thu, 0|
< 0020  38 20 4f 63 74 20 32 30  32 36 20 31 33 3a 31 39  |8 Oct 2026 13:19|
< 0030  3a 30 35 20 47 4d 54 87  00 19 74 65 78 74 2f 70  |:05 GMT...text/p|
< 0040  6c 61 69 6e 3b 20 63 68  61 72 73 65 74 3d 75 74  |lain; charset=ut|
< 0050  66 2d 38 88 00 02 31 33  89 00 1d 54 68 75 2c 20  |f-8...13...Thu, |
< 0060  30 38 20 4f 63 74 20 32  30 32 36 20 31 33 3a 31  |08 Oct 2026 13:1|
< 0070  33 3a 35 33 20 47 4d 54  8a 00 0c 22 36 61 63 37  |3:53 GMT..."6ac7|
< 0080  39 37 31 31 2d 64 22                              |9711-d"|
< 200 OK
<   server: bserve/1.0
<   date: Thu, 08 Oct 2026 13:19:05 GMT
<   content-type: text/plain; charset=utf-8
<   content-length: 13
<   last-modified: Thu, 08 Oct 2026 13:13:53 GMT
<   etag: "6ac79711-d"
< DATA  stream=1 flags=0x01 END_STREAM length=13
< 0000  00 0d 03 01 00 00 00 01  68 65 6c 6c 6f 2c 20 77  |........hello, w|
< 0010  6f 72 6c 64 0a                                    |orld.|
```

## Client → server: 84 bytes

Offsets below count from the first byte the client wrote on the connection.

```
off   bytes                                 meaning                                    spec
----  ------------------------------------  -----------------------------------------  ----
                                            PREFACE, sent once per connection
0000  42 48 54 50 2f 31 0d 0a               "BHTP/1\r\n"                                §1

                                            REQUEST frame header
0008  00 44                                 Length = 0x0044 = 68 payload bytes          §2
000a  01                                    Type = 0x01 REQUEST
000b  01                                    Flags = END_STREAM: no body follows
000c  00 00 00 01                           Stream ID = 1, first request on this conn.

                                            REQUEST payload
0010  01                                    Method = 1 GET                              §3
0011  00 0a                                 Path length = 10
0013  2f 68 65 6c 6c 6f 2e 74 78 74         "/hello.txt"

001d  81                                    1|0000001: indexed name #1 = host           §3
001e  00 0e                                 value length = 14
0020  6c 6f 63 61 6c 68 6f 73 74 3a 39 30   "localhost:9000"
002c  30 30

002e  82                                    1|0000010: indexed name #2 = user-agent
002f  00 09                                 value length = 9
0031  62 63 75 72 6c 2f 31 2e 30            "bcurl/1.0"

003a  83                                    1|0000011: indexed name #3 = accept
003b  00 03                                 value length = 3
003d  2a 2f 2a                              "*/*"

0040  0f                                    0|0001111: literal name, 15 bytes follow
0041  61 63 63 65 70 74 2d 6c 61 6e 67 75   "accept-language"
004d  61 67 65
0050  00 02                                 value length = 2
0052  65 6e                                 "en"
                                            payload ends at 0x0054 = 0x0010 + 68 ✓
```

There is no count of header fields anywhere. The block simply ends where `Length` says the
payload ends, and the parser stops exactly on the last byte of `"en"`.

## Server → client: 156 bytes

Offsets restart at 0 for the server's direction. The server sends no preface.

```
off   bytes                                 meaning                                    spec
----  ------------------------------------  -----------------------------------------  ----
                                            RESPONSE frame header
0000  00 7f                                 Length = 0x007f = 127 payload bytes         §2
0002  02                                    Type = 0x02 RESPONSE
0003  00                                    Flags = 0: not the last frame, DATA follows
0004  00 00 00 01                           Stream ID = 1, answers request 1

                                            RESPONSE payload
0008  00 c8                                 Status = 0x00c8 = 200                       §3

000a  85                                    1|0000101: indexed name #5 = server
000b  00 0a                                 value length = 10
000d  62 73 65 72 76 65 2f 31 2e 30         "bserve/1.0"

0017  86                                    1|0000110: indexed name #6 = date
0018  00 1d                                 value length = 29
001a  54 68 75 2c 20 30 38 20 4f 63 74 20   "Thu, 08 Oct 2026 13:19:05 GMT"
0026  32 30 32 36 20 31 33 3a 31 39 3a 30
0032  35 20 47 4d 54

0037  87                                    1|0000111: indexed name #7 = content-type
0038  00 19                                 value length = 25
003a  74 65 78 74 2f 70 6c 61 69 6e 3b 20   "text/plain; charset=utf-8"
0046  63 68 61 72 73 65 74 3d 75 74 66 2d
0052  38

0053  88                                    1|0001000: indexed name #8 = content-length
0054  00 02                                 value length = 2
0056  31 33                                 "13", matches the DATA below               §3

0058  89                                    1|0001001: indexed name #9 = last-modified
0059  00 1d                                 value length = 29
005b  54 68 75 2c 20 30 38 20 4f 63 74 20   "Thu, 08 Oct 2026 13:13:53 GMT"
0067  32 30 32 36 20 31 33 3a 31 33 3a 35
0073  33 20 47 4d 54

0078  8a                                    1|0001010: indexed name #10 = etag
0079  00 0c                                 value length = 12
007b  22 36 61 63 37 39 37 31 31 2d 64 22   "\"6ac79711-d\""
                                            payload ends at 0x0087 = 0x0008 + 127 ✓

                                            DATA frame header
0087  00 0d                                 Length = 13                                 §2
0089  03                                    Type = 0x03 DATA
008a  01                                    Flags = END_STREAM: last frame of response
008b  00 00 00 01                           Stream ID = 1

                                            DATA payload, the file itself
008f  68 65 6c 6c 6f 2c 20 77 6f 72 6c 64   "hello, world\n"
009b  0a
                                            ends at 0x009c = 156 ✓
```

The etag is bserve's own format, `"<mtime>-<size>"` in hex: `0x6ac79711` is 1791465233,
which is 2026-10-08 13:13:53 UTC (the `last-modified` above), and `0xd` is the file's 13 bytes.
Sending it back as `if-none-match` gets a `304` with no DATA frame.

## Things worth noticing

- **Where the message ends.** The REQUEST carries END_STREAM, so the server knows no body is
  coming. The RESPONSE does *not*, so bcurl keeps reading. The DATA frame does, and that frame
  is the end of the response. `content-length: 13` is only a cross-check: bcurl compares it with
  the 13 bytes it actually got (§5).
- **Both name mechanisms.** Every name in this exchange except one is a single byte (`0x81`
  through `0x8a`). `accept-language` is not in the table, so it costs 1 + 15 bytes instead.
  That is the trade the static table makes. The names we send on every request are cheap, and
  everything else still works.
- **The connection is still open.** Nothing above closes it. bcurl closed it because it had no
  more paths to fetch. Had there been a second path, the next bytes on the client side would have
  been another REQUEST with Stream ID 2.
- **Size.** The same exchange in HTTP/1.1 text is 106 bytes up and 216 down. Here it's 84 up
  (76 without the once-per-connection preface) and 156 down. Most of what's left is the dates.
