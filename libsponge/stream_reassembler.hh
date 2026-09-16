#ifndef SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH
#define SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH

#include "byte_stream.hh"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

//! \brief A class that assembles a series of excerpts from a byte stream (possibly out of order,
//! possibly overlapping) into an in-order byte stream.
class StreamReassembler {
  private:
    // Your code here -- add private members as necessary.
    ByteStream _output;  //!< The reassembled in-order byte stream
    size_t _capacity;    //!< The maximum number of bytes

    //! Bytes that have arrived but cannot be pushed to `_output` yet, because an earlier
    //! byte of the stream is still missing. Keys are indices in the stream; the values are
    //! non-empty, pairwise disjoint, and never adjacent, so consecutive entries are always
    //! separated by at least one hole. Keeping one copy of every index is what makes
    //! `_capacity` a real memory limit rather than a limit on what we have been handed.
    std::map<uint64_t, std::string> _stored{};

    //! Number of bytes held in `_stored`, maintained incrementally so that
    //! `unassembled_bytes()` costs O(1) instead of a walk over every substring.
    size_t _stored_bytes = 0;

    //! Index just past the last byte of the stream, once a segment carrying the end
    //! of the stream has been accepted in full.
    std::optional<uint64_t> _eof_index{};

    //! Merge the bytes `[start, end)` of `data` -- whose own first byte sits at stream
    //! index `index` -- into `_stored`, coalescing them with whatever they overlap or
    //! touch. The caller has already clipped `[start, end)` to the new, storable bytes.
    void merge_pending_bytes(const std::string &data, uint64_t index, uint64_t start, uint64_t end);

  public:
    //! \brief Construct a `StreamReassembler` that will store up to `capacity` bytes.
    //! \note This capacity limits both the bytes that have been reassembled,
    //! and those that have not yet been reassembled.
    StreamReassembler(const size_t capacity);

    //! \brief Receives a substring and writes any newly contiguous bytes into the stream.
    //!
    //! If accepting all the data would overflow the `capacity` of this
    //! `StreamReassembler`, then only the part of the data that fits will be
    //! accepted. If the substring is only partially accepted, then the `eof`
    //! will be disregarded.
    //!
    //! \param data the string being added
    //! \param index the index of the first byte in `data`
    //! \param eof whether or not this segment ends with the end of the stream
    void push_substring(const std::string &data, const uint64_t index, const bool eof);

    //! \name Access the reassembled byte stream
    //!@{
    const ByteStream &stream_out() const { return _output; }
    ByteStream &stream_out() { return _output; }
    //!@}

    //! The number of bytes in the substrings stored but not yet reassembled
    //!
    //! \note If the byte at a particular index has been submitted twice, it
    //! should only be counted once for the purpose of this function.
    size_t unassembled_bytes() const;

    //! \brief Is the internal state empty (other than the output stream)?
    //! \returns `true` if no substrings are waiting to be assembled
    bool empty() const;
};

#endif  // SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH
