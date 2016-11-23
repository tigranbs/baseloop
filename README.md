# Why Making new Event Loop ?
Currently there is couple of really great Event Loop engines like `Libuv`, `Libev` or `Boost ASIO`. But all of them is more for making specific API calls and they have a lot of overhead based on
their internal function calls.

The Goal of this project is to make a super simple implementation of `Epoll and Kqueue` inside single header file using C++ language features like `virtual functions`, which are actually not giving any
overhead after final object initialization.

# General API
Main functionality is given by overloading 4 functions, and writing functionality for specific socket types
```cpp
class TestServer: public BaseLoop {
protected:
    // callback for accepting connection here
    void acceptable(loop_event_data *data);
    // callback for reading data from socket here
    void readable(loop_event_data *data);
    // callback for writing data to socket
    void writable(loop_event_data *data);
    // get notification from pipe and read commands
    void notify();
};
```

# Project Status
This single header file already in production and in benchmarks served `~3.2 GB/s` in memory file transfer, because it doesn't have any overhead during Kernel Event loop functionality and your code.

**NOTE:** there is still a lot of work to do, and there is couple of cases which I need to handle for better error handling, but in general it is working and is extremely simple in usage.

# Contribution
I don't have enough experience in writing Kernel level events, but I'm in process of learning, that's why if you found something very dummy or you know how to fix something please open an issue or send me a pull request !