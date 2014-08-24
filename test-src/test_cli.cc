#include <iostream>       // std::cout
#include <sstream>
#include <set>

#include "workbit.h"

using namespace std;

class testpeer : public workbit<testpeer> {
public:
  struct write_state_t {
    void* buf;
    size_t len;
    list<pair<int,int> > fds; //fd,offset
  };
  struct mcast_state_t {
    list<write_state_t> writeq;
  };

  void* connection_accepted(int fd, struct sockaddr* addr) {
    cout << " accepted : " << fd << endl;
    _fds.insert(fd);
    return new mcast_state_t();
  }
  void* connection_made(int fd) {
    cout << " connected: " << fd << endl;
    _fds.insert(fd);
    return new mcast_state_t();
  }
  void connection_closed(const connection_t& conn) {
    cout << " lost     : " << conn.fd << endl;
    _fds.erase(conn.fd);
    delete (mcast_state_t*)(conn.extra);
  }
  int readable(const connection_t& conn) {
    _mcast_data(conn.fd, *(mcast_state_t*)conn.extra);
  }
  int writable(const connection_t& conn) {
    _mcast_data(conn.fd, *(mcast_state_t*)conn.extra);
  }

  int dump(int v) {
    _dump = v;
    return _dump;
  }

  testpeer(): _dump(1) {}
private:
  int _dump;
  set<int> _fds;
  bool _should_resent(int r) {
    return (r < 0) && ((errno == EAGAIN || errno == EWOULDBLOCK));
  }

  void _mcast_data(int srcfd, mcast_state_t& state) {
    list<write_state_t>& q = state.writeq;
    bool cont_loop = false;
    while ( cont_loop && q.size() > 0) {
      write_state_t& w = q.front();
      list<pair<int, int> > l;
      l.swap(w.fds);
      for(pair<int, int> i: l) {
        int r = write(i.first, (uint8_t*)w.buf + i.second, w.len - i.second);
        if (_should_resent(r))
          w.fds.push_back(i);
      }
      if (w.fds.size() > 0)
        cont_loop = false;
      else {
        free(w.buf);
        q.pop_front();
      }
    }
    if (q.size() > 0)
      return;

    size_t len = 4096;
    void * buf = malloc(len);
    if (buf == NULL) {
      cout << "malloc failed \n";
      return;
    }
    int r = read(srcfd, buf, len);
    while (r > 0) {
      int wl = r;
      if (_dump > 0) {
        cout.write((char*)buf, r);
        cout << "\n";
        r = read(srcfd, buf, len);
        continue;
      }
      write_state_t ws;
      ws.buf = buf;
      ws.len = wl;
      for(int fd: _fds) {
        if (fd == srcfd)
          continue;
        r = write(fd, buf, wl);
        if (_should_resent(r)) {
          ws.fds.push_back(pair<int,int>(fd, 0));
        } else if (r == 0) {
        }
      }
      if (ws.fds.size()> 0) {
        state.writeq.push_back(ws);
      }
      r = read(srcfd, buf, len);
    }
    if (r == 0) {
      _fds.erase(srcfd);
    }
    if (state.writeq.size() == 0) {
      free(buf);
    }
  }
};

int main (int argc, char** argv)
{
  testpeer wb;
  wb.start();
  if (argc == 2) {
    cout<<"prepare_listen : " << wb.prepare_listen("0.0.0.0", atoi(argv[1]))
        <<endl;
  } else if (argc == 3) {
    cout<<"prepare_connect: " << wb.prepare_connect(argv[1], atoi(argv[2]))
        <<endl;
  }
  string cmd;
  getline(cin, cmd);
  while (cmd.find("quit") == string::npos) {
    if (cmd.find("listen") == 0) {
      stringstream ss(cmd.substr(7));
      string host;
      int port;
      ss >> host >> port;
      cout<<"prepare_listen: "<<wb.prepare_listen(host.c_str(), port)
          <<endl;
    } else if(cmd.find("connect") == 0) {
      stringstream ss(cmd.substr(8));
      string host;
      int port;
      ss >> host >> port;
      cout<< "prepare_connect: "<< wb.prepare_connect(host.c_str(), port)
          << endl;
    } else if (cmd.find("stat") == 0) {
      testpeer::bitstat_t stat = wb.get_stat();
      cout<<" send_retry: " << stat.send_retry
          <<", sent: " << stat.sent_bytes << "/" << stat.send_count 
          <<", recv: " << stat.recv_bytes << "/" << stat.recv_count 
          << "\n";
    } else if (cmd.find("dump") == 0) {
      int v = 0;
      stringstream ss(cmd.substr(5));
      ss >> v;
      cout<<" dump: " << wb.dump(v) << "\n";
    }
    getline(cin, cmd);
  }
  wb.stop();
  testpeer::bitstat_t stat = wb.get_stat();
  cout<<" send_retry: " << stat.send_retry
      <<", sent_count: " << stat.sent_bytes
      <<", recv_count: " << stat.recv_bytes
      << "\n";
  return 0;
}

