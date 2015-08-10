#ifndef CHUNKY_HPP
#define CHUNKY_HPP
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

#include <algorithm>
#include <deque>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
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
            strand_.post([=]() mutable {
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
   class HTTPTemplate : public std::enable_shared_from_this<HTTPTemplate<T> >
                      , boost::noncopyable {
   public:
      typedef std::map<std::string, std::string, detail::CaselessCompare> Headers;

      typedef boost::system::error_code error_code;
      typedef std::function<void(const error_code&)> Handler;
      typedef std::function<void(const error_code&, const std::shared_ptr<HTTPTemplate>&)> CreateHandler;
      
      static void async_create(const std::shared_ptr<T>& stream, const CreateHandler& handler) {
         using namespace std::placeholders;
         std::shared_ptr<HTTPTemplate> http(new HTTPTemplate(stream));
         http->create(
            std::bind(&HTTPTemplate::async_load_buffer, http, _1, _2),
            [=](const error_code& error) {
               handler(error, http);
            });
      }

      static std::shared_ptr<HTTPTemplate> create(const std::shared_ptr<T>& stream) {
         using namespace std::placeholders;
         std::shared_ptr<HTTPTemplate> http(new HTTPTemplate(stream));
         http->create(
            std::bind(&HTTPTemplate::sync_load_buffer, http, _1, _2),
            [=](const error_code& error) {
               if (error)
                  throw boost::system::system_error(error);
            });

         return http;
      }

      // The handler will be called as:
      //   handler(const boost::system::error_code&);
      template<typename FinishHandler>
      void async_finish(FinishHandler&& handler) {
         // Use shared_ptr lifetime to execute the handler exactly
         // once when both the final reads and writes (which may
         // overlap in time) are complete.
         std::shared_ptr<error_code> result(new error_code, [=](error_code* pointer) {
               handler(*pointer);
               delete pointer;
            });
         
         async_discard([=](const error_code& error) mutable {
               // Replace any unused bytes read by get_line().
               if (auto unused = streambuf_.size()) {
                  stream()->put_back(streambuf_.data());
                  streambuf_.consume(unused);
               }

               *result = error;
            });
         
         // Use Content-Length for empty body.
         if (!responseBytes_)
            response_headers()["Content-Length"] = "0";

         // Output final empty chunk.
         async_write_some(
            boost::asio::null_buffers(),
            [=](const error_code& error, size_t) {
               *result = error;
            });
      }
      
      void finish() {
         sync_discard([=](const error_code& error) {
               if (error)
                  throw boost::system::system_error(error);
               
               // Replace any unused bytes read by get_line().
               if (auto unused = streambuf_.size()) {
                  stream()->put_back(streambuf_.data());
                  streambuf_.consume(unused);
               }
            });
         
         // Replace any unused bytes read by get_line().
         if (auto unused = streambuf_.size()) {
            stream()->put_back(streambuf_.data());
            streambuf_.consume(unused);
         }

         // Use Content-Length for empty body.
         if (!responseBytes_)
            response_headers()["Content-Length"] = "0";
         
         // Output final empty chunk.
         write_some(boost::asio::null_buffers());
      }
      
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
         // Take data from the streambuf first.
         size_t nBytesRead = 0;
         const auto bufferSize = boost::asio::buffer_size(buffers);
         if (streambuf_.size()) {
            auto nBytes = boost::asio::buffer_copy(buffers, streambuf_.data(), requestBytes_);
            streambuf_.consume(nBytes);
            requestBytes_ -= nBytes;
            nBytesRead += nBytes;
         }

         using namespace std::placeholders;
         auto loadBufferFunc = std::bind(&HTTPTemplate::async_load_buffer, this, _1, _2);
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
         using namespace std::placeholders;
         auto loadBufferFunc = std::bind(&HTTPTemplate::sync_load_buffer, this, _1, _2);
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

         // for (const auto& b : *chunk)
         //    std::cout << boost::format("(%s)")
         //       % std::string(boost::asio::buffer_cast<const char*>(b), boost::asio::buffer_size(b));
         
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

      const std::string& request_method() const { return requestMethod_; }
      const std::string& request_version() const { return requestVersion_; }
      const std::string& request_resource() const { return requestResource_; }
      const Headers& request_headers() const { return requestHeaders_; }

      unsigned int& response_status() { return responseStatus_; }
      Headers& response_headers() { return responseHeaders_; }
      Headers& response_trailers() { return responseTrailers_; }
      
   private:
      std::shared_ptr<T> stream_;
      boost::asio::streambuf streambuf_;
      
      std::string requestMethod_;
      std::string requestVersion_;
      std::string requestResource_;
      Headers requestHeaders_;

      size_t requestBytes_;
      bool requestChunksPending_;
      
      unsigned int responseStatus_;
      Headers responseHeaders_;
      Headers responseTrailers_;

      size_t responseBytes_;
      bool responseChunked_;

      HTTPTemplate(const std::shared_ptr<T>& stream)
         : stream_(stream)
         , requestBytes_(0)
         , requestChunksPending_(false)
         , responseBytes_(0)
         , responseChunked_(false) {
      }

      static const std::string& crlf() {
         static const std::string s("\r\n");
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

      // Asynchronously discard any unread body.
      void async_discard(const Handler& handler) {
         if (requestBytes_) {
            boost::asio::async_read(
               *this, streambuf_,
               boost::asio::transfer_exactly(requestBytes_),
               [=](const error_code& error, size_t nBytes) {
                  if (error) {
                     handler(error);
                     return;
                  }

                  streambuf_.consume(nBytes);
                  async_discard(handler);
               });
         }
         else
            handler(error_code());
      }

      // Synchronously discard any unread body.
      void sync_discard(const Handler& handler) {
         while (requestBytes_) {
            error_code error;
            size_t nBytes = boost::asio::read(
               *this, streambuf_,
               boost::asio::transfer_exactly(requestBytes_),
               error);
            if (error) {
               handler(error);
               return;
            }

            streambuf_.consume(nBytes);
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
         read_request_line(loadBufferFunc, [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }

               read_request_headers(loadBufferFunc, [=](const error_code& error) {
                     if (error) {
                        handler(error);
                        return;
                     }

                     read_length(loadBufferFunc, [=](const error_code& error) {
                           handler(error);
                        });
                  });
            });
      }

      void read_request_line(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         loadBufferFunc(crlf(), [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }

               auto s = get_line();
               
               std::smatch requestMatch;
               static const std::regex requestRegex("([-!#$%^&*+._'`|~0-9A-Za-z]+) (\\S+) (HTTP/\\d\\.\\d)");
               if (std::regex_match(s, requestMatch, requestRegex)) {
                  requestMethod_   = requestMatch[1];
                  requestResource_ = requestMatch[2];
                  requestVersion_  = requestMatch[3];

                  if (requestVersion_ == "HTTP/1.1")
                     handler(error_code());
                  else
                     handler(make_error_code(unsupported_http_version));
               }
               else
                  handler(make_error_code(invalid_request_line));
            });
      }

      void read_request_headers(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         static const std::string crlf2 = "\r\n\r\n";
         loadBufferFunc(crlf2, [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }
               
               for (auto s = get_line(); !s.empty(); s = get_line()) {
                  const auto colon = s.find_first_of(':');
                  if (colon == std::string::npos) {
                     handler(make_error_code(invalid_request_header));
                     return;
                  }

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

               handler(error_code());
            });
      }
      
      void read_length(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         try {
            auto contentLength = requestHeaders_.find("content-length");
            if (contentLength != requestHeaders_.end())
               requestBytes_ = static_cast<size_t>(std::stoul(contentLength->second));
         }
         catch (...) {
            // std::stoul() threw on the Content-Length header.
            handler(make_error_code(invalid_content_length));
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
                  read_request_trailers(loadBufferFunc, handler);
               }
               else
                  handler(error);
            });
      }

      void read_request_trailers(const LoadBufferFunc& loadBufferFunc, const Handler& handler) {
         loadBufferFunc(crlf(), [=](const error_code& error) {
               if (error) {
                  handler(error);
                  return;
               }
               
               std::string s = get_line();
               if (!s.empty()) {
                  const auto colon = s.find_first_of(':');
                  if (colon == std::string::npos) {
                     handler(make_error_code(invalid_request_header));
                     return;
                  }

                  std::string key = "/" + s.substr(0, colon);
                  std::string value = s.substr(colon + 1);
                  boost::algorithm::trim_left(value);
                  requestHeaders_.insert({{std::move(key), std::move(value)}});

                  // Recursion could overflow the stack if loadBufferFunc
                  // is synchronous, but large numbers of trailers are
                  // not expected.
                  read_request_trailers(loadBufferFunc, handler);
               }
               else
                  handler(error);
            });
      }

      std::string prepare_write_prefix(size_t nBytes) {
         std::string s;
         // The prefix includes the status line and headers if this is
         // the first write.
         boost::iostreams::filtering_ostream os(boost::iostreams::back_inserter(s));
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
            
            // Determine whether to use chunked transfer.
            auto transferEncoding = response_headers().find("transfer-encoding");
            if (transferEncoding != response_headers().end() &&
                transferEncoding->second != "identity") {
               responseChunked_ = true;
               response_headers().erase("content-length");
            }
            else if (response_headers().count("content-length") == 0 && nBytes) {
               responseChunked_ = true;
               response_headers()["Transfer-Encoding"] = "chunked";
            }

            write_status(os);
            write_headers(os, response_headers());
         }

         if (responseChunked_)
            os << boost::format("%x\r\n") % nBytes;
         
         os.reset();
         return s;
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
         os << boost::format("HTTP/1.1 %d %s\r\n")
            % responseStatus_
            % (reason != reasons.end() ? reason->second : std::string());
      }

      void write_headers(std::ostream& os, const Headers& headers) {
         for (const auto& value : headers) {
            os << boost::format("%s: %s\r\n")
               % value.first
               % value.second;
         }
         os << crlf();
      }
   };
   
   typedef HTTPTemplate<TCP> HTTP;
}

#endif // CHUNKY_HPP
