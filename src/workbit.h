#include <thread>
#include <string>
#include <future>
#include <cstring>
#include <utility>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <error.h>
#include <netinet/in.h>
#include <arpa/inet.h>

template<class T> class workbit {
public:
  enum fd_state_t {
    STATE_LISTEN = 0x100000000L,
    STATE_CONNECTING = 0x200000000L,
    STATE_CONNECTED  = 0x300000000L,
    STATE_CTRL       = 0x400000000L,
  };
  workbit():_stop(true), _epfd(-1){}

  bool start() {
    if (!_stop)
      return true;
    _epfd = epoll_create(1); //the input is not used
    if (_epfd > 0) {
      _stop = false;
      std::packaged_task<int(workbit*)> task(_loop);
      _future_stop = task.get_future();
      int pair[2] = {0};
      if ( socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) < 0) {
        close(_epfd);
        return false;
      } else {
        _readfd = pair[0];
        _writefd = pair[1];
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u64 = STATE_CTRL | (0xffffffff & _readfd);
        if (epoll_ctl(_epfd, EPOLL_CTL_ADD, _readfd, &ev) < 0) {
          close(_epfd);
          close(_readfd);
          close(_writefd);
          return -1;
        }
      }
      std::thread(std::move(task), this).detach();
    }
    return false;
  }

  bool stop() {
    _stop = true;
    write(_writefd, " ", 1);
    _future_stop.get();
    close(_epfd);
    return true;
  }

  int prepare_listen(const char* host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
      return -1;

    listen(sockfd, 5);
 
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = STATE_LISTEN | (0xffffffff & sockfd);
    if (epoll_ctl(_epfd, EPOLL_CTL_ADD, sockfd, &ev) < 0)
      return -1;
    return 0;
  }

  int prepare_connect(const char* host, int port) {
    int c_fd, flags, ret;
    struct sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(port);
    s_addr.sin_addr.s_addr = inet_addr(host);

    if ((c_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
      return -1;
    if (_setnonblocking(c_fd) < 0)
      return -1;
    ret = connect(c_fd, (struct sockaddr*)&s_addr, sizeof(s_addr));
    if (ret >= 0)
      return 0;
    else if ((ret < 0) && (errno != EINPROGRESS)) {
      return -1;
    } else {
      struct epoll_event ev;
      ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
      ev.data.u64 = STATE_CONNECTING | (0xffffffff & c_fd);
      if ( epoll_ctl(_epfd, EPOLL_CTL_ADD, c_fd, &ev) < 0) {
        close(c_fd);
        return -1;
      }
      return 1;
    }
  }

private:
  static int _setnonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag < 0) return -1;
    return fcntl(fd, F_SETFL, flag | O_NONBLOCK);
  }
  static int _loop(workbit* wb) {
    return wb->__loop();
  }
  int __loop() {
    struct epoll_event evs[1000];
    int status, err;
    socklen_t len = sizeof(err);
    while ( ! _stop ) {
      int n = epoll_wait( _epfd, evs, 20, -1);
      for (int i = 0; i<n; i++) {
        epoll_event& ev = evs[i];
        uint64_t state = (ev.data.u64) & 0xf00000000L;
        switch (state) {
        case STATE_CONNECTED: _handle_connected(ev); break;
        case STATE_LISTEN: _handle_listen(ev); break;
        case STATE_CONNECTING: _handle_connecting(ev); break;
        case STATE_CTRL: _handle_ctrl(ev); break;
        }
      }
    }
    return 0;
  }

  int _handle_ctrl(epoll_event& ev) {
    _stop = true;
  }

  int _handle_listen(epoll_event& ev) {
    int listen_sock = (ev.data.u64 & 0xffffffff);
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int conn_sock = accept(listen_sock, (struct sockaddr*)&client_addr,
                           &len);
    if (conn_sock < 0) return -1;
    if (_setnonblocking(conn_sock) < 0) return -1;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = STATE_CONNECTED | (0xffffffff & conn_sock);
    if (epoll_ctl( _epfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) return -1;
    static_cast<T*>(this)->connection_accepted(conn_sock, (struct sockaddr*)&client_addr);
    return 0;
  }

  int _handle_connecting(epoll_event &ev) {
    int events = ev.events;
    int c_fd = (ev.data.u64 & 0xffffffff);
    if (events & EPOLLERR) {
      int err = 0;
      socklen_t len = sizeof(err);
      int ret = getsockopt(c_fd, SOL_SOCKET, SO_ERROR, &err, &len);
      epoll_ctl(_epfd, EPOLL_CTL_DEL, c_fd, &ev);
      close(c_fd);
      return 0;
    }
    if (events & EPOLLOUT) {
      ev.data.u64 = STATE_CONNECTED | (0xffffffff & c_fd);
      ev.events = (EPOLLIN | EPOLLOUT | EPOLLET);
      if ( epoll_ctl( _epfd, EPOLL_CTL_MOD, c_fd, &ev) < 0) {
        close(c_fd);
      } else {
        static_cast<T*>(this)->connection_made(c_fd);
      }
    }
    return 0;
  }
        
  int _handle_connected(epoll_event &ev) {
    int events = ev.events;
    int c_fd = (ev.data.u64 & 0xffffffff);
    if (events & EPOLLERR) {
      int err = 0;
      socklen_t len = sizeof(err);
      int ret = getsockopt(c_fd, SOL_SOCKET, SO_ERROR, &err, &len);
      epoll_ctl( _epfd, EPOLL_CTL_DEL, c_fd, &ev);
      close(c_fd);
      static_cast<T*>(this)->connection_lost(c_fd);
      return 0;
    }
    if (events & EPOLLIN) {
      size_t len = 0;
      void* buf = static_cast<T*>(this)->allocate_buf(c_fd, len);
      int cnt = 0;
      do {
        cnt = read(c_fd, buf, len);
        if (cnt > 0)
          static_cast<T*>(this)->data(c_fd, cnt, buf);
      } while (cnt > 0);
      static_cast<T*>(this)->release_buf(c_fd, buf);
      if (cnt == 0) {
        epoll_ctl( _epfd, EPOLL_CTL_DEL, c_fd, &ev);
        close(c_fd);
        static_cast<T*>(this)->connection_lost(c_fd);
      } else {
        ev.events = (EPOLLIN | EPOLLOUT | EPOLLET);
        epoll_ctl(_epfd, EPOLL_CTL_MOD, c_fd, &ev);
      }
    }
  }

  std::future<int> _future_stop;
  bool _stop;
  int _epfd;
  int _readfd;
  int _writefd;
};


