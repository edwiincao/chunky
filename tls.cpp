#include <iostream>
#include <boost/asio/ssl.hpp>

#define BOOST_LOG_DYN_LINK
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

namespace chunky {
}

int main() {
   namespace ip = boost::asio::ip;
   namespace ssl = boost::asio::ssl;

   using boost::asio::ip::tcp;
   boost::asio::io_service io;
   boost::asio::ip::tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8843));
   boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);
   context.set_options(boost::asio::ssl::context::no_sslv3);
   context.use_certificate_chain_file("server.pem");
   context.use_private_key_file("server.pem", boost::asio::ssl::context::pem);
   
   chunky::TLS::async_connect(
      acceptor, context,
      [=](const boost::system::error_code& error, const std::shared_ptr<chunky::TLS>& tls) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         chunky::HTTPS http(tls);
         boost::system::error_code error1;
         http.read_some(boost::asio::null_buffers(), error1);
         BOOST_LOG_TRIVIAL(info) << boost::format("%s %s")
            % http.request_method()
            % http.request_resource();

         http.response_status() = 200;
         http.response_headers()["Content-Type"] = "text/plain";
         
         boost::asio::write(http, boost::asio::buffer(std::string("how now brown cow\n")));
         http.finish();
      });
      
   io.run();
   return 0;
}
