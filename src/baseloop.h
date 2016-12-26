//
// Created by Tigran on 11/23/16.
//

#ifndef BASELOOP_BASELOOP_H
#define BASELOOP_BASELOOP_H

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

namespace BaseLoop {

    struct loop_cmd {
        int cmd;
        void *data;
    };

    struct loop_event_data {
        // socket handler
        int fd;

        // public data for outside usage
        void *data;
    };

    class BaseLoop {
    public:
        int make_socket_non_blocking (int sfd) {
            int flags, s;

            flags = fcntl (sfd, F_GETFL, 0);
            if (flags == -1)
            {
                return -1;
            }

            flags |= O_NONBLOCK;
            s = fcntl (sfd, F_SETFL, flags);
            if (s == -1)
            {
                return -1;
            }

            return 0;
        }
    protected:
        // just raw number to make sure if we don't have server listener
        // making sure that event loop would't compare it as a socket number
        int listener = -57;
        int max_event_count = 5000;
        std::list<loop_cmd> commands;

        // callback for accepting connection here
        virtual void acceptable(loop_event_data *data) {};
        // callback for reading data from socket here
        virtual void readable(loop_event_data *data) {};
        // callback for writing data to socket
        virtual void writable(loop_event_data *data) {};
        // get notification from pipe and read commands
        virtual void notify() {};

    private:
        int pipe_chan;
        int event_fd;

        // locking command for thread safety
        std::mutex loop_cmd_locker;

        bool pipe_writable = false;

        std::list<loop_cmd> _commands;

        loop_event_data pipe_event_data;
    public:

        void init_loop()
        {
            int pipe_chans[2];
            int r = pipe(pipe_chans);
            this->pipe_chan = pipe_chans[1];
            this->make_socket_non_blocking(this->pipe_chan);

#ifdef USE_KQUEUE
            this->event_fd = kqueue();
#elif defined(USE_EPOLL)
            this->event_fd = epoll_create(1);
#endif
            this->pipe_event_data.data = NULL;
            this->pipe_event_data.fd = this->pipe_chan;
            this->register_event(this->pipe_chan, &this->pipe_event_data, false);
        }

        void send_cmd(loop_cmd cmd)
        {
            loop_cmd_locker.lock();
            this->_commands.push_back(std::move(cmd));

            // not making system call if we did already on one cycle
            // waiting until command will complete, then we would make it writable again
            if(!this->pipe_writable)
            {
                // just making pipe writable to trigger event in the loop
                this->reregister_event(this->pipe_chan, &this->pipe_event_data, true);
                this->pipe_writable = true; // keeping track of writable pipe
            }

            loop_cmd_locker.unlock();
        }

        void register_event(int fd, loop_event_data *data, bool readable) {
#ifdef USE_KQUEUE
            struct kevent set_events[4];
            // in any case adding read and write events for this socket, then we will modify it
            // during reregister process
            EV_SET(&set_events[0], fd, EVFILT_READ, EV_ADD, 0, 0, (void *)data);
            EV_SET(&set_events[1], fd, EVFILT_WRITE, EV_ADD, 0, 0, (void *)data);
            EV_SET(&set_events[2], fd, EVFILT_READ, EV_DISABLE, 0, 0, (void *)data);
            EV_SET(&set_events[3], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, (void *)data);
            kevent(this->event_fd, set_events, 4, NULL, 0, NULL);
            this->reregister_event(fd, data, readable);

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

        void reregister_event(int fd, loop_event_data *data, bool writable) {
#ifdef USE_KQUEUE
            struct kevent set_events[2];
            // keeping data pointer here
            // in any case we need to enable read
            EV_SET(&set_events[0], fd, EVFILT_READ, EV_ENABLE, 0, 0, (void *)data);
            if(!writable)
            {
                // if we updating socket to readable then we need to disable writable socket
                EV_SET(&set_events[1], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, (void *)data);
            }
            else
            {
                EV_SET(&set_events[1], fd, EVFILT_WRITE, EV_ENABLE, 0, 0, (void *)data);
            }

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

        // delete read, write events from given socket
        void clear_fd(int fd) {
#ifdef USE_KQUEUE
            struct kevent set_events[2];
            // in any case we need to enable read
            EV_SET(&set_events[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            EV_SET(&set_events[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(this->event_fd, set_events, 2, NULL, 0, NULL);
#elif defined(USE_EPOLL)
            struct epoll_event event;
            event.events = EPOLLIN | EPOLLOUT | EPOLLET;
            epoll_ctl (this->event_fd, EPOLL_CTL_DEL, fd, &event);
#endif
        }

        void close_fd(int fd) {
            close(fd);
        }

        void stop_loop() {
            close(this->event_fd);
        }

        void run_loop() {

#ifdef USE_KQUEUE
            struct kevent get_events[this->max_event_count];

            int nev, i;

            while (true) {
                nev = kevent(this->event_fd, NULL, 0, get_events, this->max_event_count, NULL);
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

                        this->reregister_event(this->pipe_chan, NULL, false);
                        this->pipe_writable = false;
                        loop_cmd_locker.unlock();
                        this->notify();
                    }
                    else if(get_events[i].ident == this->listener) {
                        this->acceptable((loop_event_data *)get_events[i].udata);
                    }
                    else if(get_events[i].filter == EVFILT_READ) {
                        this->readable((loop_event_data *)get_events[i].udata);
                    }
                    else if(get_events[i].filter == EVFILT_WRITE) {
                        this->writable((loop_event_data *)get_events[i].udata);
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
