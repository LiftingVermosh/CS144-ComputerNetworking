#include "stream_reassembler.hh"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

using namespace std;

StreamReassembler::StreamReassembler(const size_t capacity) : _output(capacity), _capacity(capacity) {}

//! \details This function accepts a substring (aka a segment) of bytes,
//! possibly out-of-order, from the logical stream, and assembles any newly
//! contiguous substrings and writes them into the output stream in order.
void StreamReassembler::push_substring(const string &data, const uint64_t index, const bool eof) {
    // Bytes are only ever pushed to `_output` in stream order starting at index 0, so the
    // number of bytes written so far is exactly the index of the next byte we are waiting for.
    const uint64_t first_unassembled = _output.bytes_written();

    // Bytes still sitting unread in `_output` count against the capacity too, so this is how
    // much room is left for bytes that the reassembler would have to hold on to by itself.
    const uint64_t limit = first_unassembled + (_capacity - _output.buffer_size());

    // Keep only the part of the segment that is both new (bytes before `first_unassembled`
    // have already been written to `_output`) and within the capacity. Whatever lies at or
    // beyond `limit` cannot be stored, so it has to be dropped.
    const uint64_t start = max<uint64_t>(index, first_unassembled);
    const uint64_t segment_end = index + data.size();
    const bool fully_accepted = segment_end <= limit;

    // A segment only tells us where the stream ends if we were able to keep all of it; if it
    // was truncated, its tail says nothing about the end of the stream.
    if (eof and fully_accepted) {
        _eof_index = segment_end;
    }

    const uint64_t end = fully_accepted ? segment_end : limit;
    if (start < end) {
        merge_pending_bytes(data, index, start, end);
    }

    // Push out every byte that has just become contiguous with the stream. Nothing below
    // `first_unassembled` is ever stored, so the first pending substring is the next in line.
    string assembled;
    while (not _stored.empty() and _stored.begin()->first == first_unassembled + assembled.size()) {
        const auto next = _stored.begin();
        assembled += next->second;
        _stored_bytes -= next->second.size();
        _stored.erase(next);
    }
    if (not assembled.empty()) {
        _output.write(assembled);
    }

    // The reader has reached the end of the stream once the writer's end has been learned and
    // every byte before it has been assembled.
    if (_eof_index and first_unassembled + assembled.size() >= *_eof_index) {
        _output.end_input();
    }
}

void StreamReassembler::merge_pending_bytes(const string &data,
                                            const uint64_t index,
                                            const uint64_t start,
                                            const uint64_t end) {
    // The first pending substring that can possibly overlap or touch the new bytes is either
    // the first one starting at or after `start`, or the one right before it, which may reach
    // into them. Nothing earlier can: consecutive entries are never adjacent.
    auto first = _stored.lower_bound(start);
    if (first != _stored.begin()) {
        const auto previous = prev(first);
        if (previous->first + previous->second.size() >= start) {
            first = previous;
        }
    }

    // Every byte handed to us is a slice of one underlying stream, so if the substring that
    // starts here already covers the new bytes, there is nothing to record.
    if (first != _stored.end() and first->first <= start and end <= first->first + first->second.size()) {
        return;
    }

    // The new bytes coalesce everything they overlap, plus their immediate neighbours -- the
    // neighbour test is `<= end`, which also keeps two substrings from ending up adjacent.
    auto last = first;
    uint64_t merged_start = start;
    uint64_t merged_end = end;
    while (last != _stored.end() and last->first <= end) {
        merged_start = min(merged_start, last->first);
        merged_end = max(merged_end, last->first + last->second.size());
        ++last;
    }

    // `[merged_start, merged_end)` is covered without gaps: each substring in the range ends
    // at or after `merged_start`, and the next one always starts no later than where the
    // covered region has reached, so filling the whole span leaves no uninitialised hole.
    string merged(merged_end - merged_start, '\0');
    for (auto known = first; known != last; ++known) {
        merged.replace(known->first - merged_start, known->second.size(), known->second);
        _stored_bytes -= known->second.size();
    }
    merged.replace(start - merged_start, end - start, data, start - index, end - start);

    _stored.erase(first, last);
    _stored_bytes += merged.size();
    _stored.emplace(merged_start, move(merged));
}

size_t StreamReassembler::unassembled_bytes() const { return _stored_bytes; }

bool StreamReassembler::empty() const { return _stored.empty(); }
