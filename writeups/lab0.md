Lab 0 Writeup
=============

Scope: the coding portions of the lab -- `webget`, an HTTP/1.1 client built on the OS
stream-socket API, and the in-memory flow-controlled byte stream. The by-hand networking
exercises (telnet/SMTP) were out of scope.

Design of the ByteStream:
The byte stream is a flow-controlled FIFO buffer with a fixed capacity. It holds the
bytes in a `std::string` plus a `_buffer_start` offset that marks the first byte that
has not been popped yet; `buffer_size()` is therefore just `_buffer.size() -
_buffer_start`, and it is never larger than the capacity because `write()` only accepts
`min(data.size(), remaining_capacity())` bytes.

`pop_output()` just advances the offset, and only touches the storage when the whole
buffer has drained (clear it) or when at least half of it is dead *and* more than 4096
bytes are dead (erase the consumed prefix). Since a compaction only happens after at
least as many bytes have been popped as are copied by it, the amortized cost of a pop
is O(1), and a one-byte-at-a-time reader of a very long stream does not degrade to
quadratic behaviour.

Two counters, `_bytes_written` and `_bytes_read`, are accumulated independently instead
of being derived, so they keep counting bytes that have already been discarded from the
buffer. `eof()` is `input_ended() and buffer_empty()`: reaching the end of the stream
requires both that the writer closed the input and that the reader has consumed
everything that was still buffered.

Implementation Challenges:
The class itself is small; the fiddly part is the difference between `input_ended()` and
`eof()`. A stream whose writer has closed but whose buffer still holds bytes reports
`input_ended() == true` and `eof() == false`, and it only flips to `eof()` once the last
byte is popped. Tests "write-end-pop" and "write-pop-end" pin exactly that ordering.

For `webget`, the request has to end each line with CRLF and finish with a blank line;
using `endl` or `"\n"` leaves the server waiting for the rest of the request forever.
The reply must also be read in a loop until the socket reaches EOF -- the server is free
to split it across as many TCP segments as it likes, so a single `read()` can easily
return only part of the body. `Connection: close` is what makes the end of the reply
observable: the server closes its half of the stream, and the next `read()` returns 0
and sets the EOF flag.

Remaining Bugs:
None that I know of: all five `t_byte_stream_*` tests and the doctests pass.

- Optional: I had unexpected difficulty with: Mostly environment setup rather than the
  networking itself. This checkout of the starter code did not compile as-is: `util/buffer.cc`
  used `std::out_of_range` / `std::runtime_error` without including `<stdexcept>`, and
  `util/address.cc` used `std::array` without including `<array>`. I added those two missing
  includes, which is a compile fix only and does not change any behaviour. Also, the conda
  `g++` 10.4 is first on `PATH` here and is older than the system `g++` 13.3, so the build
  has to be configured with `-DCMAKE_CXX_COMPILER=/usr/bin/g++-13`.

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: How much of "reliable byte stream" is just bookkeeping.
  The object that turns best-effort datagrams into an ordered stream is built out of a
  buffer, two counters and a boolean -- all the hard part is on the wire, not in the data
  structure.

- Optional: I'm not sure about: Whether `write()` should refuse data after `end_input()`.
  The documentation says that once the writer ends the input "no more bytes can be
  written", but nothing in the tests exercises that case, so I left `write()` accepting
  bytes and let `end_input()` only affect `input_ended()` / `eof()`.
