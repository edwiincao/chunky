#define BOOST_LOG_DYN_LINK
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include "chunky.hpp"

class WebSocket {
public:
   typedef std::vector<char> FramePayload;
   
   enum FrameType {
      continuation = 0x0,
      text         = 0x1,
      binary       = 0x2,
      close        = 0x8,
      ping         = 0x9,
      pong         = 0xa,
      fin          = 0x80
   };

   // Receive frames from the stream continuously.
   template<typename Stream, typename ReadHandler>
   static void receive_frames(const std::shared_ptr<Stream>& stream, ReadHandler&& handler) {
      receive_frame(
         stream,
         [=](const boost::system::error_code& error,
             uint8_t type,
             const std::shared_ptr<FramePayload>& payload) {
            if (error) {
               handler(error, 0, std::shared_ptr<FramePayload>());
               return;
            }

            handler(boost::system::error_code(), type, payload);
            receive_frames(stream, handler);
         });
   }

   // Receive frame asynchronously.
   template<typename Stream, typename ReadHandler>
   static void receive_frame(const std::shared_ptr<Stream>& stream, ReadHandler&& handler) {
      // Read the first two bytes of the frame header.
      auto header = std::make_shared<std::array<char, 14> >();
      boost::asio::async_read(
         *stream, boost::asio::mutable_buffers_1(&(*header)[0], 2),
         [=](const boost::system::error_code& error, size_t) {
            if (error) {
               handler(error, 0, std::shared_ptr<FramePayload>());
               return;
            }

            // Determine the payload size format.
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

            // Configure the mask.
            const size_t nMaskBytes = ((*header)[1] & 0x80) ? 4 : 0;
            char* mask = &(*header)[2 + nLengthBytes];
            std::fill(mask, mask + nMaskBytes, 0);

            // Read the payload size and mask.
            boost::asio::async_read(
               *stream, boost::asio::mutable_buffers_1(&(*header)[2], nLengthBytes + nMaskBytes),
               [=](const boost::system::error_code& error, size_t) mutable {
                  if (error) {
                     handler(error, 0, std::shared_ptr<FramePayload>());
                     return;
                  }
                  
                  for (size_t i = 0; i < nLengthBytes; ++i) {
                     nPayloadBytes <<= 8;
                     nPayloadBytes |= static_cast<uint8_t>((*header)[2 + i]);
                  }

                  // Read the payload itself.
                  auto payload = std::make_shared<FramePayload>(nPayloadBytes);
                  boost::asio::async_read(
                     *stream, boost::asio::buffer(*payload),
                     [=](const boost::system::error_code& error, size_t) {
                        if (error) {
                           handler(error, 0, std::shared_ptr<FramePayload>());
                           return;
                        }

                        // Unmask the payload buffer.
                        size_t cindex = 0;
                        for (char& c : *payload)
                           c ^= mask[cindex++ & 0x3];

                        // Dispatch the frame.
                        const uint8_t type = static_cast<uint8_t>((*header)[0]);
                        handler(boost::system::error_code(), type, payload);
                     });
               });
         });
   }

   // Send frame asynchronously.
   template<typename Stream, typename ConstBufferSequence, typename WriteHandler>
   static void send_frame(
      const std::shared_ptr<Stream>& stream,
      uint8_t type,
      const ConstBufferSequence& buffers,
      WriteHandler&& handler) {
      // Build the frame header.
      auto header = std::make_shared<FramePayload>(build_header(type, buffers));

      // Assemble the frame from the header and payload.
      std::vector<boost::asio::const_buffer> frame;
      frame.emplace_back(header->data(), header->size());
      for (const auto& buffer : buffers)
         frame.emplace_back(buffer);
      
      boost::asio::async_write(
         *stream, frame,
         [=](const boost::system::error_code& error, size_t) {
            if (error) {
               handler(error);
               return;
            }

            header.get();
            stream.get();
            handler(error);
         });
   }

   // Send frame synchronously returning error via error_code argument.
   template<typename Stream, typename ConstBufferSequence>
   static void send_frame(
      const std::shared_ptr<Stream>& stream,
      uint8_t type,
      const ConstBufferSequence& buffers,
      boost::system::error_code& error) {
      auto header = build_header(type, buffers);

      // Assemble the frame from the header and payload.
      std::vector<boost::asio::const_buffer> frame;
      frame.emplace_back(header.data(), header.size());
      for (const auto& buffer : buffers)
         frame.emplace_back(buffer);
      
      boost::asio::write(*stream, frame, error);
   }
   
   // Send frame synchronously returning error via exception.
   template<typename Stream, typename ConstBufferSequence>
   static void send_frame(
      const std::shared_ptr<Stream>& stream,
      uint8_t type,
      const ConstBufferSequence& buffers) {
      boost::system::error_code error;
      send_frame(stream, type, buffers, error);
      if (error)
         throw boost::system::system_error(error);
   }

   // Transform Sec-WebSocket-Key value to Sec-WebSocket-Accept value.
   static std::string process_key(const std::string& key) {
      EVP_MD_CTX sha1;
      EVP_DigestInit(&sha1, EVP_sha1());
      EVP_DigestUpdate(&sha1, key.data(), key.size());

      static const std::string suffix("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
      EVP_DigestUpdate(&sha1, suffix.data(), suffix.size());

      unsigned int digestSize;
      unsigned char digest[EVP_MAX_MD_SIZE];
      EVP_DigestFinal(&sha1, digest, &digestSize);

      using namespace boost::archive::iterators;
      typedef base64_from_binary<transform_width<const unsigned char*, 6, 8>> Iterator;
      std::string result((Iterator(digest)), (Iterator(digest + digestSize)));
      result.resize((result.size() + 3) & ~size_t(3), '=');
      return result;
   }
   
private:
   template<typename ConstBufferSequence>
   static FramePayload build_header(uint8_t type, const ConstBufferSequence& buffers) {
      FramePayload header;
      header.push_back(static_cast<char>(type));
      
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

static void handle_connection(const std::shared_ptr<chunky::TCP>& tcp) {
   WebSocket::send_frame(
      tcp, WebSocket::fin | WebSocket::text,
      boost::asio::buffer(std::string("synchronous send")));
   
   static const std::string s("asynchronous send");
   WebSocket::send_frame(
      tcp, WebSocket::fin | WebSocket::text, boost::asio::buffer(s),
      [=](const boost::system::error_code& error) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }
      });

   WebSocket::receive_frames(
      tcp,
      [=](const boost::system::error_code& error,
          uint8_t type,
          const std::shared_ptr<WebSocket::FramePayload>& payload) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         BOOST_LOG_TRIVIAL(info) << std::string(payload->begin(), payload->end());
      });
   
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

         auto key = http->request_headers().find("Sec-WebSocket-Key");
         if (key != http->request_headers().end()) {
            http->response_status() = 101; // Switching Protocols
            http->response_headers()["Upgrade"] = "websocket";
            http->response_headers()["Connection"] = "Upgrade";
            http->response_headers()["Sec-WebSocket-Accept"] = WebSocket::process_key(key->second);
         }
         else {
            http->response_status() = 400; // Bad Request
            http->response_headers()["Connection"] = "close";
         }

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
