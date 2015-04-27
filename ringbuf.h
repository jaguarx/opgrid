#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef LIKELY__
#undef LIKELY__
#endif
#define LIKELY__(expr__) (__builtin_expect(((expr__) ? 1 : 0), 1))

#ifdef UNLIKELY__
#undef UNLIKELY__
#endif
#define UNLIKELY__(expr__) (__builtin_expect(((expr__) ? 1 : 0), 0))

constexpr int cache_line_padded_size(int element_size, int cache_line_size)
{
    return (cache_line_size > element_size) ?
            cache_line_size - element_size : element_size % cache_line_size;
}

template<class T, int cache_line_size>
struct cache_line_aligned_struct {
    T content;
    uint8_t __padding[cache_line_padded_size(sizeof(T), cache_line_size)];
} __attribute__((aligned(cache_line_size)));

#define VACANT__ (UINT_FAST64_MAX)

template<class T, int processor_capacity, int cache_line_size = 64, int page_size = 4096>
struct ring_buffer_t {
    struct count_t {
        uint_fast64_t count;
        uint8_t __padding[cache_line_padded_size(sizeof(uint_fast64_t), cache_line_size)];
    } __attribute__((aligned(cache_line_size)));

    struct cursor_t {
        uint_fast64_t sequence;
        uint8_t __padding[cache_line_padded_size(sizeof(uint_fast64_t), cache_line_size)];
    } __attribute__((aligned(cache_line_size)));

    typedef cache_line_aligned_struct<T, cache_line_size> entry_t;
    typedef cache_line_aligned_struct<timespec, cache_line_size> yield_t;

    ring_buffer_t(int buf_size = 128) :
            impl_(NULL), buf_size_(0)
    {
        timeout__ = {{0,1},{0}};
        int total_size = sizeof(_impl_t) + buf_size * sizeof(entry_t);
        impl_ = (_impl_t*)aligned_alloc(page_size, total_size);
        //posix_memalign((void**)&impl_, PAGE_SIZE, total_size);
        buf_size_ = buf_size;
        memset(impl_, 0, total_size);
        for (unsigned int n = 0; n < processor_capacity; ++n)
        impl_->entry_processor_cursors[n].sequence = VACANT__;
        __atomic_store_n(&impl_->reduced_size.count, buf_size - 1, __ATOMIC_SEQ_CST);
    }

    uint_fast64_t processor_barrier_register(count_t& entry_processor_number)
    {
        uint_fast64_t vacant = VACANT__;

        do {
            for (unsigned int n = 0; n < processor_capacity; ++n) {
                if (__atomic_compare_exchange_n(&impl_->entry_processor_cursors[n].sequence,
                                &vacant,
                                __atomic_load_n(&impl_->slowest_entry_processor.sequence, __ATOMIC_CONSUME),
                                1,
                                __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED))
                {
                    entry_processor_number.count = n;
                    goto out;
                }
                vacant = VACANT__;
            }
        }while (1);
        out:
        if (!impl_->entry_processor_cursors[entry_processor_number.count].sequence)
        {
            __atomic_store_n(&impl_->entry_processor_cursors[entry_processor_number.count].sequence, 1, __ATOMIC_RELEASE);
            return 1;
        }
        return impl_->entry_processor_cursors[entry_processor_number.count].sequence;
    }

    void processor_barrier_unregister(count_t& entry_processor_number)
    {
        __atomic_store_n(&impl_->entry_processor_cursors[entry_processor_number.count].sequence, VACANT__, __ATOMIC_RELEASE);
    }

    const entry_t& show_entry(cursor_t& cursor)
    {
        return impl_->buffer[impl_->reduced_size.count & cursor.sequence];
    }

    entry_t& processor_acquire_entry(cursor_t& cursor)
    {
        return impl_->buffer[impl_->reduced_size.count & cursor.sequence];
    }

    void processor_barrier_wait_blocking(cursor_t& cursor)
    {
        const cursor_t incur = { cursor.sequence, {0}};

        while (incur.sequence > __atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_RELAXED))
            nanosleep(&timeout__.content, NULL);

        cursor.sequence = __atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_ACQUIRE);
    }

    bool processor_barrier_wait_nonblocking(cursor_t& cursor)
    {
        const cursor_t incur = { cursor.sequence, {0} };

        if (incur.sequence > __atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_RELAXED))
            return false;

        cursor.sequence = __atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_ACQUIRE);

        return true;
    }

    void processor_barrier_release_entry(count_t& entry_processor_number,
            cursor_t& cursor)
    {
        __atomic_store_n(&impl_->entry_processor_cursors[entry_processor_number.count].sequence,
                cursor.sequence, __ATOMIC_RELAXED);
    }

    void publisher_next_entry_blocking(cursor_t& cursor)
    {
        cursor_t seq;
        cursor_t slowest_reader;
        const cursor_t incur = {1 + __atomic_fetch_add(&impl_->write_cursor.sequence, 1, __ATOMIC_RELAXED),
            {0}};

        cursor.sequence = incur.sequence;
        do {
            slowest_reader.sequence = VACANT__;
            for (unsigned int n = 0; n < processor_capacity; ++n) {
                seq.sequence = __atomic_load_n(&impl_->entry_processor_cursors[n].sequence, __ATOMIC_RELAXED);
                if (seq.sequence < slowest_reader.sequence)
                    slowest_reader.sequence = seq.sequence;
            }

            if (UNLIKELY__(VACANT__ == slowest_reader.sequence))
                slowest_reader.sequence = incur.sequence - (impl_->reduced_size.count & incur.sequence);

            __atomic_store_n(&impl_->slowest_entry_processor.sequence, slowest_reader.sequence, __ATOMIC_RELAXED);

            if (LIKELY__((incur.sequence- slowest_reader.sequence) <= impl_->reduced_size.count))
                return;
            nanosleep(&timeout__.content, NULL);
        } while (1);
    }

    bool publisher_next_entry_nonblocking(cursor_t& cursor)
    {
        cursor_t seq;
        cursor_t slowest_reader;
        const cursor_t incur = {1 + __atomic_load_n(&impl_->write_cursor.sequence, __ATOMIC_RELAXED),
            {0}};

        cursor.sequence = incur.sequence;
        slowest_reader.sequence = VACANT__;
        for (unsigned int n = 0; n < processor_capacity; ++n)
        {
            seq.sequence = __atomic_load_n(&impl_->entry_processor_cursors[n].sequence, __ATOMIC_RELAXED);
            if (seq.sequence < slowest_reader.sequence)
                slowest_reader.sequence = seq.sequence;
        }

        if (UNLIKELY__(VACANT__ == slowest_reader.sequence))
            slowest_reader.sequence = incur.sequence - (impl_->reduced_size.count & incur.sequence);
        __atomic_store_n(&impl_->slowest_entry_processor.sequence, slowest_reader.sequence, __ATOMIC_RELAXED);

        if (LIKELY__((incur.sequence - slowest_reader.sequence) <= impl_->reduced_size.count))
        {
            seq.sequence = incur.sequence - 1;
            if (__atomic_compare_exchange_n(&impl_->write_cursor.sequence, &seq.sequence, incur.sequence, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return true;
        }
        return false;
    }

    void publisher_commit_entry_blocking (cursor_t& cursor)
    {
        const uint_fast64_t required_read_sequence = cursor.sequence - 1;

        while (__atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_RELAXED) != required_read_sequence)
            nanosleep(&timeout__.content, NULL);

        __atomic_fetch_add(&impl_->max_read_cursor.sequence, 1, __ATOMIC_RELEASE);
    }

    int publisher_commit_entry_nonblocking (cursor_t& cursor)
    {
        const uint_fast64_t required_read_sequence = cursor->seq - 1;

        if (__atomic_load_n(&impl_->max_read_cursor.sequence, __ATOMIC_RELAXED) != required_read_sequence)
            return 0;

        __atomic_fetch_add(&impl_->max_read_cursor.sequence, 1, __ATOMIC_RELEASE);
        return 1;
    }

private:
    struct _impl_t {
        count_t reduced_size;
        cursor_t slowest_entry_processor;
        cursor_t max_read_cursor;
        cursor_t write_cursor;
        cursor_t entry_processor_cursors[processor_capacity];
        entry_t buffer[0];
    };
    struct _impl_t *impl_;
    int buf_size_;
    yield_t timeout__;
};

