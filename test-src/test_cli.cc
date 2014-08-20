#include <iostream>       // std::cout
#include <sstream>

#include "workbit.h"

using namespace std;

class testpeer : public workbit<testpeer> {
public:
  void* connection_accepted(int fd, struct sockaddr* addr) {
    cout << " accepted : " << fd << endl;
    return NULL;
  }
  void* connection_made(int fd) {
    cout << " connected: " << fd << endl;
    return NULL;
  }
  void connection_closed(const connection_t& conn) {
    cout << " lost     : " << conn.fd << endl;
  }
  void* allocate_buf(int fd, size_t& len) {
    len = 65536;
    return malloc(len);
  }
  void release_buf(int fd, void* buf) {
    free(buf);
  }
  int data(const connection_t& conn, size_t len, void* data) {
    //cout << " data     : " << fd << ", len: " << len << endl;
    void* buf = NULL;
    if (len > 0) { 
      buf = malloc(len);
      memcpy(buf, data, len);
      request(conn.fd, len, buf, write_done, NULL);
    } else {
      prepare_close(conn.fd);
    }
  }
  static void write_done(void* parm, int fd, void* data) {
    if (data != NULL)
      free(data);
  }
};

int main (int argc, char** argv)
{
  testpeer wb;
  wb.start();
  if (argc == 3) {
    cout << "prepare_listen: " << wb.prepare_listen(argv[1], atoi(argv[2]))
         << endl;
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
      cout<< "prepase_connect: "<< wb.prepare_connect(host.c_str(), port)
          << endl;
    } else if (cmd.find("stat") == 0) {
      testpeer::bitstat_t stat = wb.get_stat();
      cout<<" send_retry: " << stat.send_retry
          <<", sent: " << stat.sent_bytes << "/" << stat.send_count 
          <<", recv: " << stat.recv_bytes << "/" << stat.recv_count 
          << "\n";
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

