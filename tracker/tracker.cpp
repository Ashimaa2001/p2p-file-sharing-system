#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
using namespace std;

class fileInfo {
public:
    string file_name;           
    long long int no_of_chunks;
    long long int size;
    string sha;
};

unordered_map<string, fileInfo> files;

struct PairHash {
    template <class T1, class T2>
    size_t operator()(const pair<T1, T2> &p) const {
        auto h1 = hash<T1>{}(p.first);
        auto h2 = hash<T2>{}(p.second);
        return h1 ^ h2;
    }
};

class User {
public:
    string user_id;            
    string password;
    string ip_address;
    string port;
    bool is_active = false;

    unordered_map<pair<string, string>, string, PairHash> files;
};

unordered_map<string, User> users;

class Group {
public:
    string group_id;
    string owner_user_id;
    unordered_map<string, int> pending_users;
    unordered_map<string, int> accepted_users; 
};

unordered_map<string, Group> groups;

void *handleClient(void *socket_desc) {
    int client_sock = *(int *)socket_desc;
    delete (int *)socket_desc;

    char buffer[1024] = {0};

    int bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
    if (bytes_received > 0) {
        cout << "[Tracker] Received: " << buffer << endl;
        string response = "ACK from Tracker";
        send(client_sock, response.c_str(), response.size(), 0);
    }

    close(client_sock);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./tracker tracker_info.txt tracker_no" << endl;
        return -1;
    }

    string tracker_info_file = argv[1];
    int tracker_no = stoi(argv[2]);

    ifstream fin(tracker_info_file);
    string ip;
    int port;
    fin >> ip >> port;
    fin.close();

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(ip.c_str());
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    cout << "[Tracker] Listening on " << ip << ":" << port << endl;

    while (true) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        pthread_t thread_id;
        int *sock_ptr = new int(new_socket);
        pthread_create(&thread_id, NULL, handleClient, (void *)sock_ptr);
        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}
