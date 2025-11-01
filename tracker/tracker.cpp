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
    unordered_map<string, fileInfo> files; 
};

unordered_map<string, Group> groups;

vector<string> tokenize(const string &s) {
    vector<string> tokens;
    stringstream ss(s);
    string temp;
    while (ss >> temp)
        tokens.push_back(temp);
    return tokens;
}

string handleCommand(const string &cmdline, const string &client_ip, const string &client_port) {
    vector<string> args = tokenize(cmdline);
    if (args.empty()) return "Invalid command";

    string cmd = args[0];

    string current_user_id = "";
    for (auto &u : users) {
        if (u.second.ip_address == client_ip && u.second.port == client_port) {
            current_user_id = u.first;
            break;
        }
    }

    unordered_set<string> login_required = {
        "create_group", "join_group", "leave_group",
        "list_groups", "list_files", "accept_request", "logout"
    };

    if (login_required.count(cmd)) {
        if (current_user_id.empty()) return "You must login first";
        if (!users[current_user_id].is_active) return "You must login first";
    }

    if (cmd == "create_user") {
        if (args.size() < 3) return "Usage: create_user <user_id> <password>";
        string user_id = args[1], password = args[2];
        if (users.count(user_id)) return "User already exists";
        User u;
        u.user_id = user_id;
        u.password = password;
        u.ip_address = client_ip;
        u.port = client_port;
        users[user_id] = u;
        return "User created successfully";
    }

    else if (cmd == "login") {
        if (args.size() < 3) return "Usage: login <user_id> <password>";
        string user_id = args[1], password = args[2];
        if (!users.count(user_id)) return "User not found";
        if (users[user_id].password != password) return "Invalid password";
        users[user_id].is_active = true;
        return "Login successful";
    }

    else if (cmd == "create_group") {
        if (args.size() < 2) return "Usage: create_group <group_id>";
        string group_id = args[1];
        if (groups.count(group_id)) return "Group already exists";
        Group g;
        g.group_id = group_id;
        g.owner_user_id = current_user_id;
        g.accepted_users[current_user_id] = 1;
        groups[group_id] = g;
        return "Group created successfully";
    }

    else if (cmd == "join_group") {
        if (args.size() < 2) return "Usage: join_group <group_id>";
        string group_id = args[1];
        if (!groups.count(group_id)) return "Group not found";
        string uid = current_user_id;
        if (groups[group_id].accepted_users.count(uid)) return "Already a member";
        groups[group_id].pending_users[uid] = 1;
        return "Join request sent to admin";
    }

    else if (cmd == "leave_group") {
        if (args.size() < 2) return "Usage: leave_group <group_id>";
        string group_id = args[1];
        string uid = current_user_id;
        if (!groups.count(group_id)) return "Group not found";
        if (!groups[group_id].accepted_users.count(uid)) return "You are not a member";
        groups[group_id].accepted_users.erase(uid);
        return "Left group successfully";
    }

    else if (cmd == "list_groups") {
        if (groups.empty()) return "No groups available";
        string res;
        for (auto &it : groups) res += it.first + "\n";
        return res;
    }

    else if (cmd == "list_files") {
        if (args.size() < 2) return "Usage: list_files <group_id>";
        string group_id = args[1];
        if (!groups.count(group_id)) return "Group not found";
        if (groups[group_id].files.empty()) return "No files in group";
        string res;
        for (auto &f : groups[group_id].files) res += f.first + "\n";
        return res;
    }

    else if (cmd == "list_requests") {
        if (args.size() < 2) return "Usage: list_requests <group_id>";
        string group_id = args[1];

        if (!groups.count(group_id)) return "Group not found";

        if (groups[group_id].owner_user_id != current_user_id)
            return "Only group owner can view pending requests";

        if (groups[group_id].pending_users.empty()) return "No pending requests";

        string res;
        for (auto &p : groups[group_id].pending_users) {
            res += p.first + "\n";  
        }

        return res;
    }

   else if (cmd == "accept_request") {
        if (args.size() < 3) return "Usage: accept_request <group_id> <username>";
        string group_id = args[1];
        string username = args[2];

        if (!groups.count(group_id)) return "Group not found";

        if (groups[group_id].owner_user_id != current_user_id)
            return "Only group owner can accept requests";

        if (groups[group_id].pending_users.count(username) == 0) {
            return "No pending request from this user";
        }

        groups[group_id].pending_users.erase(username);
        groups[group_id].accepted_users[username] = 1;

        return "User added to group successfully";
    }

    else if (cmd == "upload_file") {
        if (args.size() < 6) return "Usage: upload_file <file_path> <group_id> <file_name> <sha> <no_of_chunks> <file_size>";

        string file_path = args[1];
        string group_id = args[2];
        string file_name = args[3];
        string sha = args[4];
        long long int no_of_chunks = stoll(args[5]); 
        long long int file_size = stoll(args[6]);   

        string current_user_id = "";
        for (auto &u : users) {
            if (u.second.ip_address == client_ip && u.second.port == client_port) {
                current_user_id = u.first;
                break;
            }
        }

        if (!groups.count(group_id)) return "Group not found";

        if (current_user_id.empty()) return "You must login first";
        if (groups[group_id].accepted_users.count(current_user_id) == 0) {
            return "User must be a member of the group to upload a file.";
        }

        users[current_user_id].files[{group_id, file_name}] = sha;

        fileInfo f;
        f.file_name = file_name;
        f.sha = sha;
        f.no_of_chunks = no_of_chunks;
        f.size = file_size;
        groups[group_id].files[file_name] = f;

        return "File uploaded successfully.";
    }



    else if (cmd == "logout") {
        string user_id = current_user_id;
        if (!users[user_id].is_active) return "User is not logged in";
        users[user_id].is_active = false;
        return "Logout successful";
    }

    return "Unknown command";
}

void *handleClient(void *socket_desc) {
    int client_sock = *(int *)socket_desc;
    delete (int *)socket_desc;

    char buffer[1024] = {0};
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(addr);
    getpeername(client_sock, (struct sockaddr *)&addr, &addr_size);
    string client_ip = inet_ntoa(addr.sin_addr);
    string client_port = to_string(ntohs(addr.sin_port));

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) break;

        string command(buffer);
        cout << "[Tracker] Received: " << command << endl;
        string response = handleCommand(command, client_ip, client_port);
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
