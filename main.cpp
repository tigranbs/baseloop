//
// Created by Tigran on 11/23/16.
//

#include "baseloop.h"

using namespace BaseLoop;

class TestServer: public BaseLoop {
protected:
    // callback for accepting connection here
    void acceptable(loop_event_data_t *data);
    // callback for reading data from socket here
    void readable(loop_event_data_t *data);
    // callback for writing data to socket
    void writable(loop_event_data_t *data);
    // get notification from pipe and read commands
    void notify();
};

int main() {
    TestServer server;

}
