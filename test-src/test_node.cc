#include <iostream>       // std::cout
#include <sstream>

#include "opnode.h"

using namespace std;

int main (int argc, char** argv)
{
  opnode wb;
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
    }
    getline(cin, cmd);
  }
  wb.stop();
  return 0;
}

