#ifndef ASYNC_OSTREAM
#define ASYNC_OSTREAM

#include <cstring>
#include <iostream>
#include <boost/noncopyable.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/iostreams/async/detail/tpd.hpp>
// support of boost::exception
#include <boost/exception/all.hpp>

// what's needed for compatibility with boost::asio::async_write
namespace boost { namespace asio { class io_service; } }

namespace async_ostream {

	// three classes that form async_ostream
	class stream;
	class service;
	class error_policy;
	class allocator;

	// 1. stream
	class stream : boost::noncopyable {
	public:

		// formatted output
		template <typename formattable> 
		stream& operator<< (const formattable& value);

		// formatted input
		template <typename consumer>
		stream& operator>> (consumer& c);

		// completion notification (for whatever writing or/and reading was scheduled before this)
		template <typename callback> 
		void when_done(callback value);

		// formatted input (asio-style)
		template <typename reader, typename read_handler>
		void async_parse(reader& r, read_handler handler);
		// 1. reader won't be copied, read_handler will (so make sure to use boost::ref if you need)
		// 2. operator>>(istream&, reader) will be called (on the background thread, so it can afford to be slow),
		// 3. upon completion, read_handler(boost::system::error_code, size_t) will be called
		
		// compatibility with boost::asio::async_write
		template <typename asio_buffer, typename write_handler>
		void async_write_some(const asio_buffer& b, write_handler handler);

		// making it look like std::ostream as much as it's possible
		stream& put(char c);
		stream& write(const char* s, std::streamsize size);
		stream& seekp(std::streampos pos);
		stream& seekp(std::streamoff off, std::ios_base::seekdir dir);
		void clear(std::ios_base::iostate state = std::ios_base::goodbit);
		void setstate(std::ios_base::iostate state);
		void imbue(const std::locale& loc);

		// creating streams without boost::asio
		stream(std::ostream& target, service& s);
		stream(std::istream& target, service& s);
		stream(std::iostream& target, service& s);

		// creating streams using boost::asio
		struct sched_policy {
			enum policy_type { 
				low_enqueue_latency_by_polling /* makes sure that the call to stream::operator<< returns quickly (and doesn't waste much CPU cycles) */, 
				low_overall_latency_by_spinning /* makes sure that the call to stream::operator<< returns quickly and data is written into the target ostream quickly too (wastes CPU cycles) */, 
				low_power /* is not as optimized for latency, but for saving CPU cycles and battery */,
				default_policy = low_enqueue_latency_by_polling
			};
			typedef boost::posix_time::time_duration duration;
			sched_policy(policy_type t, duration polling_period = boost::posix_time::milliseconds(10)) : 
				type_(t), polling_period_(polling_period) 
			{}
			policy_type type_;
			duration polling_period_; 
		};
		stream(std::ostream& target, boost::asio::io_service& s, sched_policy p = sched_policy::default_policy, error_policy* ep = NULL);
		stream(std::istream& target, boost::asio::io_service& s, sched_policy p = sched_policy::default_policy, error_policy* ep = NULL);
		stream(std::iostream& target, boost::asio::io_service& s, sched_policy p = sched_policy::default_policy, error_policy* ep = NULL);

		// because there is a way to inherit from this class (in theory)
		virtual ~stream();

		// warming things up so a cold call to operator<< or write(...) takes less time
		void warmup();

		// you don't really have to call this because it will auto-flush asynchronously, 
		// but you can call it on your thread if you want to make sure it's flushed at a certain point
		bool try_drain();
		void flush();

		// support of stream manipulators such as endl, setprecision, etc.
		typedef std::ostream& (*stream_manipulator) (std::ostream&); 
		void insert(stream_manipulator o);

	protected:
		typedef tpd<allocator> buffer;
		void insert(buffer::insert_transaction& transaction);
		static char* add(allocator& a, size_t size);
		void signal_work_available();

	private:
		struct impl; impl* impl_;
	};

	// 2. service
	class service : boost::noncopyable {
	protected:
		service(bool suppress_work_available_call, error_policy* ep);
		virtual ~service();
		// dervied classes should do two things:
		//  1. call this method from any thread that can afford to be slow
		bool run();
		//  2. call this when work is available
		void work_available() { if (!suppress_work_available_call_) work_available_impl(); }
	protected:
		// define this method -- it will be called when work is available and service::run must be called
		virtual void work_available_impl() = 0;
	private:
		const bool suppress_work_available_call_;
		struct impl; impl* impl_;
		friend class stream;
	};

	// 3. error policy (run-time polymorphism because we expect these methods to be called infrequently and on background thread)
	class error_policy : boost::noncopyable {
	public:
		virtual void catch_boost_exception(boost::exception& e) throw() = 0;
		virtual void catch_std_exception(std::exception& e) throw() = 0;
		virtual void catch_unknown_exception() throw() = 0;
	};

	// 4. a special allocator optimized for our purposes here
	class allocator : boost::noncopyable {
	public:
		// what you have to use
		void* allocate(size_t size);
		template <typename T> T* allocate_typed(size_t size);
		char* const& last_allocated() const;
		// compatibility with double buffer
		void set_sequence_number(std::size_t s) { sequence_number_ = s; }
		std::size_t get_sequence_number() const { return sequence_number_; }
		void clear();
		// ctor/dtor (in the CPP file)
		allocator(error_policy* ep);
		~allocator();
	private:
		std::size_t sequence_number_;
		struct impl; impl* impl_;
		friend class stream;
		char* add(size_t size);
	};

	// 4. what you can define for some types you want to write more efficiently 
	// (you can define a specialization of struct formatter<your_type> -- for examples see "5." below)
	struct formatter_or_parser {
		virtual void apply(std::ostream* o, std::istream* i) = 0;
	};
	struct formatter_base : public formatter_or_parser { /* empty derived class used for debug assertions */ };
	struct parser_base : public formatter_or_parser { /* empty derived class used for debug assertions */	};
	template <typename formattable>
	struct formatter : public formatter_base {
		formatter(const formattable& l, allocator& /*alloc*/) : l_(l) {}
		void apply(std::ostream* o, std::istream* i) { (*o) << l_; }
		formattable l_;
	};
	template <typename consumer>
	struct parser : public parser_base {
	        parser(consumer& c, allocator& /*alloc*/) : c_(c) {}
		void apply(std::ostream* o, std::istream* i) { (*i) >> c_; }
		consumer& c_;
	};

	// 5. examples of specializations that improve performance (because dynamic memory allocation is avoided)
	template <>
	struct formatter<const char*> : public formatter_base {
		formatter(const char* formattable, allocator& alloc)
		{
			size_t size = strlen(formattable) + 1;
			copy_ = (char*)alloc.allocate(size);
			memcpy(copy_, formattable, size);
		}
		void apply(std::ostream* o, std::istream* i) { 
			(*o) << copy_;
		}
		char* copy_;
	};
	template <>
	struct formatter<char*> : public formatter<const char*> {
		formatter(char* formattable, allocator& alloc) : formatter<const char*>(formattable, alloc) {}
	};
	template <>
	struct formatter<std::string> : public formatter<const char*> {
		formatter(const std::string& formattable, allocator& alloc) : formatter<const char*>(formattable.c_str(), alloc) {}
	};
	template <>
	struct formatter<const std::string> : public formatter<const char*> {
		formatter(const std::string& formattable, allocator& alloc) : formatter<const char*>(formattable.c_str(), alloc) {}
	};
	template <size_t size>
	struct formatter<char[size]> : public formatter<const char*> {
		formatter(char (&formattable)[size], allocator& alloc) : formatter<const char*>(&formattable[0], alloc) {}
	};
	template <size_t size>
	struct formatter<const char[size]> : public formatter<const char*> {
		formatter(const char (&formattable)[size], allocator& alloc) : formatter<const char*>(&formattable[0], alloc) {}
	};
}

#include <boost/iostreams/async/detail/stream.ipp>

#endif // ASYNC_OSTREAM
