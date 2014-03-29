// Copyright (C) 2014 Gene Panov
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.)

#ifndef BOOST_IOSTREAMS_ASYNC_DETAIL_STREAM_IPP_INCLUDED
#define BOOST_IOSTREAMS_ASYNC_DETAIL_STREAM_IPP_INCLUDED

// this file is only supposed to be included from async_ostream.hpp
#if defined(_MSC_VER)
#pragma once
#endif

#include <boost/system/error_code.hpp>
#include <boost/ref.hpp>

namespace boost { namespace asio { namespace detail {
	const char* buffer_cast_helper(void* dummy);
	size_t buffer_size_helper(void* dummy);
} } }
//#include <boost/asio/buffer.hpp>

namespace boost { namespace iostreams { namespace async {

inline char* stream::add(allocator& a, size_t size) { 
    return (char*)a.add(size); 
}

// const buffer (some similarity with asio)
struct unformatted_buffer {
    unformatted_buffer(const char* bytes, size_t size, allocator& alloc) {
        char* copy = (char*)alloc.allocate((size_t)size);
        memcpy(copy, bytes, (size_t)size);
        size_ = size;
        copy_ = copy;
    }
    size_t size_;
    char* copy_;
};

// write
struct unformatted : public formatter_base {
    unformatted(const char* bytes, size_t size, allocator& alloc) : 
        buffer_(bytes, size, alloc) 
    {}
    void apply(std::ostream* o, std::istream* i) { 
        o->write(buffer_.copy_, (std::streamsize)buffer_.size_);
    }
    unformatted_buffer buffer_;
};
inline stream& stream::write(const char* s, std::streamsize size) {
    // prepare to insert one unformatted instance into the queue
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(unformatted));
    // copy the data into an unformatted instance
    new (location) unformatted(s, (size_t)size, *transaction.buffer());
    // notify whoever schedules a background thread that work is available
    signal_work_available();
    return *this;
}

// async_write_some_work
template <typename write_handler>
struct async_write_some_work : public formatter_base {
    async_write_some_work(const char* bytes, size_t size, allocator& alloc, write_handler handler) : 
        buffer_(bytes, size, alloc), 
        handler_(handler)
    {}
    void apply(std::ostream* o, std::istream* i) { 
        // write data
        o->write(buffer_.copy_, (std::streamsize)buffer_.size_);
        // question: is there a better way to convert stream buffer error into boost::system::error_code
        boost::system::error_code ec((int)o->rdstate(), boost::system::generic_category());
        // notify of completion and possible errors
        handler_(ec, buffer_.size_);
    }
    write_handler handler_;
    unformatted_buffer buffer_;
};
template <typename asio_buffer, typename write_handler>
inline void stream::async_write_some(const asio_buffer& b, write_handler handler) {
    typedef async_write_some_work<write_handler> work_item;

    // 1. prepare to insert one async_write_some_work instance into the queue
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(work_item));

    // 2. copy the data from an asio buffer into async_write_some_work instance
    size_t size = boost::asio::detail::buffer_size_helper(b);
    const char* bytes = (const char*)boost::asio::detail::buffer_cast_helper(b);
    new (location) work_item(bytes, size, *transaction.buffer(), handler);

    // 3. notify whoever schedules a background thread that work is available
    signal_work_available();
}

// formatted output
template <typename Formattable>
inline stream& stream::operator<< (const Formattable& value) {
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(formatter<Formattable>));
    new (location) formatter<Formattable>(const_cast<Formattable&>(value), *transaction.buffer());
    signal_work_available();
    return *this;
}
inline void stream::insert(stream_manipulator f) {
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(formatter<stream_manipulator>));
    new (location) formatter<stream_manipulator>(f, *transaction.buffer());
    signal_work_available();
}
inline stream& operator<< (stream& s, std::ostream& (*f)(std::ostream&)) {
    s.insert((stream::stream_manipulator)f);
    return s;
}

// formatted input
template <typename consumer>
stream& stream::operator>> (consumer& c) {
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(parser<consumer>));
    new (location) parser<consumer>(c, *transaction.buffer());
    signal_work_available();
    return *this;
}

// completion notification
template <typename callback>
struct callback_wrapper : public formatter_or_parser {
    callback_wrapper(const callback& c) : c_(c) {}
    void apply(std::ostream*, std::istream*) { c_(); } 
    callback c_;
};
template <typename callback>
void stream::when_done(callback c) {
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(callback_wrapper<callback>));
    new (location) callback_wrapper<callback>(c);
    signal_work_available();
};

// asio-style formatted input
template <typename r, typename h>
struct reader_with_read_handler : public parser_base {
    // when it reads, it reads and then notifies the completion handler
    void parse(std::istream& i) { 
        // let the reader read from the stream until it's satisfied
        i >> r_;
        // call the handler to notify of completion
        boost::system::error_code ec((int)i.rdstate(), boost::system::generic_category());
        h_(ec, 0 /* can I actually know how many bytes were read total? */);
    }
    // it makes a copy of reader and handler
    reader_with_read_handler(r& reader, const h& handler, allocator& /* not used */) : 
        r_(reader), 
        h_(handler) 
    {}
private:
    r& r_;
    h h_;
};
template <typename r, typename h>
inline void stream::async_parse(r& reader, h handler) {
    buffer::insert_transaction transaction; insert(transaction);
    char* location = add(*transaction.buffer(), sizeof(reader_with_read_handler<r, h>));
    new (location) reader_with_read_handler<r, h>(reader, handler, *transaction.buffer());
    signal_work_available();		
}

template <typename T> 
inline T* allocator::allocate_typed(size_t size) { return (T*)allocate(sizeof(T) * size); }

}}}

#endif // BOOST_IOSTREAMS_ASYNC_DETAIL_STREAM_IPP_INCLUDED
