/*
 *  Copyright (C) 2012-2013 Jules Colding <jcolding@gmail.com>
 *
 *  All Rights Reserved.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  You can use, modify and redistribute it in any way you want.
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>

#include "disrupp.h"

#define STOP UINT_FAST64_MAX
#define ENTRIES_TO_GENERATE (50 * 1000 * 1000 * 5)
#define ENTRY_BUFFER_SIZE (1024*4) // must be a power of two
#define MAX_ENTRY_PROCESSORS (8)

typedef ring_buffer_t<uint_fast64_t, MAX_ENTRY_PROCESSORS> u64_ring_buffer_t;
u64_ring_buffer_t ring_buffer(ENTRY_BUFFER_SIZE);

struct timeval start;
struct timeval end;

static int create_thread(pthread_t * const thread_id, void *thread_arg,
        void *(*thread_func)(void *))
{
    int retv = 0;
    pthread_attr_t thread_attr;

    if (pthread_attr_init(&thread_attr))
        return 0;

    if (pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE))
        goto err;

    if (pthread_create(thread_id, &thread_attr, thread_func, thread_arg))
        goto err;

    retv = 1;
    err: pthread_attr_destroy(&thread_attr);

    return retv;
}

static void*
entry_processor_thread(void *arg)
{
    u64_ring_buffer_t::cursor_t n;
    u64_ring_buffer_t *buffer = (u64_ring_buffer_t*) arg;
    u64_ring_buffer_t::cursor_t cursor;
    u64_ring_buffer_t::cursor_t cursor_upper_limit;
    u64_ring_buffer_t::count_t reg_number;

    // register and setup entry processor
    cursor.sequence = buffer->processor_barrier_register(reg_number);
    cursor_upper_limit.sequence = cursor.sequence;
    int err_count = 0;
    int total_record = 0;
    do
    {
        buffer->processor_barrier_wait_blocking(cursor_upper_limit);
        for (n.sequence = cursor.sequence;
                n.sequence <= cursor_upper_limit.sequence; ++n.sequence)
        { // batching
            total_record ++;
            const u64_ring_buffer_t::entry_t& entry = buffer->show_entry(n);
            if (STOP == entry.content)
                goto out;
            if (n.sequence != entry.content)
                err_count++;
        }
        buffer->processor_barrier_release_entry(reg_number, cursor_upper_limit);

        ++cursor_upper_limit.sequence;
        cursor.sequence = cursor_upper_limit.sequence;
    } while (1);
out:
    gettimeofday(&end, NULL);

    buffer->processor_barrier_unregister(reg_number);
    printf("Entry processor done (entry/error: %d/%d)\n", total_record, err_count);

    return NULL;
}

int main(int argc, char *argv[])
{
    double start_time;
    double end_time;
    const int num_threads = MAX_ENTRY_PROCESSORS;
    pthread_t thread_id[num_threads]; // consumer/entry processor
    u64_ring_buffer_t::cursor_t cursor;
    u64_ring_buffer_t::entry_t *entry;
    uint_fast64_t reps;
    u64_ring_buffer_t *ring_buffer_heap;
    u64_ring_buffer_t ring_buffer_stack;

    ring_buffer_heap = new u64_ring_buffer_t(128);
    if (!ring_buffer_heap)
    {
        printf("Malloc ring buffer - ERROR\n");
        return EXIT_FAILURE;
    }

    ////////////////////////////////////////////////////////////////////////////////////////
    //                global variable with a non-blocking next_entry test
    ////////////////////////////////////////////////////////////////////////////////////////

    //ring_buffer_init(&ring_buffer);
    for(int i=0; i< num_threads; ++i)
        if (!create_thread(&thread_id[i], &ring_buffer, entry_processor_thread)) {
            printf("could not create entry processor thread\n");
            return EXIT_FAILURE;
        }

    reps = ENTRIES_TO_GENERATE;
    gettimeofday(&start, NULL);
    {
        do {
            again1: if (!ring_buffer.publisher_next_entry_nonblocking(cursor))
                goto again1;
            u64_ring_buffer_t::entry_t& entry =
                    ring_buffer.processor_acquire_entry(cursor);
            entry.content = cursor.sequence;
            ring_buffer.publisher_commit_entry_blocking(cursor);
        } while (--reps);

        ring_buffer.publisher_next_entry_blocking(cursor);
        u64_ring_buffer_t::entry_t& entry = ring_buffer.processor_acquire_entry(
                cursor);
        entry.content = STOP;
        ring_buffer.publisher_commit_entry_blocking(cursor);
    }

    // join entry processor
    for(int i=0; i<num_threads; ++i)
        pthread_join(thread_id[i], NULL);
    printf("Publisher done\n");

    start_time = (double) start.tv_sec + (double) start.tv_usec / 1000000.0;
    end_time = (double) end.tv_sec + (double) end.tv_usec / 1000000.0;
    printf("Elapsed time = %lf seconds\n", end_time - start_time);
    printf("Entries per second %lf\n",
            (double) ENTRIES_TO_GENERATE / (end_time - start_time));
    printf("As-Global-Variable non-blocking test done\n\n");

    /*
     ////////////////////////////////////////////////////////////////////////////////////////
     //                global variable with a blocking next_entry test
     ////////////////////////////////////////////////////////////////////////////////////////

     //ring_buffer_init(&ring_buffer);
     if (!create_thread(&thread_id, &ring_buffer, entry_processor_thread)) {
     printf("could not create entry processor thread\n");
     return EXIT_FAILURE;
     }

     reps = ENTRIES_TO_GENERATE;
     gettimeofday(&start, NULL);
     do {
     publisher_next_entry_blocking(&ring_buffer, &cursor);
     entry = ring_buffer_acquire_entry(&ring_buffer, &cursor);
     entry->content = cursor.sequence;
     publisher_commit_entry_blocking(&ring_buffer, &cursor);
     } while (--reps);

     publisher_next_entry_blocking(&ring_buffer, &cursor);
     entry = ring_buffer_acquire_entry(&ring_buffer, &cursor);
     entry->content = STOP;
     publisher_commit_entry_blocking(&ring_buffer, &cursor);

     // join entry processor
     pthread_join(thread_id, NULL);
     printf("Publisher done\n");

     start_time = (double)start.tv_sec + (double)start.tv_usec/1000000.0;
     end_time = (double)end.tv_sec + (double)end.tv_usec/1000000.0;
     printf("Elapsed time = %lf seconds\n", end_time - start_time);
     printf("Entries per second %lf\n", (double)ENTRIES_TO_GENERATE/(end_time - start_time));
     printf("As-Global-Variable blocking test done\n\n");


     ////////////////////////////////////////////////////////////////////////////////////////
     //               stack variable with a non-blocking next_entry test
     ////////////////////////////////////////////////////////////////////////////////////////

     ring_buffer_init(&ring_buffer_stack);
     if (!create_thread(&thread_id, &ring_buffer_stack, entry_processor_thread)) {
     printf("could not create entry processor thread\n");
     return EXIT_FAILURE;
     }

     reps = ENTRIES_TO_GENERATE;
     gettimeofday(&start, NULL);
     do {
     again2:
     if (!publisher_next_entry_nonblocking(&ring_buffer_stack, &cursor))
     goto again2;
     entry = ring_buffer_acquire_entry(&ring_buffer_stack, &cursor);
     entry->content = cursor.sequence;
     publisher_commit_entry_blocking(&ring_buffer_stack, &cursor);
     } while (--reps);

     publisher_next_entry_blocking(&ring_buffer_stack, &cursor);
     entry = ring_buffer_acquire_entry(&ring_buffer_stack, &cursor);
     entry->content = STOP;
     publisher_commit_entry_blocking(&ring_buffer_stack, &cursor);

     // join entry processor
     pthread_join(thread_id, NULL);
     printf("Publisher done\n");

     start_time = (double)start.tv_sec + (double)start.tv_usec/1000000.0;
     end_time = (double)end.tv_sec + (double)end.tv_usec/1000000.0;
     printf("Elapsed time = %lf seconds\n", end_time - start_time);
     printf("Entries per second %lf\n", (double)ENTRIES_TO_GENERATE/(end_time - start_time));
     printf("As-Stack-Variable non-blocking test done\n\n");


     ////////////////////////////////////////////////////////////////////////////////////////
     // stack variable with a blocking next_entry test
     ////////////////////////////////////////////////////////////////////////////////////////

     ring_buffer_init(&ring_buffer_stack);
     if (!create_thread(&thread_id, &ring_buffer_stack, entry_processor_thread)) {
     printf("could not create entry processor thread\n");
     return EXIT_FAILURE;
     }

     reps = ENTRIES_TO_GENERATE;
     gettimeofday(&start, NULL);
     do {
     publisher_next_entry_blocking(&ring_buffer_stack, &cursor);
     entry = ring_buffer_acquire_entry(&ring_buffer_stack, &cursor);
     entry->content = cursor.sequence;
     publisher_commit_entry_blocking(&ring_buffer_stack, &cursor);
     } while (--reps);

     publisher_next_entry_blocking(&ring_buffer_stack, &cursor);
     entry = ring_buffer_acquire_entry(&ring_buffer_stack, &cursor);
     entry->content = STOP;
     publisher_commit_entry_blocking(&ring_buffer_stack, &cursor);

     // join entry processor
     pthread_join(thread_id, NULL);
     printf("Publisher done\n");

     start_time = (double)start.tv_sec + (double)start.tv_usec/1000000.0;
     end_time = (double)end.tv_sec + (double)end.tv_usec/1000000.0;
     printf("Elapsed time = %lf seconds\n", end_time - start_time);
     printf("Entries per second %lf\n", (double)ENTRIES_TO_GENERATE/(end_time - start_time));
     printf("As-Stack-Variable blocking test done\n\n");


     ////////////////////////////////////////////////////////////////////////////////////////
     //            Now as allocated on the heap with a non-blocking next_entry
     ////////////////////////////////////////////////////////////////////////////////////////

     ring_buffer_init(ring_buffer_heap);
     if (!create_thread(&thread_id, ring_buffer_heap, entry_processor_thread)) {
     printf("could not create entry processor thread\n");
     return EXIT_FAILURE;
     }

     reps = ENTRIES_TO_GENERATE;
     gettimeofday(&start, NULL);
     do {
     again3:
     if (!publisher_next_entry_nonblocking(ring_buffer_heap, &cursor))
     goto again3;
     entry = ring_buffer_acquire_entry(ring_buffer_heap, &cursor);
     entry->content = cursor.sequence;
     publisher_commit_entry_blocking(ring_buffer_heap, &cursor);
     } while (--reps);

     publisher_next_entry_blocking(ring_buffer_heap, &cursor);
     entry = ring_buffer_acquire_entry(ring_buffer_heap, &cursor);
     entry->content = STOP;
     publisher_commit_entry_blocking(ring_buffer_heap, &cursor);

     // join entry processor
     pthread_join(thread_id, NULL);
     printf("Publisher done\n");

     start_time = (double)start.tv_sec + (double)start.tv_usec/1000000.0;
     end_time = (double)end.tv_sec + (double)end.tv_usec/1000000.0;
     printf("Elapsed time = %lf seconds\n", end_time - start_time);
     printf("Entries per second %lf\n", (double)ENTRIES_TO_GENERATE/(end_time - start_time));
     printf("On-The-Heap non-blocking test done\n\n");


     ////////////////////////////////////////////////////////////////////////////////////////
     //              Now as allocated on the heap with a blocking next_entry
     ////////////////////////////////////////////////////////////////////////////////////////

     ring_buffer_init(ring_buffer_heap);
     if (!create_thread(&thread_id, ring_buffer_heap, entry_processor_thread)) {
     printf("could not create entry processor thread\n");
     return EXIT_FAILURE;
     }

     reps = ENTRIES_TO_GENERATE;
     gettimeofday(&start, NULL);
     do {
     publisher_next_entry_blocking(ring_buffer_heap, &cursor);
     entry = ring_buffer_acquire_entry(ring_buffer_heap, &cursor);
     entry->content = cursor.sequence;
     publisher_commit_entry_blocking(ring_buffer_heap, &cursor);
     } while (--reps);

     publisher_next_entry_blocking(ring_buffer_heap, &cursor);
     entry = ring_buffer_acquire_entry(ring_buffer_heap, &cursor);
     entry->content = STOP;
     publisher_commit_entry_blocking(ring_buffer_heap, &cursor);

     // join entry processor
     pthread_join(thread_id, NULL);
     printf("Publisher done\n");

     start_time = (double)start.tv_sec + (double)start.tv_usec/1000000.0;
     end_time = (double)end.tv_sec + (double)end.tv_usec/1000000.0;
     printf("Elapsed time = %lf seconds\n", end_time - start_time);
     printf("Entries per second %lf\n", (double)ENTRIES_TO_GENERATE/(end_time - start_time));
     printf("On-The-Heap blocking test done\n");
     */
    return EXIT_SUCCESS;
}
