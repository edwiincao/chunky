#define BOOST_LOG_DYN_LINK
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

int main() {
   // Construct a simple HTTP server with a request callback function.
   chunky::SimpleHTTPServer server([](const std::shared_ptr<chunky::HTTP>& http) {
         BOOST_LOG_TRIVIAL(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

         if (http->request_path() == "/") {
            http->response_status() = 200;
            http->response_headers()["Content-Type"] = "text/plain";
            boost::asio::write(*http, boost::asio::buffer(std::string("how now brown cow")));
         }
         else
            http->response_status() = 404;
         
         http->finish();
      });

   // Set the optional logging callback.
   server.set_log([](const std::string& message) {
         BOOST_LOG_TRIVIAL(info) << message;
      });

   // Run the server on all IPv4 and IPv6 interfaces.
   using boost::asio::ip::tcp;
   server.listen(tcp::endpoint(tcp::v4(), 8800));
   server.listen(tcp::endpoint(tcp::v6(), 8800));
   server.run();

   // Server destructor will block until all existing TCP connections
   // are completed.
   std::this_thread::sleep_for(std::chrono::seconds(60));
   return 0;
}
