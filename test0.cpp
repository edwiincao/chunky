#include <iostream>

#include "chunky.hpp"

static void create_http(const std::shared_ptr<chunky::TCP>& tcp) {
   using boost::system::error_code;
   using namespace chunky;
   HTTP::async_create(
      tcp,
      [=](const error_code& error, const std::shared_ptr<HTTP>& http) {
         if (error) {
            std::cout << error.message() << '\n';
            return;
         }

         std::cout << http->request_method()
                   << ' ' << http->request_resource()
                   << '\n';
         for (const auto& header : http->request_headers())
            std::cout << header.first << ": " << header.second << '\n';

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";

         boost::asio::write(*http, boost::asio::buffer(std::string("how now brown cow")));
         http->finish();

         create_http(tcp);
      });
}

static void create_tcp(boost::asio::ip::tcp::acceptor& acceptor) {
   using boost::system::error_code;
   using namespace chunky;
   TCP::async_create(
      acceptor,
      [&](const error_code& error, const std::shared_ptr<TCP>& tcp) {
         create_tcp(acceptor);
         if (error) {
            std::cout << error.message() << '\n';
            return;
         }

         create_http(tcp);
      });
}

int main() {
   using boost::asio::ip::tcp;
   boost::asio::io_service io;
   tcp::acceptor a4(io, tcp::endpoint(tcp::v4(), 8800));
   tcp::acceptor a6(io, tcp::endpoint(tcp::v6(), 8800));
   create_tcp(a4);
   create_tcp(a6);
   
   io.run();
   return 0;
}
