#ifndef FILE_DOWNLOADER_H
#define FILE_DOWNLOADER_H

#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <openssl/evp.h>

#include "thread_pool.h"
#include "data_structures.h"

using namespace std;

// Global map to track files that this peer has (defined in client.cpp)
extern unordered_map<string, FilesStructure> filesIHave;

class FileDownloader {
private:
    string file_name;
    string group_id;
    string complete_file_sha;
    string dest_path;
    vector<string> chunk_shas;
    long long int total_chunks;
    long long int total_size;
    vector<pair<string, string>> peers; // vector of {ip, port}
    vector<bool> chunk_downloaded;
    vector<vector<int>> chunk_availability; // peer_index -> vector of chunk numbers they have
    mutex download_mutex;
    condition_variable cv;
    ThreadPool& thread_pool;
    int tracker_sock;

    // File writing
    int output_fd;
    mutex file_mutex;
    
    // Track if we've notified the tracker about this file
    bool tracker_notified;
    mutex notify_mutex;

public:
    FileDownloader(const string& fname, const string& dest_path, const string& gid, ThreadPool& pool, int tracker_socket)
        : file_name(fname), group_id(gid), dest_path(dest_path), thread_pool(pool), tracker_sock(tracker_socket), tracker_notified(false) {
        
        // Initialize with download request to tracker
        string cmd = "download_file " + group_id + " " + file_name;
        char buffer[4096] = {0};
        string response;
        try {
            if (send(tracker_sock, cmd.c_str(), cmd.size(), 0) < 0) {
                throw runtime_error("Failed to send command to tracker");
            }

            int bytes_received = recv(tracker_sock, buffer, sizeof(buffer), 0);
            if (bytes_received <= 0) {
                throw runtime_error("Failed to get response from tracker");
            }
            response = string(buffer, bytes_received);
        } catch (const runtime_error& e) {
            cout << "Error in tracker communication: " << e.what() << endl;
            throw;  // Re-throw to handle in caller
        }
        
        // Parse tracker response
        vector<string> parts;
        stringstream ss(response);
        string temp;
        while (getline(ss, temp, '#')) {
            parts.push_back(temp);
        }
        
        cout << "Response received by client: " << response << endl;

        // Debug: show raw tracker response when parsing
        if (parts.empty()) {
            throw runtime_error("Empty/invalid response from tracker: '" + response + "'");
        }

        // Check if we have the necessary parts in the response
        if (parts.size() < 5) { // Need at least no_of_chunks, file_sha, chunk_shas, total_size, and at least one peer
            throw runtime_error("Invalid response from tracker (not enough parts): '" + response + "'");
        }

        // Parse response format: no_of_chunks#file_sha#chunk_shas#total_size#peer_info
        try {
            total_chunks = stoll(parts[0]); // First part is the number of chunks
        } catch (const exception& e) {
            throw runtime_error("Failed to parse total_chunks from tracker response ('" + parts[0] + "'): " + e.what() + " -- raw response: '" + response + "'");
        }

        // Parse chunk SHAs, which are in parts[2] to parts[2 + total_chunks - 1]
        for (size_t i = 1; i < 1 + total_chunks; i++) {
            if (i < parts.size()) {
                chunk_shas.push_back(parts[i]);
            } else {
                throw runtime_error("Not enough chunk SHAs in the response");
            }
        }

        complete_file_sha = parts[1 + total_chunks];  // Second part is the complete file SHA

        if (chunk_shas.size() != total_chunks) {
            throw runtime_error("Number of chunk SHAs does not match total_chunks from tracker response");
        }

        try {
            total_size = stoll(parts[2 + total_chunks]); // The part after chunk SHAs is the total_size
        } catch (const exception& e) {
            throw runtime_error("Failed to parse total_size from tracker response ('" + parts[2 + total_chunks] + "'): " + e.what() + " -- raw response: '" + response + "'");
        }

        chunk_downloaded.resize(total_chunks, false);

        for (size_t i = 3 + total_chunks; i < parts.size(); i++) {
            string peer = parts[i];
            size_t colon = peer.find(":");
            if (colon != string::npos) {
                string ip = peer.substr(0, colon);
                string port = peer.substr(colon + 1);
                cout << "Found peer from tracker with IP: " << ip << ":" << port << endl;
                peers.push_back({ip, port});
            }
        }

        if (peers.empty()) {
            throw runtime_error("No peers available in tracker response: " + response);
        }

        // Initialize chunk availability tracking
        chunk_availability.resize(peers.size());

        string output_path = dest_path + "/" + file_name;
        output_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output_fd == -1) {
            throw runtime_error("Failed to create output file: " + output_path + " (errno: " + to_string(errno) + ")");
        }
        cout << "[Download] Created output file: " << output_path << endl;

        if (ftruncate(output_fd, total_size) == -1) {
            close(output_fd);
            throw runtime_error("Failed to extend file to full size");
        }
    }

    ~FileDownloader() {
        if (output_fd != -1) {
            if (isDownloadComplete()) {
                verifyCompleteFile();
            }
            close(output_fd);
        }
    }

    bool verifyChunk(const string& data, const string& expected_sha) {
        return true;
    }

    bool verifyCompleteFile() {
        cout << "[Download] Verifying complete file integrity..." << endl;
        
        string output_path = dest_path + "/" + file_name;
        int fd = open(output_path.c_str(), O_RDONLY);
        if (fd < 0) {
            cerr << "[Download] Error opening file for verification: " << strerror(errno) << endl;
            return false;
        }

        size_t chunkSize = 1024;
        vector<unsigned char> fileBuffer(chunkSize);
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLength;

        EVP_MD_CTX* mdContext = EVP_MD_CTX_new();
        if (mdContext == nullptr) {
            cerr << "[Download] Error creating EVP context for file verification" << endl;
            close(fd);
            return false;
        }

        const EVP_MD* md = EVP_sha256();
        if (EVP_DigestInit_ex(mdContext, md, nullptr) != 1) {
            cerr << "[Download] Error initializing SHA256 for file verification" << endl;
            EVP_MD_CTX_free(mdContext);
            close(fd);
            return false;
        }

        ssize_t bytesRead;
        while ((bytesRead = read(fd, fileBuffer.data(), chunkSize)) > 0) {
            if (EVP_DigestUpdate(mdContext, fileBuffer.data(), bytesRead) != 1) {
                cerr << "[Download] Error updating SHA256 with file data" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return false;
            }
        }

        if (EVP_DigestFinal_ex(mdContext, hash, &hashLength) != 1) {
            cerr << "[Download] Error finalizing SHA256 for file verification" << endl;
            EVP_MD_CTX_free(mdContext);
            close(fd);
            return false;
        }

        EVP_MD_CTX_free(mdContext);
        close(fd);

        // Convert hash to string
        stringstream ss;
        for (unsigned int i = 0; i < hashLength; i++) {
            ss << hex << setw(2) << setfill('0') << (int)hash[i];
        }
        string calculated_file_sha = ss.str();

        cout << "[Download] Complete file SHA verification:" << endl;
        cout << "[Download]   Expected: " << complete_file_sha << endl;
        cout << "[Download]   Calculated: " << calculated_file_sha << endl;

        if (calculated_file_sha != complete_file_sha) {
            cout << "[Download] ERROR: Complete file SHA mismatch!" << endl;
            cout << "[Download] File integrity verification FAILED" << endl;
            
            // Delete the corrupted file
            if (remove(output_path.c_str()) == 0) {
                cout << "[Download] Deleted corrupted file: " << output_path << endl;
            } else {
                cout << "[Download] Failed to delete corrupted file: " << output_path << endl;
            }
            
            return false;
        }

        cout << "[Download] Complete file SHA verification SUCCESSFUL!" << endl;
        return true;
    }

    void notifyTrackerOfFile() {
        lock_guard<mutex> lock(notify_mutex);
        
        if (tracker_notified) {
            return;  
        }

        tracker_notified = true;  

        thread notification_thread([this]() {
            try {
                this_thread::sleep_for(chrono::milliseconds(50));
                
                string cmd = "notify_file_chunk " + group_id + " " + file_name;
                
                cout << "[Download] Notifying tracker: " << cmd << endl;
                
                if (send(tracker_sock, cmd.c_str(), cmd.size(), 0) < 0) {
                    cout << "[Download] Failed to send notification to tracker" << endl;
                    return;
                }

                this_thread::sleep_for(chrono::milliseconds(100));
                
                char response_buffer[1024] = {0};
                int flags = MSG_DONTWAIT;  
                int bytes_received = recv(tracker_sock, response_buffer, sizeof(response_buffer), flags);
                if (bytes_received > 0) {
                    string response(response_buffer, bytes_received);
                    cout << "[Download] Tracker notification response: " << response << endl;
                }

                string file_key = file_name;  // Use just file_name as key, matching client.cpp convention
                
                // Check if file already exists in filesIHave
                if (filesIHave.find(file_key) == filesIHave.end()) {
                    FilesStructure file_struct;
                    file_struct.file_name = file_name;
                    file_struct.file_path = dest_path + "/" + file_name;
                    file_struct.sha = complete_file_sha;
                    file_struct.total_chunks = total_chunks;
                    file_struct.total_size = total_size;
                    filesIHave[file_key] = file_struct;
                    cout << "[Download] Created new entry in filesIHave for " << file_key << endl;
                }
                
                filesIHave[file_key].chunks_I_have.clear();  // Clear previous chunks
                filesIHave[file_key].no_of_chunks_I_have = 0;
                
                for (int i = 0; i < total_chunks; i++) {
                    if (chunk_downloaded[i]) {
                        filesIHave[file_key].chunks_I_have.push_back(to_string(i));
                        filesIHave[file_key].no_of_chunks_I_have++;
                    }
                }
                
                cout << "[Download] Updated filesIHave for " << file_key << " with " << filesIHave[file_key].no_of_chunks_I_have << " chunks" << endl;
                cout << "[Download] Successfully notified tracker about file " << file_name << endl;
            } catch (const exception& e) {
                cout << "[Download] Error notifying tracker: " << e.what() << endl;
            }
        });
        
        notification_thread.detach(); 
    }

    void updateChunkAvailability(int peer_idx, const vector<int>& chunks) {
        lock_guard<mutex> lock(download_mutex);
        chunk_availability[peer_idx] = chunks;
    }

    void markChunkDownloaded(int chunk_no) {
        lock_guard<mutex> lock(download_mutex);
        chunk_downloaded[chunk_no] = true;

        cout << "[Download] Marking chunk " << chunk_no << " as downloaded." << endl;
    }

    bool isDownloadComplete() {
        cout << "[Download] Checking if download is complete..." << endl;
        lock_guard<mutex> lock(download_mutex);
        bool complete = all_of(chunk_downloaded.begin(), chunk_downloaded.end(), [](bool v) { return v; });
        cout << "[Download] Download complete: " << complete << " (All chunks: " << chunk_downloaded.size() << ")" << endl;
        return complete;
    }

    int selectRarestChunk() {
        lock_guard<mutex> lock(download_mutex);

        vector<int> chunk_frequency(total_chunks, 0);
        for (const auto& peer_chunks : chunk_availability) {
            for (int chunk : peer_chunks) {
                if (chunk >= 0 && chunk < total_chunks) {
                    chunk_frequency[chunk]++;
                }
            }
        }

        cout << "[Download] Chunk Frequencies: ";
        for (int i = 0; i < total_chunks; ++i) {
            cout << chunk_frequency[i] << " ";
        }
        cout << endl;

        int rarest_chunk = -1;
        int min_frequency = INT_MAX;

        for (int i = 0; i < total_chunks; i++) {
            if (!chunk_downloaded[i] && chunk_frequency[i] > 0 && chunk_frequency[i] < min_frequency) {
                min_frequency = chunk_frequency[i];
                rarest_chunk = i;
            }
        }

        cout << "[Download] Selected rarest chunk: " << rarest_chunk << " with frequency: " << min_frequency << endl;
        return rarest_chunk;
    }

    // Find a peer that has a specific chunk
    int selectPeerForChunk(int chunk_no) {
        lock_guard<mutex> lock(download_mutex);

        // Find all peers that have this chunk
        vector<int> available_peers;
        for (int i = 0; i < (int)chunk_availability.size(); i++) {
            const auto& peer_chunks = chunk_availability[i];
            if (find(peer_chunks.begin(), peer_chunks.end(), chunk_no) != peer_chunks.end()) {
                available_peers.push_back(i);
            }
        }

        if (available_peers.empty()) {
            cout << "[Download] No available peers for chunk " << chunk_no << endl;
            return -1;
        }

        // Select a random peer from available peers
        int selected_peer = available_peers[rand() % available_peers.size()];
        cout << "[Download] Selected peer " << selected_peer << " for chunk " << chunk_no << endl;

        return selected_peer;
    }

    // Download a single chunk from a peer
    bool downloadChunkFromPeer(int chunk_no, int peer_idx) {
        auto peer = peers[peer_idx];
        int sock = 0;

        cout << "[Download] Requesting chunk " << chunk_no << " from peer " << peer.first << ":" << peer.second << endl;

        try {
            struct sockaddr_in serv_addr;

            if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                throw runtime_error("Socket creation error for chunk " + to_string(chunk_no));
            }

            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(stoi(peer.second));
            if (inet_pton(AF_INET, peer.first.c_str(), &serv_addr.sin_addr) <= 0) {
                close(sock);
                throw runtime_error("Invalid peer address for chunk " + to_string(chunk_no));
            }

            cout << "[Download] Connecting to peer..." << endl;
            if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                close(sock);
                throw runtime_error("Connection failed for chunk " + to_string(chunk_no) + " (errno: " + to_string(errno) + ")");
            }
            cout << "[Download] Connected to peer successfully" << endl;

            // Request the chunk with delimiter
            string request = "get_chunk " + group_id + " " + file_name + " " + to_string(chunk_no) + "\r\n";
            cout << "[Download] Sending request: '" << request << "'" << endl;
            if (send(sock, request.c_str(), request.length(), 0) < 0) {
                close(sock);
                throw runtime_error("Failed to send request to peer");
            }

            // Receive response: just the raw chunk data
            char buffer[1024];  // Buffer for chunk data
            cout << "[Download] Waiting for chunk data..." << endl;
            int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
            if (bytes_received <= 0) {
                close(sock);
                throw runtime_error("Failed to receive chunk " + to_string(chunk_no) + 
                                    " (received " + to_string(bytes_received) + " bytes)");
            }

            string chunk_data(buffer, bytes_received);
            cout << "[Download] Received chunk " << chunk_no << " (" << bytes_received << " bytes)" << endl;
            cout << "[Download] First 20 bytes of chunk: ";
            for (int i = 0; i < min(20, bytes_received); i++) {
                cout << hex << setw(2) << setfill('0') << (int)(unsigned char)buffer[i] << " ";
            }
            cout << dec << endl;

            // Verify chunk SHA before writing
            unsigned char hash[EVP_MAX_MD_SIZE];
            unsigned int hashLength;
            EVP_MD_CTX* mdContext = EVP_MD_CTX_new();
            if (mdContext == nullptr) {
                close(sock);
                throw runtime_error("Error creating EVP context for chunk " + to_string(chunk_no));
            }

            const EVP_MD* md = EVP_sha256();
            if (EVP_DigestInit_ex(mdContext, md, nullptr) != 1) {
                EVP_MD_CTX_free(mdContext);
                close(sock);
                throw runtime_error("Error initializing SHA256 for chunk " + to_string(chunk_no));
            }

            if (EVP_DigestUpdate(mdContext, chunk_data.c_str(), chunk_data.length()) != 1) {
                EVP_MD_CTX_free(mdContext);
                close(sock);
                throw runtime_error("Error updating SHA256 for chunk " + to_string(chunk_no));
            }

            if (EVP_DigestFinal_ex(mdContext, hash, &hashLength) != 1) {
                EVP_MD_CTX_free(mdContext);
                close(sock);
                throw runtime_error("Error finalizing SHA256 for chunk " + to_string(chunk_no));
            }

            EVP_MD_CTX_free(mdContext);

            // Convert hash to string (using the same approach as file_hasher.h)
            stringstream ss;
            for (unsigned int i = 0; i < hashLength; i++) {
                ss << hex << setw(2) << setfill('0') << (int)hash[i];
            }
            string calculated_sha = ss.str();
            string expected_sha = chunk_shas[chunk_no];
            
            cout << "[Download] Chunk " << chunk_no << " SHA verification:" << endl;
            cout << "[Download]   Expected: " << expected_sha << endl;
            cout << "[Download]   Calculated: " << calculated_sha << endl;
            
            if (calculated_sha != expected_sha) {
                close(sock);
                cout << "[Download] SHA mismatch for chunk " << chunk_no << "! Retrying with different peer..." << endl;
                return false;  // Return false to trigger retry with different peer
            }
            
            cout << "[Download] SHA verification successful for chunk " << chunk_no << endl;

            // Write the chunk to file with proper locking
            {
                unique_lock<mutex> lock(file_mutex);

                // Verify chunk size
                if (chunk_data.length() > 1024) {
                    throw runtime_error("Chunk size too large: " + to_string(chunk_data.length()));
                }

                // Calculate offset
                off_t offset = chunk_no * 1024LL;
                cout << "[Download] Writing " << chunk_data.length() << " bytes at offset " << offset << endl;

                // Open file for each write to avoid seek issues
                string output_path = dest_path+ "/" + file_name;
                int fd = open(output_path.c_str(), O_WRONLY);
                if (fd == -1) {
                    throw runtime_error("Failed to open file for writing: " + string(strerror(errno)));
                }

                // Seek and write
                if (lseek(fd, offset, SEEK_SET) == -1) {
                    close(fd);
                    throw runtime_error("Failed to seek: " + string(strerror(errno)));
                }

                ssize_t written = write(fd, chunk_data.c_str(), chunk_data.length());
                if (written == -1) {
                    close(fd);
                    throw runtime_error("Failed to write: " + string(strerror(errno)));
                }

                fsync(fd); // Ensure chunk is written
                close(fd);

                cout << "[Download] Successfully wrote chunk " << chunk_no << " (" << written << " bytes)" << endl;

                // Mark chunk as downloaded only if write was successful
                markChunkDownloaded(chunk_no);
                notifyTrackerOfFile();
            }

            return true;
        } catch (const exception& e) {
            cout << "Error processing chunk " << chunk_no << ": " << e.what() << endl;
            return false;
        }
    }


    // Start parallel download using thread pool
    void startDownload() {
        cout << "\n[Download] Starting download of file: " << file_name << endl;
        cout << "[Download] Number of peers available: " << getNumPeers() << endl;

        // Create a thread pool to query peers concurrently
        ThreadPool peer_query_pool(10); 
        vector<std::future<void>> peer_query_futures;

        for (int i = 0; i < getNumPeers(); i++) {
            cout << "[Download] Queuing peer query for peer " << i << " at " << peers[i].first << ":" << peers[i].second << endl;

            // Enqueue peer query task to thread pool
            peer_query_futures.push_back(peer_query_pool.enqueue([i, this] {
                try {
                    cout << "[Download] Querying peer " << i << " for chunk info..." << endl;
                    queryPeerChunks(i); // Query chunk info from the peer
                    cout << "[Download] Peer " << i << " chunk query successful." << endl;
                } catch (const exception& e) {
                    cout << "[Download] Error querying peer " << i << ": " << e.what() << endl;
                }
            }));
        }

        // Wait for all peer queries to finish (by calling .get() on each future)
        for (auto& future : peer_query_futures) {
            future.get();
        }

        cout << "[Download] Waiting for initial chunk information..." << endl;
        // Wait a bit for chunk availability information
        this_thread::sleep_for(chrono::milliseconds(500));

        // Display initial chunk distribution
        cout << "[Download] Attempting to show initial chunk distribution..." << endl;
        try {
            displayChunkDistribution();
        } catch (const exception& e) {
            cout << "[Download] Error displaying chunk distribution: " << e.what() << endl;
        }

        // Create a thread pool for downloading chunks concurrently
        ThreadPool download_pool(15);  // 15 threads for downloading chunks concurrently

        // Track download attempts and completion status
        vector<atomic<int>> download_attempts(total_chunks);
        vector<atomic<bool>> chunk_in_progress(total_chunks);
        const int MAX_ATTEMPTS = 3;

        auto last_display = chrono::steady_clock::now();

        // Start downloading chunks
        cout << "[Download] Beginning download loop..." << endl;
        while (!isDownloadComplete()) {
            cout << "[Download] Checking for rarest chunk to download..." << endl;
            int chunk_no = selectRarestChunk();
            
            // Log the chunk selection
            cout << "[Download] Rarest chunk selected: " << chunk_no << endl;

            if (chunk_no == -1) {
                cout << "[Download] No chunks available to download." << endl;
                break;
            }

            // Check if chunk is already being downloaded
            bool expected = false;
            if (!chunk_in_progress[chunk_no].compare_exchange_strong(expected, true)) {
                cout << "[Download] Chunk " << chunk_no << " is already in progress, sleeping..." << endl;
                this_thread::sleep_for(chrono::milliseconds(100));  // Sleep if the chunk is already being downloaded
                continue;
            }

            if (download_attempts[chunk_no] >= MAX_ATTEMPTS) {
                cout << "[Download] Failed to download chunk " << chunk_no << " after " << MAX_ATTEMPTS << " attempts." << endl;
                break;
            }

            // Select a peer to download the chunk from
            int peer_idx = selectPeerForChunk(chunk_no);
            if (peer_idx == -1) {
                cout << "[Download] No peers available for chunk " << chunk_no << endl;
                chunk_in_progress[chunk_no] = false;
                this_thread::sleep_for(chrono::milliseconds(100));  // Sleep before retrying
                continue;
            }

            // Log the peer selection
            cout << "[Download] Enqueuing download task for chunk " << chunk_no << " from peer " << peer_idx << endl;

            // Queue the chunk download task in thread pool
            download_pool.enqueue([this, chunk_no, peer_idx, &download_attempts, &chunk_in_progress] {
                try {
                    download_attempts[chunk_no]++;
                    cout << "[Download] Attempting to download chunk " << chunk_no << " from peer " << peer_idx 
                        << " (Attempt " << download_attempts[chunk_no] << " of " << MAX_ATTEMPTS << ")" << endl;

                    if (!downloadChunkFromPeer(chunk_no, peer_idx)) {
                        cout << "[Download] Failed to download chunk " << chunk_no << " from peer " << peer_idx 
                            << " (attempt " << download_attempts[chunk_no] << " of " << MAX_ATTEMPTS << ")" << endl;
                    } else {
                        cout << "[Download] Successfully downloaded chunk " << chunk_no << " from peer " << peer_idx << endl;
                    }
                    
                    cout << "[Download] Chunk " << chunk_no << " download attempts: " << download_attempts[chunk_no] << endl;
                } catch (const exception& e) {
                    cout << "[Download] Error downloading chunk " << chunk_no << ": " << e.what() << endl;
                }
                chunk_in_progress[chunk_no] = false;  // Mark chunk as no longer in progress
            });

            this_thread::sleep_for(chrono::milliseconds(100));

            auto now = chrono::steady_clock::now();
            if (chrono::duration_cast<chrono::seconds>(now - last_display).count() >= 2) {
                cout << "[Download] Displaying chunk distribution..." << endl;
                displayChunkDistribution();
                last_display = now;
            }
        }

        cout << "[Download] All downloads completed." << endl;
    }

    void queryPeerChunks(int peer_idx) {
        cout << "Attempting to query chunks from peer " << peer_idx << " at " 
             << peers[peer_idx].first << ":" << peers[peer_idx].second << endl;
        auto peer = peers[peer_idx];
        
        int sock = 0;
        struct sockaddr_in serv_addr;

        try {
            if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                throw runtime_error("Socket creation error for peer " + to_string(peer_idx));
            }

            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(stoi(peer.second));
            if (inet_pton(AF_INET, peer.first.c_str(), &serv_addr.sin_addr) <= 0) {
                close(sock);
                throw runtime_error("Invalid peer address: " + peer.first + ":" + peer.second);
            }

            cout << "Attempting to connect to peer at " << peer.first << ":" << peer.second << endl;
            if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                close(sock);
                throw runtime_error("Connection failed with peer " + to_string(peer_idx) + 
                                  " at " + peer.first + ":" + peer.second + 
                                  " (errno: " + to_string(errno) + ")");
            }
            cout << "Successfully connected to peer" << endl;
        } catch (const exception& e) {
            if (sock > 0) {
                close(sock);
            }
            cout << "Error connecting to peer: " << e.what() << endl;
            return;
        }

        // Send chunk availability request with newline termination
        string request = "get_chunks " + group_id + " " + file_name + "\n";
        send(sock, request.c_str(), request.size(), 0);

        // Receive response
        char buffer[4096] = {0};
        int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        close(sock);

        if (bytes_received <= 0) {
            cout << "Failed to get response from peer " << peer_idx << endl;
            return;
        }

        string response(buffer);
        cout << "Received response from peer: '" << response << "'" << endl;
        
        // Check if response is an error message
        if (response == "File not found") {
            cout << "Peer " << peer_idx << " doesn't have the file" << endl;
            return;
        }
        
        vector<string> parts;
        stringstream ss(response);
        string temp;
        
        // Parse response: <total_chunks>#<chunk1,chunk2,...>
        if (!getline(ss, temp, '#')) {
            cout << "Invalid response format from peer" << endl;
            return;
        }
        
        try {
            int peer_total_chunks = stoi(temp);
            if (peer_total_chunks != total_chunks) {
                cout << "Peer " << peer_idx << " has incorrect number of chunks" << endl;
                return;
            }
        } catch (const std::exception& e) {
            cout << "Error parsing chunks number: " << e.what() << ", response: '" << temp << "'" << endl;
            return;
        }

        vector<int> available_chunks;
        if (getline(ss, temp)) {
            stringstream chunk_ss(temp);
            string chunk_str;
            while (getline(chunk_ss, chunk_str, ',')) {
                if (!chunk_str.empty()) {
                    available_chunks.push_back(stoi(chunk_str));
                    cout<< "Peer " << peer_idx << " has chunk " << chunk_str << endl;
                }
            }
        }

        // Update chunk availability
        updateChunkAvailability(peer_idx, available_chunks);
    }


    pair<string, string> getPeerInfo(int peer_idx) {
        if (peer_idx >= 0 && peer_idx < (int)peers.size()) {
            return peers[peer_idx];
        }
        throw runtime_error("Invalid peer index");
    }

    int getNumPeers() const {
        return peers.size();
    }

    int getTotalChunks() const {
        return total_chunks;
    }

    void displayChunkDistribution() {
        cout << "\nAttempting to display chunk distribution..." << endl;
        
        vector<vector<char>> distribution;
        try {
            cout << "\n=== Chunk Distribution for file: " << file_name << " ===\n";
            cout << "Total chunks: " << total_chunks << "\n";
            cout << "Total peers: " << peers.size() << "\n\n";

            if (peers.empty()) {
                cout << "No peers available yet!\n";
                return;
            }

            if (total_chunks <= 0) {
                cout << "No chunk information available yet!\n";
                return;
            }

            // Create a matrix of chunk availability for visualization
            distribution.resize(peers.size(), vector<char>(total_chunks, '.'));
            
            cout << "Processing chunk availability..." << endl;
            
            // Fill in the matrix based on chunk_availability
            for (size_t i = 0; i < chunk_availability.size(); i++) {
                cout << "Processing peer " << i << " with " << chunk_availability[i].size() << " chunks" << endl;
                for (int chunk : chunk_availability[i]) {
                    if (chunk >= 0 && chunk < total_chunks) {
                        distribution[i][chunk] = 'X';
                    }
                }
            }

            // Mark our downloaded chunks
            if (!peers.empty()) {  // Add our chunks as the last row
                distribution.push_back(vector<char>(total_chunks, '.'));
                for (int i = 0; i < total_chunks; i++) {
                    if (chunk_downloaded[i]) {
                        distribution.back()[i] = 'X';
                    }
                }
            }

            // Print header (chunk numbers)
            cout << "      ";
            
            for (int i = 0; i < total_chunks; i++) {
                cout << setw(3) << i;
            }
            cout << "\n";

            // Print separator
            cout << "      ";
            for (int i = 0; i < total_chunks; i++) {
                cout << "---";
            }
            cout << "\n";

            // Print peer rows
            for (size_t i = 0; i < peers.size(); i++) {
                cout << "P" << setw(4) << left << i << " |";
                for (int j = 0; j < total_chunks; j++) {
                    cout << " " << distribution[i][j] << " ";
                }
                cout << "| " << peers[i].first << ":" << peers[i].second << "\n";
            }

            // Print our chunks
            if (!peers.empty()) {
                cout << "Self  |";
                for (int j = 0; j < total_chunks; j++) {
                    cout << " " << distribution.back()[j] << " ";
                }
                cout << "| (Downloaded chunks)\n";
            }

            cout << "      ";
            for (int i = 0; i < total_chunks; i++) {
                cout << "---";
            }
            cout << "\n";

            cout << "\nLegend:\n";
            cout << "X - Has chunk\n";
            cout << ". - Does not have chunk\n";
            cout << "\nProgress: " << 
                count(chunk_downloaded.begin(), chunk_downloaded.end(), true) * 100.0 / total_chunks 
                << "% complete\n";
            cout << "==========================================\n\n";
        } catch (const exception& e) {
            cout << "Error displaying chunk distribution: " << e.what() << endl;
        }
        for (int i = 0; i < total_chunks; i++) {
            cout << setw(3) << i;
        }
        cout << "\n";

        cout << "      ";
        for (int i = 0; i < total_chunks; i++) {
            cout << "---";
        }
        cout << "\n";

        for (size_t i = 0; i < peers.size(); i++) {
            cout << "P" << setw(4) << left << i << " |";
            for (int j = 0; j < total_chunks; j++) {
                cout << " " << distribution[i][j] << " ";
            }
            cout << "| " << peers[i].first << ":" << peers[i].second << "\n";
        }

        if (!peers.empty()) {
            cout << "Self  |";
            for (int j = 0; j < total_chunks; j++) {
                cout << " " << distribution.back()[j] << " ";
            }
            cout << "| (Downloaded chunks)\n";
        }

        cout << "      ";
        for (int i = 0; i < total_chunks; i++) {
            cout << "---";
        }
        cout << "\n";

        cout << "\nLegend:\n";
        cout << "X - Has chunk\n";
        cout << ". - Does not have chunk\n";
        cout << "\nProgress: " << 
            count(chunk_downloaded.begin(), chunk_downloaded.end(), true) * 100.0 / total_chunks 
            << "% complete\n";
        cout << "==========================================\n\n";
    }
};

#endif