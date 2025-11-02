#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>

// File structure for tracking downloaded files
class FilesStructure {
public:
    std::string file_name;
    std::string file_path;
    std::string sha;
    long long int total_chunks;
    long long int total_size;
    std::vector<std::string> chunks_I_have;
    long long int no_of_chunks_I_have;
};

inline std::vector<std::string> tokenize(const std::string &s) {
    std::vector<std::string> tokens;
    std::stringstream ss(s);
    std::string temp;
    while (ss >> temp)
        tokens.push_back(temp);
    return tokens;
}

#endif // DATA_STRUCTURES_H