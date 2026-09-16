Lab 1 Writeup
=============

Scope: the coding portion of the lab -- the `StreamReassembler` that stitches
out-of-order, overlapping, duplicated substrings back into one byte stream.
The hands-on networking exercises were out of scope.

Program Structure and Design of the StreamReassembler:
=====================================================

Private state (stream_reassembler.hh):

  * `_output`       the reassembled ByteStream, inherited from the starter code.
  * `_stored`       `std::map<uint64_t, std::string>`: the bytes that have
                    arrived but cannot be pushed to `_output` yet, because an
                    earlier byte is still missing. Keyed by the start index of
                    the substring.
  * `_stored_bytes` total number of bytes in `_stored`.
  * `_eof_index`    `std::optional<uint64_t>`: the index just past the last byte
                    of the stream, once we have been told where it ends.

Invariants of `_stored`
-----------------------
The substrings are non-empty, pairwise disjoint, and never adjacent, so between
two consecutive entries there is always at least one hole. Keeping one copy of
each index (rather than storing whatever overlaps we were handed) is what makes
`_capacity` a real memory limit instead of a limit on how much we were offered.

Index bookkeeping without redundant state
-----------------------------------------
Bytes are only ever pushed to `_output` in stream order starting at index 0, so
`_output.bytes_written()` already *is* the index of the next byte we are waiting
for. There is no separate `_first_unassembled` member.

The capacity rule is one line:

    limit = first_unassembled + (_capacity - _output.buffer_size())

Bytes at or past `limit` are dropped, because bytes still unread in `_output`
occupy capacity too. That keeps `_stored_bytes + _output.buffer_size() <=
_capacity` true at all times.

`_stored_bytes` is the one piece of state that is not strictly necessary: it can
be recomputed by summing the lengths in `_stored`. It is kept because that sum
costs O(number of substrings) and `unassembled_bytes()` is queried by the TCP
receiver on essentially every segment, whereas maintaining the running total
costs one addition per merge and one subtraction per pop.

Merging a new segment
---------------------
`merge_pending_bytes()` first clips the segment to the new, storable part, then
looks up the first substring that could overlap or touch it (the first key >=
`start`, or the entry just before it, which may reach in). Everything that
overlaps the new bytes, plus the neighbours that merely touch them, is erased
and replaced by one merged substring. The `<= end` test is what stops two
substrings from ever ending up adjacent.

`push_substring()` then drains: while the first pending substring starts exactly
where the stream has reached, append it and advance. Finally, if the end of the
stream is known and has been reached, `end_input()` is called.

Segment ends
------------
`eof` is only recorded if the whole segment was accepted. A segment that had to
be truncated says nothing about where the stream ends, so its `eof` is
disregarded, as the interface documentation requires.

Alternative design choices
==========================

One `std::map<uint64_t, char>` entry per byte. This is the implementation I
wrote first, and it is much easier to convince yourself is correct:
`emplace()` naturally deduplicates, `unassembled_bytes()` is `_stored.size()`,
and there is no merging logic at all. I measured it and threw it away -- with
64 MiB pushed as ~1460-byte segments in random order it manages only
0.068 Gbit/s, below the 0.1 Gbit/s the lab asks for, and 0.039 Gbit/s with
64-byte segments. The interval map is 24x faster on the same workload:

    workload (64 MiB)              map of bytes     interval map
    in order, 1460 B segments        0.216 Gbit/s     8.742 Gbit/s
    shuffled, 1460 B segments        0.068 Gbit/s     1.632 Gbit/s
    shuffled, 64 B segments          0.039 Gbit/s     0.422 Gbit/s

The interval map bought that with roughly 40 extra lines and a merge function
whose correctness is genuinely non-obvious, which is why it is the piece of this
lab I fuzzed hardest.

Inserting intervals without merging them was never an option: storing
overlapping substrings would let a caller blow past the nominal capacity by
handing us the same bytes repeatedly, and it makes `unassembled_bytes()`
(counting each index once) much harder rather than easier.

Implementation Challenges:
=========================

The merge function. Three things had to line up, and each was wrong at least
once:

  * Picking the first candidate substring. Looking only at `lower_bound(start)`
    misses the substring that starts before `start` but reaches into it, so the
    entry just before has to be considered too.
  * Coalescing neighbours, not just overlaps. Without the `<= end` test, two
    substrings that happen to sit end-to-end stay separate and the map slowly
    fills up with adjacent entries.
  * Filling the merged span without leaving a hole. I build the merged string
    as `merged_end - merged_start` bytes and overwrite it with each known
    substring and then the new bytes. That is only safe because the span really
    is gapless: each substring in the range ends at or after `merged_start`, and
    the next one always starts no later than where the covered region has got
    to. I wrote that argument down as a comment, because the code alone does not
    make it obvious.

The distinction between `input_ended()` and `eof()` shows up again here: a
segment carrying the end of the stream can arrive long before the bytes before
it do, so `_eof_index` is recorded when the segment is accepted, but the stream
is only ended when the reassembler has actually reached that index.

Debugging and testing:
=====================

Beyond the eight `t_strm_reassem_*` tests, I wrote a randomised differential
test that runs the real reassembler against the per-byte map implementation on
randomised inputs -- random capacities from 1 byte up, random out-of-order
indices, duplicated and overlapping segments, random reads, and random `eof`
flags -- comparing buffered size and contents, `bytes_written()`,
`unassembled_bytes()`, `empty()` and the EOF flag after every step. 140,000
trials across seven parameter sets produced no disagreeing state.

That mattered: my first attempt at the fuzzer generated random *bytes* for each
push, so the same index carried different values in different segments. The two
implementations disagreed immediately (one keeps the first copy of a byte, the
other the last). That is not a bug in either -- the lab guarantees a single
underlying byte stream, so inconsistent substrings are simply out of scope --
but it was a good reminder that the fuzzer has to respect the contract it is
testing. The version above slices one generated source string so that every push
is real data.

Remaining Bugs:
===============

None that I know of. All eight `t_strm_reassem_*` tests pass, and the randomised
differential test agrees with the reference on every state it can observe.

One deliberate limitation: if a caller ever does provide inconsistent
substrings, this implementation keeps the most recently submitted copy of a
byte, whereas the per-byte reference keeps the first. The lab assumes this
cannot happen, so no attempt is made to detect it.

- Optional: I had unexpected difficulty with: Getting the merge function right.
  The per-byte version was working within minutes; the fast one needed the
  differential test before I trusted it, and even then the first "failure" it
  found was in the test rather than the code.

- Optional: I think you could make this lab better by: [describe]

- Optional: I was surprised by: How much the data structure choice dominates.
  Same algorithm, same tests, roughly 40 lines apart -- 0.068 Gbit/s versus
  1.6 Gbit/s. The eight provided tests pass either way, so nothing in the
  supplied suite would have told me which one I had written.

- Optional: I'm not sure about: What the reassembler should do with bytes it is
  holding *past* a known end of stream. The `fsm_stream_reassembler_many` test
  explicitly allows either truncating at the end of stream or keeping the extra
  bytes, and I keep them.
