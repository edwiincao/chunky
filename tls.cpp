#include <iostream>
#include <boost/asio/ssl.hpp>

#define BOOST_LOG_DYN_LINK
#include <boost/log/trivial.hpp>

#include "chunky.hpp"

namespace chunky {
#ifdef BOOST_ASIO_SSL_HPP
   class TLS : public Stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> > {
   public:
      typedef boost::system::error_code error_code;
      
      template<typename CreateHandler>
      static void async_connect(
         boost::asio::ip::tcp::acceptor& acceptor,
         boost::asio::ssl::context& context,
         CreateHandler handler) {
         // Accept a TCP connection.
         std::shared_ptr<TLS> tls(new TLS(acceptor.get_io_service(), context));
         acceptor.async_accept(
            tls->stream().lowest_layer(),
            [=](const error_code& error) {
               if (error) {
                  handler(error, tls);
                  return;
               }

               // Perform TLS handshake.
               tls->stream().async_handshake(
                  boost::asio::ssl::stream_base::server,
                  [=](const error_code& error) {
                        handler(error, tls);
                  });
            });
      }

      template<typename ShutdownHandler>
      void async_shutdown(ShutdownHandler&& handler) {
         stream().async_shutdown(std::forward<ShutdownHandler>(handler));
      }
      
   private:
      TLS(boost::asio::io_service& io, boost::asio::ssl::context& context)
         : Stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(io, context) {
      }
   
   };
#endif // BOOST_ASIO_SSL_HPP
}

int main() {
   namespace ip = boost::asio::ip;
   namespace ssl = boost::asio::ssl;

   using boost::asio::ip::tcp;
   boost::asio::io_service io;
   boost::asio::ip::tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8843));
   boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);
   context.use_certificate_chain_file("server.pem");
   context.use_private_key_file("server.pem", boost::asio::ssl::context::pem);
   
   chunky::TLS::async_connect(
      acceptor, context,
      [=](const boost::system::error_code& error, const std::shared_ptr<chunky::TLS>& tls) {
         if (error) {
            BOOST_LOG_TRIVIAL(error) << error.message();
            return;
         }

         boost::asio::write(*tls, boost::asio::buffer(std::string("how now brown cow\n")));
      });
      
   io.run();
   return 0;
}
