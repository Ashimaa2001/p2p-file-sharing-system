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

    int output_fd;
    mutex file_mutex;

public:
    FileDownloader(const string& fname, const string& dest_path, const string& gid, ThreadPool& pool, int tracker_socket) 
        : file_name(fname), group_id(gid), thread_pool(pool), tracker_sock(tracker_socket) {
       
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
            throw; 
        }
          
            vector<string> parts;
            stringstream ss(response);
            string temp;
            while (getline(ss, temp, '#')) {
                parts.push_back(temp);
            }

            if (parts.empty()) {
                throw runtime_error(string("Empty/invalid response from tracker: '") + response + "'");
            }

        if (parts.size() < 6) { 
            throw runtime_error(string("Invalid response from tracker (not enough parts): '") + response + "'");
        }       
        complete_file_sha = parts[0];  
        chunk_shas.push_back(parts[1]); 
            try {
            total_chunks = stoll(parts[3]); 
        } catch (const exception& e) {
            throw runtime_error(string("Failed to parse total_chunks from tracker response ('") + parts[3] + "'): " + e.what() + " -- raw response: '" + response + "'");
        }

        try {
            total_size = stoll(parts[4]); 
        } catch (const exception& e) {
            throw runtime_error(string("Failed to parse total_size from tracker response ('") + parts[4] + "'): " + e.what() + " -- raw response: '" + response + "'");
        }
        chunk_downloaded.resize(total_chunks, false);
        
        for (size_t i = 5; i < parts.size(); i++) {
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
        return true;
    }

    void updateChunkAvailability(int peer_idx, const vector<int>& chunks) {
        lock_guard<mutex> lock(download_mutex);
        chunk_availability[peer_idx] = chunks;
    }

    void markChunkDownloaded(int chunk_no) {
        lock_guard<mutex> lock(download_mutex);
        chunk_downloaded[chunk_no] = true;
        
        reportProgressToTracker();
    }

    void reportProgressToTracker() {
        vector<int> downloaded_chunks;
        for (int i = 0; i < total_chunks; i++) {
            if (chunk_downloaded[i]) {
                downloaded_chunks.push_back(i);
            }
        }

        stringstream chunks_ss;
        for (size_t i = 0; i < downloaded_chunks.size(); i++) {
            if (i > 0) chunks_ss << ",";
            chunks_ss << downloaded_chunks[i];
        }

        string update_cmd = "update_chunks " + group_id + " " + file_name + " " + chunks_ss.str();
        char resp[1024] = {0};
        try {
            if (send(tracker_sock, update_cmd.c_str(), update_cmd.size(), 0) < 0) {
                throw runtime_error("Failed to send update to tracker");
            }
            
            if (recv(tracker_sock, resp, sizeof(resp), 0) <= 0) {
                throw runtime_error("Failed to receive response from tracker");
            }
        } catch (const runtime_error& e) {
            cout << "Error in tracker communication: " << e.what() << endl;
            return;
        }

        bool download_complete = false;
        try {
            download_complete = isDownloadComplete();
        } catch (const exception& e) {
            cout << "Error checking download completion: " << e.what() << endl;
            return;
        }

        if (download_complete && verifyCompleteFile()) {
            // Add file to filesIHave when download is complete
            FilesStructure new_file;
            new_file.file_name = file_name;
            new_file.file_path = file_name;  
            new_file.sha = complete_file_sha;
            new_file.total_chunks = total_chunks;
            new_file.total_size = total_size;
            new_file.no_of_chunks_I_have = total_chunks;
            
            for (int chunk : downloaded_chunks) {
                new_file.chunks_I_have.push_back(chunk_shas[chunk]);
            }

            filesIHave[file_name] = new_file;
        }
    }

    bool isDownloadComplete() {
        lock_guard<mutex> lock(download_mutex);
        return all_of(chunk_downloaded.begin(), chunk_downloaded.end(), [](bool v) { return v; });
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

        int rarest_chunk = -1;
        int min_frequency = INT_MAX;

        for (int i = 0; i < total_chunks; i++) {
            if (!chunk_downloaded[i] && chunk_frequency[i] > 0 && chunk_frequency[i] < min_frequency) {
                min_frequency = chunk_frequency[i];
                rarest_chunk = i;
            }
        }

        cout << "Selected rarest chunk " << rarest_chunk << " with frequency " << min_frequency << endl;
        return rarest_chunk;
    }

    // Find a peer that has a specific chunk
    int selectPeerForChunk(int chunk_no) {
        lock_guard<mutex> lock(download_mutex);
        
        vector<int> available_peers;
        for (int i = 0; i < (int)chunk_availability.size(); i++) {
            const auto& peer_chunks = chunk_availability[i];
            if (find(peer_chunks.begin(), peer_chunks.end(), chunk_no) != peer_chunks.end()) {
                available_peers.push_back(i);
            }
        }

        if (available_peers.empty()) return -1;

        // Select a random peer from available peers
        return available_peers[rand() % available_peers.size()];
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
        } catch (const exception& e) {
            if (sock > 0) {
                close(sock);
            }
            cout << "Error connecting to peer: " << e.what() << endl;
            return false;
        }

        // Request the chunk with delimiter
        string request = "get_chunk " + group_id + " " + file_name + " " + to_string(chunk_no) + "\r\n";
        cout << "[Download] Sending request: '" << request << "'" << endl;
        if (send(sock, request.c_str(), request.length(), 0) < 0) {
            close(sock);
            throw runtime_error("Failed to send request to peer");
        }

        try {
            char buffer[1024];  
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
                string output_path = "downloads/" + file_name;
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
            }
            
            markChunkDownloaded(chunk_no);

            // Notify tracker about the new chunk
            string update_cmd = "update_chunks " + group_id + " " + file_name + " " + to_string(chunk_no);
            send(tracker_sock, update_cmd.c_str(), update_cmd.size(), 0);
            
            char resp[1024] = {0};
            recv(tracker_sock, resp, sizeof(resp), 0);

            return true;
        }
        catch (const exception& e) {
            cout << "Error processing chunk " << chunk_no << ": " << e.what() << endl;
            return false;
        }
    }

    // Start parallel download using thread pool
    void startDownload() {
        cout << "\nStarting download of file: " << file_name << endl;
        cout << "Number of peers available: " << getNumPeers() << endl;
        
        bool any_peer_responded = false;
        for (int i = 0; i < getNumPeers(); i++) {
            cout << "Querying peer " << i << " at " << peers[i].first << ":" << peers[i].second << endl;
            try {
                queryPeerChunks(i); 
                any_peer_responded = true;
                break; 
            } catch (const exception& e) {
                cout << "Warning: Failed to query peer " << i << ": " << e.what() << endl;
                continue;
            }
        }
        
        if (!any_peer_responded) {
            throw runtime_error("Failed to connect to any peers. Please ensure the peers are running and available.");
        }

        cout << "Waiting for initial chunk information..." << endl;
        this_thread::sleep_for(chrono::milliseconds(500));

        cout << "Attempting to show initial distribution..." << endl;
        try {
            displayChunkDistribution();
        } catch (const exception& e) {
            cout << "Error displaying chunk distribution: " << e.what() << endl;
        }

            cout << "Beginning download loop..." << endl;
            auto last_display = chrono::steady_clock::now();
            
            vector<atomic<int>> download_attempts(total_chunks);
            vector<atomic<bool>> chunk_in_progress(total_chunks);
            const int MAX_ATTEMPTS = 3;
            
            while (!isDownloadComplete()) {
                int chunk_no = selectRarestChunk();
                if (chunk_no == -1) {
                    cout << "No chunks available to download" << endl;
                    break;
                }

                // Check if chunk is already being downloaded
                bool expected = false;
                if (!chunk_in_progress[chunk_no].compare_exchange_strong(expected, true)) {
                    this_thread::sleep_for(chrono::milliseconds(100));
                    continue;
                }

                if (download_attempts[chunk_no] >= MAX_ATTEMPTS) {
                    cout << "Failed to download chunk " << chunk_no << " after " << MAX_ATTEMPTS << " attempts" << endl;
                    break;
                }

                int peer_idx = selectPeerForChunk(chunk_no);
                if (peer_idx == -1) {
                    cout << "No peers available for chunk " << chunk_no << endl;
                    chunk_in_progress[chunk_no] = false;
                    this_thread::sleep_for(chrono::milliseconds(100));
                    continue;
                }

                // Queue the chunk download in thread pool
                thread_pool.enqueue([this, chunk_no, peer_idx, &download_attempts, &chunk_in_progress] {
                    try {
                        download_attempts[chunk_no]++;
                        if (!downloadChunkFromPeer(chunk_no, peer_idx)) {
                            cout << "Failed to download chunk " << chunk_no << " from peer " << peer_idx 
                                 << " (attempt " << download_attempts[chunk_no] << " of " << MAX_ATTEMPTS << ")" << endl;
                        } else {
                            cout << "Successfully downloaded chunk " << chunk_no << " from peer " << peer_idx << endl;
                        }
                    } catch (const exception& e) {
                        cout << "Error downloading chunk " << chunk_no << ": " << e.what() << endl;
                    }
                    chunk_in_progress[chunk_no] = false;  // Mark chunk as no longer in progress
                });

                // Wait a bit before queuing next chunk
                this_thread::sleep_for(chrono::milliseconds(100));            // Display chunk distribution every 2 seconds
            auto now = chrono::steady_clock::now();
            if (chrono::duration_cast<chrono::seconds>(now - last_display).count() >= 2) {
                displayChunkDistribution();
                last_display = now;
            }
        }
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

#endif // FILE_DOWNLOADER_H