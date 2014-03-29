// Copyright (C) 2014 Gene Panov
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.)

#ifndef BOOST_IOSTREAMS_ASYNC_DETAIL_TPD_HPP_INCLUDED
#define BOOST_IOSTREAMS_ASYNC_DETAIL_TPD_HPP_INCLUDED

#include <boost/smart_ptr/detail/spinlock.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/noncopyable.hpp>
#include <cassert>

namespace boost { namespace iostreams { namespace async {

// TPD = two-page disruptor (because it only has two buffer pages, while the original LMAX disruptor would usually have more than two buffer pages)

struct tpd_consume_result {
    // ctor
    enum outcome { 
        cr_no_more_work = 0, 
        cr_consumed = 1 << 0, 
        cr_queue_not_empty = 1 << 1, 
        cr_consumer_congestion = 1 << 2, 
        cr_success = cr_consumed | cr_queue_not_empty, 
        cr_too_many_consumers = cr_consumer_congestion | cr_queue_not_empty 
    };
    tpd_consume_result& operator|= (outcome rhs) {
        outcome_ = (outcome)(outcome_ | rhs);
        return *this;
    }
    tpd_consume_result& operator|= (tpd_consume_result rhs) {
        outcome_ = (outcome)(outcome_ | rhs.outcome_);
        return *this;
    }
    tpd_consume_result(outcome o) : outcome_(o) {}
    // properties
    bool too_many_consumers() const { return 0 != (outcome_ & cr_consumer_congestion); }
    bool queue_not_empty() const { return 0 != (outcome_ & cr_queue_not_empty); }
    bool consumed() const { return 0 != (outcome_ & cr_consumed); }
    operator bool() const { return consumed(); }
private:
    outcome outcome_;
};

struct tpd_insert_result {
    // tells you whether this page is freshly cleaned or it has something inserted into it before
    bool inserting_into_new_page() const { return new_buffer_page_; }
    operator bool() const { return inserting_into_new_page(); }
    // ctor
    tpd_insert_result(bool new_buffer_page) : new_buffer_page_(new_buffer_page) {}
private:
    bool new_buffer_page_;
};

template <typename buffer_type>
class tpd : boost::noncopyable {
public:

    // 1. inserting ("producing"): user must provide a freshly default-constructed transaction
    struct insert_transaction;
    typedef tpd_insert_result insert_result;
    insert_result begin_insert(insert_transaction& transaction);
    void warmup_before_inserting(); // on the inserting ("producer") thread you may choose to call this method in order to warm up the caches, so that begin_insert(...) takes less time afterwards

    // 2. consuming: if returns a consume_result that's "true", the queue was definitely not empty and the consume_transaction *may* contain something (depending on the details of consume_result returned)
    struct consume_transaction;
    typedef tpd_consume_result consume_result;
    consume_result try_consume(consume_transaction& transaction);

    // 3. is the queue empty? is it initialized?
    bool empty() const { assert(initialized()); return last_inserted_sequence_number_ == last_consumed_sequence_number_; }
    bool initialized() const { return inserter_ && consumer_; }

    // the only way to initialize: ctor + delayed init(...)
    tpd() : seq_no_generator_(1), inserter_(NULL), consumer_(NULL) {}
    // supply two memory locations where you have these buffer pages (you can place them in memory any way you like)
    void init(buffer_type* buffer1, buffer_type* buffer2);

    // transaction that the producer thread should use
    struct insert_transaction {
        void commit();
        buffer_type* buffer() { return buffer_; }
        buffer_type* producer() { return buffer(); }
        void init(buffer_type* buffer, tpd* parent) { assert(!buffer_ && !parent_); buffer_ = buffer; parent_ = parent; }
        insert_transaction() : buffer_(0), parent_(0) {}
        ~insert_transaction() { commit(); }
    private:
        buffer_type* buffer_;
        tpd* parent_;
    };

    // transaction that the consumer thread should use
    struct consume_transaction {
        void commit();
        const buffer_type* buffer() const { return buffer_; }
        const buffer_type* consumer() const { return buffer(); }
        void init(buffer_type* buffer, tpd* parent) { assert(!buffer_ && !parent_); buffer_ = buffer; parent_ = parent; }
        consume_transaction() : buffer_(0), parent_(0) {}
        ~consume_transaction() { commit(); }
    private:
        buffer_type* buffer_;
        tpd* parent_;
    };

    // will return the number of inserts that were performed that are not consumed yet (based on dirty reads though)
    std::size_t size() const { 
        std::size_t c = last_consumed_sequence_number_;
        std::size_t i = last_inserted_sequence_number_; 
        /* order of these loads above matters to the extent that we want the return value to be >= 0 */
        return (std::size_t)(i - c);
    }

private:
    typedef buffer_type* buffer_ptr;
    typedef boost::detail::spinlock spinlock;

    /* cache line #1: things that only inserter (producer) thread writes */
    spinlock inserting_;
    volatile std::size_t last_inserted_sequence_number_;
    std::size_t inserter_switched_pages_, seq_no_generator_; // not volatile, because only inserter reads it or writes it
    char padding1_[64 - 3 * sizeof(std::size_t) - sizeof(spinlock)];
		
    /* cache line #2: things that consumer thread writes */
    spinlock consuming_;
    volatile std::size_t last_consumed_sequence_number_;
    char padding2_[64 - sizeof(std::size_t) - sizeof(spinlock)];

    /* cache line #3: things that the least busy thread is more likely to write (i.e. whoever will end up switching pages) */
    volatile buffer_ptr inserter_;
    volatile buffer_ptr consumer_;
    volatile std::size_t last_enqueued_sequence_number_;
    char padding3_[64 - sizeof(std::size_t) - 2 * sizeof(buffer_ptr)];

    /* cache line #4: things that hopefully nobody will touch, but consumer can, very occasionally */
    volatile std::size_t consumer_couldnt_switch_; // >0 if inserter keeps inserting ~99.99% of the time
    char padding4_[64 - sizeof(std::size_t)];

    /* swapping buffer pages between producer and consumer */
    void switch_pages();
};

// similar to boost::detail::spinlock::scoped_lock, but it calls spinlock::try_lock() instead
class scoped_try_lock : boost::noncopyable {
public:
    typedef boost::detail::spinlock spinlock;
    scoped_try_lock(spinlock& target) : target_(target), locked_(target.try_lock()) {}
    ~scoped_try_lock() { if (locked_) target_.unlock(); }
    operator bool() const { return locked_; }
    bool locked() const { return locked_; }
private:
    spinlock& target_;
    bool locked_;
};

// inline implementations

template <typename buffer_type>
inline void tpd<buffer_type>::init(buffer_type* buffer1, buffer_type* buffer2) {
    assert(!initialized() && "queue initialized already");
    assert(buffer1 && buffer2 && "NULL buffer1 or buffer2 given");

    // spinlocks
    spinlock compliant_initializer = BOOST_DETAIL_SPINLOCK_INIT;
    inserting_ = compliant_initializer;
    consuming_ = compliant_initializer;

    // initializing other fields
    inserter_ = buffer1;
    consumer_ = buffer2;
    last_inserted_sequence_number_ = 1;
    last_enqueued_sequence_number_ = 1;
    last_consumed_sequence_number_ = 1;
    consumer_couldnt_switch_ = 0; 
    inserter_switched_pages_ = 0;

    // initializing things that don't depend on us or are just nice
    consumer_->set_sequence_number(0);
    inserter_->set_sequence_number(0);
}

template <typename buffer_type>
inline void tpd<buffer_type>::warmup_before_inserting() {
    assert(initialized());
    spinlock::scoped_lock guard(inserting_); // if we have more than one inserter at a time, they will all wait at this lock (which also gives us a full memory barrier that we need for what happens on the next line)
    std::size_t isn = inserter_->get_sequence_number();
    std::size_t esn = last_enqueued_sequence_number_;
    if (isn < esn) {
        inserter_->clear(); // *must* clear() on the producer thread to avoid CPU cache transfer-of-ownership mess and slowness
        inserter_->set_sequence_number(esn);
    }
}

template <typename buffer_type>
inline tpd_insert_result tpd<buffer_type>::begin_insert(insert_transaction& transaction) {
    assert(initialized());
    assert(0 == transaction.buffer() && "transaction given to begin_insert must be uninitialized or committed");
		
    inserting_.lock(); // if we have more than one inserter at a time, they will all wait at this lock (which also gives us a full memory barrier that we need for what happens on the next line)
    std::size_t sequence_number = ++seq_no_generator_;

    transaction.init(inserter_, this);
    insert_result result(false);
    {
        // copy two sequence numbers because they are volatile and take std::size_ter to load with all memory ordering rules
        std::size_t isn = inserter_->get_sequence_number();
        std::size_t esn = last_enqueued_sequence_number_;
        // compare and decide to clear the buffer possibly
        if (isn <= esn) {
            result = insert_result(true);
            if (isn < esn)
                inserter_->clear(); // *must* clear() on the producer thread to avoid CPU cache transfer-of-ownership mess and slowness
        }
    }
    inserter_->set_sequence_number(sequence_number);
    last_inserted_sequence_number_ = sequence_number;
    return result;

    /* insert_transaction::commit() or ~insert_transaction() is supposed release the inserting_ lock and to try to switch pages */
}

template <typename buffer_type>
inline void tpd<buffer_type>::insert_transaction::commit() {
    if (parent_) {
        // normally, inserter is not supposed to switch pages, 
        // but in case it was blocking the consumer from doing it, it has to take responsibility for switching
        if (parent_->inserter_switched_pages_ != parent_->consumer_couldnt_switch_)
        {
            scoped_try_lock consumer_not_consuming_now(parent_->consuming_);
            if (consumer_not_consuming_now.locked() && !(parent_->last_enqueued_sequence_number_ > parent_->last_consumed_sequence_number_)) {
                parent_->switch_pages();
                parent_->inserter_switched_pages_ = parent_->consumer_couldnt_switch_; // "caught up"
            }
        }
        parent_->inserting_.unlock(); // gives us a nice memory barrier here
    }
    parent_ = 0;
    buffer_ = 0;
}

template <typename buffer_type>
inline tpd_consume_result 
tpd<buffer_type>::try_consume(consume_transaction& transaction) 
{
    assert(initialized());
    assert(0 == transaction.buffer() && "transaction must be uninitialized or committed");

    // is it consumable? it is usually consumer's responsibility to switch, unless it can't (see below)
    if (!(last_enqueued_sequence_number_ > last_consumed_sequence_number_)) {
        // if not, try to enqueue if there is anything to enqueue
        scoped_try_lock inserter_not_inserting_now(inserting_);
        if (!inserter_not_inserting_now) {
            /* 1. inserter is inserting, so we cannot switch -- complain and inserter will then switch for us */
            ++consumer_couldnt_switch_;
            return consume_result::cr_queue_not_empty; // can't prove that queue is empty because somebody else is finishing insertion now: caller should try consuming again soon
        } else if (last_inserted_sequence_number_ > last_enqueued_sequence_number_) {
            /* 2. we're allowed to switch pages + something may be waiting for us on the next page, so switch */
            switch_pages();
        } else {
            /* 3. we would switch pages, but there is nothing for us on the next page anyway, so don't do it */
            return consume_result::cr_no_more_work;
        }

        // is it consumable now?
        if (!(last_enqueued_sequence_number_ > last_consumed_sequence_number_))
            return consume_result::cr_no_more_work;
    }

    // try to acquire a spinlock (and
    if (!consuming_.try_lock())
        return consume_result::cr_too_many_consumers; // queue not empty and caller *might* try consuming again soon, but it seems that there is no shortage of consumers

    transaction.init(consumer_, this);
    last_consumed_sequence_number_ = consumer_->get_sequence_number();
    return consume_result::cr_success; /* cr_success means: something was consumed, so transaction is initialized *and* it's proven that queue was not empty */

    /* consume_transaction::commit() or ~consume_transaction() is supposed to release the consuming_ lock and try to switch pages */
}

template <typename buffer_type>
inline void tpd<buffer_type>::consume_transaction::commit() {
    if (parent_) {
        {
            scoped_try_lock inserter_not_inserting_now(parent_->inserting_);
            if (inserter_not_inserting_now && (parent_->last_inserted_sequence_number_ > parent_->last_enqueued_sequence_number_))
                parent_->switch_pages();
        }
        parent_->consuming_.unlock();
    }
    parent_ = 0;
    buffer_ = 0;
}


template <typename buffer_type>
inline void tpd<buffer_type>::switch_pages() {

    // swap first
    buffer_ptr temp = inserter_;
    inserter_ = consumer_;
    consumer_ = temp;

    // indicate through sequence numbers next
    assert(consumer_->get_sequence_number() > last_enqueued_sequence_number_); 
    last_enqueued_sequence_number_ = consumer_->get_sequence_number(); 
}

}}}

#endif // BOOST_IOSTREAMS_ASYNC_DETAIL_TPD_HPP_INCLUDED
