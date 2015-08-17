#define BOOST_LOG_DYN_LINK
#include <iostream>
#include <boost/log/trivial.hpp>

#include <boost/asio/ssl.hpp>
#include "chunky.hpp"

int main() {
   // Configure the TLS context.
   boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);
   context.set_options(boost::asio::ssl::context::no_sslv3);
   context.use_certificate_chain_file("server.pem");
   context.use_private_key_file("server.pem", boost::asio::ssl::context::pem);

   // Create the server and add a sample handler.
   chunky::SimpleHTTPSServer server(context);
   server.add_handler("/", [](const std::shared_ptr<chunky::HTTPS>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         static const std::string html =
            "<!DOCTYPE html>"
            "<title>chunky SimpleHTTPSServer></title>"
            "<h1>HTTP over TLS</h1>";

         // Demonstrate error handling using error_code argument.
         boost::system::error_code error;
         boost::asio::write(*http, boost::asio::buffer(html), error);
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }
         
         http->finish(error);
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }
      });
         
   // Set the optional logging callback.
   server.set_logger([](const std::string& message) {
         BOOST_LOG_TRIVIAL(info) << message;
      });

   // Run the server on all IPv4 and IPv6 interfaces.
   using boost::asio::ip::tcp;
   try { server.listen(tcp::endpoint(tcp::v4(), 8443)); } catch (...) {}
   try { server.listen(tcp::endpoint(tcp::v6(), 8443)); } catch (...) {}
   server.run();
   BOOST_LOG_TRIVIAL(info) << "listening on port 8443";
   
   // Accept new connections for 60 seconds. After that, the server
   // destructor will block until all existing TCP connections are
   // completed. Note that browsers may leave a connection open for
   // several minutes.
   std::this_thread::sleep_for(std::chrono::seconds(60));
   BOOST_LOG_TRIVIAL(info) << "exiting (blocks until existing connections close)";
   return 0;
}
