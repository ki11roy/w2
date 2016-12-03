#include "wayward/server.hpp"
#include "wayward/util/linklist.hpp"

#include <asio.hpp>
#include <http_parser.h>

#include <iostream>
#include <sstream>

namespace wayward {
    struct Server::Client {
        Server::Impl& server_impl;
        util::IntrusiveListAnchor<Client> anchor;
        asio::ip::tcp::socket socket;
        http_parser parser;

        static constexpr size_t recv_buffer_size = 1024;
        std::unique_ptr<char[]> recv_buffer;
        std::string send_buffer; // TODO

        std::string current_header_field;
        Request current_request;

        Client(Impl& server_impl);

        void keep_reading();
        void keep_writing();
        void send_response(Response);
        void close();

        static int on_message_begin(http_parser*);
        static int on_headers_complete(http_parser*);
        static int on_message_complete(http_parser*);
        static int on_chunk_header(http_parser*);
        static int on_chunk_complete(http_parser*);

        static int on_url(http_parser*, const char*, size_t);
        static int on_status(http_parser*, const char*, size_t);
        static int on_header_field(http_parser*, const char*, size_t);
        static int on_header_value(http_parser*, const char*, size_t);
        static int on_body(http_parser*, const char*, size_t);

        static const http_parser_settings parser_settings;
    };

    struct Server::Impl {
        asio::io_service service;
        asio::ip::tcp::acceptor acceptor;
        util::IntrusiveList<Client, &Client::anchor> clients;
        Client* next_client = nullptr;
        IRequestResponder* responder = nullptr;

        Impl() : acceptor(service) {}
        ~Impl();

        void keep_accepting();
    };


    const http_parser_settings Server::Client::parser_settings = {
        .on_message_begin = &Server::Client::on_message_begin,
        .on_url = &Server::Client::on_url,
        .on_status = &Server::Client::on_status,
        .on_header_field = &Server::Client::on_header_field,
        .on_header_value = &Server::Client::on_header_value,
        .on_headers_complete = &Server::Client::on_headers_complete,
        .on_body = &Server::Client::on_body,
        .on_message_complete = &Server::Client::on_message_complete,
        .on_chunk_header = &Server::Client::on_chunk_header,
        .on_chunk_complete = &Server::Client::on_chunk_complete,
    };

    Server::Server() : impl_(new Impl) {}

    Server::~Server() {}

    Server::Impl::~Impl() {
        while (!clients.empty()) {
            auto it = clients.begin();
            Client* client = &*it;
            delete client;
        }
    }

    Server& Server::listen(std::string addr, unsigned int port) {
        // TODO: Add support for listening on multiple endpoints
        auto ip_addr = asio::ip::address::from_string(addr.c_str());
        asio::ip::tcp::endpoint endpoint(ip_addr, port);
        impl_->acceptor.open(endpoint.protocol());
        impl_->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        impl_->acceptor.bind(endpoint);
        impl_->acceptor.listen(10);
        std::cout << "Wayward Server listening on " << impl_->acceptor.local_endpoint().address() << ":" << impl_->acceptor.local_endpoint().port() << ".\n";
        impl_->keep_accepting();
        return *this;
    }

    int Server::run(IRequestResponder& responder) {
        impl_->responder = &responder;
        impl_->service.run();
        return 0;
    }

    void Server::Impl::keep_accepting() {
        assert(next_client == nullptr);
        next_client = new Client(*this);
        clients.link_front(next_client);
        acceptor.async_accept(next_client->socket, [this](std::error_code ec) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            if (ec) {
                std::cerr << "accept(): " << ec.message() << "\n";
                std::abort();
            }
            next_client->keep_reading();
            next_client = nullptr;
            keep_accepting();
        });
    }

    Server::Client::Client(Server::Impl& impl)
        : server_impl(impl)
        , socket(impl.service)
        , recv_buffer(new char[recv_buffer_size])
    {
        http_parser_init(&parser, HTTP_REQUEST);
        parser.data = this;
    }

    void Server::Client::keep_reading() {
        auto handler = [this](std::error_code ec, size_t len) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            if (ec == asio::error::connection_reset) {
                close();
                return;
            }
            if (ec == asio::error::eof) {
                http_parser_execute(&parser, &parser_settings, nullptr, 0);
                if (http_should_keep_alive(&parser))
                    keep_reading();
                else
                    close();
                return;
            }

            if (ec) {
                std::cerr << "socket error: " << ec.message() << "\n";
                close();
            }
            else {
                http_parser_execute(&parser, &parser_settings, recv_buffer.get(), len);
            }
        };
        socket.async_read_some(asio::buffer(recv_buffer.get(), recv_buffer_size), std::move(handler));
    }

    void Server::Client::keep_writing() {
        auto handler = [this](std::error_code ec, size_t len) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            if (ec == asio::error::connection_reset) {
                close();
                return;
            }
            if (ec == asio::error::eof) {
                close();
                return;
            }
            if (ec) {
                std::cerr << "socket error while writing: " << ec.message() << "\n";
                close();
                return;
            }
            keep_reading();
        };
        asio::async_write(socket, asio::buffer(send_buffer.data(), send_buffer.size()), std::move(handler));
    }

    void Server::Client::send_response(Response res) {
        std::stringstream ss;
        ss << "HTTP/1.1 " << int(res.status) << "\r\n";
        for (auto& pair: res.headers) {
            ss << pair.first << ": " << pair.second << "\r\n";
        }
        ss << "Content-Length: " << res.body.size() << "\r\n";
        ss << "\r\n";
        ss << res.body;
        send_buffer = ss.str();
        keep_writing();
    }

    void Server::Client::close() {
        Client* dead_client = this;
        auto handler = [dead_client]() {
            delete dead_client;
        };
        server_impl.service.post(std::move(handler));
    }

    int Server::Client::on_message_begin(http_parser* parser) {
        Client& client = *static_cast<Client*>(parser->data);
        return 0;
    }
    int Server::Client::on_headers_complete(http_parser* parser) {
        Client& client = *static_cast<Client*>(parser->data);
        return 0;
    }
    int Server::Client::on_message_complete(http_parser* parser) {
        Client& client = *static_cast<Client*>(parser->data);
        Response response;
        client.server_impl.responder->respond(client.current_request, response);
        client.send_response(std::move(response));
        return 0;
    }
    int Server::Client::on_chunk_header(http_parser* parser) {
        Client& client = *static_cast<Client*>(parser->data);
        return 0;
    }
    int Server::Client::on_chunk_complete(http_parser* parser) {
        Client& client = *static_cast<Client*>(parser->data);
        return 0;
    }

    int Server::Client::on_url(http_parser* parser, const char* url, size_t len) {
        Client& client = *static_cast<Client*>(parser->data);
        client.current_request.url = std::string(url, len);
        return 0;
    }
    int Server::Client::on_status(http_parser* parser, const char*, size_t) {
        Client& client = *static_cast<Client*>(parser->data);
        return 0;
    }
    int Server::Client::on_header_field(http_parser* parser, const char* field, size_t len) {
        Client& client = *static_cast<Client*>(parser->data);
        client.current_header_field = std::string(field, len);
        return 0;
    }
    int Server::Client::on_header_value(http_parser* parser, const char* value, size_t len) {
        Client& client = *static_cast<Client*>(parser->data);
        client.current_request.headers[std::move(client.current_header_field)] = std::string(value, len);
        return 0;
    }
    int Server::Client::on_body(http_parser* parser, const char* body, size_t len) {
        Client& client = *static_cast<Client*>(parser->data);
        client.current_request.body = std::string(body, len);
        return 0;
    }
}

