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
   
   // void handler(const error_code& error, uint8_t meta, size_t nBytes)
   template<typename MutableBufferSequence, typename ReadHandler>
   void async_receive(const MutableBufferSequence& buffers, ReadHandler handler) {
   }

   template<typename ReadHandler>
   void async_receive(boost::asio::streambuf& streambuf, ReadHandler handler) {
   }

   // void handler(const error_code& error, size_t nBytes)
   template<typename ConstBufferSequence, typename WriteHandler>
   void async_send(uint8_t meta, const ConstBufferSequence& buffers, WriteHandler&& handler) {
   }

private:
   std::shared_ptr<T> stream_;
};

static void handle_connection(const std::shared_ptr<chunky::TCP>& tcp) {
   BOOST_LOG_TRIVIAL(info) << "creating WebSocket";
   WebSocket<chunky::TCP> ws(tcp);
}

template<typename T>
static std::string encode64(T bgn, T end) {
    using namespace boost::archive::iterators;
    typedef base64_from_binary<transform_width<T, 6, 8>> Iterator;
    std::string result((Iterator(bgn)), (Iterator(end)));
    result.resize((result.size() + 3) & ~3, '=');
    return result;
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
            "  }\n"
            "  socket.onmessage = function(e) {\n"
            "    console.log(e.data);\n"
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
