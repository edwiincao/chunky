#define BOOST_LOG_DYN_LINK
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include "chunky.hpp"

template<typename T>
class WebSocket {
public:
   typedef boost::system::error_code error_code;
   
   enum MessageType {
      continuation = 0x0,
      text         = 0x1,
      binary       = 0x2,
      close        = 0x8,
      ping         = 0x9,
      pong         = 0xa,
      fin          = 0x80
   };
      
   WebSocket(const std::shared_ptr<T>& stream)
      : stream_(stream) {
   }

   std::shared_ptr<T> stream() { return stream_; }
   
   void read_frame() {
      auto header = std::make_shared<std::array<char, 14> >();
      boost::asio::async_read(
         *stream(), boost::asio::mutable_buffers_1(&(*header)[0], 2),
         [=](const boost::system::error_code& error, size_t) {
            if (error) {
               BOOST_LOG_TRIVIAL(error) << error.message();
               return;
            }

            size_t nLengthBytes = 0;
            size_t nPayloadBytes = (*header)[1] & 0x7f;
            switch (nPayloadBytes) {
            case 126:
               nLengthBytes = 2;
               nPayloadBytes = 0;
               break;
            case 127:
               nLengthBytes = 4;
               nPayloadBytes = 0;
               break;
            }

            const size_t nMaskBytes = ((*header)[1] & 0x80) ? 4 : 0;
            char* mask = &(*header)[2 + nLengthBytes];
            std::fill(mask, mask + nMaskBytes, 0);
      
            boost::asio::async_read(
               *stream(), boost::asio::mutable_buffers_1(&(*header)[2], nLengthBytes + nMaskBytes),
               [=](const boost::system::error_code& error, size_t) mutable {
                  if (error) {
                     BOOST_LOG_TRIVIAL(error) << error.message();
                     return;
                  }
                  
                  for (size_t i = 0; i < nLengthBytes; ++i) {
                     nPayloadBytes <<= 8;
                     nPayloadBytes |= static_cast<uint8_t>((*header)[2 + i]);
                  }

                  auto payload = std::make_shared<std::vector<char> >(nPayloadBytes);
                  boost::asio::async_read(
                     *stream(), boost::asio::buffer(*payload),
                     [=](const boost::system::error_code& error, size_t) {
                        if (error) {
                           BOOST_LOG_TRIVIAL(error) << error.message();
                           return;
                        }

                        size_t cindex = 0;
                        for (char& c : *payload)
                           c ^= mask[cindex++ & 0x3];

                        handle_frame((*header)[0], std::move(payload));
                     });
               });
         });

   }

   void handle_ping(uint8_t type, const std::shared_ptr<std::vector<char> >& payload) {
   }
   
   void handle_close(uint8_t type, const std::shared_ptr<std::vector<char> >& payload) {
   }
   
   void handle_frame(uint8_t type, const std::shared_ptr<std::vector<char> >& payload) {
      if (type == (fin | ping))
         handle_ping(type, payload);
      else if (type == (fin | close))
         handle_close(type, payload);

      BOOST_LOG_TRIVIAL(info) << boost::format("0x%0x %s")
         % static_cast<unsigned int>(type)
         % std::string(payload->begin(), payload->end());
      
      read_frame();
   }
   
   template<typename ConstBufferSequence, typename WriteHandler>
   void async_send(uint8_t meta, const ConstBufferSequence& buffers, WriteHandler&& handler) {
      // Build the frame header.
      auto header = std::make_shared<std::vector<char> >(generate_header(meta, buffers));

      // Assemble the frame from the header and payload.
      std::vector<boost::asio::const_buffer> frame;
      frame.emplace_back(header->data(), header->size());
      for (const auto& buffer : buffers)
         frame.emplace_back(buffer);
      
      boost::asio::async_write(
         *stream(), frame,
         [=](const error_code& error, size_t) {
            if (error) {
               handler(error, 0);
               return;
            }

            header.get();
            handler(error, boost::asio::buffer_size(buffers));
         });
   }

   template<typename ConstBufferSequence>
   size_t send(uint8_t meta, const ConstBufferSequence& buffers, error_code& error) {
      auto header = generate_header(meta, buffers);
      boost::asio::write(*stream(), boost::asio::buffer(header), error);
      if (error)
         return 0;

      return boost::asio::write(*stream(), buffers, error);
   }
   
private:
   std::shared_ptr<T> stream_;
   boost::asio::streambuf streambuf_;
   
   template<typename ConstBufferSequence>
   std::vector<char> generate_header(uint8_t meta, const ConstBufferSequence& buffers) {
      std::vector<char> header;
      header.push_back(static_cast<char>(meta));
      
      const size_t bufferSize = boost::asio::buffer_size(buffers);
      if (bufferSize < 126) {
         header.push_back(static_cast<char>(bufferSize));
      }
      else if (bufferSize < 65536) {
         header.push_back(static_cast<char>(126));
         header.push_back(static_cast<char>((bufferSize >> 8) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 0) & 0xff));
      }
      else {
         header.push_back(static_cast<char>(127));
         header.push_back(static_cast<char>((bufferSize >> 56) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 48) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 40) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 32) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 24) & 0xff));
         header.push_back(static_cast<char>((bufferSize >> 16) & 0xff));
         header.push_back(static_cast<char>((bufferSize >>  8) & 0xff));
         header.push_back(static_cast<char>((bufferSize >>  0) & 0xff));
      }

      return header;
   }
};

template<typename T>
static std::string encode64(T bgn, T end) {
    using namespace boost::archive::iterators;
    typedef base64_from_binary<transform_width<T, 6, 8>> Iterator;
    std::string result((Iterator(bgn)), (Iterator(end)));
    result.resize((result.size() + 3) & ~size_t(3), '=');
    return result;
}

static void handle_connection(const std::shared_ptr<chunky::TCP>& tcp) {
   typedef WebSocket<chunky::TCP> WS;
   
   BOOST_LOG_TRIVIAL(info) << "creating WebSocket";
   auto ws = std::make_shared<WS>(tcp);

   boost::system::error_code error;
   ws->send(WS::fin | WS::text, boost::asio::buffer(std::string("synchronous send")), error);
   
   static const std::string s("asynchronous send");
   ws->async_send(
      WS::fin | WS::text, boost::asio::buffer(s),
      [=](const boost::system::error_code& error, size_t) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         ws.get();
      });

   ws->read_frame();
   
   while (true)
      std::this_thread::sleep_for(std::chrono::seconds(1));
}

int main() {
   chunky::SimpleHTTPServer server;
   server.add_handler("/", [](const std::shared_ptr<chunky::HTTP>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         static const std::string html =
            "<!DOCTYPE html>"
            "<title>chunky WebSocket</title>"
            "<h1>chunky WebSocket</h1>"
            "<script>\n"
            "  var socket = new WebSocket('ws://localhost:8800/ws');\n"
            "  socket.onopen = function() {\n"
            "    console.log('onopen')\n;"
            "    socket.send('from onopen');\n"   
            "  }\n"
            "  socket.onmessage = function(e) {\n"
            "    console.log(e.data);\n"
            "    socket.send('from onmessage');\n"   
            "  }\n"
            "  socket.onerror = function(error) {\n"
            "    console.log(error);\n"
            "  }\n"
            "</script>\n";
         
         boost::system::error_code error;
         boost::asio::write(*http, boost::asio::buffer(html), error);
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }
         
         http->finish(error);
      });

   server.add_handler("/ws", [](const std::shared_ptr<chunky::HTTP>& http) {
         BOOST_LOG_TRIVIAL(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();
         for (const auto& value : http->request_headers())
            BOOST_LOG_TRIVIAL(info) << boost::format("%s: %s")
               % value.first
               % value.second;

         http->response_status() = 101; // Switching Protocols
         http->response_headers()["Upgrade"] = "websocket";
         http->response_headers()["Connection"] = "Upgrade";

         // Compute Sec-WebSocket-Accept header value.
         EVP_MD_CTX sha1;
         EVP_DigestInit(&sha1, EVP_sha1());
         auto key = http->request_headers().find("Sec-WebSocket-Key");
         if (key != http->request_headers().end())
            EVP_DigestUpdate(&sha1, key->second.data(), key->second.size());

         static const std::string suffix("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
         EVP_DigestUpdate(&sha1, suffix.data(), suffix.size());

         unsigned int digestSize;
         unsigned char digest[EVP_MAX_MD_SIZE];
         EVP_DigestFinal(&sha1, digest, &digestSize);
         
         http->response_headers()["Sec-WebSocket-Accept"] = encode64(digest, digest + digestSize);

         boost::system::error_code error;
         http->finish();
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         handle_connection(http->stream());
      });
   
   // Set the optional logging callback.
   server.set_logger([](const std::string& message) {
         BOOST_LOG_TRIVIAL(info) << message;
      });

   // Run the server on all IPv4 and IPv6 interfaces.
   using boost::asio::ip::tcp;
   server.listen(tcp::endpoint(tcp::v4(), 8800));
   server.listen(tcp::endpoint(tcp::v6(), 8800));
   server.run();
   BOOST_LOG_TRIVIAL(info) << "listening on port 8800";
   
   // Accept new connections for 60 seconds. After that, the server
   // destructor will block until all existing TCP connections are
   // completed. Note that browsers may leave a connection open for
   // several minutes.
   std::this_thread::sleep_for(std::chrono::seconds(60));
   BOOST_LOG_TRIVIAL(info) << "exiting (blocks until existing connections close)";
   return 0;
}
