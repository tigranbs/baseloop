//
// Created by Tigran on 11/23/16.
//

#ifndef BASELOOP_BASELOOP_H
#define BASELOOP_BASELOOP_H

/** Defining members which are probably could be changed */

#define BASE_LOOP_EVENTS_COUNT 5000

/** End of definitions */


#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define USE_KQUEUE

#include <sys/socket.h>
#include <cstdlib>
#include <cstdio>
#include "sys/event.h"
#elif defined(__linux__)
#define USE_EPOLL
#include <sys/epoll.h>

#endif

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "list"
#include "mutex"
#include "memory"
#include "vector"
#include "string"

#include "helpers.h"

namespace BaseLoop {

    struct loop_cmd_t {
        int cmd = -1;
        void *data = nullptr;

        loop_cmd_t(int cmd, void *data) {
            this->cmd = cmd;
            this->data = data;
        }
    };

    struct loop_event_data_t {
        // socket handler
        int fd = -1;

        // public data for outside usage
        void *data = nullptr;
    };

    class BaseLoop {
    public:
        /// Just an empty destructor/constructor
        ~BaseLoop() {}
        BaseLoop() {}

    protected:
        /// Commands List which are used during execution
        std::list<loop_cmd_t*> commands;

        /// callback for accepting connection here
        virtual void acceptable(loop_event_data_t *data, int fd) {};

        /// callback for reading data from socket here
        virtual void readable(loop_event_data_t *data) {};

        /// callback for writing data to socket
        virtual void writable(loop_event_data_t *data) {};

        /// get notification from pipe and read commands
        virtual void notify() {};

    private:
        /// just raw number to make sure if we don't have server listener
        /// making sure that event loop would't compare it as a socket number
        int listener = -1;

        /// member for keeping pipe handle which would be used for sending commands
        /// from other threads to this loop (similar to Thread Channels, but it's NOT!)
        int pipe_chan = -1;
        int event_fd = -1;

        /// Because we are actually keeping command's List as a Queue
        /// we need some locking mutex for thread safe command insert/delete
        std::mutex loop_cmd_locker;

        /// keeping track about our PIPe is writable or not
        bool pipe_writable = false;

        /// List of commands which are used to keep them before event loop would have a time to process them
        std::list<loop_cmd_t*> _commands;

        /// just a local data for keeping some reference to our pipe options inside Kernel Loop
        loop_event_data_t pipe_event_data;

        /// Accepting connections from server socket
        inline void accept_conn(loop_event_data_t *data) {
            if(this->listener <= 0)
                return;

            struct sockaddr addr;
            socklen_t socklen;
            int fd = -1;
            for(;;) {
                fd = accept(this->listener, &addr, &socklen);
                if(fd <= 0)
                    break;

                // calling callback function for handling accepted connection
                this->acceptable(data, fd);
            }
        }

    public:

        /// making connection non blocking for handling async events from kernel Event Loop
        int make_socket_non_blocking (int sfd) {
            const int flags = fcntl (sfd, F_GETFL, 0);
            if ( flags == -1) {
                return -1;
            }

            const int temp_flags = flags | O_NONBLOCK;
            const int s = fcntl (sfd, F_SETFL, temp_flags);
            if (s == -1) {
                return -1;
            }

            return 0;
        }

        /// one time actions which are needed to init base components for event loop
        void init_loop()
        {
            int pipe_chans[2];
            pipe(pipe_chans);
            this->pipe_chan = pipe_chans[1];
            this->make_socket_non_blocking(this->pipe_chan);

#ifdef USE_KQUEUE
            this->event_fd = kqueue();
#elif defined(USE_EPOLL)
            this->event_fd = epoll_create(1);
#endif
            this->pipe_event_data.data = NULL;
            this->pipe_event_data.fd = this->pipe_chan;
            this->register_handle(&this->pipe_event_data);
        }

        /// Base function for sending commands to this loop
        /// it will make pipe writable and will insert command to our list
        /// so when loop would be ready to consume pipe we will iterate over commands
        void send_cmd(loop_cmd_t *cmd)
        {
            loop_cmd_locker.lock();
            this->_commands.push_back(cmd);

            // not making system call if we did already on one cycle
            // waiting until command will complete, then we would make it writable again
            if(!this->pipe_writable)
            {
                // just making pipe writable to trigger event in the loop
                this->make_writable_one_shot(&this->pipe_event_data, true);
                this->pipe_writable = true; // keeping track of writable pipe
            }

            loop_cmd_locker.unlock();
        }

        /// Registering Socket handle for this event loop based on environment Epoll or Kqueue
        /// This will make a system call for just adding given handle to Kernel file handles list for consuming events
        /// But at this point we are not registering any events, we will add them as a separate function calls
        void register_handle(loop_event_data_t *data) {
            if(data == nullptr)
                return;

#ifdef USE_KQUEUE
            struct kevent set_events[4];
            // in any case adding read and write events for this socket, then we will modify it
            // during reregister process
            EV_SET(&set_events[0], data->fd, EVFILT_READ, EV_ADD, 0, 0, (void *)data);
            EV_SET(&set_events[1], data->fd, EVFILT_WRITE, EV_ADD, 0, 0, (void *)data);
            EV_SET(&set_events[2], data->fd, EVFILT_READ, EV_DISABLE, 0, 0, (void *)data);
            EV_SET(&set_events[3], data->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, (void *)data);
            kevent(this->event_fd, set_events, 4, NULL, 0, NULL);

#elif defined(USE_EPOLL)
            struct epoll_event event;
            event.data.ptr = data;
            if(readable)
                event.events = EPOLLIN;
            else
                event.events = EPOLLIN | EPOLLOUT;
            epoll_ctl (this->event_fd, EPOLL_CTL_ADD, fd, &event);
#endif
        }


        /// Making socket readable for starting data handling
        /// This will disable "Write" events from this socket
        /// If "one_shot" is true then socket would be registered as "ONE_SHOT" based on OS Epoll or Kqueue
        void make_readable(loop_event_data_t *data) {
            this->reregister_handle(data, false, false);
        }

        /// Same as "make_readable" but registering event as "One Shot" which means it will trigger only once for this handle
        /// So if we want to get another event we need to reregister this handle again
        void make_readable_one_shot(loop_event_data_t *data) {
            this->reregister_handle(data, false, true);
        }

        /// Making socket writable for getting event when this socket is ready for writing buffer
        /// This wouldn't disable socket for read events by default, but it is possible to specify "write_only" boolean to true
        void make_writable(loop_event_data_t *data, bool write_only = false) {
            this->reregister_handle(data, false, false, write_only);
        }

        /// Same as "make_writable" but registering with one shot principle
        void make_writable_one_shot(loop_event_data_t *data, bool write_only = false) {
            this->reregister_handle(data, false, true, write_only);
        }

        /// Base function for reregistering event handle for this loop
        void reregister_handle(loop_event_data_t *data, bool readable, bool one_shot, bool write_only = false) {
            if(data == nullptr)
                return;

#ifdef USE_KQUEUE
            struct kevent set_events[2];

            EV_SET(&set_events[1], data->fd, EVFILT_WRITE, (readable ? EV_DISABLE : (one_shot ? EV_ENABLE | EV_ONESHOT : EV_ENABLE)), 0, 0, (void *)data);
            EV_SET(&set_events[0], data->fd, EVFILT_READ, (!readable && write_only ? EV_DISABLE : (one_shot ? EV_ENABLE | EV_ONESHOT : EV_ENABLE)), 0, 0, (void *)data);

            kevent(this->event_fd, set_events, 2, NULL, 0, NULL);

#elif defined(USE_EPOLL)
            struct epoll_event event;
            event.data.ptr = data;
            if(!writable)
                event.events = EPOLLIN;
            else
                event.events = EPOLLIN | EPOLLOUT;

            epoll_ctl (this->event_fd, EPOLL_CTL_MOD, fd, &event);
#endif
        }

        /// delete read, write events from given socket
        void clear_fd(int fd) {
#ifdef USE_KQUEUE
            struct kevent set_events[2];
            EV_SET(&set_events[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            EV_SET(&set_events[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(this->event_fd, set_events, 2, NULL, 0, NULL);
#elif defined(USE_EPOLL)
            struct epoll_event event;
            event.events = EPOLLIN | EPOLLOUT | EPOLLET;
            epoll_ctl (this->event_fd, EPOLL_CTL_DEL, fd, &event);
#endif
        }

        /// Closing given handle
        void close_fd(int fd) {
            this->clear_fd(fd);
            close(fd);
        }

        /// stopping event loop, but this actually not a thread safe call !!
        void stop_loop() {
            close(this->event_fd);
        }

        /// Start base loop running Epoll or Kqueue
        void run_loop() {

#ifdef USE_KQUEUE
            struct kevent get_events[BASE_LOOP_EVENTS_COUNT];

            int nev, i;

            while (true) {
                nev = kevent(this->event_fd, NULL, 0, get_events, BASE_LOOP_EVENTS_COUNT, NULL);
                if(nev < 0)
                {
                    if(errno == EFAULT || errno == ENOMEM || errno == EINTR || errno == EACCES)
                        break;
                    else
                        continue;
                }

                for(i = 0; i < nev; i++)
                {
                    if(get_events[i].flags & EV_ERROR) {
                        fprintf(stderr, "Event Loop error -> %s", strerror(get_events[i].data));
                        return;
                    }
                    else if(get_events[i].ident == this->pipe_chan) {
                        // triggering notification
                        loop_cmd_locker.lock();
                        if(!this->_commands.empty())
                        {
                            // copying commands here for disabling locking here
                            this->commands.insert(this->commands.end(), this->_commands.begin(), this->_commands.end());
                            // clearing original list for adding to it more
                            this->_commands.erase(this->_commands.begin(), this->_commands.end());
                            this->_commands.clear();
                        }

                        this->pipe_writable = false;
                        loop_cmd_locker.unlock();
                        this->notify();
                    }
                    else if(get_events[i].ident == this->listener) {
                        this->accept_conn((loop_event_data_t *)get_events[i].udata);
                    }
                    else if(get_events[i].filter == EVFILT_READ) {
                        this->readable((loop_event_data_t *)get_events[i].udata);
                    }
                    else if(get_events[i].filter == EVFILT_WRITE) {
                        this->writable((loop_event_data_t *)get_events[i].udata);
                    }
                }
            }
#elif defined(USE_EPOLL)

            struct epoll_event get_events[this->max_event_count];

            int nev, i;

            while (true) {
                nev = epoll_wait (this->event_fd, get_events, this->max_event_count, -1);
                if(nev < 0)
                {
                    if(errno == EBADF || errno == EINVAL)
                        break;
                    else
                        continue;
                }

                for(i = 0; i < nev; i++)
                {
                    // extracting event data
                    loop_event_data *ev_data = (loop_event_data*)get_events[i].data.ptr;

//                    if ((get_events[i].events & EPOLLERR) ||
//                      (get_events[i].events & EPOLLHUP))
//                    {
//                        fprintf (stderr, "epoll error, closing connection !\n");
//                        this->close_fd(ev_data->fd);
//                        continue;
//                    }
//                    else
                    if(ev_data->fd == this->pipe_chan) {
                        // triggering notification
                        loop_cmd_locker.lock();
                        if(!this->_commands.empty())
                        {
                            // copying commands here for disabling locking here
                            this->commands.insert(this->commands.end(), this->_commands.begin(), this->_commands.end());
                            // clearing original list for adding to it more
                            this->_commands.erase(this->_commands.begin());
                            this->_commands.clear();
                        }
                        this->reregister_event(this->pipe_chan, &this->pipe_event_data, false);
                        this->pipe_writable = false;
                        loop_cmd_locker.unlock();
                        this->notify();
                    }
                    else if(ev_data->fd == this->listener) {
                        this->acceptable(ev_data);
                    }
                    else if(get_events[i].events & EPOLLOUT) {
                        this->writable(ev_data);
                    }
                    else if((get_events[i].events & EPOLLIN) || (get_events[i].events & EPOLLPRI)) {
                        this->readable(ev_data);
                    }
                }
            }
#endif
        }
    };
}

#endif //BASELOOP_BASELOOP_H
