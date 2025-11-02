#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

#include "thread_pool.h"
#include "data_structures.h"
#include "file_downloader.h"
#include "file_hasher.h"

using namespace std;

unordered_map<string, FilesStructure> filesIHave;
unordered_map<string, pair<string, string>> downloadStart;
unordered_map<string, pair<string, string>> downloadPending;
vector<pair<string, string>> downloadComplete;

mutex downloadMutex;
void *peerServer(void *arg) {
    string client_info = *(string *)arg;
    delete (string *)arg;

    string ip = client_info.substr(0, client_info.find(":"));
    int port = stoi(client_info.substr(client_info.find(":") + 1));

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        pthread_exit(NULL);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        pthread_exit(NULL);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(ip.c_str());
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        pthread_exit(NULL);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        pthread_exit(NULL);
    }

    cout << "[Peer Server] Listening on " << ip << ":" << port << endl;

    while (true) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        char buffer[1024] = {0};
        int bytes_recv = recv(new_socket, buffer, sizeof(buffer), 0);
        if (bytes_recv <= 0) {
            cout << "[Peer Server] Error receiving data" << endl;
            close(new_socket);
            continue;
        }

        string request(buffer, bytes_recv);
        cout << "[Peer Server] Received raw request (" << bytes_recv << " bytes): '" << request << "'" << endl;

        cout << "[Peer Server] Request characters:" << endl;
        for (int i = 0; i < bytes_recv; i++) {
            cout << "  " << i << ": '" << buffer[i] << "' (ASCII: " << (int)(unsigned char)buffer[i] << ")" << endl;
        }

        vector<string> args = tokenize(request);
        cout << "[Peer Server] Parsed " << args.size() << " arguments:" << endl;
        for (size_t i = 0; i < args.size(); i++) {
            cout << "  " << i << ": '" << args[i] << "'" << endl;
        }

        if (args.empty()) {
            cout << "[Peer Server] No valid arguments found in request" << endl;
            string error = "Unknown command";
            send(new_socket, error.c_str(), error.length(), 0);
            close(new_socket);
            continue;
        }

        cout << "[Peer Server] Command: '" << args[0] << "'" << endl;

        if (args[0] == "get_chunks") {
            cout << "[Peer Server] Handling get_chunks command" << endl;
            // Request format: get_chunks <group_id> <file_name>
            if (args.size() < 3) {
                cout << "[Peer Server] Invalid request format - expected 3 args, got " << args.size() << endl;
                string error = "Invalid request format";
                send(new_socket, error.c_str(), error.length(), 0);
                close(new_socket);
                continue;
            }
            
            for (size_t i = 0; i < args.size(); i++) {
                cout << "[Peer Server] Arg " << i << ": '" << args[i] << "'" << endl;
            }

            string group_id = args[1];
            string file_name = args[2];

            cout << "[Peer Server] Looking for file " << file_name << " in files I have" << endl;

            if (filesIHave.find(file_name) == filesIHave.end()) {
                cout << "[Peer Server] File " << file_name << " not found in filesIHave" << endl;
                cout << "[Peer Server] Available files: ";
                for (const auto& pair : filesIHave) {
                    cout << pair.first << " ";
                }
                cout << endl;
                
                string error = "File not found";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            FilesStructure &file = filesIHave[file_name];
            stringstream ss;
            ss << file.total_chunks << "#";
            
            vector<int> chunk_numbers;
            for (size_t i = 0; i < file.chunks_I_have.size(); i++) {
                chunk_numbers.push_back(i);
            }
            
            for (size_t i = 0; i < chunk_numbers.size(); i++) {
                if (i > 0) ss << ",";
                ss << chunk_numbers[i];
            }

            string response = ss.str();
            cout << "[Peer Server] Sending response for file " << file_name 
                 << ": '" << response << "'" << endl;
            send(new_socket, response.c_str(), response.size(), 0);
        }
        else if (args[0] == "get_chunk") {
            // Request format: get_chunk <group_id> <file_name> <chunk_no>
            if (args.size() < 4) {
                string error = "Invalid request format";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            string group_id = args[1];
            string file_name = args[2];
            int chunk_no = stoi(args[3]);

            if (!filesIHave.count(file_name)) {
                string error = "File not found";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            FilesStructure &file = filesIHave[file_name];
            
            cout << "[Peer Server] Request for chunk " << chunk_no << " of file " << file_name << endl;
            
            if (chunk_no < 0 || chunk_no >= file.total_chunks) {
                string error = "Invalid chunk number";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            int fd = open(file.file_path.c_str(), O_RDONLY);
            if (fd == -1) {
                string error = "Failed to open file";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            off_t offset = chunk_no * 1024LL;
            if (lseek(fd, offset, SEEK_SET) == -1) {
                string error = "Failed to seek in file";
                send(new_socket, error.c_str(), error.size(), 0);
                close(fd);
                close(new_socket);
                continue;
            }

            char chunk_data[1024];
            ssize_t bytes_read = read(fd, chunk_data, 1024);
            close(fd);

            if (bytes_read <= 0) {
                string error = "Failed to read chunk";
                send(new_socket, error.c_str(), error.size(), 0);
                close(new_socket);
                continue;
            }

            cout << "[Peer Server] Sending chunk " << chunk_no << " (" << bytes_read << " bytes)" << endl;
            
            ssize_t sent = send(new_socket, chunk_data, bytes_read, 0);
            if (sent != bytes_read) {
                cout << "[Peer Server] Error: Failed to send complete chunk data" << endl;
            } else {
                cout << "[Peer Server] Successfully sent " << sent << " bytes" << endl;
            }
        } else {
            cout << "[Peer Server] Unknown command: " << args[0] << endl;
            string error = "Unknown command";
            send(new_socket, error.c_str(), error.length(), 0);
        }

        close(new_socket);
    }
    close(server_fd);
    pthread_exit(NULL);
}

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

string g_client_ip;
string g_client_port;

void handleTrackerCommands(int tracker_sock) {
    cout << "\nAvailable Commands:\n";
    cout << "1. create_user <user_id> <password>\n";
    cout << "2. login <user_id> <password>\n";
    cout << "3. create_group <group_id>\n";
    cout << "4. join_group <group_id>\n";
    cout << "5. leave_group <group_id>\n";
    cout << "6. list_groups\n";
    cout << "7. list_files <group_id>\n";
    cout << "8. list_requests <group_id>\n";
    cout << "9. accept_request <group_id> <user_id>\n";
    cout << "10. upload_file <file_path> <group_id>\n";
    cout << "11. download_file <group_id> <file_name>\n";
    cout << "12. logout <user_id>\n";
    cout << "Type 'exit' to quit.\n\n";

    string cmd;
    char buffer[4096];

    while (true) {
        cout << "> ";
        getline(cin, cmd);

        if (cmd == "exit")
            break;

        if (cmd.find("download_file") == 0) {
            stringstream ss(cmd);
            string command, group_id, file_name, dest_path;
            ss >> command >> group_id >> file_name >> dest_path;

            if (group_id.empty() || file_name.empty()) {
                cout << "Usage: download_file <group_id> <file_name> [destination_path]" << endl;
                continue;
            }
            
            if (dest_path.empty()) {
                dest_path = "downloads";
                mkdir(dest_path.c_str(), 0755);
            }

            // Create a thread pool for downloads
            ThreadPool download_pool(10);  
            
            try {
                cout << "Creating FileDownloader instance..." << endl;
                FileDownloader downloader(file_name, dest_path, group_id, download_pool, tracker_sock);
                cout << "Starting download..." << endl;
                downloader.startDownload();
                cout << "Download completed." << endl;
            } catch (const exception& e) {
                cout << "Error during download: " << e.what() << endl;
            }
            
            continue;
        }
        
        if (cmd.find("upload_file") == 0) {
            stringstream ss(cmd);
            string command, file_path, group_id;
            ss >> command >> file_path >> group_id;

            if (file_path.empty() || group_id.empty()) {
                cout << "Usage: upload_file <file_path> <group_id>" << endl;
                continue;
            }

            string file_name = file_path.substr(file_path.find_last_of("/") + 1);
            long long int file_size = 0;
            long long int no_of_chunks = 0;
            string final_sha = "";

            ifstream file(file_path, ios::binary | ios::ate);
            if (file.is_open()) {
                file_size = file.tellg();
                no_of_chunks = (file_size / 1024) + (file_size % 1024 != 0);  
                 
                FileHasher hasher(file_path);

                hasher.calculateHashValues();

                vector<string> chunk_shas = hasher.getChunkHashes();
                string complete_file_sha = hasher.getCompleteFileHash();

                string concatenated_sha = complete_file_sha;
                for (const string& chunk_sha : chunk_shas) {
                    concatenated_sha += "#" + chunk_sha;
                }

                final_sha = concatenated_sha;

                FilesStructure new_file;
                new_file.file_name = file_name;
                new_file.file_path = file_path;
                new_file.sha = complete_file_sha;
                new_file.total_chunks = no_of_chunks;
                new_file.total_size = file_size;
                new_file.no_of_chunks_I_have = no_of_chunks;
                new_file.chunks_I_have = chunk_shas;  // Store all chunk hashes
                filesIHave[file_name] = new_file;

                cout << "[Upload] Added file " << file_name << " to filesIHave map with " 
                     << no_of_chunks << " chunks" << endl;
            }
            file.close();
 
            string full_cmd = "upload_file " + file_path + " " + group_id + " " 
                              + file_name + " " + final_sha + " " + to_string(no_of_chunks) + " " + to_string(file_size);

            send(tracker_sock, full_cmd.c_str(), full_cmd.size(), 0);

            memset(buffer, 0, sizeof(buffer));
            int bytes_received = recv(tracker_sock, buffer, sizeof(buffer), 0);
            if (bytes_received > 0)
                cout << "[Tracker Response]: " << buffer << endl;
            else
                cout << "[Error] No response from tracker.\n";

            continue;
        }

        string modified_cmd = cmd;
        if (cmd.find("create_user") == 0 || cmd.find("login") == 0) {
            // For create_user and login, append both client IP and port
            modified_cmd += " " + g_client_ip + " " + g_client_port;
        }
        send(tracker_sock, modified_cmd.c_str(), modified_cmd.size(), 0);

        memset(buffer, 0, sizeof(buffer));
        int bytes_received = recv(tracker_sock, buffer, sizeof(buffer), 0);
        if (bytes_received > 0)
            cout << "[Tracker Response]: " << buffer << endl;
        else
            cout << "[Error] No response from tracker.\n";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./client <client_ip:port> tracker_info.txt" << endl;
        return -1;
    }

    string client_info = argv[1];
    string tracker_info_file = argv[2];

    // Parse and store client info globally
    size_t colon_pos = client_info.find(":");
    g_client_ip = client_info.substr(0, colon_pos);
    g_client_port = client_info.substr(colon_pos + 1);

    // Start peer upload server
    pthread_t uploadThread;
    string *clientArg = new string(client_info);
    pthread_create(&uploadThread, NULL, peerServer, (void *)clientArg);
    pthread_detach(uploadThread);

    int tracker_sock = connectToTracker(tracker_info_file);
    if (tracker_sock < 0) {
        cerr << "Failed to connect to tracker." << endl;
        return -1;
    }

    handleTrackerCommands(tracker_sock);

    //  Simulating parallel chunk downloads using thread pool
    // cout << "\n[Client] Starting simulated parallel downloads..." << endl;

    // ThreadPool pool(4); // 4 worker threads
    // for (int i = 1; i <= 8; i++) {
    //     pool.enqueue([i] {
    //         this_thread::sleep_for(chrono::milliseconds(300 + rand() % 400));
    //         lock_guard<mutex> lock(downloadMutex);
    //         cout << "[Download] Chunk " << i << " downloaded by thread "
    //              << this_thread::get_id() << endl;
    //     });
    // }

    // this_thread::sleep_for(chrono::seconds(4));
    // cout << "[Client] All simulated downloads finished.\n";

    close(tracker_sock);
    return 0;
}