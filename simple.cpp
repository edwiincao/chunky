#define BOOST_LOG_DYN_LINK

#include <iostream>
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

#define LOG(LEVEL) BOOST_LOG_TRIVIAL(LEVEL)

int main() {
   chunky::SimpleHTTPServer server([](const std::shared_ptr<chunky::HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";
         boost::asio::write(*http, boost::asio::buffer(std::string("how now brown cow")));
         http->finish();
      });
   
   server.set_log([](const std::string& message) {
         LOG(info) << message;
      });
   
   server.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 8800));
   server.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v6(), 8800));
   server.run();

   std::this_thread::sleep_for(std::chrono::seconds(60));
   
   return 0;
}
