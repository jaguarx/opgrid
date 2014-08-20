#include <map>
#include <string>
#include <algorithm>
#include <iostream>

#include "workbit.h"

class opnode : public workbit<opnode> {
public:
  void* connection_accepted(int fd, struct sockaddr* addr) {
    std::cout << " accepted: " << fd << "\n";
    return NULL;
  }

  void* connection_made(int fd) {
    std::cout << " connected: " << fd << "\n";
    return NULL;
  }

  void connection_closed(const connection_t& conn) {
    std::cout << " lost     : " << conn.fd << "\n";
  }

  void* allocate_buf(int fd, size_t& len) {
    if (len == 0)
      len = 65535;
    return malloc(len);
  }

  void release_buf(int fd, void* buf) {
    free(buf);
  }

  int data(const connection_t& conn, size_t len, void* data) {
    nodeconnection_t& nc = *(nodeconnection_t*)conn.extra;
    size_t bytes_read = _read_frame(data, len, nc);
    while (nc.bytes_read == sizeof(nc.frame_length)+nc.frame_length) {
      std::cout << "packet length:"<< nc.frame_length << "\n";
      nc.reset();
      data = (uint8_t*)data + bytes_read;
      len -= bytes_read;
      bytes_read = _read_frame(data, len, nc);
    }
    return 0;
  }

private:
  struct nodeconnection_t {
    size_t bytes_read;
    size_t frame_length;
    std::string packet;
    void reset(){
      bytes_read = 0; frame_length = 0;
      packet.clear();
    }
  };
  int _read_frame(void* buf, size_t len, nodeconnection_t& conn) {
    size_t bytes_read = 0;
    if (conn.bytes_read < sizeof(conn.frame_length)) {
      size_t to_read = sizeof(conn.frame_length) - conn.bytes_read;
      to_read = std::min(to_read, len);
      memcpy((uint8_t*)&conn.frame_length + conn.bytes_read, buf, to_read);
      conn.bytes_read += to_read;
      buf = (uint8_t*)buf + to_read;
      len -= to_read;
      bytes_read += to_read;
      if (conn.bytes_read < sizeof(conn.frame_length))
        return to_read;
      conn.frame_length = ntohl(conn.frame_length);
    }
    size_t to_read = sizeof(conn.frame_length) + conn.frame_length 
                     - conn.bytes_read;
    to_read = std::min(to_read, len);
    conn.packet.append((char*)buf, to_read);
    conn.bytes_read += to_read;
    bytes_read += to_read;
    return bytes_read;
  }
};

