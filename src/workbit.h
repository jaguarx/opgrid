#include <thread>
#include <string>
#include <future>
#include <cstring>
#include <utility>
#include <vector>
#include <list>
#include <map>

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
  struct bitstat_t {
    uint64_t send_retry;
    uint64_t send_count;
    uint64_t recv_count;
    size_t sent_bytes;
    size_t recv_bytes;
    void reset() {
      sent_bytes = recv_bytes = 0;
      send_retry = send_count = recv_count = 0;
    }
  };
  enum fd_state_t {
    STATE_INVALID    = 0,
    STATE_LISTEN     = 1,
    STATE_CONNECTING = 2,
    STATE_CONNECTED  = 3,
    STATE_CTRL       = 4,
  };
  typedef void(*write_cb_t)(void* parm, int fd, void* data);
  struct write_req_t {
    void* data;
    size_t off;
    size_t len;
    write_cb_t cb;
    void* parm;
    write_req_t():data(NULL), off(0), len(0), cb(NULL), parm(NULL){}
  };
  struct connection_t {
    fd_state_t state;
    int fd;
    int shutdown_flag;
    std::list<write_req_t> write_queue;
    void* extra;
    connection_t(int _fd, fd_state_t _state):fd(_fd), state(_state),
      shutdown_flag(0), extra(NULL){}
  };
  workbit():_stop(true), _epfd(-1){}

  bool start() {
    if (!_stop)
      return true;
    _stat.reset();
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
        connection_t* pconn = new connection_t(_readfd, STATE_CTRL);
        ev.data.ptr = pconn;
        if (epoll_ctl(_epfd, EPOLL_CTL_ADD, _readfd, &ev) < 0) {
          delete pconn;
          close(_epfd);
          close(_readfd);
          close(_writefd);
          return -1;
        }
        _conns[_readfd] = pconn;
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
    close(_readfd);
    close(_writefd);
    return true;
  }

  int prepare_listen(const char* host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    int flag = 1;
    if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag) ) < 0)
      return -1;

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
    connection_t* pconn = new connection_t(sockfd, STATE_LISTEN);
    ev.data.ptr = pconn; 
    if (epoll_ctl(_epfd, EPOLL_CTL_ADD, sockfd, &ev) < 0) {
      delete pconn;
      return -1;
    }
    _conns[sockfd] = pconn;
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
      ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
      connection_t *pconn = new connection_t(c_fd, STATE_CONNECTING);
      ev.data.ptr = pconn;
      if ( epoll_ctl(_epfd, EPOLL_CTL_ADD, c_fd, &ev) < 0) {
        delete pconn;
        close(c_fd);
        return -1;
      }
      return 1;
    }
  }

  int prepare_close(int fd) {
    if (_conns.find(fd) == _conns.end())
      return -1;

    connection_t* pconn = _conns[fd];
    write_req_t req;
    pconn->write_queue.push_back(req);
    return 0;
  }

  int request(int fd, size_t len, void* data, write_cb_t cb, void* parm) {
    if (_conns.find(fd) == _conns.end()) 
      return -1;

    connection_t* pconn = _conns[fd];
    int r = 0;
    if (pconn->write_queue.size()>0) {
      r = -1;
    } else {
      r = write(fd, data, len);
    }
    if (r < 0 || r != len) {
      if (r < len || errno == EAGAIN || errno == EWOULDBLOCK) {
        write_req_t req;
        req.data = data;
        req.parm = parm;
        req.cb = cb;
        req.len = len;
        req.off = (r > 0) ? r:0;
        pconn->write_queue.push_back(req);
        return len;
      }
    } else {
      _stat.sent_bytes += r;
      cb(parm, fd, data);
    }
    return r;
  }

  bitstat_t get_stat() const {
    return _stat;
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
        connection_t* pconn = (connection_t*)ev.data.ptr;
        switch (pconn->state) {
        case STATE_CONNECTED: _handle_connected(ev); break;
        case STATE_LISTEN: _handle_listen(ev); break;
        case STATE_CONNECTING: _handle_connecting(ev); break;
        case STATE_CTRL: _handle_ctrl(ev); break;
        }
      }
    }
    for (std::pair<int, connection_t*> item :_conns) {
      delete item.second;
      close(item.first);
    }
    _conns.clear();
    return 0;
  }

  int _cleanup_connection(connection_t* pconn, int flag) {
    pconn->shutdown_flag |= flag;
    if (pconn->shutdown_flag == (SHUT_RD | SHUT_WR)) {
      _conns.erase(pconn->fd);
      close(pconn->fd);
      delete pconn;
      return 0;
    }
    return pconn->fd;
  }
  int _handle_ctrl(epoll_event& ev) {
    uint8_t bv;
    read(_readfd, &bv, 1);
    _stop = true;
  }

  int _handle_listen(epoll_event& ev) {
    connection_t& lconn = *(connection_t*)ev.data.ptr;
    int listen_sock = lconn.fd;
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int conn_sock = accept(listen_sock, (struct sockaddr*)&client_addr,
                           &len);
    if (conn_sock < 0)
      return -1;
    if (_setnonblocking(conn_sock) < 0)
      return -1;
    ev.events = EPOLLIN | EPOLLET | EPOLLOUT | EPOLLRDHUP;
    connection_t* pconn = new connection_t(conn_sock, STATE_CONNECTED);
    ev.data.ptr = pconn;
    _conns[conn_sock] = pconn;
    if (epoll_ctl( _epfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1){
      delete pconn;
      return -1;
    }
    pconn->extra = static_cast<T*>(this)->connection_accepted(conn_sock, 
        (struct sockaddr*)&client_addr);
    return 0;
  }

  int _handle_connecting(epoll_event &ev) {
    int events = ev.events;
    connection_t* pconn = (connection_t*)ev.data.ptr;
    int c_fd = pconn->fd;
    if (events & EPOLLERR) {
      int err = 0;
      socklen_t len = sizeof(err);
      int ret = getsockopt(c_fd, SOL_SOCKET, SO_ERROR, &err, &len);
      epoll_ctl(_epfd, EPOLL_CTL_DEL, c_fd, &ev);
      close(c_fd);
      return 0;
    }
    if (events & EPOLLOUT) {
      pconn->state = STATE_CONNECTED;
      _conns[c_fd] = pconn;
      pconn->extra = static_cast<T*>(this)->connection_made(c_fd);
    }
    return 0;
  }
        
  int _handle_connected(epoll_event &ev) {
    connection_t* pconn = (connection_t*)ev.data.ptr;
    int events = ev.events;
    int c_fd = pconn->fd;
    int flag = 0;
    if (events & EPOLLERR) {
      int err = 0;
      socklen_t len = sizeof(err);
      int ret = getsockopt(c_fd, SOL_SOCKET, SO_ERROR, &err, &len);
      epoll_ctl( _epfd, EPOLL_CTL_DEL, c_fd, &ev);
      close(c_fd);
      static_cast<T*>(this)->connection_closed(*pconn);
      _conns.erase(c_fd);
      delete pconn;
      return 0;
    }
    if (events & EPOLLOUT) {
      static_cast<T*>(this)->writable(*pconn);
    }
    if (events & EPOLLIN) {
      static_cast<T*>(this)->readable(*pconn);
    }
    if (events & EPOLLRDHUP) {
      static_cast<T*>(this)->connection_closed(*pconn);
    }
    if (flag != 0) {
      _cleanup_connection(pconn, flag);
    }
  }
  
  std::future<int> _future_stop;
  bool _stop;
  int _epfd;
  int _readfd;
  int _writefd;
  bitstat_t _stat;
  std::map<int, connection_t*> _conns;
};

