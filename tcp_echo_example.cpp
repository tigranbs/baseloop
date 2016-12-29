//
// Created by Tigran on 11/23/16.
//

#include "baseloop.h"
#include "iostream"

using namespace BaseLoop;
using namespace std;

struct TcpConn {
    int fd;
    loop_event_data_t loop_data;

    TcpConn(int fd) {
        this->fd = fd;
        this->loop_data.data = this;
        this->loop_data.fd = fd;
    }
};

class TcpEcho: protected EventLoop {
public:
    TcpEcho() {}

    void start(std::string &address) {
        this->init_loop();
        this->listen_tcp(address);
        this->run_loop();
    }

protected:
    /// callback for accepting connection here
    void acceptable(loop_event_data_t *data, int fd) {
        auto conn = new TcpConn(fd);
        this->register_handle(&conn->loop_data);
        this->make_readable(&conn->loop_data);
        cout << "Connection Accepted !" << endl;
    };

    /// callback for reading data from socket here
    void readable(loop_event_data_t *data, char *buffer, size_t read_len) {
        cout << "Read Data -> " << string(buffer, read_len) << endl;
        write(data->fd, buffer, read_len);
    };

    /// callback for writing data to socket
    void writable(loop_event_data_t *data) {
        this->make_readable(data);
    };

    /// callback function for getting connection close event
    void closed(loop_event_data_t *data) {
        auto conn = (TcpConn *) data->data;
        delete conn;

        cout << "Connection closed !" << endl;
    };
};

int main() {
    string address("127.0.0.1:8888");
    TcpEcho echo;
    echo.start(address);
}
