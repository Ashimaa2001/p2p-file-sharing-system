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

mutex downloadMutex;

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;

public:
    ThreadPool(size_t threads);
    template <class F>
    void enqueue(F f);
    ~ThreadPool();
};

ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                function<void()> task;
                {
                    unique_lock<mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty())
                        return;
                    task = move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

template <class F>
void ThreadPool::enqueue(F f) {
    {
        unique_lock<mutex> lock(queue_mutex);
        if (stop)
            throw runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace(f);
    }
    condition.notify_one();
}

ThreadPool::~ThreadPool() {
    {
        unique_lock<mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (thread &worker : workers)
        worker.join();
}

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
        recv(new_socket, buffer, sizeof(buffer), 0);
        cout << "[Peer Server] Received request: " << buffer << endl;

        string msg = "Chunk data (simulated)";
        send(new_socket, msg.c_str(), msg.size(), 0);
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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./client <client_ip:port> tracker_info.txt" << endl;
        return -1;
    }

    string client_info = argv[1];
    string tracker_info_file = argv[2];

    pthread_t uploadThread;
    string *clientArg = new string(client_info);
    pthread_create(&uploadThread, NULL, peerServer, (void *)clientArg);
    pthread_detach(uploadThread);

    int tracker_sock = connectToTracker(tracker_info_file);
    if (tracker_sock < 0) {
        cerr << "Failed to connect to tracker." << endl;
        return -1;
    }

    string msg = "Hello from client!";
    send(tracker_sock, msg.c_str(), msg.size(), 0);

    char buffer[1024] = {0};
    int bytes_received = recv(tracker_sock, buffer, sizeof(buffer), 0);
    if (bytes_received > 0)
        cout << "[Client] Received: " << buffer << endl;

    cout << "\n[Client] Starting simulated parallel downloads..." << endl;

    ThreadPool pool(4); 
    for (int i = 1; i <= 8; i++) {
        pool.enqueue([i] {
            this_thread::sleep_for(chrono::milliseconds(500 + rand() % 500));
            lock_guard<mutex> lock(downloadMutex);
            cout << "[Download] Chunk " << i << " downloaded by thread "
                 << this_thread::get_id() << endl;
        });
    }

    this_thread::sleep_for(chrono::seconds(5));
    cout << "[Client] All chunk downloads finished.\n";

    close(tracker_sock);
    return 0;
}
