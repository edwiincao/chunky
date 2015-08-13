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
            "<li><form id=\"f\" action=\"post\" method=\"post\"><input type=\"hidden\" name=\"a\" value=\"Lorem ipsum dolor sit amet\"><input type=\"hidden\" name=\"foo\" value=\"bar\"><input type=\"hidden\" name=\"special\" value=\"~`!@#$%^&*()-_=+[]{}\\|;:,.<>\"></form><a href=\"javascript:{}\" onclick=\"document.getElementById('f').submit(); return false;\">post</a></li>"
            "<li><a href=\"invalid\">invalid link</a></li>"
            "</ul>";

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
                  BOOST_LOG_TRIVIAL(error) << error.message();
                  return;
               }
            
               http->async_finish([=](boost::system::error_code& error) {
                     if (error) {
                        BOOST_LOG_TRIVIAL(error) << error.message();
                        return;
                     }

                     // The lifetime of the HTTP object must be preserved
                     // until this final continuation. This can be done
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

         // Demonstrate error handling using exceptions.
         try {
            boost::asio::write(*http, boost::asio::buffer(os.str()));
            http->finish();
         }
         catch (const boost::system::system_error& e) {
            BOOST_LOG_TRIVIAL(error) << e.what();
         }
      });
   
   server.add_handler("/post", [](const std::shared_ptr<chunky::HTTP>& http) {
         // Demonstrate returning 100 Continue status. This is really
         // only useful if the client sends a 'Expect: 100-continue'
         // header but conformant clients should accept it in all
         // cases.
         boost::system::error_code error;
         http->response_status() = 100; // Continue
         http->finish(error);
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }
         
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/html";

         // Read through end of payload.
         auto streambuf = std::make_shared<boost::asio::streambuf>();
         boost::asio::async_read(
            *http, *streambuf,
            [=](const boost::system::error_code& error, size_t nBytes) {
               // EOF is not an error here.
               if (error && error != make_error_code(boost::asio::error::eof)) {
                  BOOST_LOG_TRIVIAL(error) << error.message();
                  return;
               }

               std::ostringstream os;
               os << "<!DOCTYPE html>"
                  << "<title>post</title>"
                  << "<h1>Post parameters</h1>"
                  << "<ul>";

               std::string s(boost::asio::buffers_begin(streambuf->data()),
                             boost::asio::buffers_end(streambuf->data()));
               for (const auto& value : chunky::HTTP::parse_query(s)) {
                  os << boost::format("<li>%s = \"%s\"</li>")
                     % value.first
                     % value.second;
               }
               os << "</ul>";
               os << "<p><a href=\"/\">back</a></p>";

               // Mixing synchronous and asynchronous I/O is okay. The
               // synchronous API is not as efficient but is easier to
               // code.
               boost::system::error_code error1;
               boost::asio::write(*http, boost::asio::buffer(os.str()), error1);
               if (error1) {
                  BOOST_LOG_TRIVIAL(error) << error1.message();
                  return;
               }

               http->async_finish([=](const boost::system::error_code& error) {
                     if (error) {
                        BOOST_LOG_TRIVIAL(error) << error.message();
                        return;
                     }

                     http.get();
                  });
            });
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
