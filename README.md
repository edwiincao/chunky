# chunky
chunky.hpp is a C++ header that provides wrappers for [Boost
Asio](http://www.boost.org/doc/libs/1_59_0/doc/html/boost_asio.html)
streams for creating embedded HTTP(S) servers.

chunky is developed by Shoestring Research, LLC and is available under
the [Apache License Version
2.0](http://www.apache.org/licenses/LICENSE-2.0).

## Prerequisites
chunky requires a C++11 compiler. It relies heavily on
[Boost](http://www.boost.org/), and linking with the Boost System
library is always required for applications using chunky.

In addition the chunky unit tests require linking with [Boost
Log](http://www.boost.org/doc/libs/1_59_0/libs/log/doc/html/index.html)
and [libcurl](http://curl.haxx.se/libcurl/). The TLS and WebSocket
samples require linking with Boost Log and
[OpenSSL](https://www.openssl.org/).
