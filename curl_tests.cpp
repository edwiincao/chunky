#define BOOST_TEST_MODULE curl
#define BOOST_LOG_DYN_LINK
#define BOOST_TEST_DYN_LINK

#include <future>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/test/unit_test.hpp>

#include <curl/curl.h>

#include "chunky.hpp"

using namespace chunky;
using boost::system::error_code;

#define LOG(LEVEL) BOOST_LOG_TRIVIAL(LEVEL)

class TestServer : public chunky::SimpleHTTPServer {
   unsigned short port_;
public:
   TestServer(const chunky::SimpleHTTPServer::Handler& callback)
      : chunky::SimpleHTTPServer(callback) {
      using boost::asio::ip::tcp;
      port_ = listen(tcp::endpoint(tcp::v4(), 0));
      run(1);
   }

   unsigned short port() const { return port_; }
   
   void log(const std::string& message) {
      LOG(info) << message;
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
   TestServer server([](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();
         
         BOOST_CHECK_EQUAL(http->request_method(), "GET");
         BOOST_CHECK_EQUAL(http->request_resource(), "/Minimal");
         
         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";
         http->finish();
      });

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
   TestServer server([](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/ContentLength");

         // By default, libcurl uses Expect: 100-continue requesting
         // an informational response before uploading data. This code
         // issues the provisional response, which is then to be
         // followed by the final response.
         http->response_status() = 100;
         http->finish();
         
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

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/ContentLength") % server.port()).str();
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
      BOOST_CHECK_EQUAL(os.str(), dnData);
   }
   
   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(Chunked) {
   TestServer server([](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

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
      });

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
   TestServer server([](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

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

                           http.get();
                        });
                  });
            });
      });
   
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
   TestServer server([](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

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
                        LOG(error) << error.message();
                        return;
                     }

                     http->async_finish([=](const error_code& error) {
                           if (error) {
                              LOG(error) << error.message();
                              return;
                           }
                        });
                  });
            });
      });

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

static size_t writeAsyncBigCB(char *s, size_t size, size_t n, void *) {
   return size*n;
}

static size_t readAsyncBigCB(char *s, size_t size, size_t n, void *data) {
   static std::default_random_engine rd;
   size_t& nReadBytes = *static_cast<size_t*>(data);
   if (nReadBytes == 0)
      return 0;
   
   std::uniform_int_distribution<size_t> d(1, std::min(nReadBytes, size*n));
   size_t nTransfer = d(rd);
   nReadBytes -= nTransfer;
   return nTransfer;
}

class WriteState {
   std::shared_ptr<HTTP> http_;
   size_t nBytesRemaining_;

   std::vector<char> data_;
   static std::default_random_engine rd;

public:
   WriteState(const std::shared_ptr<HTTP>& http, size_t nBytes)
      : http_(http)
      , nBytesRemaining_(nBytes) {
   }

   void doIt(const error_code& error, size_t nBytes) {
      if (error) {
         LOG(error) << error.message();
         return;
      }

      nBytesRemaining_ -= nBytes;
      if (nBytesRemaining_) {
         // Choose some number.
         std::uniform_int_distribution<size_t> d(1, nBytesRemaining_);
         size_t n = d(rd);
         data_.resize(n);
         
         boost::asio::async_write(
            *http_, boost::asio::buffer(data_),
            [=](const error_code& error, size_t nBytes) mutable {
               doIt(error, nBytes);
            });
      }
      else {
         http_->async_finish([=](const error_code& error) {
               if (error) {
                  LOG(error) << error.message();
                  return;
               }

               http_.reset();
            });
      }
   }
};

std::default_random_engine WriteState::rd;

BOOST_AUTO_TEST_CASE(AsyncBig) {
   std::shared_ptr<WriteState> writeState;
   
   TestServer server([&](const std::shared_ptr<HTTP>& http) {
         LOG(info) << boost::format("%s %s")
            % http->request_method()
            % http->request_resource();

         BOOST_CHECK_EQUAL(http->request_method(), "PUT");
         BOOST_CHECK_EQUAL(http->request_resource(), "/AsyncBig");

         http->response_status() = 200;
         http->response_headers()["Content-Type"] = "text/plain";

         // Don't read; let finish() drain the input stream.

         writeState = std::make_shared<WriteState>(http, 1 << 10);
         writeState->doIt(error_code(), 0);
      });
   
   CURL *curl = curl_easy_init();
   BOOST_REQUIRE(curl);

   for (int i = 0; i < 8; ++i) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
   
      auto url = (boost::format("http://localhost:%d/AsyncBig") % server.port()).str();
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      curl_slist* headers = curl_slist_append(nullptr, "Expect:");
      headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      auto nReadBytes = std::make_shared<size_t>(1 << 20);
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, &readAsyncBigCB);
      curl_easy_setopt(curl, CURLOPT_READDATA, nReadBytes.get());
      curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(*nReadBytes));

      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeAsyncBigCB);

      auto status = curl_easy_perform(curl);
      BOOST_CHECK_EQUAL(status, CURLE_OK);

      curl_slist_free_all(headers);
   }
   
   curl_easy_cleanup(curl);
}

BOOST_AUTO_TEST_CASE(Query) {
   {
      HTTP::Query query = HTTP::parse_query("");
      BOOST_CHECK(query.empty());
   }

   {
      HTTP::Query query = HTTP::parse_query("foo");
      BOOST_CHECK(query.empty());
   }

   {
      HTTP::Query query = HTTP::parse_query("foo=bar");
      BOOST_CHECK_EQUAL(query.size(), 1);
      BOOST_CHECK_EQUAL(query.at("foo"), "bar");
   }

   {
      HTTP::Query query = HTTP::parse_query("a=b&c=d&foo=bar");
      BOOST_CHECK_EQUAL(query.size(), 3);
      BOOST_CHECK_EQUAL(query.at("a"), "b");
      BOOST_CHECK_EQUAL(query.at("c"), "d");
      BOOST_CHECK_EQUAL(query.at("foo"), "bar");
   }

   {
      HTTP::Query query = HTTP::parse_query("foo=");
      BOOST_CHECK_EQUAL(query.size(), 1);
      BOOST_CHECK_EQUAL(query.at("foo"), "");
   }

   {
      HTTP::Query query = HTTP::parse_query("foo+bar%3f=a%20%3D%26");
      BOOST_CHECK_EQUAL(query.size(), 1);
      BOOST_CHECK_EQUAL(query.at("foo bar?"), "a =&");
   }
}
