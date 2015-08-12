#define BOOST_LOG_DYN_LINK
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

// This is a simple resource handler registry that maps a path string
// to a handler function.
static std::map<std::string, std::function<void(const std::shared_ptr<chunky::HTTP>&)> > handlers;

int main() {
   // Construct a simple HTTP server with a request callback function.
   chunky::SimpleHTTPServer server([](const std::shared_ptr<chunky::HTTP>& http) {
         BOOST_LOG_TRIVIAL(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

         // Look up a handler function for this resource.
         auto handler = handlers.find(http->request_path());
         if (handler != handlers.end()) {
            // This example only supports HTTP GET. If we wanted to
            // accept other methods, we might move method
            // discrimination into the individual resource handlers.
            if (http->request_method() == "GET") {
               // Invoke the resource-specific handler.
               handler->second(http);
            }
            else {
               http->response_status() = 405; // Method Not Allowed
               http->response_headers()["Allow"] = "GET";
               boost::asio::write(*http, boost::asio::null_buffers());
               http->finish();
               return;
            }
         }
         else {
            http->response_status() = 404; // Not Found

            static const std::string html =
               "<!DOCTYPE html>"
               "<title>404 - Not Found</title>"
               "<h1>404 - Not Found</h1>";
            boost::asio::write(*http, boost::asio::buffer(html));
            http->finish();
         }
      });

   // Register some sample handlers.
   
   handlers["/"] = [](const std::shared_ptr<chunky::HTTP>& http) {
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
   };
   
   handlers["/async"] = [](const std::shared_ptr<chunky::HTTP>& http) {
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
   };
   
   handlers["/query"] = [](const std::shared_ptr<chunky::HTTP>& http) {
      http->response_status() = 200;
      http->response_headers()["Content-Type"] = "text/html";

      static const std::string html =
         "<!DOCTYPE html>"
         "<title>query</title>";

      std::ostringstream os;
      os << "<!DOCTYPE html>"
         << "<title>query</title>"
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
   };
   
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
   
   // Server destructor will stop listening for new connections and
   // block until all existing TCP connections are completed. Note
   // that browsers may leave a connection open for several minutes.
   std::this_thread::sleep_for(std::chrono::seconds(60));
   return 0;
}
