#include <iostream>       // std::cout
#include <sstream>

#include "workbit.h"

using namespace std;

class testpeer : public workbit<testpeer> {
public:
  void connection_accepted(int fd, struct sockaddr* addr) {
    cout << " accepted : " << fd << endl;
  }
  void connection_made(int fd) {
    cout << " connected: " << fd << endl;
  }
  void connection_lost(int fd) {
    cout << " lost     : " << fd << endl;
  }
  void* allocate_buf(int fd, size_t& len) {
    len = 65536;
    return malloc(len);
  }
  void release_buf(int fd, void* buf) {
    free(buf);
  }
  int data(int fd, size_t len, void* data) {
    cout << " data     : " << fd << ", len: " << len << endl;
  }
};

int main (int argc, char** argv)
{
  testpeer wb;
  wb.start();
  string cmd;
  getline(cin, cmd);
  cout << cmd << endl;
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
    }
    getline(cin, cmd);
    cout << cmd << endl;
  }
  wb.stop();
  return 0;
}

