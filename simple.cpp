#define BOOST_LOG_DYN_LINK
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

int main() {
   // Construct a simple HTTP server with sample handlers.
   chunky::SimpleHTTPServer server;

   server.add_handler("/", [](const std::shared_ptr<chunky::HTTP>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         static const std::string html =
            "<!DOCTYPE html>"
            "<title>chunky SimpleHTTPServer</title>"
            "<h1>chunky SimpleHTTPServer example</h1>"
            "<ul>"
            "<li><a href=\"async\">asynchronous</a></li>"
            "<li><a href=\"query?foo=chunky+web+server&bar=baz\">query</a></li>"
            "<li><a href=\"invalid\">invalid link</a></li>"
            "</ul>";
         boost::asio::write(*http, boost::asio::buffer(html));
         http->finish();
      });
   
   server.add_handler("/async", [](const std::shared_ptr<chunky::HTTP>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         static const std::string html =
            "<!DOCTYPE html>"
            "<title>asynchronous</title>"
            "<div>This content was delivered asynchronously.<div>"
            "<p><a href=\"/\">back</a></p>";

         boost::asio::async_write(
            *http, boost::asio::buffer(html),
            [=](const boost::system::error_code& error, size_t nBytes) {
               if (error) {
                  BOOST_LOG_TRIVIAL(info) << error.message();
                  return;
               }
            
               http->async_finish([=](boost::system::error_code& error) {
                     if (error) {
                        BOOST_LOG_TRIVIAL(info) << error.message();
                        return;
                     }

                     // The lifetime of the HTTP object must be preserved
                     // until this final callback. This can be done
                     // either by specifying it explicitly in the lambda
                     // capture list, or by referencing it in the lambda
                     // body with default capture by value as shown here.
                     http.get();
                  });
            });
      });
   
   server.add_handler("/query", [](const std::shared_ptr<chunky::HTTP>& http) {
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         static const std::string html =
            "<!DOCTYPE html>"
            "<title>query</title>";

         std::ostringstream os;
         os << "<!DOCTYPE html>"
            << "<title>query</title>"
            << "<h1>Query parameters</h1>"
            << "<ul>";
      
         for (const auto& value : http->request_query()) {
            os << boost::format("<li>%s = \"%s\"</li>")
               % value.first
               % value.second;
         }
         os << "</ul>";
         os << "<p><a href=\"/\">back</a></p>";
      
         boost::asio::write(*http, boost::asio::buffer(os.str()));
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
   BOOST_LOG_TRIVIAL(info) << "listening on port 8800";
   
   // Accept new connections for 60 seconds. After that, the server
   // destructor will block until all existing TCP connections are
   // completed. Note that browsers may leave a connection open for
   // several minutes.
   std::this_thread::sleep_for(std::chrono::seconds(60));
   return 0;
}
