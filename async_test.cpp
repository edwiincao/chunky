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

         std::cout << boost::format("%s %s\n")
            % http->request_method()
            % http->request_resource();

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";

         static std::string how("how\n");
         static std::string now("now\n");
         static std::string brown("brown\n");
         static std::string cow("cow\n");
         
         boost::asio::async_write(
            *http, boost::asio::buffer(how),
            [=](const boost::system::error_code&, size_t) {
               boost::asio::write(*http, boost::asio::buffer(now));
               boost::asio::write(*http, boost::asio::buffer(brown));
               boost::asio::async_write(
                  *http, boost::asio::buffer(cow),
                  [=](const boost::system::error_code&, size_t) {
                     http->async_finish([=](const boost::system::error_code& error) {
                           http.get();
                           create_http(http->stream());
                        });
                  });
            });
      });
}

static void create_tcp(boost::asio::ip::tcp::acceptor& acceptor) {
   using boost::system::error_code;
   using namespace chunky;
   TCP::async_connect(
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
