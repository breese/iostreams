#include "async_ostream.hpp"
#include "asio_service_wrappers.hpp"

#include <boost/smart_ptr/detail/spinlock.hpp>
#include <boost/foreach.hpp>
#include <iostream>
#include <fstream>
#include <vector>

#include <boost/static_assert.hpp>
BOOST_STATIC_ASSERT(sizeof(std::size_t) == sizeof(void*)); // allocator implementation depends on this assumption

namespace async_ostream {

	typedef boost::detail::spinlock spinlock;

	struct service::impl {
		impl(error_policy* ep) : ep_(ep) { sync_.v_ = 0; }

		bool run();

		void add(stream* s) {
			spinlock::scoped_lock guard(sync_);
			streams_.push_back(s);
		}

		void remove(stream* s) {
			spinlock::scoped_lock guard(sync_);
			for (std::vector<stream*>::iterator i = streams_.begin(); i != streams_.end(); ++i) {
				if (*i == s) {
					streams_.erase(i);
					return;
				}
			}
		}

		error_policy* ep() const { return ep_; }

	private:
		typedef std::vector<stream*> streams;
		error_policy* ep_;
		streams streams_;
		spinlock sync_;
	};

	service::service(bool suppress_work_available_call, error_policy* ep) :
		suppress_work_available_call_(suppress_work_available_call)
	{
		impl_ = new impl(ep);
	}

	service::~service() { 
		delete impl_; 
	}

	bool service::run() { 
		return impl_->run(); 
	}

	struct stream::impl {
		service* service_;
		bool owner_;
		// where do we write into and read from?
		std::ostream* ostream_;
		std::istream* istream_;
		// what do we write?
		allocator page1_, page2_;
		stream::buffer buffer_;
		bool try_drain();
		void flush();
		// ctor
		impl(service* s, error_policy* e, std::ostream* o, std::istream* i, stream* parent) :
			service_(s), owner_(false), ostream_(o), istream_(i), page1_(e), page2_(e)
		{
			buffer_.init(&page1_, &page2_);
			s->impl_->add(parent);
		}
	  	impl(boost::asio::io_service& s, error_policy* e, std::ostream* o, std::istream* i, sched_policy p, stream* parent) :
			owner_(true), ostream_(o), istream_(NULL), page1_(e), page2_(e)
		{
			buffer_.init(&page1_, &page2_);
			// choose the service type to run (depending on the sched_policy given)
			switch (p.type_) {
			case sched_policy::low_power:
				service_ = new asio_service_wrapper_low_power(s, e);
				break;
			case sched_policy::low_enqueue_latency_by_polling:
				service_ = new asio_service_wrapper_low_enqueue_latency(s, e, p.polling_period_);
				break;
			case sched_policy::low_overall_latency_by_spinning:
				service_ = new asio_service_wrapper_low_overall_latency(s, e);
				break;
			default:
				assert(false && "unknown sched_policy type");
				break;
			}
			service_->impl_->add(parent);
		}
		~impl() {
			if (owner_) {
				delete service_;
				service_ = NULL;
			}
		}
	};

	bool stream::try_drain() { 
		return impl_->try_drain(); 
	}

	void stream::flush() {
		impl_->flush();
	}

	struct page_break : public formatter_or_parser {
		void apply(std::ostream*, std::istream*) { 
			/* do exactly nothing */ 
		}
	};

	static const size_t page_break_size_in_std_size_ts = (sizeof(page_break) + sizeof(std::size_t) - 1) / sizeof(std::size_t);

	struct allocator::impl {

		// releases whatever was to be written into an ostream, 
		// reads whatever was supposed to be read from an istream,
		// and then disposes both of what was to be written and of istream consumers
		void drain(std::ostream* o, std::istream* i) const;

		// life cycle
		impl(error_policy* ep) : ep_(ep) { 
			first_ = last_ = new page(4096 - sizeof(std::size_t) * page_break_size_in_std_size_ts);
			clear();
		}
		~impl() { 
			clear(); 
			delete first_; 
		}
		void clear() {
			clear_contents();
			clear_extra_pages();
			last_allocated_ = 0;
		}
		void clear_contents() {
			std::size_t* cursor = first_->storage_begin_;
			while (*cursor) {
				formatter_or_parser* write_me = (formatter_or_parser*)(&cursor[1]);
				write_me->~formatter_or_parser();
				cursor = (std::size_t*)(*cursor);
			}
			(*first_->storage_begin_) = 0;
		}
		void clear_extra_pages() {
			while (first_ != last_) {
				page* next = first_->next_;
				delete first_;
				first_ = next;
			}
			prev_cursor_ = NULL;
			cursor_ = first_->storage_begin_;
			end_of_page_ = first_->storage_end_;
			(*cursor_) = 0;
		}
		// the actual allocation logic
		char* allocate(size_t size) {
			assert(0 != size);
			--size;
			std::size_t* next = &cursor_[1 + (size + sizeof(std::size_t) - 1) / sizeof(std::size_t)];
			if (next < end_of_page_) {
				// stay on this page
				last_allocated_ = (char*)cursor_;
				cursor_ = next;
				(*prev_cursor_) = (std::size_t)cursor_;
				(*cursor_) = 0;
				return last_allocated_;
			} else {
				// allocate another page twice the size of previous or the entry (whichever is greater)
				size_t newSize = 2 * ((char*)last_->storage_end_ - (char*)last_->storage_begin_);
				if (newSize < 2 * size)
					newSize = 2 * size;
				page* next_page = new page(newSize);
				last_->next_ = next_page;
				last_ = next_page;
				end_of_page_ = next_page->storage_end_;
				if (prev_cursor_ == cursor_) {
					/* if we were allocating space for a formattable value, place a "formattable" page break in between */
					new (&cursor_[1]) page_break();
					(*prev_cursor_) = (std::size_t)(next_page->storage_begin_);
					prev_cursor_ = next_page->storage_begin_;
				}
				cursor_ = next_page->storage_begin_;
				last_allocated_ = (char*)cursor_;
				cursor_ = &cursor_[1 + (size + sizeof(std::size_t) - 1) / sizeof(std::size_t)];
				(*prev_cursor_) = (std::size_t)cursor_;
				(*cursor_) = 0;
				return last_allocated_;
			}
		}
		char* add(size_t size) {
			prev_cursor_ = cursor_;
			std::size_t* allocated = (std::size_t*)allocate(size + sizeof(std::size_t));
			return (char*)(&allocated[1]);
		}
		struct page {
			~page() { 
				delete[] storage_begin_; 
			}
			page(size_t size) : 
				storage_begin_(new std::size_t[(size + sizeof(std::size_t) - 1) / sizeof(std::size_t) + page_break_size_in_std_size_ts]),
				storage_end_(storage_begin_ + size / sizeof(std::size_t)), 
				next_(NULL) 
			{ 
				(*storage_begin_) = 0; 
			}
			std::size_t* storage_begin_; // format here is size[8] + next_ptr[8] + payload[ceiling(size / 8)]
			std::size_t* storage_end_;
			page* next_;
		};
		std::size_t* cursor_;
		std::size_t* prev_cursor_;
		std::size_t* end_of_page_;
		page* first_;
		page* last_;
		error_policy* ep_;
		// a helper
		char* last_allocated_;
	};

	allocator::~allocator() { delete impl_; }
	allocator::allocator(error_policy* ep) : sequence_number_(0), impl_(new impl(ep)) {}
	char* const& allocator::last_allocated() const { return impl_->last_allocated_; }
	void* allocator::allocate(size_t size) { return impl_->allocate(size); }
	char* allocator::add(size_t size) { return impl_->add(size); }
	void allocator::clear() { impl_->clear(); }

	void allocator::impl::drain(std::ostream* o, std::istream* i) const {
		std::size_t went_through = 0;
		std::size_t* cursor = first_->storage_begin_;
		while (*cursor) {
			formatter_or_parser* worklet = (formatter_or_parser*)(&cursor[1]);

			try {
				assert((!dynamic_cast<formatter_base*>(worklet) || o) && 
				       "attempting to write, but ostream& wasn't given to stream constructor");
				assert((!dynamic_cast<parser_base*>(worklet) || i) && 
				       "attempting to read, but istream& wasn't given to stream constructor");
				worklet->apply(o, i);
			} catch (boost::exception& be) {
				if (ep_)
					ep_->catch_boost_exception(be);
			} catch (std::exception& se) {
				if (ep_)
					ep_->catch_std_exception(se);
			} catch (...) {
				if (ep_)
					ep_->catch_unknown_exception();
			}

			cursor = (std::size_t*)(*cursor);
			++went_through;
		}
	}

	bool stream::impl::try_drain() {
		bool result = false;
		while (true) {
			// anything available to be written or any parsers need to parse input?
			buffer::consume_transaction t;
			if (!buffer_.try_consume(t))
				break;
			// yes, so let's write and/or read
			t.buffer()->impl_->drain(ostream_, istream_);
			result = true;
		}
		return result;
	};

	void stream::impl::flush() {
		if (ostream_)
			ostream_->flush(); // but we don't need to flush an istream obviously
	}

	void stream::insert(buffer::insert_transaction& transaction) {
		impl_->buffer_.begin_insert(transaction);
	}

	void stream::warmup() {
		impl_->buffer_.warmup_before_inserting();
	}

	bool service::impl::run() {
		bool result = false;
		spinlock::scoped_lock guard(sync_);
		for (streams::iterator s = streams_.begin(), e = streams_.end(); s != e; ++s) {
			if ((*s)->try_drain())
				result = true;
		}
		if (result) {
			for (streams::iterator s = streams_.begin(), e = streams_.end(); s != e; ++s)
				(*s)->flush();
		}
		return result;
	}

	stream::stream(std::ostream& target, service& s) {
		impl_ = new impl(&s, s.impl_->ep(), &target, NULL, this);
	}

	stream::stream(std::istream& target, service& s) {
		impl_ = new impl(&s, s.impl_->ep(), NULL, &target, this);
	}

	stream::stream(std::iostream& target, service& s) {
		impl_ = new impl(&s, s.impl_->ep(), &target, &target, this);
	}

	stream::stream(std::ostream& target, boost::asio::io_service& s, sched_policy p, error_policy* ep) {
		impl_ = new impl(s, ep, &target, NULL, p, this);
	}

	stream::stream(std::istream& target, boost::asio::io_service& s, sched_policy p, error_policy* ep) {
		impl_ = new impl(s, ep, NULL, &target, p, this);
	}

	stream::stream(std::iostream& target, boost::asio::io_service& s, sched_policy p, error_policy* ep) {
		impl_ = new impl(s, ep, &target, &target, p, this);
	}

	stream::~stream() {
		// first, removes itself from the list
		impl_->service_->impl_->remove(this);
		// second, makes sure that it's fully drained
		while (!impl_->buffer_.empty()) {
			if (try_drain())
				flush();
		}
		// finally
		delete impl_;
	}

	void stream::signal_work_available() {
		impl_->service_->work_available();
	}

	// stream::put
	stream& stream::put(char c) {
		return (*this) << c;
	}

	// stream::seekp
	namespace {
		struct seekp_arg1 { 
			std::streampos pos_;
			seekp_arg1(std::streampos pos = 0) : pos_(pos) {}
		};
		struct seekp_arg2 { 
			std::streamoff off_;
			std::ios_base::seekdir way_;
			seekp_arg2(std::streamoff off = 0, std::ios_base::seekdir way = std::ios_base::seekdir(0)) : off_(off), way_(way) {}
		};
	}
	template <>
	struct formatter<seekp_arg1> : public formatter_base {
		formatter(const seekp_arg1& arg, allocator& alloc) : arg_(arg) {}
		void apply(std::ostream* o, std::istream* i) { o->seekp(arg_.pos_); }
		seekp_arg1 arg_;
	};
	template <>
	struct formatter<seekp_arg2> : public formatter_base {
		formatter(const seekp_arg2& arg, allocator& alloc) : arg_(arg) {}
		void apply(std::ostream* o, std::istream*) { o->seekp(arg_.off_, arg_.way_); }
		seekp_arg2 arg_;
	};
	stream& stream::seekp(std::streampos pos) {
		return (*this) << seekp_arg1(pos);
	}
	stream& stream::seekp(std::streamoff off, std::ios_base::seekdir dir) {
		return (*this) << seekp_arg2(off, dir);
	}

	// stream::imbue
	template <>
	struct formatter<std::locale> : public formatter_base {
		formatter(const std::locale& arg, allocator& alloc) : arg_(arg) {}
		void apply(std::ostream* o, std::istream*) { o->imbue(arg_); }
		std::locale arg_;
	};
	void stream::imbue(const std::locale& loc) {
		(*this) << loc;
	}

	// stream::clear
	namespace {
		struct clear_arg { 
			std::ios_base::iostate s_;
			clear_arg(std::ios_base::iostate s) : s_(s) {}
		};
	}
	template <>
	struct formatter<clear_arg> : public formatter_base {
		formatter(const clear_arg& arg, allocator& alloc) : arg_(arg) {}
		void apply(std::ostream* o, std::istream* i) { o->clear(arg_.s_); }
		clear_arg arg_;
	};
	void stream::clear(std::ios_base::iostate state) {
		(*this) << clear_arg(state);
	}

	// stream::setstate
	namespace {
		struct setstate_arg { 
			std::ios_base::iostate s_;
			setstate_arg(std::ios_base::iostate s) : s_(s) {}
		};
	}
	template <>
	struct formatter<setstate_arg> : public formatter_base {
		formatter(const setstate_arg& arg, allocator& alloc) : arg_(arg) {}
		void apply(std::ostream* o, std::istream* i) { i->setstate(arg_.s_); }
		setstate_arg arg_;
	};
	void stream::setstate(std::ios_base::iostate state) {
		(*this) << setstate_arg(state);
	}
}
