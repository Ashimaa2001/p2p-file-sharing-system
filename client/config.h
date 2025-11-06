#ifndef CONFIG_H
#define CONFIG_H

#include <bits/stdc++.h>
#include <fstream>
#include <sstream>

using namespace std;

class Config {
private:
    unordered_map<string, string> values;

    // Default configuration values
    void setDefaults() {
        values["chunk_size"] = "1024";
        values["thread_pool_size"] = "15";
        values["peer_query_threads"] = "10";
        values["sha_type"] = "sha256";
        values["max_download_attempts"] = "3";
        values["peer_query_timeout_ms"] = "5000";
    }

    // Trim whitespace from string
    string trim(const string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        size_t last = str.find_last_not_of(" \t\r\n");
        if (first == string::npos) return "";
        return str.substr(first, (last - first + 1));
    }

public:
    Config() {
        setDefaults();
    }

    // Load configuration from file
    bool loadFromFile(const string& filename) {
        // Try multiple path variations
        vector<string> paths_to_try = {
            filename,
            "config/" + filename,
            "../config/" + filename,
            "../../config/" + filename
        };

        ifstream file;
        string successful_path;
        
        for (const auto& path : paths_to_try) {
            file.open(path);
            if (file.is_open()) {
                successful_path = path;
                break;
            }
        }

        if (!file.is_open()) {
            cerr << "[Config] Warning: Could not open config file: " << filename << endl;
            cerr << "[Config] Tried paths: ";
            for (const auto& p : paths_to_try) cerr << p << ", ";
            cerr << endl;
            cerr << "[Config] Using default values" << endl;
            return false;
        }

        string line;
        int line_num = 0;
        while (getline(file, line)) {
            line_num++;
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;

            // Find the delimiter
            size_t delimiter_pos = line.find('=');
            if (delimiter_pos == string::npos) {
                cerr << "[Config] Warning: Invalid line format at line " << line_num << ": " << line << endl;
                continue;
            }

            // Extract key and value
            string key = trim(line.substr(0, delimiter_pos));
            string value = trim(line.substr(delimiter_pos + 1));

            if (key.empty() || value.empty()) {
                cerr << "[Config] Warning: Empty key or value at line " << line_num << endl;
                continue;
            }

            values[key] = value;
            cout << "[Config] Loaded: " << key << " = " << value << endl;
        }

        file.close();
        cout << "[Config] Configuration loaded successfully from " << successful_path << endl;
        return true;
    }

    // Get integer configuration value
    int getInt(const string& key) const {
        auto it = values.find(key);
        if (it != values.end()) {
            try {
                return stoi(it->second);
            } catch (const exception& e) {
                cerr << "[Config] Error converting '" << key << "' to int: " << e.what() << endl;
                return stoi(values.at(key)); // fallback to default
            }
        }
        cerr << "[Config] Key not found: " << key << endl;
        return -1;
    }

    // Get string configuration value
    string getString(const string& key) const {
        auto it = values.find(key);
        if (it != values.end()) {
            return it->second;
        }
        cerr << "[Config] Key not found: " << key << endl;
        return "";
    }

    // Get long long configuration value
    long long getLongLong(const string& key) const {
        auto it = values.find(key);
        if (it != values.end()) {
            try {
                return stoll(it->second);
            } catch (const exception& e) {
                cerr << "[Config] Error converting '" << key << "' to long long: " << e.what() << endl;
                return -1;
            }
        }
        cerr << "[Config] Key not found: " << key << endl;
        return -1;
    }

    // Display all configuration values
    void printConfig() const {
        cout << "\n=== Current Configuration ===" << endl;
        for (const auto& pair : values) {
            cout << pair.first << " = " << pair.second << endl;
        }
        cout << "============================\n" << endl;
    }

    // Check if key exists
    bool hasKey(const string& key) const {
        return values.find(key) != values.end();
    }
};

#endif
