// Copyright (c) 2021 Michael G. Kazakov. All rights reserved. Distributed under the MIT License.
#pragma once
#include <algorithm>
#include <numeric>
#include <iterator>
#include <vector>

namespace pstld {

namespace internal {

size_t max_hw_threads() noexcept;

void dispatch_apply(size_t iterations, void *ctx, void (*function)(void *, size_t)) noexcept;

template <class It>
using iterator_category_t = typename std::iterator_traits<It>::iterator_category;

template <class It>
inline constexpr bool is_random_iterator_v =
    std::is_convertible_v<iterator_category_t<It>, std::random_access_iterator_tag>;

template <class It>
using iterator_value_t = typename std::iterator_traits<It>::value_type;

struct no_op {
    template <typename T>
    T &&operator()(T &&v) const
    {
        return std::forward<T>(v);
    }
};

struct parallelism_exception : std::exception {
    const char *what() const noexcept override;
    [[noreturn]] static void raise();
};

template <class T>
struct parallelism_allocator {
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    T *allocate(size_t count)
    {
        if( void *ptr = ::operator new(count * sizeof(T), std::nothrow) )
            return static_cast<T *>(ptr);
        else
            parallelism_exception::raise();
    }

    void deallocate(T *ptr, size_t count) noexcept { ::operator delete(ptr, count * sizeof(T)); }

    template <class Other>
    bool operator==(const parallelism_allocator<Other> &) const noexcept
    {
        return true;
    }

    template <class Other>
    bool operator!=(const parallelism_allocator<Other> &) const noexcept
    {
        return false;
    }
};

template <class T>
using parallelism_vector = std::vector<T, parallelism_allocator<T>>;

template <class T>
struct unitialized_array : parallelism_allocator<T> {
    using allocator = parallelism_allocator<T>;
    T *m_data;
    size_t m_size;
    unitialized_array(size_t size) : m_size(size), m_data(allocator::allocate(size)) {}

    ~unitialized_array()
    {
        std::destroy(m_data, m_data + m_size);
        allocator::deallocate(m_data, m_size);
    }

    template <class... Args>
    void put(size_t ind, Args &&...vals) noexcept
    {
        ::new(m_data + ind) T(std::forward<Args>(vals)...);
    }

    T *begin() noexcept { return m_data; }

    T *end() noexcept { return m_data + m_size; }
};

inline constexpr size_t chunks_per_cpu = 8;

template <class T>
constexpr size_t work_chunks_min_fraction_1(T count)
{
    return std::min(max_hw_threads() * chunks_per_cpu, static_cast<size_t>(count));
}

template <class T>
constexpr size_t work_chunks_min_fraction_2(T count)
{
    return std::min(max_hw_threads() * chunks_per_cpu, static_cast<size_t>(count / 2));
}

template <class It>
struct ItRange {
    It first;
    It last;
};

template <class It, bool IsRandomAccess = is_random_iterator_v<It>>
struct Partition;

template <class It>
struct Partition<It, true> {
    It base;
    size_t fraction;
    size_t leftover;
    Partition(It first, size_t count, size_t chunks)
        : base(first), fraction(count / chunks), leftover(count % chunks)
    {
    }

    ItRange<It> at(size_t chunk_no)
    {
        if( leftover ) {
            if( chunk_no >= leftover ) {
                const auto first =
                    base + (fraction + 1) * leftover + fraction * (chunk_no - leftover);
                const auto last = first + fraction;
                return {first, last};
            }
            else {
                const auto first = base + (fraction + 1) * chunk_no;
                const auto last = first + fraction + 1;
                return {first, last};
            }
        }
        else {
            const auto first = base + fraction * chunk_no;
            const auto last = first + fraction;
            return {first, last};
        }
    }
};

template <class It>
struct Partition<It, false> {
    parallelism_vector<ItRange<It>> segments;
    Partition(It first, size_t count, size_t chunks) : segments(chunks)
    {
        size_t fraction = count / chunks;
        size_t leftover = count % chunks;
        It it = first;
        for( size_t i = 0; i != chunks; ++i ) {
            auto first = it;
            auto diff = fraction;
            if( leftover != 0 ) {
                ++diff;
                --leftover;
            }
            auto last = std::next(it, diff);
            segments[i] = {first, last};
            it = last;
        }
    }

    ItRange<It> at(size_t chunk_no) { return segments[chunk_no]; }
};

template <class T>
struct Dispatchable {
    static void dispatch(void *ctx, size_t ind) noexcept { static_cast<T *>(ctx)->run(ind); }
};

} // namespace internal

namespace internal {

template <class It, class T, class BinOp, class UnOp>
struct TransformReduce : Dispatchable<TransformReduce<It, T, BinOp, UnOp>> {
    Partition<It> m_partition;
    unitialized_array<T> m_results;
    BinOp m_reduce;
    UnOp m_transform;

    TransformReduce(size_t count, size_t chunks, It first, BinOp reduce_op, UnOp transform_op)
        : m_partition(first, count, chunks), m_results(chunks), m_reduce(reduce_op),
          m_transform(transform_op)
    {
    }

    void run(size_t ind) noexcept
    {
        auto p = m_partition.at(ind);
        m_results.put(ind, transform_reduce_at_least_2(p.first, p.last));
    }

    T transform_reduce_at_least_2(It first, It last)
    {
        auto next = first;
        T val = m_reduce(m_transform(*first), m_transform(*++next));
        while( ++next != last )
            val = m_reduce(std::move(val), m_transform(*next));
        return val;
    }
};

} // namespace internal

template <class FwdIt, class T, class BinOp, class UnOp>
T transform_reduce(FwdIt first, FwdIt last, T val, BinOp reduce_op, UnOp transform_op) noexcept
{
    const auto count = std::distance(first, last);
    const auto chunks = internal::work_chunks_min_fraction_2(count);
    if( chunks > 1 ) {
        try {
            internal::TransformReduce<FwdIt, T, BinOp, UnOp> op{
                static_cast<size_t>(count), chunks, first, reduce_op, transform_op};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return std::reduce(op.m_results.begin(), op.m_results.end(), val, reduce_op);
        } catch( const internal::parallelism_exception & ) {
        }
    }

    return std::transform_reduce(first, last, val, reduce_op, transform_op);
}

template <class It>
internal::iterator_value_t<It> reduce(It first, It last) noexcept
{
    using T = internal::iterator_value_t<It>;
    return ::pstld::transform_reduce(first, last, T{}, std::plus<>{}, ::pstld::internal::no_op{});
}

template <class It, class T>
T reduce(It first, It last, T val) noexcept
{
    return ::pstld::transform_reduce(first, last, val, std::plus<>{}, ::pstld::internal::no_op{});
}

template <class It, class T, class BinOp>
T reduce(It first, It last, T val, BinOp op) noexcept
{
    return ::pstld::transform_reduce(first, last, val, op, ::pstld::internal::no_op{});
}

namespace internal {

template <class It1, class It2, class T, class BinRedOp, class BinTrOp>
struct TransformReduce2 : Dispatchable<TransformReduce2<It1, It2, T, BinRedOp, BinTrOp>> {
    Partition<It1> m_partition1;
    Partition<It2> m_partition2;
    unitialized_array<T> m_results;
    BinRedOp m_reduce;
    BinTrOp m_transform;

    TransformReduce2(size_t count,
                     size_t chunks,
                     It1 first1,
                     It2 first2,
                     BinRedOp reduce_op,
                     BinTrOp transform_op)
        : m_partition1(first1, count, chunks), m_partition2(first2, count, chunks),
          m_results(chunks), m_reduce(reduce_op), m_transform(transform_op)
    {
    }

    void run(size_t ind) noexcept
    {
        auto p1 = m_partition1.at(ind);
        auto p2 = m_partition2.at(ind);
        m_results.put(ind, transform_reduce_at_least_2(p1.first, p1.last, p2.first));
    }

    T transform_reduce_at_least_2(It1 first1, It1 last1, It2 first2)
    {
        auto next1 = first1;
        auto next2 = first2;
        T val = m_reduce(m_transform(*first1, *first2), m_transform(*++next1, *++next2));
        while( ++next1 != last1 )
            val = m_reduce(std::move(val), m_transform(*next1, *++next2));
        return val;
    }
};

} // namespace internal

template <class FwdIt1, class FwdIt2, class T, class BinRedOp, class BinTrOp>
T transform_reduce(FwdIt1 first1,
                   FwdIt1 last1,
                   FwdIt2 first2,
                   T val,
                   BinRedOp reduce_op,
                   BinTrOp transform_op) noexcept
{
    const auto count = std::distance(first1, last1);
    const auto chunks = internal::work_chunks_min_fraction_2(count);
    if( chunks > 1 ) {
        try {
            internal::TransformReduce2<FwdIt1, FwdIt2, T, BinRedOp, BinTrOp> op{
                static_cast<size_t>(count), chunks, first1, first2, reduce_op, transform_op};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return std::reduce(op.m_results.begin(), op.m_results.end(), val, reduce_op);
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::transform_reduce(first1, last1, first2, val, reduce_op, transform_op);
}

template <class It1, class It2, class T>
T transform_reduce(It1 first1, It1 last1, It2 first2, T val) noexcept
{
    return ::pstld::transform_reduce(
        first1, last1, first2, val, std::plus<>{}, std::multiplies<>{});
}

namespace internal {

template <class It, class UnPred, bool Expected, bool Init>
struct AllOf : Dispatchable<AllOf<It, UnPred, Expected, Init>> {
    Partition<It> m_partition;
    UnPred m_pred;
    std::atomic_bool m_done{false};
    bool m_result = Init;

    AllOf(size_t count, size_t chunks, It first, UnPred pred)
        : m_partition(first, count, chunks), m_pred(pred)
    {
    }

    void run(size_t ind) noexcept
    {
        if( m_done )
            return;
        for( auto p = m_partition.at(ind); p.first != p.last; ++p.first )
            if( static_cast<bool>(m_pred(*p.first)) == !Expected ) {
                m_done = true;
                m_result = !Init;
                return;
            }
    }
};

} // namespace internal

template <class FwdIt, class UnPred>
bool all_of(FwdIt first, FwdIt last, UnPred pred) noexcept
{
    const auto count = std::distance(first, last);
    const auto chunks = internal::work_chunks_min_fraction_1(count);
    if( chunks > 1 ) {
        try {
            internal::AllOf<FwdIt, UnPred, true, true> op{
                static_cast<size_t>(count), chunks, first, pred};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return op.m_result;
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::all_of(first, last, pred);
}

template <class FwdIt, class UnPred>
bool none_of(FwdIt first, FwdIt last, UnPred pred) noexcept
{
    const auto count = std::distance(first, last);
    const auto chunks = internal::work_chunks_min_fraction_1(count);
    if( chunks > 1 ) {
        try {
            internal::AllOf<FwdIt, UnPred, false, true> op{
                static_cast<size_t>(count), chunks, first, pred};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return op.m_result;
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::none_of(first, last, pred);
}

template <class FwdIt, class UnPred>
bool any_of(FwdIt first, FwdIt last, UnPred pred) noexcept
{
    const auto count = std::distance(first, last);
    const auto chunks = internal::work_chunks_min_fraction_1(count);
    if( chunks > 1 ) {
        try {
            internal::AllOf<FwdIt, UnPred, false, false> op{
                static_cast<size_t>(count), chunks, first, pred};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return op.m_result;
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::any_of(first, last, pred);
}

namespace internal {

template <class It, class Func>
struct ForEach : Dispatchable<ForEach<It, Func>> {
    Partition<It> m_partition;
    Func m_func;

    ForEach(size_t count, size_t chunks, It first, Func func)
        : m_partition(first, count, chunks), m_func(func)
    {
    }

    void run(size_t ind) noexcept
    {
        for( auto p = m_partition.at(ind); p.first != p.last; ++p.first )
            m_func(*p.first);
    }
};

} // namespace internal

template <class FwdIt, class Func>
void for_each(FwdIt first, FwdIt last, Func func) noexcept
{
    const auto count = std::distance(first, last);
    const auto chunks = internal::work_chunks_min_fraction_1(count);
    if( chunks > 1 ) {
        try {
            internal::ForEach<FwdIt, Func> op{static_cast<size_t>(count), chunks, first, func};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return;
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::for_each(first, last, func);
}

template <class FwdIt, class Size, class Func>
void for_each_n(FwdIt first, Size count, Func func) noexcept
{
    const auto chunks = internal::work_chunks_min_fraction_1(count);
    if( chunks > 1 ) {
        try {
            internal::ForEach<FwdIt, Func> op{static_cast<size_t>(count), chunks, first, func};
            internal::dispatch_apply(chunks, &op, op.dispatch);
            return;
        } catch( const internal::parallelism_exception & ) {
        }
    }
    return std::for_each_n(first, count, func);
}

} // namespace pstld
