#ifndef BOOST_IOSTREAMS_ASYNC_SERVICES_HPP_INCLUDED
#define BOOST_IOSTREAMS_ASYNC_SERVICES_HPP_INCLUDED

#include <boost/iostreams/async/stream.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>

namespace async_ostream {

	// this implementation gives you the lowest latency when you write into an async_ostream::stream
	class polling_service : public service {
	public:
		polling_service(boost::posix_time::time_duration polling_period = boost::posix_time::milliseconds(10), error_policy* e = NULL);
		virtual ~polling_service();

		// 1. stopping
		void stop();

		// 2. ensuring that it's stopped
		void join(); // blocking
		bool stopped() const; // non-blocking

	protected:
		void work_available_impl() { /* do nothing and save CPU cycles: the polling thread will eventually find out that there was work available */ }

	private:
		struct impl;
		impl* impl_;
	};


	// this implementation gives you the lowest power consumption while you wait for anything to be writte into an async_ostream::stream
	class waiting_service : public service {
	public:
		waiting_service(error_policy* e = NULL);
		virtual ~waiting_service();

		// 1. stopping
		void stop();

		// 2. ensuring that it's stopped
		void join(); // blocking
		bool stopped() const; // non-blocking

	protected:
		void work_available_impl();

	private:
		struct impl;
		impl* impl_;
	};

	// this implementation is mainly useful for debugging, 
	// because all the writing and reading will be done deterministically on one thread (inside ~stream destructor)
	class idle_service : public service {
	public:
		idle_service() : service(false, NULL) {}
	protected:
		void work_available_impl() {}
	};

}

#endif // BOOST_IOSTREAMS_ASYNC_SERVICES_HPP_INCLUDED
