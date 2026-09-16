#include "byte_stream.hh"

#include <algorithm>

using namespace std;

namespace {
//! Reclaim the consumed prefix of the buffer once enough of it is dead to be worth the copy.
constexpr size_t COMPACT_THRESHOLD = 4096;
}  // namespace

ByteStream::ByteStream(const size_t capacity) : _capacity(capacity) {}

size_t ByteStream::write(const string &data) {
    const size_t len = min(data.size(), remaining_capacity());
    _buffer.append(data, 0, len);
    _bytes_written += len;
    return len;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    return _buffer.substr(_buffer_start, min(len, buffer_size()));
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
    const size_t popped = min(len, buffer_size());
    _buffer_start += popped;
    _bytes_read += popped;

    if (_buffer_start == _buffer.size()) {
        // Everything has been read: drop the storage and start over.
        _buffer.clear();
        _buffer_start = 0;
    } else if (_buffer_start >= COMPACT_THRESHOLD && _buffer_start * 2 >= _buffer.size()) {
        // Half (or more) of the buffer is dead, and it is big enough to be worth reclaiming.
        _buffer.erase(0, _buffer_start);
        _buffer_start = 0;
    }
}

void ByteStream::end_input() { _input_ended = true; }

bool ByteStream::input_ended() const { return _input_ended; }

size_t ByteStream::buffer_size() const { return _buffer.size() - _buffer_start; }

bool ByteStream::buffer_empty() const { return buffer_size() == 0; }

bool ByteStream::eof() const { return _input_ended and buffer_empty(); }

size_t ByteStream::bytes_written() const { return _bytes_written; }

size_t ByteStream::bytes_read() const { return _bytes_read; }

size_t ByteStream::remaining_capacity() const { return _capacity - buffer_size(); }
