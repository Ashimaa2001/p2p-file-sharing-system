#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
using namespace std;

class FilesStructure {
public:
    string file_name;               
    string file_path;
    string sha;
    long long int total_chunks;
    long long int total_size;
    vector<string> chunks_I_have;
    long long int no_of_chunks_I_have; 
};

unordered_map<string, FilesStructure> filesIHave;

unordered_map<string, pair<string, string>> downloadStart;   
unordered_map<string, pair<string, string>> downloadPending;
vector<pair<string, string>> downloadComplete;


int connectToTracker(const string &tracker_info_file) {
    ifstream fin(tracker_info_file);
    string ip;
    int port;
    fin >> ip >> port;
    fin.close();

    int sock = 0;
    struct sockaddr_in serv_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    cout << "[Client] Connected to Tracker " << ip << ":" << port << endl;
    return sock;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./client <client_ip:port> tracker_info.txt" << endl;
        return -1;
    }

    string client_info = argv[1];
    string tracker_info_file = argv[2];

    int tracker_sock = connectToTracker(tracker_info_file);
    if (tracker_sock < 0) {
        cerr << "Failed to connect to tracker." << endl;
        return -1;
    }

    // Send dummy message
    string msg = "Hello from client!";
    send(tracker_sock, msg.c_str(), msg.size(), 0);

    char buffer[1024] = {0};
    int bytes_received = recv(tracker_sock, buffer, sizeof(buffer), 0);
    if (bytes_received > 0)
        cout << "[Client] Received: " << buffer << endl;

    close(tracker_sock);
    return 0;
}
