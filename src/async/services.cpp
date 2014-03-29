// Copyright (C) 2014 Gene Panov
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.)

#include <boost/iostreams/async/services.hpp>
#include <boost/thread.hpp>

namespace boost { namespace iostreams { namespace async {

struct service_impl : boost::noncopyable 
{
    bool stop_requested_, actually_stopped_;
    boost::thread thread_;

    service_impl() : 
        stop_requested_(false),
        actually_stopped_(false)
    {}
    virtual ~service_impl() {}

    void start() { 
        thread_ = boost::thread(boost::ref(*this)); 
    }

    // what boost::thread will actually call
    virtual void operator() () = 0;
};


struct polling_service::impl : 
        public service_impl
{
    // order of fields matters because thread_ should be the last to initialize
    polling_service& parent_;
    boost::posix_time::time_duration period_;

    // instantiation
    impl(polling_service& parent, boost::posix_time::time_duration polling_period_ms) : 
        parent_(parent),
        period_(polling_period_ms)
    {}

    // what will boost::thread actually call
    void operator() () {
        while (!stop_requested_) {
            if (!parent_.run())
                boost::this_thread::sleep(period_);
        }
        parent_.run(); // just in case we recovered from sleep to find that stop was requested, however the buffers may not be empty
        actually_stopped_ = true;
    }
};

polling_service::polling_service(boost::posix_time::time_duration polling_period_ms, error_policy* e) : 
    service(true /* suppress the call to work_available */, e), impl_(new impl(*this, polling_period_ms)) 
{
    impl_->start();
}

polling_service::~polling_service() { 
    // stop and join to guarantee that the other thread is finished (will have no effect it somebody already stopped and joined)
    stop();
    join();
    // now actually delete impl_
    delete impl_; 
}

void polling_service::stop() { 
    impl_->stop_requested_ = true; 
}

bool polling_service::stopped() const { 
    return impl_->actually_stopped_; 
}

void polling_service::join() { 
    impl_->thread_.join(); 
    boost::this_thread::sleep(
                              boost::posix_time::milliseconds(10)); // just in case (and because on some platforms boost::thread::join does not guarantee that the other thread is out of post-execution cleanup)
}


struct waiting_service::impl : public service_impl
{
    // this is what minimizes power consumption
    boost::condition_variable cond_var_;

    // order of fields matters because thread_ should be the last to initialize
    waiting_service& parent_;
    boost::thread thread_;
		
    // instantiation
    impl(waiting_service& parent) : 
    parent_(parent)
    {}

    // what will boost::thread actually call
    void operator() () {
        boost::mutex dummy;
        boost::unique_lock<boost::mutex> lock(dummy);
        while (!stop_requested_) {
            if (!parent_.run())
                cond_var_.wait(lock); // most of the time will be spent here -- so saving the energy, and in case we get a spurious wake it's totally fine too
        }
        parent_.run(); // just in case we recovered from sleep to find that stop was requested, however the buffers may not be empty
        actually_stopped_ = true;
    }
};

waiting_service::waiting_service(error_policy* e) : 
    service(false/* do not suppress the call to work_available() */, e), impl_(new impl(*this)) 
{
    impl_->start();
}

waiting_service::~waiting_service() { 
    // stop and join to guarantee that the other thread is finished (will have no effect it somebody already stopped and joined)
    stop();
    join();
    // now actually delete impl_
    delete impl_; 
}

void waiting_service::work_available_impl() { 
    impl_->cond_var_.notify_all(); /* this can cost >10k CPU cycles on Intel if the writing thread is sleeping, which will probably be the case most of the time */ 
}

void waiting_service::stop() { 
    impl_->stop_requested_ = true; 
    impl_->cond_var_.notify_all(); // have to wake the other thread up
}

bool waiting_service::stopped() const { 
    return impl_->actually_stopped_; 
}

void waiting_service::join() { 
    impl_->thread_.join();
    boost::this_thread::sleep(
                              boost::posix_time::milliseconds(10)); // just in case (and because on some platforms boost::thread::join does not guarantee that the other thread is out of post-execution cleanup)
}

}}}
