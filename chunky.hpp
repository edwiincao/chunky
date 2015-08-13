/*
Copyright 2015 Shoestring Research, LLC.  All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef CHUNKY_HPP
#define CHUNKY_HPP

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/utility.hpp>

namespace chunky {
   namespace detail {
      struct CaselessCompare {
         bool operator()(const std::string& a,const std::string& b) const {
            return boost::ilexicographical_compare(a,b);
         }
      };
   }
   
   enum errors {
      invalid_request_line = 1,
      invalid_request_header,
      unsupported_http_version,
      invalid_content_length,
      invalid_chunk_length,
      invalid_chunk_delimiter
   };
   
   inline boost::system::error_code make_error_code(errors e) {
      class error_category : public boost::system::error_category
      {
      public:
         const char* name() const noexcept {
            return "chunky";
         }

         std::string message(int value) const {
            switch (value) {
            case invalid_request_line:
               return "Invalid request line";
            case invalid_request_header:
               return "Invalid request header";
            case unsupported_http_version:
               return "Unsupported HTTP version";
            case invalid_content_length:
               return "Invalid Content-Length";
            case invalid_chunk_length:
               return "Invalid chunk length";
            case invalid_chunk_delimiter:
               return "Invalid chunk delimiter";
            default:
               return "chunky error";
            }
         }
      };
      static error_category category;

      return boost::system::error_code(
         static_cast<int>(e), category);
   }

   // This is a wrapper for a boost::asio stream class (e.g.
   // boost::asio::ip::tcp::socket). It provides three features:
   //
   // 1. Asynchronous operations are thread-safe via a strand.
   // 2. A put back buffer is available for overread data.
   // 3. Stream lifetime is ensured (via shared_ptr) for asynchronous
   //    operations.
   template<typename T>
   class Stream : public std::enable_shared_from_this<Stream<T> >
                , boost::noncopyable {
   public:
      typedef T stream_t;
      
      virtual ~Stream() {}

      stream_t& stream() {
         return stream_;
      }
      
      boost::asio::io_service& get_io_service() {
         return stream_.get_io_service();
      }

      template<typename MutableBufferSequence, typename ReadHandler>
      void async_read_some(
         const MutableBufferSequence& buffers,
         ReadHandler&& handler) {
         if (!readBuffer_.empty()) {
            // Call the synchronous function. It won't block because
            // the data are buffered.
            boost::system::error_code error;
            const auto nBytes = read_some(buffers, error);
            get_io_service().post([=]() mutable {
                  handler(error, nBytes);
               });
         }
         else {
            auto this_ = this->shared_from_this();
            strand_.dispatch([=]() mutable {
                  // Wrapping the handler is unnecessary because the
                  // call is not a composed operation.
                  this_->stream_.async_read_some(buffers, handler);
               });
         }
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some(
         const ConstBufferSequence& buffers,
         WriteHandler&& handler) {
         auto this_ = this->shared_from_this();
         strand_.dispatch([=]() mutable {
               // Wrapping the handler is unnecessary because the call
               // is not a composed operation.
               this_->stream_.async_write_some(buffers, handler);
            });
      }

      template<typename MutableBufferSequence>
      size_t read_some(
         const MutableBufferSequence& buffers,
         boost::system::error_code& error) {
         if (!readBuffer_.empty()) {
            const auto nBytes = std::min(readBuffer_.size(), boost::asio::buffer_size(buffers));
            const auto iBegin = readBuffer_.begin();
            const auto iEnd = iBegin + nBytes;
            std::copy(iBegin, iEnd, boost::asio::buffers_begin(buffers));
            readBuffer_.erase(iBegin, iEnd);
            return nBytes;
         }
         else
            return stream_.read_some(buffers, error);
      }
      
      template<typename MutableBufferSequence>
      size_t read_some(
         const MutableBufferSequence& buffers) {
         boost::system::error_code error;
         const auto nBytes = read_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }

      template<typename ConstBufferSequence>
      size_t write_some(
         const ConstBufferSequence& buffers,
         boost::system::error_code& error) {
         return stream_.write_some(buffers, error);
      }

      template<typename ConstBufferSequence>
      size_t write_some(
         const ConstBufferSequence& buffers) {
         boost::system::error_code error;
         const auto nBytes = write_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }
      
      template<typename ConstBufferSequence>
      void put_back(const ConstBufferSequence& buffers) {
         readBuffer_.insert(
            readBuffer_.begin(),
            boost::asio::buffers_begin(buffers), boost::asio::buffers_end(buffers));
      }

   protected:
      template<typename A0>
      Stream(A0&& a0)
         : stream_(std::forward<A0>(a0))
         , strand_(stream_.get_io_service()) {
      }
      
   private:
      T stream_;
      boost::asio::io_service::strand strand_;
      std::deque<char> readBuffer_;
   };

   // This is a wrapped boost::asio TCP stream.
   class TCP : public Stream<boost::asio::ip::tcp::socket> {
   public:
      template<typename CreateHandler>
      static void async_connect(
         boost::asio::ip::tcp::acceptor& acceptor,
         CreateHandler handler) {
         std::shared_ptr<TCP> tcp(new TCP(acceptor.get_io_service()));
         acceptor.async_accept(
            tcp->stream(),
            [=](const boost::system::error_code& error) mutable {
               if (error)
                  tcp.reset();
               handler(error, tcp);
            });
      }

      static std::shared_ptr<TCP> create(boost::asio::ip::tcp::socket&& socket) {
         return std::shared_ptr<TCP>(new TCP(std::move(socket)));
      }

      ~TCP() {
         if (stream().is_open()) {
            boost::system::error_code error;
            stream().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
            stream().close(error);
         }
      }
   private:
      TCP(boost::asio::io_service& io)
         : Stream<boost::asio::ip::tcp::socket>(io) {
      }

      TCP(boost::asio::ip::tcp::socket&& socket)
         : Stream<boost::asio::ip::tcp::socket>(std::move(socket)) {
      }
   };

   template<typename T>
   class HTTPTemplate : boost::noncopyable {
   public:
      typedef std::map<std::string, std::string, detail::CaselessCompare> Headers;
      typedef std::map<std::string, std::string> Query;
      
      typedef boost::system::error_code error_code;
      typedef std::function<void(const error_code&)> Handler;
      typedef std::function<void(const error_code&, const std::shared_ptr<HTTPTemplate>&)> CreateHandler;
      
      HTTPTemplate(const std::shared_ptr<T>& stream)
         : stream_(stream)
         , requestBytes_(0)
         , requestChunksPending_(false)
         , responseStatus_(0)
         , responseBytes_(0)
         , responseChunked_(false) {
      }

      const std::string& request_method() const { return requestMethod_; }
      const std::string& request_version() const { return requestVersion_; }
      const std::string& request_resource() const { return requestResource_; }
      const Headers& request_headers() const { return requestHeaders_; }

      const std::string& request_path() const { return requestPath_; }
      const std::string& request_fragment() const { return requestFragment_; }
      const Query& request_query() const { return requestQuery_; }
      
      unsigned int& response_status() { return responseStatus_; }
      Headers& response_headers() { return responseHeaders_; }
      Headers& response_trailers() { return responseTrailers_; }

      // Either async_finish() or finish() must be called on each
      // HTTPTemplate instance to ensure valid I/O on the stream. In
      // most cases exactly one call should be made with no further
      // usage of the instance (the exception is for returning 1xx
      // status).
      template<typename FinishHandler>
      void async_finish(FinishHandler&& handler) {
         // Use shared_ptr lifetime to execute the handler exactly
         // once when both the final reads and writes (which may
         // overlap in time) are complete.
         std::shared_ptr<error_code> result(new error_code, [=](error_code* pointer) {
               handler(*pointer);
               delete pointer;
            });

         assert(response_status() >= 100);
         if (response_status() >= 200) {
            async_discard([=](const error_code& error) mutable {
                  // Replace any unused bytes read by get_line().
                  putback_buffer();
                  *result = error;
               });
         }
         else
            putback_buffer();
         
         // Output final empty chunk.
         async_write_some(
            boost::asio::null_buffers(),
            [=](const error_code& error, size_t) {
               *result = error;
            });
      }
      
      // Either async_finish() or finish() must be called on each
      // HTTPTemplate instance to ensure valid I/O on the stream. In
      // most cases exactly one call should be made with no further
      // usage of the instance(the exception is for returning 1xx
      // status).
      void finish() {
         assert(response_status() >= 100);
         if (response_status() >= 200) {
            sync_discard([=](const error_code& error) {
                  if (error)
                     throw boost::system::system_error(error);
               
                  // Replace any unused bytes read by get_line().
                  putback_buffer();
               });
         }
         else
            putback_buffer();
         
         // Replace any unused bytes read by get_line().
         if (auto unused = streambuf_.size()) {
            stream()->put_back(streambuf_.data());
            streambuf_.consume(unused);
         }

         // Output final empty chunk.
         write_some(boost::asio::null_buffers());
      }
      
      // Either async_finish() or finish() must be called on each
      // HTTPTemplate instance to ensure valid I/O on the stream. In
      // most cases exactly one call should be made with no further
      // usage of the instance(the exception is for returning 1xx
      // status).
      void finish(error_code& error) {
         try {
            finish();
         }
         catch (const boost::system::system_error& e) {
            error = e.code();
         }
      }

      boost::asio::io_service& get_io_service() {
         return stream()->get_io_service();
      }
      
      template<typename MutableBufferSequence, typename ReadHandler>
      void async_read_some(MutableBufferSequence&& buffers, ReadHandler&& handler) {
         using namespace std::placeholders;
         auto loadBufferFunc = std::bind(&HTTPTemplate::async_load_buffer, this, _1, _2);
         if (requestMethod_.empty()) {
            create(loadBufferFunc, [=](const error_code& error) mutable {
                  if (error) {
                     handler(error, 0);
                     return;
                  }

                  async_read_some(buffers, handler);
               });
            return;
         }
         
         // Take data from the streambuf first.
         size_t nBytesRead = 0;
         const auto bufferSize = boost::asio::buffer_size(buffers);
         if (streambuf_.size()) {
            auto nBytes = boost::asio::buffer_copy(buffers, streambuf_.data(), requestBytes_);
            streambuf_.consume(nBytes);
            requestBytes_ -= nBytes;
            nBytesRead += nBytes;
         }

         boost::asio::async_read(
            *stream(), buffers, boost::asio::transfer_exactly(nBytesRead ? 0 : requestBytes_),
            [=](const error_code& error, size_t nBytes) mutable {
               if (error) {
                  handler(error, nBytesRead);
                  return;
               }

               // Read the chunk delimiter and next chunk header if chunked.
               requestBytes_ -= nBytes;
               nBytesRead += nBytes;
               if (bufferSize && requestChunksPending_ && !requestBytes_) {
                  loadBufferFunc(crlf(), [=](const error_code& error) mutable {
                        if (error) {
                           handler(error, nBytesRead);
                           return;
                        }
                        
                        std::string s = get_line();
                        if (!s.empty()) {
                           handler(make_error_code(invalid_chunk_delimiter), nBytesRead);
                           return;
                        }
                        
                        read_chunk_header(
                           loadBufferFunc,
                           [=](const error_code& error) mutable {
                              handler(error, nBytesRead);
                           });
                     });
               }
               else {
                  error_code error;
                  if (nBytesRead == 0 && bufferSize > 0)
                     error = boost::asio::error::make_error_code(boost::asio::error::eof);
                  handler(error, nBytesRead);
               }
            });
      }

      template<typename MutableBufferSequence>
      size_t read_some(MutableBufferSequence&& buffers, error_code& error) {
         using namespace std::placeholders;
         auto loadBufferFunc = std::bind(&HTTPTemplate::sync_load_buffer, this, _1, _2);
         if (requestMethod_.empty()) {
            create(loadBufferFunc, [&](const error_code& e) {
                  error = e;
               });
            if (error)
               return 0;
         }
         
         size_t result;
         auto handler = [&](const error_code& e, size_t n) {
            error = e;
            result = n;
         };
         
         // Take data from the streambuf first.
         size_t nBytesRead = 0;
         const auto bufferSize = boost::asio::buffer_size(buffers);
         if (streambuf_.size()) {
            auto nBytes = boost::asio::buffer_copy(buffers, streambuf_.data(), requestBytes_);
            streambuf_.consume(nBytes);
            requestBytes_ -= nBytes;
            nBytesRead += nBytes;
         }

         // Jump through some hoops to make the inner lambda exactly
         // the same as async_read_some().
         size_t nBytes = boost::asio::read(
            *stream(),
            buffers,
            boost::asio::transfer_exactly(nBytesRead ? 0 : requestBytes_),
            error);
         [=](const std::function<void(const error_code&, size_t)>& f) {
            f(error, nBytes);
         }([=](const error_code& error, size_t nBytes) mutable {
               if (error) {
                  handler(error, nBytesRead);
                  return;
               }

               // Read the chunk delimiter and next chunk header if chunked.
               requestBytes_ -= nBytes;
               nBytesRead += nBytes;
               if (bufferSize && requestChunksPending_ && !requestBytes_) {
                  using namespace std::placeholders;
                  loadBufferFunc(crlf(), [=](const error_code& error) mutable {
                        if (error) {
                           handler(error, nBytesRead);
                           return;
                        }
                        
                        std::string s = get_line();
                        if (!s.empty()) {
                           handler(make_error_code(invalid_chunk_delimiter), nBytesRead);
                           return;
                        }
                        
                        read_chunk_header(
                           loadBufferFunc,
                           [=](const error_code& error) mutable {
                              handler(error, nBytesRead);
                           });
                     });
               }
               else {
                  error_code error;
                  if (nBytesRead == 0 && bufferSize > 0)
                     error = boost::asio::error::make_error_code(boost::asio::error::eof);
                  handler(error, nBytesRead);
               }
            });

         return result;
      }

      template<typename MutableBufferSequence>
      size_t read_some(MutableBufferSequence&& buffers) {
         error_code error;
         size_t nBytes = read_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some(ConstBufferSequence&& buffers, WriteHandler&& handler) {
         // Add prefix (response line, response headers, and chunk
         // header) and suffix (chunk delimiter) around the client
         // buffers.
         auto nBytes = boost::asio::buffer_size(buffers);
         auto chunk = std::make_shared<std::vector<boost::asio::const_buffer> >();
         
         auto prefix = std::make_shared<std::string>(prepare_write_prefix(nBytes));
         if (!prefix->empty())
            chunk->push_back(boost::asio::const_buffer(prefix->data(), prefix->size()));

         for (const auto& buffer : buffers)
            chunk->push_back(boost::asio::const_buffer(buffer));
         
         auto suffix = std::make_shared<std::string>(prepare_write_suffix(nBytes));
         if (!suffix->empty())
            chunk->push_back(boost::asio::const_buffer(suffix->data(), suffix->size()));

         boost::asio::async_write(
            *stream(), *chunk,
            [=](const error_code& error, size_t) mutable {
               if (error) {
                  handler(error, 0);
                  return;
               }

               responseBytes_ += nBytes;
               handler(error, nBytes);

               // References for lifetime extension.
               prefix.get();
               suffix.get();
               chunk.get();
            });
      }
      
      template<typename ConstBufferSequence>
      size_t write_some(ConstBufferSequence&& buffers, error_code& error) {
         // Add prefix (response line, response headers, and chunk
         // header) and suffix (chunk delimiter) around the client
         // buffers.
         auto nBytes = boost::asio::buffer_size(buffers);
         auto chunk = std::make_shared<std::vector<boost::asio::const_buffer> >();
         
         auto prefix = std::make_shared<std::string>(prepare_write_prefix(nBytes));
         if (!prefix->empty())
            chunk->push_back(boost::asio::const_buffer(prefix->data(), prefix->size()));

         for (const auto& buffer : buffers)
            chunk->push_back(boost::asio::const_buffer(buffer));
         
         auto suffix = std::make_shared<std::string>(prepare_write_suffix(nBytes));
         if (!suffix->empty())
            chunk->push_back(boost::asio::const_buffer(suffix->data(), suffix->size()));

         boost::asio::write(*stream(), *chunk, error);
         responseBytes_ += nBytes;
         return nBytes;
      }
      
      template<typename ConstBufferSequence>
      size_t write_some(ConstBufferSequence&& buffers) {
         error_code error;
         size_t nBytes = write_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }

      std::shared_ptr<T>& stream() {
         return stream_;
      }

      // Convert '+' to ' ' and percent decoding.
      template<typename Iterator>
      static std::string decode(Iterator&& bgn, Iterator&& end) {
         std::string result;

         auto i = bgn;
         std::smatch escapeMatch;
         static std::regex escapeRegex("^([^+%]*)(?:\\+|(?:%([0-9A-Fa-f]{2})))");
         while (i != end) {
            // Look for a '+' or '%HH' where H is a valid hex digit.
            if (std::regex_search(i, end, escapeMatch, escapeRegex)) {
               // Concatenate everything up to the special character.
               result += escapeMatch[1].str();

               // Concatenate the unescaped character.
               if (escapeMatch[2].matched)
                  result += static_cast<char>(std::stoi(escapeMatch[2].str(), 0, 16));
               else
                  result += ' ';

               // Update the iterator.
               i = escapeMatch[0].second;
            }
            else {
               // No matches so concatenate the remainder.
               result += std::string(i, end);
               i = end;
            }
         }
         
         return result;
      }

      static Query parse_query(const std::string& s) {
         Query query;

         auto i = s.begin();
         std::smatch paramMatch;
         static std::regex paramRegex("^([^=&]+)(=)?([^&]*)&?");
         while (std::regex_search(i, s.end(), paramMatch, paramRegex)) {
            if (paramMatch[2].matched) {
               std::string key = decode(paramMatch[1].first, paramMatch[1].second);
               std::string value = decode(paramMatch[3].first, paramMatch[3].second);
               query[key] = value;
            }
            
            i = paramMatch[0].second;
         }

         return query;
      }
      
   private:
      enum { MaxDiscardBufferSize = 65536 };
      
      std::shared_ptr<T> stream_;
      boost::asio::streambuf streambuf_;
      
      std::string requestMethod_;
      std::string requestVersion_;
      std::string requestResource_;
      Headers requestHeaders_;

      std::string requestPath_;
      std::string requestFragment_;
      Query requestQuery_;
      
      size_t requestBytes_;
      bool requestChunksPending_;
      
      unsigned int responseStatus_;
      Headers responseHeaders_;
      Headers responseTrailers_;

      size_t responseBytes_;
      bool responseChunked_;

      static const std::string& crlf() {
         static const std::string s("\r\n");
         return s;
      }

      static const std::string& crlf2() {
         static const std::string s("\r\n\r\n");
         return s;
      }

      // Asynchronously guarantee that the body buffer contains the
      // delimiter. This allows subsequent synchronous read_until()
      // calls to succeed without blocking.
      void async_load_buffer(const std::string& delimiter, const Handler& handler) {
         boost::asio::async_read_until(
            *stream(), streambuf_, delimiter,
            [=](const error_code& error, size_t) {
               handler(error);
            });
      }

      // Synchronously guarantee that the body buffer contains the
      // delimiter.
      void sync_load_buffer(const std::string& delimiter, const Handler& handler) {
         error_code error;
         boost::asio::read_until(*stream(), streambuf_, delimiter, error);
         handler(error);
      }

      void putback_buffer() {
         if (auto unused = streambuf_.size()) {
            stream()->put_back(streambuf_.data());
            streambuf_.consume(unused);
         }
      }
      
      // Asynchronously discard any unread body.
      void async_discard(const Handler& handler) {
         if (requestBytes_) {
            auto bufferSize = std::min(requestBytes_, static_cast<size_t>(MaxDiscardBufferSize));
            auto buffer = std::make_shared<std::vector<char> >(bufferSize);
            boost::asio::async_read(
               *this, boost::asio::buffer(*buffer),
               boost::asio::transfer_exactly(requestBytes_),
               [=](const error_code& error, size_t) {
                  if (error) {
                     handler(error);
                     return;
                  }

                  async_discard(handler);
                  buffer.get();
               });
         }
         else
            handler(error_code());
      }

      // Synchronously discard any unread body.
      void sync_discard(const Handler& handler) {
         while (requestBytes_) {
            error_code error;
            auto bufferSize = std::min(requestBytes_, static_cast<size_t>(MaxDiscardBufferSize));
            auto buffer = std::make_shared<std::vector<char> >(bufferSize);
            boost::asio::read(
               *this, boost::asio::buffer(*buffer),
               boost::asio::transfer_exactly(requestBytes_),
               error);
            if (error) {
               handler(error);
               return;
            }

            buffer.get();
         }

         handler(error_code());
      }

      // Get the next line (through CRLF) from the underlying stream.
      // The read is via the streambuf so it should not block if
      // the buffer has previously been loaded.
      std::string get_line() {
         auto nBytes = boost::asio::read_until(*stream(), streambuf_, crlf());
         auto i = boost::asio::buffers_begin(streambuf_.data());
         std::string s(i, i + nBytes - crlf().size());
         streambuf_.consume(nBytes);
         return s;
      }

      // Common synchronous/asynchronous create() helper.
      typedef std::function<void(const std::string&, const Handler&)> LoadBufferFunc;
      void create(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         loadBufferFunc(crlf2(), [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }
               
               if (error_code error = read_request_line()) {
                  handler(error);
                  return;
               }
                  
               if (error_code error = read_request_headers()) {
                  handler(error);
                  return;
               }
               
               read_length(loadBufferFunc, [=](const error_code& error) {
                     handler(error);
                  });
            });
      }

      error_code read_request_line() {
         std::smatch requestMatch;
         static const std::regex requestRegex("([-!#$%^&*+._'`|~0-9A-Za-z]+) (\\S+) (HTTP/\\d\\.\\d)");
         if (!std::regex_match(get_line(), requestMatch, requestRegex))
            return make_error_code(invalid_request_line);
         
         requestMethod_   = requestMatch[1];
         requestResource_ = requestMatch[2];
         requestVersion_  = requestMatch[3];

         if (requestVersion_ != "HTTP/1.1")
            return make_error_code(unsupported_http_version);

         std::smatch resourceMatch;
         static const std::regex resourceRegex("(/[^?#]*)(?:\\?([^#]*))?(?:#(.*))?");
         if (std::regex_match(requestResource_, resourceMatch, resourceRegex)) {
            requestPath_ = decode(resourceMatch[1].first, resourceMatch[1].second);
            requestFragment_ = decode(resourceMatch[3].first, resourceMatch[3].second);
            requestQuery_ = parse_query(resourceMatch[2].str());
         }
         
         return error_code();
      }

      error_code read_request_headers() {
         for (auto s = get_line(); !s.empty(); s = get_line()) {
            const auto colon = s.find_first_of(':');
            if (colon == std::string::npos)
               return make_error_code(invalid_request_header);

            std::string key = s.substr(0, colon);
            std::string value = s.substr(colon + 1);
            boost::algorithm::trim_left(value);

            // Coalesce values with the same key.
            const auto i = requestHeaders_.find(key);
            if (i != requestHeaders_.end()) {
               i->second += ", ";
               i->second += value;
            }
            else
               requestHeaders_.insert({{std::move(key), std::move(value)}});
         }

         return error_code();
      }
      
      void read_length(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         auto contentLength = requestHeaders_.find("content-length");
         if (contentLength != requestHeaders_.end()) {
            boost::iostreams::filtering_istream is(
               boost::make_iterator_range(
                  contentLength->second.begin(),
                  contentLength->second.end()));
            is >> requestBytes_;
            if (!is) {
               handler(make_error_code(invalid_content_length));
               return;
            }
         }
         
         auto transferEncoding = requestHeaders_.find("transfer-encoding");
         if (transferEncoding != requestHeaders_.end() &&
             transferEncoding->second != "identity") {
            requestBytes_ = 0U;
            requestChunksPending_ = true;
            read_chunk_header(loadBufferFunc, handler);
         }
         else
            handler(error_code());
      }

      void read_chunk_header(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         assert(requestBytes_ == 0);
         assert(requestChunksPending_);
         loadBufferFunc(crlf(), [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }
                  
               // Chunk header begins with a hexadecimal chunk length.
               std::istringstream is(get_line());
               is >> std::hex >> requestBytes_;
               if (!is) {
                  handler(make_error_code(invalid_chunk_length));
                  return;
               }
               
               if (!requestBytes_) {
                  requestChunksPending_ = false;

                  // Read trailers after the terminating chunk. The
                  // trailers end with an empty line, i.e. consecutive
                  // CRLF, but when there are no trailers we have
                  // already consumed the first CRLF. We restore it
                  // before loading the buffer through the empty line.
                  streambuf_.sungetc();
                  streambuf_.sungetc();
                  loadBufferFunc(crlf2(), [=](const error_code& error) {
                        if (error) {
                           handler(error);
                           return;
                        }

                        // Skip the restored CRLF to position the
                        // stream at the start of trailers.
                        streambuf_.snextc();
                        streambuf_.snextc();
                        handler(read_request_headers());
                     });
               }
               else
                  handler(error);
            });
      }

      std::string prepare_write_prefix(size_t nBytes) {
         // The prefix includes the status line and headers if this is
         // the first write.
         std::ostringstream os;
         if (responseBytes_ == 0) {
            // Set Date header if not already present.
            if (response_headers().find("date") == response_headers().end()) {
               std::time_t t;
               std::time(&t);
               std::tm tm = *std::gmtime(&t);
               char s[30];
               auto n = strftime(s, sizeof(s), "%a, %d %b %Y %T GMT", &tm);
               response_headers()["Date"] = std::string(s, n);
            }

            // RFC 2616 section 4.4:
            //  Any response message which "MUST NOT" include a
            //  message-body (such as the 1xx, 204, and 304 responses
            //  and any response to a HEAD request) is always
            //  terminated by the first empty line after the header
            //  fields, regardless of the entity-header fields present
            //  in the message.
            auto status = response_status();
            static const std::string head = "HEAD";
            if (status >= 200 && status != 204 && status != 304 &&
                request_method() != head) {
               // Determine whether to use chunked transfer.
               auto transferEncoding = response_headers().find("transfer-encoding");
               if (transferEncoding != response_headers().end() &&
                   transferEncoding->second != "identity") {
                  responseChunked_ = true;
                  response_headers().erase("content-length");
               }
               else if (response_headers().count("content-length") == 0) {
                  responseChunked_ = true;
                  response_headers()["Transfer-Encoding"] = "chunked";
               }
            }

            write_status(os);
            write_headers(os, response_headers());
         }

         if (responseChunked_)
            os << boost::format("%x%s") % nBytes % crlf();
         
         return os.str();
      }

      std::string prepare_write_suffix(size_t nBytes) {
         std::ostringstream os;
         if (responseChunked_) {
            // Add crlf to all chunks except the final one.
            if (nBytes)
               os << crlf();
            else
               write_headers(os, response_trailers());
         }

         return os.str();
      }

      void write_status(std::ostream& os) {
         static std::map<unsigned int, std::string> reasons = {
            { 100, "Continue" },
            { 101, "Switching Protocols" },
            { 200, "OK" },
            { 201, "Created" },
            { 202, "Accepted" },
            { 203, "Non-Authoritative Information" },
            { 204, "No Content" },
            { 205, "Reset Content" },
            { 206, "Partial Content" },
            { 300, "Multiple Choices" },
            { 301, "Moved Permanently" },
            { 302, "Found" },
            { 303, "See Other" },
            { 304, "Not Modified" },
            { 305, "Use Proxy" },
            { 307, "Temporary Redirect" },
            { 400, "Bad Request" },
            { 401, "Unauthorized" },
            { 402, "Payment Required" },
            { 403, "Forbidden" },
            { 404, "Not Found" },
            { 405, "Method Not Allowed" },
            { 406, "Not Acceptable" },
            { 407, "Proxy Authentication Required" },
            { 408, "Request Timeout" },
            { 409, "Conflict" },
            { 410, "Gone" },
            { 411, "Length Required" },
            { 412, "Precondition Failed" },
            { 413, "Payload Too Large" },
            { 414, "URI Too Long" },
            { 415, "Unsupported Media Type" },
            { 416, "Range Not Satisfiable" },
            { 417, "Expectation Failed" },
            { 426, "Upgrade Required" },
            { 500, "Internal Server Error" },
            { 501, "Not Implemented" },
            { 502, "Bad Gateway" },
            { 503, "Service Unavailable" },
            { 504, "Gateway Timeout" },
            { 505, "HTTP Version Not Supported" }
         };

         auto reason = reasons.find(responseStatus_);
         os << boost::format("HTTP/1.1 %d %s%s")
            % responseStatus_
            % (reason != reasons.end() ? reason->second : std::string())
            % crlf();
      }

      void write_headers(std::ostream& os, const Headers& headers) {
         for (const auto& value : headers) {
            os << boost::format("%s: %s%s")
               % value.first
               % value.second
               % crlf();
         }
         os << crlf();
      }
   };
   
   typedef HTTPTemplate<TCP> HTTP;

   // This is a convenience class that may be sufficient for simple
   // embedded server use cases, or as a starting point to build a
   // more capable server.
   class SimpleHTTPServer : boost::noncopyable {
   public:
      typedef boost::system::error_code error_code;
      typedef std::function<void(const std::shared_ptr<HTTP>&)> Handler;
      typedef std::function<void(const std::string&)> LogCallback;
      
      SimpleHTTPServer(const Handler& defaultHandler = Handler())
         : running_(false)
         , strand_(io_) {
         using namespace std::placeholders;
         if (defaultHandler)
            handlers_[""] = defaultHandler;
         else
            handlers_[""] = std::bind(&SimpleHTTPServer::default_handler, this, _1);;
      }

      virtual ~SimpleHTTPServer() {
         stop();
      }

      // Add a local address/port to bind and listen.
      virtual unsigned short listen(const boost::asio::ip::tcp::acceptor::endpoint_type& endpoint) {
         acceptors_.emplace_back(io_, endpoint);
         return acceptors_.back().local_endpoint().port();
      }

      virtual void add_handler(const std::string& path, const Handler& handler) {
         if (handler)
            handlers_[path] = handler;
         else
            handlers_.erase(path);
      }
      
      // Run the server using worker threads.
      virtual void run(size_t nThreads = std::thread::hardware_concurrency()) {
         if (running_)
            throw std::runtime_error("server already running");
         running_ = true;

         for (auto& acceptor : acceptors_)
            strand_.dispatch([&]() { accept(acceptor); });
         
         for (size_t i = 0; i < nThreads; ++i) {
            threads_.emplace_back([this]() {
                  io_.run();
               });
         }
      }

      // Stop the server, blocking until in-progress connections end.
      virtual void stop() {
         running_ = false;
         for (auto& acceptor : acceptors_)
            strand_.dispatch([&]() { acceptor.cancel(); });
         for (auto& thread : threads_)
            thread.join();

         acceptors_.clear();
         threads_.clear();
      }

      virtual void set_log(const LogCallback& logCallback) {
         logCallback_ = logCallback;
      }
      
      virtual void log(const std::string& message) {
         if (logCallback_)
            logCallback_(message);
      }
      
   protected:
      std::atomic<bool> running_;
      boost::asio::io_service io_;
      boost::asio::io_service::strand strand_;
      std::vector<boost::asio::ip::tcp::acceptor> acceptors_;
      std::vector<std::thread> threads_;
      
      std::map<std::string, Handler> handlers_;
      LogCallback logCallback_;
      
      virtual void accept(boost::asio::ip::tcp::acceptor& acceptor) {
         assert(strand_.running_in_this_thread());
         TCP::async_connect(
            acceptor,
            [&](const error_code& error, std::shared_ptr<TCP>& tcp) {
               if (error) {
                  log(error.message());
                  return;
               }

               log((boost::format("connect %s:%d")
                    % tcp->stream().remote_endpoint().address().to_string()
                    % tcp->stream().remote_endpoint().port()).str());
               handle_connect(error, tcp);

               if (running_)
                  strand_.dispatch([&]() { accept(acceptor); });
            });
      }
      
      virtual void handle_connect(
         const boost::system::error_code& error,
         const std::shared_ptr<TCP>& tcp) {
         // Use the shared_ptr custom deleter to determine when to
         // instantiate the next request on the same connection.
         auto status = std::make_shared<error_code>();
         std::shared_ptr<HTTP> http(
            new HTTP(tcp),
            [=](HTTP* pointer) {
               if (!*status && running_) {
                  io_.post([=]() {
                        handle_connect(error, tcp);
                     });
               }

               delete pointer;
            });

         // For convenience, issue a null read so that request
         // metadata is already valid for the callback.
         http->async_read_some(
            boost::asio::null_buffers(),
            [=](const boost::system::error_code& error, size_t) {
               if (error) {
                  *status = error;
                  log(error.message());
                  return;
               }

               auto i = handlers_.find(http->request_path());
               if (i != handlers_.end())
                  i->second(http);
               else
                  handlers_.at(std::string())(http);
            });
      }

      void default_handler(const std::shared_ptr<HTTP>& http) {
         http->response_status() = 404;
         http->response_headers()["Content-Type"] = "text/html";
         
         static std::string NotFound("<title>404 - Not Found</title><h1>404 - Not Found</h1>");
         boost::asio::async_write(
            *http, boost::asio::buffer(NotFound),
            [=](const boost::system::error_code& error, size_t) {
               if (error) {
                  log(error.message());
                  return;
               }

               http->async_finish([=](const boost::system::error_code& error) {
                     if (error) {
                        log(error.message());
                        return;
                     }

                     http.get();
                  });
            });
      }
   };
}

#endif // CHUNKY_HPP
