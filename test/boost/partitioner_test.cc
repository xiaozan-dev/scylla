/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/algorithm/cxx11/all_of.hpp>
#include <seastar/testing/thread_test_case.hh>

#include "dht/i_partitioner.hh"
#include "dht/murmur3_partitioner.hh"
#include "schema.hh"
#include "types.hh"
#include "schema_builder.hh"
#include "utils/div_ceil.hh"
#include "repair/range_split.hh"

#include "test/lib/simple_schema.hh"
#include "test/boost/total_order_check.hh"

template <typename... Args>
static
void
debug(Args&&... args) {
    if (false) {
        print(std::forward<Args>(args)...);
    }
}

static dht::token token_from_long(uint64_t value) {
    return dht::token(dht::token::kind::key, value);
}

static int64_t long_from_token(dht::token token) {
    return token._data;
}

SEASTAR_THREAD_TEST_CASE(test_decorated_key_is_compatible_with_origin) {
    auto s = schema_builder("ks", "cf")
        .with_column("c1", int32_type, column_kind::partition_key)
        .with_column("c2", int32_type, column_kind::partition_key)
        .with_column("v", int32_type)
        .build();

    dht::murmur3_partitioner partitioner;
    auto key = partition_key::from_deeply_exploded(*s, {143, 234});
    auto dk = partitioner.decorate_key(*s, key);

    // Expected value was taken from Origin
    BOOST_REQUIRE_EQUAL(dk._token, token_from_long(4958784316840156970));
    BOOST_REQUIRE(dk._key.equal(*s, key));
}

SEASTAR_THREAD_TEST_CASE(test_token_wraparound_1) {
    auto t1 = token_from_long(0x7000'0000'0000'0000);
    auto t2 = token_from_long(0xa000'0000'0000'0000);
    dht::murmur3_partitioner partitioner;
    BOOST_REQUIRE(t1 > t2);
    // Even without knowing what the midpoint is, it needs to be inside the
    // wrapped range, i.e., between t1 and inf, OR between -inf and t2
    auto midpoint = dht::token::midpoint(t1, t2);
    BOOST_REQUIRE(midpoint > t1 || midpoint < t2);
    // We can also calculate the actual value the midpoint should have:
    BOOST_REQUIRE_EQUAL(midpoint, token_from_long(0x8800'0000'0000'0000));
}

SEASTAR_THREAD_TEST_CASE(test_token_wraparound_2) {
    auto t1 = token_from_long(0x6000'0000'0000'0000);
    auto t2 = token_from_long(0x9000'0000'0000'0000);
    dht::murmur3_partitioner partitioner;
    BOOST_REQUIRE(t1 > t2);
    auto midpoint = dht::token::midpoint(t1, t2);
    BOOST_REQUIRE(midpoint > t1 || midpoint < t2);
    BOOST_REQUIRE_EQUAL(midpoint, token_from_long(0x7800'0000'0000'0000));
}

SEASTAR_THREAD_TEST_CASE(test_ring_position_is_comparable_with_decorated_key) {
    auto s = schema_builder("ks", "cf")
        .with_column("pk", bytes_type, column_kind::partition_key)
        .with_column("v", int32_type)
        .build();

    std::vector<dht::decorated_key> keys = {
        dht::global_partitioner().decorate_key(*s,
            partition_key::from_single_value(*s, "key1")),
        dht::global_partitioner().decorate_key(*s,
            partition_key::from_single_value(*s, "key2")),
    };

    std::sort(keys.begin(), keys.end(), dht::decorated_key::less_comparator(s));

    auto& k1 = keys[0];
    auto& k2 = keys[1];

    BOOST_REQUIRE(k1._token != k2._token); // The rest of the test assumes that.

    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position::starting_at(k1._token)) > 0);
    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position::ending_at(k1._token)) < 0);
    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position(k1)) == 0);

    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position::starting_at(k2._token)) < 0);
    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position::ending_at(k2._token)) < 0);
    BOOST_REQUIRE(k1.tri_compare(*s, dht::ring_position(k2)) < 0);

    BOOST_REQUIRE(k2.tri_compare(*s, dht::ring_position::starting_at(k1._token)) > 0);
    BOOST_REQUIRE(k2.tri_compare(*s, dht::ring_position::ending_at(k1._token)) > 0);
    BOOST_REQUIRE(k2.tri_compare(*s, dht::ring_position(k1)) > 0);
}

SEASTAR_THREAD_TEST_CASE(test_ring_position_ordering) {
    simple_schema table;
    auto cmp = dht::ring_position_comparator(*table.schema());

    std::vector<dht::decorated_key> keys = table.make_pkeys(6);

    // Force keys[2-4] to share the same token
    keys[2]._token = keys[3]._token = keys[4]._token;
    std::sort(keys.begin() + 2, keys.begin() + 5, dht::ring_position_less_comparator(*table.schema()));

    BOOST_TEST_MESSAGE(format("Keys: {}", keys));

    auto positions = boost::copy_range<std::vector<dht::ring_position>>(keys);
    auto ext_positions = boost::copy_range<std::vector<dht::ring_position_ext>>(keys);
    auto views = boost::copy_range<std::vector<dht::ring_position_view>>(positions);

    total_order_check<dht::ring_position_comparator, dht::ring_position, dht::ring_position_view, dht::decorated_key>(cmp)
      .next(dht::ring_position_view::min())
          .equal_to(dht::ring_position_ext::min())

      .next(dht::ring_position(keys[0].token(), dht::ring_position::token_bound::start))
          .equal_to(dht::ring_position_view(keys[0].token(), nullptr, -1))
          .equal_to(dht::ring_position_ext(keys[0].token(), std::nullopt, -1))
      .next(views[0])
        .equal_to(keys[0])
        .equal_to(positions[0])
        .equal_to(ext_positions[0])
      .next(dht::ring_position_view::for_after_key(keys[0]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[0]))
      .next(dht::ring_position(keys[0].token(), dht::ring_position::token_bound::end))
        .equal_to(dht::ring_position_view(keys[0].token(), nullptr, 1))
        .equal_to(dht::ring_position_ext(keys[0].token(), std::nullopt, 1))

      .next(dht::ring_position(keys[1].token(), dht::ring_position::token_bound::start))
          .equal_to(dht::ring_position_view(keys[1].token(), nullptr, -1))
          .equal_to(dht::ring_position_ext(keys[1].token(), std::nullopt, -1))
      .next(views[1])
        .equal_to(keys[1])
        .equal_to(positions[1])
        .equal_to(ext_positions[1])
      .next(dht::ring_position_view::for_after_key(keys[1]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[1]))

      .next(dht::ring_position(keys[1].token(), dht::ring_position::token_bound::end))
        .equal_to(dht::ring_position_view(keys[1].token(), nullptr, 1))
        .equal_to(dht::ring_position_ext(keys[1].token(), std::nullopt, 1))

      .next(dht::ring_position(keys[2].token(), dht::ring_position::token_bound::start))
          .equal_to(dht::ring_position_view(keys[2].token(), nullptr, -1))
          .equal_to(dht::ring_position_ext(keys[2].token(), std::nullopt, -1))

      .next(views[2])
        .equal_to(keys[2])
        .equal_to(positions[2])
        .equal_to(ext_positions[2])
      .next(dht::ring_position_view::for_after_key(keys[2]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[2]))

      .next(views[3])
        .equal_to(keys[3])
        .equal_to(positions[3])
        .equal_to(ext_positions[3])
      .next(dht::ring_position_view::for_after_key(keys[3]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[3]))

      .next(views[4])
        .equal_to(keys[4])
        .equal_to(positions[4])
        .equal_to(ext_positions[4])
      .next(dht::ring_position_view::for_after_key(keys[4]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[4]))

      .next(dht::ring_position(keys[4].token(), dht::ring_position::token_bound::end))
        .equal_to(dht::ring_position_view(keys[4].token(), nullptr, 1))
        .equal_to(dht::ring_position_ext(keys[4].token(), std::nullopt, 1))

      .next(dht::ring_position(keys[5].token(), dht::ring_position::token_bound::start))
        .equal_to(dht::ring_position_view(keys[5].token(), nullptr, -1))
        .equal_to(dht::ring_position_ext(keys[5].token(), std::nullopt, -1))
      .next(views[5])
        .equal_to(keys[5])
        .equal_to(positions[5])
        .equal_to(ext_positions[5])
      .next(dht::ring_position_view::for_after_key(keys[5]))
        .equal_to(dht::ring_position_ext::for_after_key(keys[5]))
      .next(dht::ring_position(keys[5].token(), dht::ring_position::token_bound::end))
        .equal_to(dht::ring_position_view(keys[5].token(), nullptr, 1))
        .equal_to(dht::ring_position_ext(keys[5].token(), std::nullopt, 1))

      .next(dht::ring_position_view::max())
        .equal_to(dht::ring_position_ext::max())
      .check();
}

SEASTAR_THREAD_TEST_CASE(test_token_no_wraparound_1) {
    auto t1 = token_from_long(0x5000'0000'0000'0000);
    auto t2 = token_from_long(0x7000'0000'0000'0000);
    BOOST_REQUIRE(t1 < t2);
    auto midpoint = dht::token::midpoint(t1, t2);
    BOOST_REQUIRE(midpoint > t1 && midpoint < t2);
    BOOST_REQUIRE_EQUAL(midpoint, token_from_long(0x6000'0000'0000'0000));
}

void test_partitioner_sharding(const dht::i_partitioner& part, unsigned shards, std::vector<dht::token> shard_limits,
        std::function<dht::token (const dht::i_partitioner&, dht::token)> prev_token, unsigned ignorebits = 0) {
    auto s = schema_builder("ks", "cf")
        .with_column("c1", int32_type, column_kind::partition_key)
        .with_column("c2", int32_type, column_kind::partition_key)
        .with_column("v", int32_type)
        .build();
    for (unsigned i = 0; i < (shards << ignorebits); ++i) {
        auto lim = shard_limits[i];
        BOOST_REQUIRE_EQUAL(part.shard_of(lim), i % shards);
        if (i != 0) {
            BOOST_REQUIRE_EQUAL(part.shard_of(prev_token(part, lim)), (i - 1) % shards);
            BOOST_REQUIRE_EQUAL(lim, part.token_for_next_shard(prev_token(part, lim), i % shards));
        }
        if (i != (shards << ignorebits) - 1) {
            auto next_shard = (i + 1) % shards;
            BOOST_REQUIRE_EQUAL(part.shard_of(part.token_for_next_shard(lim, next_shard)), next_shard);
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_murmur3_sharding) {
    auto prev_token = [] (const dht::i_partitioner&, dht::token token) {
        return token_from_long(long_from_token(token) - 1);
    };
    auto make_token_vector = [] (std::vector<int64_t> v) {
        return boost::copy_range<std::vector<dht::token>>(
                v | boost::adaptors::transformed(token_from_long));
    };
    dht::murmur3_partitioner mm3p7s(7);
    auto mm3p7s_shard_limits = make_token_vector({
        -9223372036854775807, -6588122883467697006+1, -3952873730080618204+1,
        -1317624576693539402+1, 1317624576693539401+1, 3952873730080618203+1,
        6588122883467697005+1,
    });
    test_partitioner_sharding(mm3p7s, 7, mm3p7s_shard_limits, prev_token);
    dht::murmur3_partitioner mm3p2s(2);
    auto mm3p2s_shard_limits = make_token_vector({
        -9223372036854775807, 0,
    });
    test_partitioner_sharding(mm3p2s, 2, mm3p2s_shard_limits, prev_token);
    dht::murmur3_partitioner mm3p1s(1);
    auto mm3p1s_shard_limits = make_token_vector({
        -9223372036854775807,
    });
    test_partitioner_sharding(mm3p1s, 1, mm3p1s_shard_limits, prev_token);
}

SEASTAR_THREAD_TEST_CASE(test_murmur3_sharding_with_ignorebits) {
    auto prev_token = [] (const dht::i_partitioner&, dht::token token) {
        return token_from_long(long_from_token(token) - 1);
    };
    auto make_token_vector = [] (std::vector<int64_t> v) {
        return boost::copy_range<std::vector<dht::token>>(
                v | boost::adaptors::transformed(token_from_long));
    };
    dht::murmur3_partitioner mm3p7s2i(7, 2);
    auto mm3p7s2i_shard_limits = make_token_vector({
        -9223372036854775807,
        -8564559748508006107, -7905747460161236406, -7246935171814466706, -6588122883467697005,
        -5929310595120927305, -5270498306774157604, -4611686018427387904, -3952873730080618203,
        -3294061441733848502, -2635249153387078802, -1976436865040309101, -1317624576693539401,
        -658812288346769700, 0, 658812288346769701, 1317624576693539402, 1976436865040309102,
        2635249153387078803, 3294061441733848503, 3952873730080618204, 4611686018427387904,
        5270498306774157605, 5929310595120927306, 6588122883467697006, 7246935171814466707,
        7905747460161236407, 8564559748508006108,
    });
    test_partitioner_sharding(mm3p7s2i, 7, mm3p7s2i_shard_limits, prev_token, 2);
    dht::murmur3_partitioner mm3p2s4i(2, 4);
    auto mm3p2s_shard_limits = make_token_vector({
        -9223372036854775807,
        -8646911284551352320, -8070450532247928832, -7493989779944505344, -6917529027641081856,
        -6341068275337658368, -5764607523034234880, -5188146770730811392, -4611686018427387904,
        -4035225266123964416, -3458764513820540928, -2882303761517117440, -2305843009213693952,
        -1729382256910270464, -1152921504606846976, -576460752303423488, 0, 576460752303423488,
        1152921504606846976, 1729382256910270464, 2305843009213693952, 2882303761517117440,
        3458764513820540928, 4035225266123964416, 4611686018427387904, 5188146770730811392,
        5764607523034234880, 6341068275337658368, 6917529027641081856, 7493989779944505344,
        8070450532247928832, 8646911284551352320,
    });
    test_partitioner_sharding(mm3p2s4i, 2, mm3p2s_shard_limits, prev_token, 4);
}

static
dht::partition_range
normalize(dht::partition_range pr) {
    auto start = pr.start();
    if (start && start->value().token() == dht::minimum_token()) {
        start = std::nullopt;
    }
    auto end = pr.end();
    if (end && end->value().token() == dht::maximum_token()) {
        end = std::nullopt;
    }
    return dht::partition_range(start, end);
};

static
void
test_exponential_sharder(const dht::i_partitioner& part, const schema& s, const dht::partition_range& pr) {

    dht::set_global_partitioner(part.name()); // so we can print tokens, also ring_position_comparator is not global_partitioner() clean

    // Step 1: run the exponential sharder fully, and collect all results

    debug("input range: %s\n", pr);
    auto results = std::vector<dht::ring_position_exponential_sharder_result>();
    auto sharder = dht::ring_position_exponential_sharder(part, pr);
    auto partial_result = sharder.next(s);
    while (partial_result) {
        results.push_back(std::move(*partial_result));
        partial_result = sharder.next(s);
    }

    // Step 2: "de-exponentialize" the result by fragmenting large ranges

    struct fragmented_sharder_result {
        bool inorder;
        struct shard_result {
            shard_id shard;
            std::vector<dht::partition_range> ranges;
        };
        std::vector<shard_result> shards;
    };
    auto fragmented_results = std::vector<fragmented_sharder_result>();
    for (auto&& partial_result : results) {
        auto fsr = fragmented_sharder_result();
        fsr.inorder = partial_result.inorder;
        debug("looking at partial result\n");
        for (auto&& per_shard_range : partial_result.per_shard_ranges) {
            debug("partial_result: looking at %s (shard %d)\n", per_shard_range.ring_range, per_shard_range.shard);
            auto sr = fragmented_sharder_result::shard_result();
            sr.shard = per_shard_range.shard;
            auto sharder = dht::ring_position_range_sharder(part, per_shard_range.ring_range);
            auto next = sharder.next(s);
            while (next) {
                debug("seeing: shard %d frag %s\n", next->shard, next->ring_range);
                if (next->shard == sr.shard) {
                    debug("fragmented to %d\n", next->ring_range);
                    sr.ranges.push_back(std::move(next->ring_range));
                }
                next = sharder.next(s);
            }
            fsr.shards.push_back(std::move(sr));
        }
        fragmented_results.push_back(std::move(fsr));
    }

    // Step 3: collect all fragmented ranges

    auto all_fragments = std::vector<dht::partition_range>();
    for (auto&& fr : fragmented_results) {
        for (auto&& sr : fr.shards) {
            for (auto&& f : sr.ranges) {
                all_fragments.push_back(f);
            }
        }
    }

    // Step 4: verify no overlaps

    bool no_overlaps = true;
    if (all_fragments.size() > 1) {
        for (auto i : boost::irange<size_t>(1, all_fragments.size() - 1)) {
            for (auto j : boost::irange<size_t>(0, i)) {
                no_overlaps &= !all_fragments[i].overlaps(all_fragments[j], dht::ring_position_comparator(s));
            }
        }
    }
    BOOST_REQUIRE(no_overlaps);  // We OOM if BOOST_REQUIRE() is run in the inner loop

    // Step 5: verify all fragments are contiguous

    auto rplc = dht::ring_position_less_comparator(s);
    auto rptc = dht::ring_position_comparator(s);
    boost::sort(all_fragments, [&] (const dht::partition_range& a, const dht::partition_range b) {
        if (!a.start() || !b.start()) {
            return unsigned(bool(a.start())) < unsigned(bool(b.start()));
        } else {
            return rplc(a.start()->value(), b.start()->value());
        }
    });
    auto not_adjacent = [&] (const dht::partition_range& a, const dht::partition_range b) {
        return !a.end() || !b.start() || rptc(a.end()->value(), b.start()->value()) != 0;
    };
    BOOST_REQUIRE(boost::adjacent_find(all_fragments, not_adjacent) == all_fragments.end());

    // Step 6: verify inorder is accurate; allow a false negative

    for (auto&& fsr : fragmented_results) {
        auto has_one_fragment = [] (const fragmented_sharder_result::shard_result& sr) {
            return sr.ranges.size() <= 1;  // the sharder may return a range that does not intersect the shard
        };
        BOOST_REQUIRE(!fsr.inorder || boost::algorithm::all_of(fsr.shards, has_one_fragment));
    }

    // Step 7: verify that the fragmented range matches the input range (since the fragments are
    //         contiguous, we need only test the edges).

    auto reconstructed = normalize(dht::partition_range(all_fragments.front().start(), all_fragments.back().end()));
    auto original = normalize(pr);
    debug("original %s reconstructed %s\n", original, reconstructed);
    BOOST_REQUIRE(original.contains(reconstructed, rptc) && reconstructed.contains(original, rptc));

    // Step 8: verify exponentiality
    debug("sizes %d %d\n", results.size(), 1 + log2ceil(div_ceil(all_fragments.size(), part.shard_count())) + log2ceil(part.shard_count()));
    BOOST_REQUIRE(results.size() <= 1 + log2ceil(div_ceil(all_fragments.size(), part.shard_count())) + log2ceil(part.shard_count()));
}

static
void
test_something_with_some_interesting_ranges_and_partitioners(std::function<void (const dht::i_partitioner&, const schema&, const dht::partition_range&)> func_to_test) {
    auto s = schema_builder("ks", "cf")
        .with_column("c1", int32_type, column_kind::partition_key)
        .with_column("c2", int32_type, column_kind::partition_key)
        .with_column("v", int32_type)
        .build();
    auto some_murmur3_partitioners = {
            dht::murmur3_partitioner(1, 0),
            dht::murmur3_partitioner(7, 4),
            dht::murmur3_partitioner(4, 0),
            dht::murmur3_partitioner(32, 8),  // More, and we OOM since memory isn't configured
    };
    auto t1 = token_from_long(int64_t(-0x7fff'ffff'ffff'fffe));
    auto t2 = token_from_long(int64_t(-1));
    auto t3 = token_from_long(int64_t(1));
    auto t4 = token_from_long(int64_t(0x7fff'ffff'ffff'fffe));
    auto make_bound = [] (dht::ring_position rp) {
        return std::make_optional(range_bound<dht::ring_position>(std::move(rp)));
    };
    auto some_murmur3_ranges = {
            dht::partition_range::make_open_ended_both_sides(),
            dht::partition_range::make_starting_with(dht::ring_position::starting_at(t1)),
            dht::partition_range::make_starting_with(dht::ring_position::starting_at(t2)),
            dht::partition_range::make_starting_with(dht::ring_position::ending_at(t3)),
            dht::partition_range::make_starting_with(dht::ring_position::starting_at(t4)),
            dht::partition_range::make_ending_with(dht::ring_position::starting_at(t1)),
            dht::partition_range::make_ending_with(dht::ring_position::starting_at(t2)),
            dht::partition_range::make_ending_with(dht::ring_position::starting_at(t3)),
            dht::partition_range::make_ending_with(dht::ring_position::starting_at(t4)),
            dht::partition_range(make_bound(dht::ring_position::starting_at(t2)), make_bound(dht::ring_position::ending_at(t3))),
            dht::partition_range(make_bound(dht::ring_position::ending_at(t1)), make_bound(dht::ring_position::starting_at(t4))),
    };
    for (auto&& part : some_murmur3_partitioners) {
        for (auto&& range : some_murmur3_ranges) {
            func_to_test(part, *s, range);
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_exponential_sharders) {
    return test_something_with_some_interesting_ranges_and_partitioners(test_exponential_sharder);
}

static
void
do_test_split_range_to_single_shard(const dht::i_partitioner& part, const schema& s, const dht::partition_range& pr) {
    dht::set_global_partitioner(part.name()); // so we can print tokens, also ring_position_comparator is not global_partitioner() clean

    for (auto shard : boost::irange(0u, part.shard_count())) {
        auto ranges = dht::split_range_to_single_shard(part, s, pr, shard).get0();
        auto sharder = dht::ring_position_range_sharder(part, pr);
        auto x = sharder.next(s);
        auto cmp = dht::ring_position_comparator(s);
        auto reference_ranges = std::vector<dht::partition_range>();
        while (x) {
            if (x->shard == shard) {
                reference_ranges.push_back(std::move(x->ring_range));
            }
            x = sharder.next(s);
        }
        BOOST_REQUIRE(ranges.size() == reference_ranges.size());
        for (auto&& rs : boost::combine(ranges, reference_ranges)) {
            auto&& r1 = normalize(boost::get<0>(rs));
            auto&& r2 = normalize(boost::get<1>(rs));
            BOOST_REQUIRE(r1.contains(r2, cmp));
            BOOST_REQUIRE(r2.contains(r1, cmp));
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_split_range_single_shard) {
    return test_something_with_some_interesting_ranges_and_partitioners(do_test_split_range_to_single_shard);
}

// tests for range_split() utility function in repair/range_split.hh
static int test_split(int N, int K) {
    auto t1 = token_from_long(0x2000'0000'0000'0000);
    auto t2 = token_from_long(0x5000'0000'0000'0000);
    dht::token_range r{range_bound<dht::token>(t1), range_bound<dht::token>(t2)};
    auto splitter = range_splitter(r, N, K);
    int c = 0;
    dht::token_range prev_range;
    while (splitter.has_next()) {
        auto range = splitter.next();
        //std::cerr << range << "\n";
        if (c == 0) {
            BOOST_REQUIRE(range.start() == r.start());
        } else {
            std::optional<dht::token_range::bound> e({prev_range.end()->value(), !prev_range.end()->is_inclusive()});
            BOOST_REQUIRE(range.start() == e);
        }
        prev_range = range;
        c++;
    }
    if (c > 0) {
        BOOST_REQUIRE(prev_range.end() == r.end());
    }
    return c;
}

SEASTAR_THREAD_TEST_CASE(test_split_1) {
    BOOST_REQUIRE(test_split(128, 16) == 8);
    // will make 7 binary splits: 500, 250, 125.5, 62.5, 31.25, 15.625,
    // 7.8125, so expect 2^7 = 128 ranges:
    BOOST_REQUIRE(test_split(1000, 11) == 128);
}

static
void
test_something_with_some_interesting_ranges_and_partitioners_with_token_range(std::function<void (const dht::i_partitioner&, const schema&, const dht::token_range&)> func_to_test) {
    auto s = schema_builder("ks", "cf")
        .with_column("c1", int32_type, column_kind::partition_key)
        .with_column("c2", int32_type, column_kind::partition_key)
        .with_column("v", int32_type)
        .build();
    auto some_murmur3_partitioners = {
            dht::murmur3_partitioner(1, 0),
            dht::murmur3_partitioner(7, 4),
            dht::murmur3_partitioner(4, 0),
            dht::murmur3_partitioner(32, 8),  // More, and we OOM since memory isn't configured
    };
    auto t1 = token_from_long(int64_t(-0x7fff'ffff'ffff'fffe));
    auto t2 = token_from_long(int64_t(-1));
    auto t3 = token_from_long(int64_t(1));
    auto t4 = token_from_long(int64_t(0x7fff'ffff'ffff'fffe));
    auto make_bound = [] (dht::token t) {
        return range_bound<dht::token>(std::move(t));
    };
    auto some_murmur3_ranges = {
            dht::token_range::make_open_ended_both_sides(),
            dht::token_range::make_starting_with(make_bound(t1)),
            dht::token_range::make_starting_with(make_bound(t2)),
            dht::token_range::make_starting_with(make_bound(t3)),
            dht::token_range::make_starting_with(make_bound(t4)),
            dht::token_range::make_ending_with(make_bound(t1)),
            dht::token_range::make_ending_with(make_bound(t2)),
            dht::token_range::make_ending_with(make_bound(t3)),
            dht::token_range::make_ending_with(make_bound(t4)),
            dht::token_range(make_bound(t2), make_bound(t3)),
            dht::token_range(make_bound(t1), make_bound(t4)),
    };
    for (auto&& part : some_murmur3_partitioners) {
        for (auto&& range : some_murmur3_ranges) {
            func_to_test(part, *s, range);
        }
    }
}

static
void
do_test_selective_token_range_sharder(const dht::i_partitioner& part, const schema& s, const dht::token_range& range) {
    dht::set_global_partitioner(part.name());
    bool debug = false;
    for (auto shard : boost::irange(0u, part.shard_count())) {
        auto sharder = dht::selective_token_range_sharder(part, range, shard);
        auto range_shard = sharder.next();
        while (range_shard) {
            if (range_shard->start() && range_shard->start()->is_inclusive()) {
                auto start_shard = part.shard_of(range_shard->start()->value());
                if (debug) {
                    std::cout << " start_shard " << start_shard << " shard " << shard << " range " << range_shard << "\n";
                }
                BOOST_REQUIRE(start_shard == shard);
            }
            if (range_shard->end() && range_shard->end()->is_inclusive()) {
                auto end_shard = part.shard_of(range_shard->end()->value());
                if (debug) {
                    std::cout << " end_shard " << end_shard << " shard " << shard << " range " << range_shard << "\n";
                }
                BOOST_REQUIRE(end_shard == shard);
            }
            auto midpoint = dht::token::midpoint(
                    range_shard->start() ? range_shard->start()->value() : dht::minimum_token(),
                    range_shard->end() ? range_shard->end()->value() : dht::minimum_token());
            auto mid_shard = part.shard_of(midpoint);
            if (debug) {
                std::cout << " mid " << mid_shard << " shard " << shard << " range " << range_shard << "\n";
            }
            BOOST_REQUIRE(mid_shard == shard);

            range_shard = sharder.next();
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_selective_token_range_sharder) {
    return test_something_with_some_interesting_ranges_and_partitioners_with_token_range(do_test_selective_token_range_sharder);
}
