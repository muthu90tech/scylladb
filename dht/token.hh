/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "bytes.hh"
#include "types/types.hh"

#include <seastar/net/byteorder.hh>
#include <fmt/format.h>
#include <functional>
#include <utility>
#include <compare>
#include <byteswap.h>

namespace dht {

class token;

enum class token_kind {
    before_all_keys,
    key,
    after_all_keys,
};

class token {
    // INT64_MIN is not a legal token, but a special value used to represent
    // infinity in token intervals.
    // If a token with value INT64_MIN is generated by the hashing algorithm,
    // the result is coerced into INT64_MAX.
    // (So INT64_MAX is twice as likely as every other token.)
    static inline int64_t normalize(int64_t t) {
        return t == std::numeric_limits<int64_t>::min() ? std::numeric_limits<int64_t>::max() : t;
    }
public:
    using kind = token_kind;
    kind _kind;
    int64_t _data;

    token()
        : _kind(kind::before_all_keys)
        , _data(0) {
    }

    token(kind k, int64_t d)
        : _kind(std::move(k))
        , _data(normalize(d)) { }

    // This constructor seems redundant with the bytes_view constructor, but
    // it's necessary for IDL, which passes a deserialized_bytes_proxy here.
    // (deserialized_bytes_proxy is convertible to bytes&&, but not bytes_view.)
    token(kind k, const bytes& b) : _kind(std::move(k)) {
        if (_kind != kind::key) {
            _data = 0;
        } else {
            if (b.size() != sizeof(_data)) {
                throw std::runtime_error(fmt::format("Wrong token bytes size: expected {} but got {}", sizeof(_data), b.size()));
            }
            _data = net::ntoh(read_unaligned<int64_t>(b.begin()));
        }
    }

    token(kind k, bytes_view b) : _kind(std::move(k)) {
        if (_kind != kind::key) {
            _data = 0;
        } else {
            if (b.size() != sizeof(_data)) {
                throw std::runtime_error(fmt::format("Wrong token bytes size: expected {} but got {}", sizeof(_data), b.size()));
            }
            _data = net::ntoh(read_unaligned<int64_t>(b.begin()));
        }
    }

    std::strong_ordering operator<=>(const token& o) const noexcept {
        if (_kind < o._kind) {
            return std::strong_ordering::less;
        } else if (_kind > o._kind) {
            return std::strong_ordering::greater;
        } else if (_kind == token_kind::key) [[likely]] {
            return _data <=> o._data;
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const token& o) const noexcept = default;

    bool is_minimum() const noexcept {
        return _kind == kind::before_all_keys;
    }

    bool is_maximum() const noexcept {
        return _kind == kind::after_all_keys;
    }

    // Returns true iff this is the largest token which can be associated with a partition key.
    // Note that this is different that is_maximum().
    bool is_last() const {
        return _kind == dht::token::kind::key && _data == std::numeric_limits<int64_t>::max();
    }

    size_t external_memory_usage() const {
        return 0;
    }

    size_t memory_usage() const {
        return sizeof(token);
    }

    bytes data() const {
        bytes b(bytes::initialized_later(), sizeof(_data));
        write_unaligned<int64_t>(b.begin(), net::hton(_data));
        return b;
    }

    /**
     * @return a string representation of this token
     */
    sstring to_sstring() const;

    /**
     * Calculate a token representing the approximate "middle" of the given
     * range.
     *
     * @return The approximate midpoint between left and right.
     */
    static token midpoint(const token& left, const token& right);

    /**
     * @return a randomly generated token
     */
    static token get_random_token();

    /**
     * @return a token from string representation
     */
    static dht::token from_sstring(const sstring& t);

    /**
     * @return a token from its byte representation
     */
    static dht::token from_bytes(bytes_view bytes);

    /**
     * Returns int64_t representation of the token
     */
    static int64_t to_int64(token);

    /**
     * Creates token from its int64_t representation
     */
    static dht::token from_int64(int64_t);

    /**
     * Calculate the deltas between tokens in the ring in order to compare
     *  relative sizes.
     *
     * @param sortedtokens a sorted List of tokens
     * @return the mapping from 'token' to 'percentage of the ring owned by that token'.
     */
    static std::map<token, float> describe_ownership(const std::vector<token>& sorted_tokens);

    static data_type get_token_validator();

    /**
     * Gets the first shard of the minimum token.
     */
    static unsigned shard_of_minimum_token() {
        return 0;  // hardcoded for now; unlikely to change
    }

    int64_t raw() const noexcept {
        if (is_minimum()) {
            return std::numeric_limits<int64_t>::min();
        }
        if (is_maximum()) {
            return std::numeric_limits<int64_t>::max();
        }

        return _data;
    }
};

static inline std::strong_ordering tri_compare_raw(const int64_t l1, const int64_t l2) noexcept {
    if (l1 == l2) {
        return std::strong_ordering::equal;
    } else {
        return l1 < l2 ? std::strong_ordering::less : std::strong_ordering::greater;
    }
}

template <typename T>
concept TokenCarrier = requires (const T& v) {
    { v.token() } noexcept -> std::same_as<const token&>;
};

struct raw_token_less_comparator {
    bool operator()(const int64_t k1, const int64_t k2) const noexcept {
        return dht::tri_compare_raw(k1, k2) < 0;
    }

    template <typename Key>
    requires TokenCarrier<Key>
    bool operator()(const Key& k1, const int64_t k2) const noexcept {
        return dht::tri_compare_raw(k1.token().raw(), k2) < 0;
    }

    template <typename Key>
    requires TokenCarrier<Key>
    bool operator()(const int64_t k1, const Key& k2) const noexcept {
        return dht::tri_compare_raw(k1, k2.token().raw()) < 0;
    }

    template <typename Key>
    requires TokenCarrier<Key>
    int64_t simplify_key(const Key& k) const noexcept {
        return k.token().raw();
    }

    int64_t simplify_key(int64_t k) const noexcept {
        return k;
    }
};

const token& minimum_token() noexcept;
const token& maximum_token() noexcept;

std::ostream& operator<<(std::ostream& out, const token& t);

// Returns a successor for token t.
// The caller must ensure there is a next token, otherwise
// the result is unspecified.
//
// Precondition: t.kind() == dht::token::kind::key
inline
token next_token(const token& t) {
    return {dht::token::kind::key, t._data + 1};
}

// Returns the smallest token in the ring which can be associated with a partition key.
inline
token first_token() {
    // dht::token::normalize() does not allow std::numeric_limits<int64_t>::min()
    return dht::token(dht::token_kind::key, std::numeric_limits<int64_t>::min() + 1);
}

uint64_t unbias(const token& t);
token bias(uint64_t n);
size_t compaction_group_of(unsigned most_significant_bits, const token& t);
token last_token_of_compaction_group(unsigned most_significant_bits, size_t group);

struct token_comparator {
    // Return values are those of a trichotomic comparison.
    std::strong_ordering operator()(const token& t1, const token& t2) const {
        return t1 <=> t2;
    }
};

} // namespace dht

template <>
struct fmt::formatter<dht::token> : fmt::formatter<string_view> {
    template <typename FormatContext>
    auto format(const dht::token& t, FormatContext& ctx) const {
        if (t.is_maximum()) {
            return fmt::format_to(ctx.out(), "maximum token");
        } else if (t.is_minimum()) {
            return fmt::format_to(ctx.out(), "minimum token");
        } else {
            return fmt::format_to(ctx.out(), "{}", dht::token::to_int64(t));
        }
    }
};

namespace std {

template<>
struct hash<dht::token> {
    size_t operator()(const dht::token& t) const {
        // We have to reverse the bytes here to keep compatibility with
        // the behaviour that was here when tokens were represented as
        // sequence of bytes.
        return bswap_64(t._data);
    }
};

} // namespace std
