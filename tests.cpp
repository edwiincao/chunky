#define BOOST_TEST_MODULE basic
#define BOOST_LOG_DYN_LINK
#define BOOST_TEST_DYN_LINK

#include <future>
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
   boost::asio::io_service io_;
   boost::asio::ip::tcp::acceptor acceptor_;
   std::thread t_;
   
public:
   typedef boost::system::error_code error_code;
   
   Server()
      : acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0)) {
      using namespace std::placeholders;
      LOG(info) << "listening on port " << port();
      TCP::async_connect(acceptor_, std::bind(&Server::acceptHandler, this, _1, _2));
      std::thread([this]() { io_.run(); }).swap(t_);
   }

   virtual ~Server() {
      t_.join();
   }

   virtual unsigned short port() {
      return acceptor_.local_endpoint().port();
   }

   virtual void acceptHandler(const error_code& error, const std::shared_ptr<TCP>& tcp) {
      if (error) {
         LOG(error) << "accept" << error.message();
         return;
      }

      serve(tcp);
   }

   virtual void serve(const std::shared_ptr<TCP>& tcp) {
      using namespace std::placeholders;
      HTTP::async_create(tcp, std::bind(&Server::serveHandlerWrapper, this, _1, _2));
   }

   virtual void serveHandlerWrapper(const error_code& error, const std::shared_ptr<HTTP>& http) {
      if (error) {
         LOG(info) << error.message();
         return;
      }
         
      LOG(info) << boost::format("%s %s")
         % http->request_method()
         % http->request_resource();

      serveHandler(http);
   }

   virtual void serveHandler(const std::shared_ptr<HTTP>& http) = 0;
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
   class MyServer : public Server {
      void serveHandler(const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "GET");
         BOOST_CHECK_EQUAL(http->request_resource(), "/Minimal");
         
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";
         http->finish();

         serve(http->stream());
      }
   } server;

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   auto url = (boost::format("http://localhost:%d/Minimal") % server.port()).str();
   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

   auto status = curl_easy_perform(curl);
   BOOST_CHECK_EQUAL(status, CURLE_OK);
   curl_easy_cleanup(curl);
}

static const std::string upData = "foo bar baz";
static const std::string dnData = "how now brown cow";

BOOST_AUTO_TEST_CASE(ContentLength) {
   class MyServer : public Server {
      void serveHandler(const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/ContentLength");

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

         serve(http->stream());
      }
   } server;

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/ContentLength") % server.port()).str();
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      curl_slist* headers = curl_slist_append(nullptr, "Expect:");
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
      BOOST_CHECK_EQUAL(os.str(), dnData);

      curl_slist_free_all(headers);
   }
   
   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(Chunked) {
   class MyServer : public Server {
      void serveHandler(const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/Chunked");

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

         serve(http->stream());
      }
   } server;

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/Chunked") % server.port()).str();
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      curl_slist* headers = curl_slist_append(nullptr, "Expect:");
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
      BOOST_CHECK_EQUAL(os.str(), dnData);

      curl_slist_free_all(headers);
   }

   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(AsyncContentLength) {
   class MyServer : public Server {
      void serveHandler(const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/AsyncContentLength");

         auto body = std::make_shared<boost::asio::streambuf>();
         boost::asio::async_read(
            *http, *body,
            [=](const error_code& error, size_t nBytes) {
               std::string s(boost::asio::buffers_begin(body->data()), boost::asio::buffers_end(body->data()));
               BOOST_CHECK_EQUAL(s, upData);
               
               http->response_status() = 200;
               http->response_headers()["Content-Type"] = "text/plain";

               http->response_headers()["Content-Length"] = std::to_string(dnData.size());
               boost::asio::async_write(
                  *http, boost::asio::buffer(dnData),
                  [=](const error_code& error, size_t nBytes) {
                     BOOST_CHECK_EQUAL(nBytes, dnData.size());
                     if (error) {
                        LOG(error) << error.message();
                        return;
                     }

                     http->async_finish([=](const error_code& error) {
                           if (error) {
                              LOG(error) << error.message();
                              return;
                           }
                           serve(http->stream());
                        });
                  });
            });
      }
   } server;

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/AsyncContentLength") % server.port()).str();
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      curl_slist* headers = curl_slist_append(nullptr, "Expect:");
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
      BOOST_CHECK_EQUAL(os.str(), dnData);

      curl_slist_free_all(headers);
   }
   
   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(AsyncChunked) {
   class MyServer : public Server {
      void serveHandler(const std::shared_ptr<HTTP>& http) {
         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/AsyncChunked");

         auto body = std::make_shared<boost::asio::streambuf>();
         boost::asio::async_read(
            *http, *body,
            [=](const error_code& error, size_t nBytes) {
               std::string s(boost::asio::buffers_begin(body->data()), boost::asio::buffers_end(body->data()));
               BOOST_CHECK_EQUAL(s, upData);
               
               http->response_status() = 200;
               http->response_headers()["Content-Type"] = "text/plain";

               boost::asio::async_write(
                  *http, boost::asio::buffer(dnData),
                  [=](const error_code& error, size_t nBytes) {
                     BOOST_CHECK_EQUAL(nBytes, dnData.size());
                     if (error) {
                        LOG(error) << "async_write handler " << error.message();
                        return;
                     }

                     http->async_finish([=](const error_code& error) {
                           if (error) {
                              LOG(error) << "async_finish handler\n" << error.message();
                              return;
                           }
                           serve(http->stream());
                        });
                  });
            });
      }
   } server;

   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/AsyncChunked") % server.port()).str();
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      curl_slist* headers = curl_slist_append(nullptr, "Expect:");
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
      BOOST_CHECK_EQUAL(os.str(), dnData);

      curl_slist_free_all(headers);
   }
   
   curl_easy_cleanup(curl);
}
