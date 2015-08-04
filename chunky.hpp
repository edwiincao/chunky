#ifndef CHUNKY_HPP
#define CHUNKY_HPP

#include <algorithm>
#include <deque>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio.hpp>
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
      unsupported_http_version
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
            default:
               return "chunky error";
            }
         }
      };
      static error_category category;

      return boost::system::error_code(
         static_cast<int>(e), category);
   }
   
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
            const auto nBytes = std::min(readBuffer_.size(), boost::asio::buffer_size(buffers));
            const auto iBegin = readBuffer_.begin();
            const auto iEnd = iBegin + nBytes;
            std::copy(iBegin, iEnd, boost::asio::buffers_begin(buffers));
            readBuffer_.erase(iBegin, iEnd);

            strand_.post([=]() mutable {
                  handler(boost::system::error_code(), nBytes);
               });
         }
         else {
            auto this_ = this->shared_from_this();
            strand_.dispatch([=]() mutable {
                  this_->stream_.async_read_some(buffers, strand_.wrap(handler));
               });
         }
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some(
         const ConstBufferSequence& buffers,
         WriteHandler&& handler) {
         auto this_ = this->shared_from_this();
         strand_.dispatch([=]() mutable {
               this_->stream_.async_write_some(buffers, strand_.wrap(handler));
            });
      }

      template<typename ConstBufferSequence>
      void put_back(const ConstBufferSequence& buffers) {
         readBuffer_.insert(0, boost::asio::buffers_begin(buffers), boost::asio::buffers_end(buffers));
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

   class TCP : public Stream<boost::asio::ip::tcp::socket> {
   public:
      template<typename CreateHandler>
      static void async_create(
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

   private:
      TCP(boost::asio::io_service& io)
         : Stream<boost::asio::ip::tcp::socket>(io) {
      }
   };

   template<typename T>
   class HTTPTemplate : public std::enable_shared_from_this<HTTPTemplate<T> >
                      , boost::noncopyable {
   public:
      typedef std::map<std::string, std::string, detail::CaselessCompare> Headers;

      template<typename CreateHandler>
      static void async_create(
         const std::shared_ptr<T>& stream,
         CreateHandler handler) {
         std::shared_ptr<HTTPTemplate> http(new HTTPTemplate(stream));
         http->read_request_line([=](const boost::system::error_code& error) {
               if (error) {
                  handler(error, http);
                  return;
               }

               http->read_request_headers([=](const boost::system::error_code& error) {
                     if (error) {
                        handler(error, http);
                        return;
                     }

                     // Check for non-identity Transfer-Encoding.
                     auto i = http->requestHeaders_.find("transfer-encoding");
                     if (i != http->requestHeaders_.end() && i->second != "identity") {
                        // http->read_chunk_header([=](const boost::system::error_code& error) {
                        //       handler(error, http);
                        //    });
                     }
                     else
                        handler(error, http);
                  });
            });
      }

      std::shared_ptr<T>& stream() {
         return stream_;
      }

      const std::string& request_method() const { return requestMethod_; }
      const std::string& request_version() const { return requestVersion_; }
      const std::string& request_resource() const { return requestResource_; }
      const Headers& request_headers() const { return requestHeaders_; }
      
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

      size_t responseBytes_;
      bool responseChunked_;

      HTTPTemplate(const std::shared_ptr<T>& stream)
         : stream_(stream)
         , requestBytes_(0)
         , requestChunksPending_(false)
         , responseBytes_(0)
         , responseChunked_(false) {
      }
           
      template<typename Handler>
      void get_line(Handler&& handler) {
         static const std::string delimiter("\r\n");
         boost::asio::async_read_until(
            *stream_, streambuf_, delimiter,
            [this, handler](const boost::system::error_code& error, size_t nBytes) {
               if (!error) {
                  auto i = boost::asio::buffers_begin(streambuf_.data());
                  std::string s(i, i + nBytes - delimiter.size());
                  streambuf_.consume(nBytes);
                  handler(error, std::move(s));
               }
               else
                  handler(error, std::string());
            });
      }

      template<typename Handler>
      void read_request_line(const Handler& handler) {
         get_line([this, handler](boost::system::error_code error, std::string&& s) {
               std::smatch requestMatch;
               static const std::regex requestRegex("([-!#$%^&*+._'`|~0-9A-Za-z]+) (\\S+) (HTTP/\\d\\.\\d)");
               if (std::regex_match(s, requestMatch, requestRegex)) {
                  requestMethod_   = requestMatch[1];
                  requestResource_ = requestMatch[2];
                  requestVersion_  = requestMatch[3];

                  if (requestVersion_ != "HTTP/1.1")
                     error = make_error_code(unsupported_http_version);
               }
               else
                  error = make_error_code(invalid_request_line);

               handler(error);
            });
      }

      template<typename Handler>
      void read_request_headers(const Handler& handler) {
         get_line([this, handler](const boost::system::error_code& error, std::string&& s) {
               if (!s.empty()) {
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
                     
                  read_request_headers(handler);
               }
               else
                  handler(error);
            });
      }
   };

   typedef HTTPTemplate<TCP> HTTP;
}

#endif // CHUNKY_HPP
