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
   boost::asio::io_service io;
   auto server = chunky::SimpleHTTPSServer::create(io, context);
   server->set_handler("/", [](const std::shared_ptr<chunky::HTTPS>& http) {
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
   server->set_logger([](const std::string& message) {
         BOOST_LOG_TRIVIAL(info) << message;
      });

   // Run the server on all IPv4 and IPv6 interfaces.
   using boost::asio::ip::tcp;
   try { server->listen(tcp::endpoint(tcp::v4(), 8443)); } catch (...) {}
   try { server->listen(tcp::endpoint(tcp::v6(), 8443)); } catch (...) {}
   
   // Accept new connections for 60 seconds. After that, the server
   // destructor will block until all existing TCP connections are
   // completed. Note that browsers may leave a connection open for
   // several minutes.
   boost::asio::deadline_timer timer(io, boost::posix_time::seconds(60));
   timer.async_wait([=](const boost::system::error_code&) mutable {
         BOOST_LOG_TRIVIAL(info) << "exiting (blocks until existing connections close)";
         server->destroy();
      });
   
   BOOST_LOG_TRIVIAL(info) << "listening on port 8443";
   io.run();
   
   return 0;
}
