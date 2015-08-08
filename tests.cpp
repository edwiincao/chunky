#define BOOST_TEST_MODULE basic
#define BOOST_LOG_DYN_LINK
#define BOOST_TEST_DYN_LINK

#include <iostream>
#include <sstream>
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/test/unit_test.hpp>

#include <curl/curl.h>

#include "chunky.hpp"

using namespace chunky;
#define LOG(LEVEL) BOOST_LOG_TRIVIAL(LEVEL)

class Server {
   typedef std::function<void(const std::shared_ptr<HTTP>&)> Handler;
   Handler handler_;

   boost::asio::io_service io_;
   boost::asio::ip::tcp::acceptor acceptor_;
   std::thread t_;
   
public:
   Server(const Handler handler)
      : handler_(handler)
      , acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0)) {
      using namespace std::placeholders;
      LOG(info) << "listening on port " << port();
      TCP::async_connect(acceptor_, std::bind(&Server::connectHandler, this, _1, _2));
      std::thread([this]() { io_.run(); }).swap(t_);
   }

   ~Server() {
      t_.join();
   }

   unsigned short port() {
      return acceptor_.local_endpoint().port();
   }

   void connectHandler(const boost::system::error_code& error, const std::shared_ptr<TCP>& tcp) {
      if (error) {
         LOG(error) << "accept" << error.message();
         return;
      }

      HTTP::async_create(
         tcp,
         [=](const boost::system::error_code& error, const std::shared_ptr<HTTP>& http) {
            if (error) {
               return;
            }
         
            LOG(info) << boost::format("%s %s")
               % http->request_method()
               % http->request_resource();
            handler_(http);
         });
   }
};

static size_t writeCB(char *s, size_t size, size_t n, void *data) {
   auto os = static_cast<std::ostream*>(data);
   os->write(s, size*n);
   return size*n;
}

static size_t readCB(char *s, size_t size, size_t n, void *data) {
   auto is = static_cast<std::istream*>(data);
   is->read(s, size*n);
   return is->gcount();
}

BOOST_AUTO_TEST_CASE(Minimal) {
   Server server([](const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "GET");
         BOOST_CHECK_EQUAL(http->request_resource(), "/foo");
         
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";
         http->finish();
      });

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   auto url = (boost::format("http://localhost:%d/foo") % server.port()).str();
   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

   auto status = curl_easy_perform(curl);
   BOOST_CHECK_EQUAL(status, CURLE_OK);
   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(ContentLength) {
   const std::string upData = "foo bar baz";
   const std::string dnData = "how now brown cow";

   Server server([=](const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/foo");

         boost::asio::streambuf body;
         boost::system::error_code error;
         boost::asio::read(*http, body, error);
         std::string s(boost::asio::buffers_begin(body.data()), boost::asio::buffers_end(body.data()));
         BOOST_CHECK_EQUAL(s, upData);

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";

         http->response_headers()["Content-Length"] = std::to_string(dnData.size());
         for (char c : dnData)
            boost::asio::write(*http, boost::asio::buffer(&c, 1));

         http->finish();
      });

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
   auto url = (boost::format("http://localhost:%d/foo") % server.port()).str();
   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

   std::istringstream is(upData);
   curl_easy_setopt(curl, CURLOPT_READFUNCTION, &readCB);
   curl_easy_setopt(curl, CURLOPT_READDATA, &is);
   curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(upData.size()));

   std::ostringstream os;
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCB);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &os);
   
   auto status = curl_easy_perform(curl);
   BOOST_CHECK_EQUAL(status, CURLE_OK);
   curl_easy_cleanup(curl);

   BOOST_CHECK_EQUAL(os.str(), dnData);
}

BOOST_AUTO_TEST_CASE(Chunked) {
   const std::string upData = "foo bar baz";
   const std::string dnData = "how now brown cow";

   Server server([=](const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/foo");

         boost::asio::streambuf body;
         boost::system::error_code error;
         boost::asio::read(*http, body, error);
         std::string s(boost::asio::buffers_begin(body.data()), boost::asio::buffers_end(body.data()));
         BOOST_CHECK_EQUAL(s, upData);

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";

         for (char c : dnData)
            boost::asio::write(*http, boost::asio::buffer(&c, 1));

         http->finish();
      });

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
   auto url = (boost::format("http://localhost:%d/foo") % server.port()).str();
   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

   curl_slist* headers = nullptr;
   headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   
   std::istringstream is(upData);
   curl_easy_setopt(curl, CURLOPT_READFUNCTION, &readCB);
   curl_easy_setopt(curl, CURLOPT_READDATA, &is);
   curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(upData.size()));

   std::ostringstream os;
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCB);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &os);
   
   auto status = curl_easy_perform(curl);
   BOOST_CHECK_EQUAL(status, CURLE_OK);
   curl_easy_cleanup(curl);
   curl_slist_free_all(headers);

   BOOST_CHECK_EQUAL(os.str(), dnData);
}
