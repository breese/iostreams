// Copyright (C) 2014 Gene Panov
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.)

#include <boost/iostreams/async/asio_service_wrappers.hpp>

namespace boost { namespace iostreams { namespace async {

// if we are supposed to ensure no further work is scheduled
struct asio_service_wrapper::set_stopped {
    void operator() () { 
        assert(!target_->queues_empty_ && "this means that queues_empty_ variable is lying");
        target_->stopped_ = 1;
    }
    asio_service_wrapper* target_;
    set_stopped(asio_service_wrapper* t) : target_(t) {}
};
// if we are supposed to prove that no more scheduled work in the queue
struct asio_service_wrapper::set_queues_empty {
    void operator() () { 
        assert(!target_->queues_empty_ && "this means that queues_empty_ variable is lying");
        target_->queues_empty_ = 1;
    }
    asio_service_wrapper* target_;
    set_queues_empty(asio_service_wrapper* t) : target_(t) {}
};
asio_service_wrapper::~asio_service_wrapper() {
    // ensure that no new background work is being enqueued anynmore
    s_.post(set_stopped(this));
    while (!stopped_) {
        boost::detail::yield((unsigned)-1); // don't burn CPU cycles
        if (s_.stopped())
            break;
    }
    // wait until there is no more background work left in the queue
    s_.post(set_queues_empty(this));
    while (!queues_empty_) {
        boost::detail::yield((unsigned)-1); // don't burn CPU cycles
        if (s_.stopped())
            break;
    }
    // good! now we can proceed to destructing everything associated with this service wrapper
}
asio_service_wrapper::asio_service_wrapper(bool suppress_work_available_call, asio_service& s, error_policy* ep) : 
    service(suppress_work_available_call, ep),
    s_(s), keep_asio_service_running_(s), 
    stopped_(0), queues_empty_(0)
{}

struct asio_service_wrapper_low_enqueue_latency::drain_all_buffers_and_poll {
    void operator() (const boost::system::error_code&) { 
        assert(!target_->queues_empty_ && "this means that queues_empty_ variable is lying");
        if (target_->stopped_)
            return;
        // drain the buffers until there is no more data
        for (;;) {
            if (!target_->run())
                break;
        }
        // run again when time comes
        target_->poll_timer_->expires_at(target_->poll_timer_->expires_at() + target_->poll_interval_);
        target_->poll_timer_->async_wait(drain_all_buffers_and_poll(target_));
    }
    asio_service_wrapper_low_enqueue_latency* target_;
    drain_all_buffers_and_poll(asio_service_wrapper_low_enqueue_latency* t) : target_(t) {}
};

asio_service_wrapper_low_enqueue_latency::asio_service_wrapper_low_enqueue_latency(asio_service& s, error_policy* ep, boost::posix_time::time_duration poll_interval) : 
    asio_service_wrapper(true /* don't call work_avaiable() */, s, ep), poll_interval_(poll_interval)
{
    poll_timer_.reset(new boost::asio::deadline_timer(s, poll_interval));
    poll_timer_->async_wait(drain_all_buffers_and_poll(this));
}
asio_service_wrapper_low_enqueue_latency::~asio_service_wrapper_low_enqueue_latency() {
    // ensure that no new background work is being enqueued anynmore
    s_.post(set_stopped(this));
    while (!stopped_) {
        boost::detail::yield((unsigned)-1); // don't burn CPU cycles
        if (s_.stopped())
            break;
    }
    // good! now we can proceed to destructing everything associated with this service wrapper
    if (poll_timer_.get())
        poll_timer_->cancel();
}

// what's done on the background thread
struct asio_service_wrapper_low_overall_latency::drain_all_buffers_and_spin {
    void operator() () { 
        assert(!target_->queues_empty_ && "this means that queues_empty_ variable is lying");
        if (target_->stopped_)
            return;
        // drain the buffers until there is no more data
        for (;;) {
            if (!target_->run())
                break;
        }
        // schedule to be called again ASAP *if* we are supposed to spin
        target_->s_.post(drain_all_buffers_and_spin(target_));
    }
    asio_service_wrapper_low_overall_latency* target_;
    drain_all_buffers_and_spin(asio_service_wrapper_low_overall_latency* t) : target_(t) {}
};
// what's done on the foreground
asio_service_wrapper_low_overall_latency::asio_service_wrapper_low_overall_latency(asio_service& s, error_policy* ep) : 
    asio_service_wrapper(true /* don't call work_avaiable() */, s, ep)
{
    s_.post(drain_all_buffers_and_spin(this));
}


struct asio_service_wrapper_low_power::drain_all_buffers_once {
    void operator() () { 
        assert(!target_->queues_empty_ && "this means that queues_empty_ variable is lying");
        if (target_->stopped_)
            return;
        for (;;) {
            // 1. drain the buffers until there is no more data
            for (;;) {
                if (!target_->run())
                    break;
            }

            // 2. set the flag that we are not draining anymore
            target_->draining_.store(0, boost::memory_order_release);

            // 3. ensure that queues are indeed still empty after we indicated that we aren't draining anymore
            if (!target_->run())
                break;

            // 4. but if they weren't empty again, we have to redo the whole thing
            target_->draining_.store(1, boost::memory_order_release);
        }
    }
    asio_service_wrapper_low_power* target_;
    drain_all_buffers_once(asio_service_wrapper_low_power* t) : target_(t) {}
};

// what's done on the foreground thread
asio_service_wrapper_low_power::asio_service_wrapper_low_power(asio_service& s, error_policy* ep) : 
    asio_service_wrapper(false /* call work_avaiable() */, s, ep),
    draining_(0)
{}
void asio_service_wrapper_low_power::work_available_impl() {
    // 1. check that nobody is draining those queues already
    size_t draining = draining_.load(boost::memory_order_acquire);
    // 2. if nobody is draining the queues now, schedule that to happen
    if (!draining) {
        draining_.store(1, boost::memory_order_release);
        s_.post(drain_all_buffers_once(this));
    }
}
}}}
