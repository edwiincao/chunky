#define BOOST_LOG_DYN_LINK
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include "chunky.hpp"

// This is a simple implementation of the WebSocket frame protocol for
// a server. It can be used to communicate with a WebSocket client
// after a successful handshake. The class is stateless, so all
// methods are static and require a stream argument.
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
   static void receive_frames(Stream& stream, ReadHandler&& handler) {
      receive_frame(
         stream,
         [=, &stream](const boost::system::error_code& error,
             uint8_t type,
             FramePayload&& payload) {
            if (error) {
               handler(error, 0, FramePayload());
               return;
            }

            handler(boost::system::error_code(), type, std::move(payload));
            if (type != (fin | close))
               receive_frames(stream, handler);
         });
   }

   // Receive frame asynchronously.
   template<typename Stream, typename ReadHandler>
   static void receive_frame(Stream& stream, ReadHandler&& handler) {
      // Read the first two bytes of the frame header.
      auto header = std::make_shared<std::array<char, 14> >();
      boost::asio::async_read(
         stream, boost::asio::mutable_buffers_1(&(*header)[0], 2),
         [=, &stream](const boost::system::error_code& error, size_t) {
            if (error) {
               handler(error, 0, FramePayload());
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
               nLengthBytes = 8;
               nPayloadBytes = 0;
               break;
            }

            // Configure the mask.
            const size_t nMaskBytes = ((*header)[1] & 0x80) ? 4 : 0;
            char* mask = &(*header)[2 + nLengthBytes];
            std::fill(mask, mask + nMaskBytes, 0);

            // Read the payload size and mask.
            boost::asio::async_read(
               stream, boost::asio::mutable_buffers_1(&(*header)[2], nLengthBytes + nMaskBytes),
               [=, &stream](const boost::system::error_code& error, size_t) mutable {
                  if (error) {
                     handler(error, 0, FramePayload());
                     return;
                  }
                  
                  for (size_t i = 0; i < nLengthBytes; ++i) {
                     nPayloadBytes <<= 8;
                     nPayloadBytes |= static_cast<uint8_t>((*header)[2 + i]);
                  }

                  // Read the payload itself.
                  auto payload = std::make_shared<FramePayload>(nPayloadBytes);
                  boost::asio::async_read(
                     stream, boost::asio::buffer(*payload),
                     [=](const boost::system::error_code& error, size_t) {
                        if (error) {
                           handler(error, 0, FramePayload());
                           return;
                        }

                        // Unmask the payload buffer.
                        size_t cindex = 0;
                        for (char& c : *payload)
                           c ^= mask[cindex++ & 0x3];

                        // Dispatch the frame.
                        const uint8_t type = static_cast<uint8_t>((*header)[0]);
                        handler(boost::system::error_code(), type, std::move(*payload));
                     });
               });
         });
   }

   // Send frame asynchronously.
   template<typename Stream, typename ConstBufferSequence, typename WriteHandler>
   static void send_frame(
      Stream& stream,
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
         stream, frame,
         [=](const boost::system::error_code& error, size_t) {
            if (error) {
               handler(error);
               return;
            }

            header.get();
            handler(error);
         });
   }

   // Send frame synchronously returning error via error_code argument.
   template<typename Stream, typename ConstBufferSequence>
   static void send_frame(
      Stream& stream,
      uint8_t type,
      const ConstBufferSequence& buffers,
      boost::system::error_code& error) {
      auto header = build_header(type, buffers);

      // Assemble the frame from the header and payload.
      std::vector<boost::asio::const_buffer> frame;
      frame.emplace_back(header.data(), header.size());
      for (const auto& buffer : buffers)
         frame.emplace_back(buffer);
      
      boost::asio::write(stream, frame, error);
   }
   
   // Send frame synchronously returning error via exception.
   template<typename Stream, typename ConstBufferSequence>
   static void send_frame(
      Stream& stream,
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

// This is a sample WebSocket session function. It manages one
// connection over its stream argument.
static void speak_websocket(const std::shared_ptr<chunky::TCP>& tcp) {
   static const std::vector<std::string> messages = {
      std::string(""),
      std::string(1, 'A'),
      std::string(2, 'B'),
      std::string(4, 'C'),
      std::string(8, 'D'),
      std::string(16, 'E'),
      std::string(32, 'F'),
      std::string(64, 'G'),
      std::string(128, 'H'),
      std::string(256, 'I'),
      std::string(512, 'J'),
      std::string(1024, 'K'),
      std::string(2048, 'L'),
      std::string(4096, 'M'),
      std::string(8192, 'N'),
      std::string(16384, 'O'),
      std::string(32768, 'P'),
      std::string(65536, 'Q'),
      std::string(131072, 'R'),
      std::string(262144, 'S'),
   };

   // Start with a fragmented message.
   WebSocket::send_frame(*tcp, WebSocket::text, boost::asio::buffer(std::string("frag")));
   WebSocket::send_frame(*tcp, WebSocket::continuation, boost::asio::buffer(std::string("ment")));
   WebSocket::send_frame(*tcp, WebSocket::continuation, boost::asio::buffer(std::string("ation")));
   WebSocket::send_frame(*tcp, WebSocket::continuation, boost::asio::buffer(std::string(" test")));
   WebSocket::send_frame(*tcp, WebSocket::fin | WebSocket::continuation, boost::asio::null_buffers());

   // Iterate through the array of test messages with this index.
   auto index = std::make_shared<int>(0);
      
   // Receive frames until an error or close.
   WebSocket::receive_frames(
      *tcp,
      [=](const boost::system::error_code& error,
          uint8_t type,
          WebSocket::FramePayload&& payload) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         try {
            switch(type & 0x7f) {
            case WebSocket::continuation:
            case WebSocket::text:
            case WebSocket::binary:
               BOOST_LOG_TRIVIAL(info) << boost::format("%02x %6d %s")
                  % static_cast<unsigned int>(type)
                  % payload.size()
                  % std::string(payload.begin(), payload.begin() + std::min(payload.size(), size_t(20)));

               // Send the next test message (or close) when the
               // incoming message is complete.
               if (type & WebSocket::fin) {
                  if (*index < messages.size()) {
                     WebSocket::send_frame(
                        *tcp,
                        WebSocket::fin | WebSocket::text,
                        boost::asio::buffer(messages[(*index)++]),
                        [](const boost::system::error_code& error) {
                           if (error) {
                              BOOST_LOG_TRIVIAL(error) << error.message();
                              return;
                           }
                        });
                  }
                  else
                     WebSocket::send_frame(*tcp, WebSocket::fin | WebSocket::close, boost::asio::null_buffers());
               }
               break;
            case WebSocket::ping:
               BOOST_LOG_TRIVIAL(info) << "WebSocket::ping";
               WebSocket::send_frame(*tcp, WebSocket::fin | WebSocket::pong, boost::asio::buffer(payload));
               break;
            case WebSocket::pong:
               BOOST_LOG_TRIVIAL(info) << "WebSocket::pong";
               break;
            case WebSocket::close:
               BOOST_LOG_TRIVIAL(info) << "WebSocket::close";
               break;
            }
         }
         catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << e.what();
         }
      });
}

int main() {
   chunky::SimpleHTTPServer server;

   // Simple web page that opens a WebSocket on the server.
   server.add_handler("/", [](const std::shared_ptr<chunky::HTTP>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         // The client will simply echo messages the server sends.
         static const std::string html =
            "<!DOCTYPE html>"
            "<title>chunky WebSocket</title>"
            "<h1>chunky WebSocket</h1>"
            "<script>\n"
            "  var socket = new WebSocket('ws://' + location.host + '/ws');\n"
            "  socket.onopen = function() {\n"
            "    console.log('onopen')\n;"
            "  }\n"
            "  socket.onmessage = function(e) {\n"
            "    console.log('onmessage');\n"
            "    socket.send(e.data);\n"   
            "  }\n"
            "  socket.onclose = function(error) {\n"
            "    console.log('onclose');\n"
            "  }\n"
            "  socket.onerror = function(error) {\n"
            "    console.log('onerror ' + error);\n"
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

   // Perform the WebSocket handshake on /ws.
   server.add_handler("/ws", [](const std::shared_ptr<chunky::HTTP>& http) {
         BOOST_LOG_TRIVIAL(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

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

         // Handshake complete, hand off stream.
         speak_websocket(http->stream());
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
