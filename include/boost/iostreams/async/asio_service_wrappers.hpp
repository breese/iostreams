#ifndef BOOST_IOSTREAMS_ASYNC_ASIO_SERVICE_WRAPPERS_HPP_INCLUDED
#define BOOST_IOSTREAMS_ASYNC_ASIO_SERVICE_WRAPPERS_HPP_INCLUDED

#include <boost/iostreams/async/stream.hpp>

#include <boost/atomic.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/asio.hpp>

namespace async_ostream {

	// how we wrap a boost::asio::io_service into an async_ostream::service
	struct asio_service_wrapper : public service {
		typedef boost::asio::io_service asio_service;
	protected:
		asio_service_wrapper(bool suppress_work_available_call, asio_service& s, error_policy* ep);
		virtual ~asio_service_wrapper();
	protected:
		asio_service& s_;
		boost::asio::io_service::work keep_asio_service_running_; // tells asio service that we need it to be running
		// is it stopped?
		struct set_stopped;
		size_t stopped_;
		// can we guarantee that there is no more work queued before?
		struct set_queues_empty;
		size_t queues_empty_;
	};

	struct asio_service_wrapper_low_enqueue_latency : public asio_service_wrapper {
		// what's done on the background thread
		struct drain_all_buffers_and_poll;
		// what's done on the foreground
		asio_service_wrapper_low_enqueue_latency(asio_service& s, error_policy* ep, boost::posix_time::time_duration poll_interval);
		~asio_service_wrapper_low_enqueue_latency();
		void work_available_impl() {}
	private:
		// timer on which the buffer-draining code will be called
		boost::scoped_ptr<boost::asio::deadline_timer> poll_timer_;
		boost::posix_time::time_duration poll_interval_;
	};

	struct asio_service_wrapper_low_overall_latency : public asio_service_wrapper {
		// what's done on the background thread
		struct drain_all_buffers_and_spin;
		// what's done on the foreground
		asio_service_wrapper_low_overall_latency(asio_service& s, error_policy* ep);
		void work_available_impl() {}
	};

	struct asio_service_wrapper_low_power : public asio_service_wrapper {
		// what's done on the background thread
		struct drain_all_buffers_once;
		// what's done on the foreground
		asio_service_wrapper_low_power(asio_service& s, error_policy* ep);
		void work_available_impl();
	private:
		// on another cache line, place a flag that would say whether buffers are drained currently or not
		char padding_[64];
		boost::atomic<size_t> draining_;
	};
}

#endif // BOOST_IOSTREAMS_ASYNC_ASIO_SERVICE_WRAPPERS_HPP_INCLUDED
