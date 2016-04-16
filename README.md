# chunky
chunky.hpp is a C++ header that provides wrappers for [Boost
Asio](http://www.boost.org/doc/libs/1_59_0/doc/html/boost_asio.html)
streams for creating embedded HTTP(S) servers.

chunky was created primarily to provide an HTML5 interface to a C++
application (e.g. a control panel); its features for this purpose
include chunked transfer and WebSocket hand-off. It is not recommended
for handling public internet traffic because it is not hardened
against denial of service attacks.

chunky is developed by Shoestring Research, LLC and is available under
the [Apache License Version
2.0](http://www.apache.org/licenses/LICENSE-2.0).

## Prerequisites
chunky requires a C++11 compiler and standard library. It relies
heavily on [Boost](http://www.boost.org/) headers, and linking with
the Boost System library is always required for applications using
chunky.

In addition, the chunky unit tests require linking with
[Boost Log](http://www.boost.org/doc/libs/1_59_0/libs/log/doc/html/index.html),
[Boost Test](http://www.boost.org/doc/libs/1_59_0/libs/test/doc/html/index.html),
and [libcurl](http://curl.haxx.se/libcurl/). All samples require
linking with Boost Date/Time and Boost Log, and the TLS and WebSocket
samples additionally require [OpenSSL](https://www.openssl.org/).

## Basic usage
Here is a minimal program that creates an HTTP server on port 8800:

    #include "chunky.hpp"

    int main() {
       boost::asio::io_service io;
       auto server = chunky::SimpleHTTPServer::create(io);

       server->set_handler("/", [](const std::shared_ptr<chunky::HTTP>& http) {
             http->response_status() = 200;
             http->response_header("Content-Type") = "text/html";
             boost::asio::write(*http, boost::asio::buffer(std::string("Hello, World!")));
             http->finish();
          });

       using boost::asio::ip::tcp;
       server->listen(tcp::endpoint(tcp::v4(), 8800));

       io.run();
       return 0;
    }

## Other examples
All the example programs serve requests for 1 minute, then exit when
all open connections are closed. Note that specific web browsers may
keep idle connections open for several minutes.

### simple.cpp
This example program demonstrates:

* Adding multiple handlers to an embedded server.
* Adding a logging callback.
* Listening on IPv4 and IPv6 interfaces.
* Using boost::asio synchronous and asynchronous I/O for HTTP bodies.
* Provisional 100 Continue response.
* 404 Not Found response.

### tls.cpp
This example program demonstrates HTTP over TLS. It requires a
certificate and private key in a file named `server.pem`. For testing
convenience, a self-signed `server.pem` is provided in this
distribution but please note that connecting clients will likely
complain about an untrusted certificate that does not provide any
real security.

### websocket.cpp
This example program includes an implementation of the WebSocket data
transfer protocol and demonstrates how to use chunky to handle the
WebSocket handshake before handing off the stream for data transfer.
