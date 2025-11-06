#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "config.h"

using namespace std;

// FileHasher: computes SHA256 for each chunk and the full file
// Chunk size is read from config (default: 1024 bytes)
class FileHasher {
private:
    Config& config;
    
public:
    string filePath; 
    vector<string> hashValues;

    FileHasher(const string& file, Config& cfg) : filePath(file), config(cfg) {}

    void calculateHashValues() {
        int fd = open(filePath.c_str(), O_RDONLY);
        if (fd < 0) {
            cerr << "Wrong file path" << endl;
            return;
        }

        int chunkSize = config.getInt("chunk_size");
        vector<unsigned char> chunkBuffer(chunkSize);
        unsigned char hashValue[EVP_MAX_MD_SIZE];  
        unsigned int hashLength;  

        EVP_MD_CTX* mdContext = EVP_MD_CTX_new();
        if (mdContext == nullptr) {
            cerr << "Error creating EVP context" << endl;
            close(fd);
            return;
        }

        const EVP_MD* md = EVP_sha256(); 
        if (EVP_DigestInit_ex(mdContext, md, nullptr) != 1) {
            cerr << "Error initializing SHA256" << endl;
            EVP_MD_CTX_free(mdContext);
            close(fd);
            return;
        }

        ssize_t bytesRead;
        while ((bytesRead = read(fd, chunkBuffer.data(), chunkSize)) > 0) {
            if (EVP_DigestUpdate(mdContext, chunkBuffer.data(), bytesRead) != 1) {
                cerr << "Error updating SHA256 with chunk data" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return;
            }

            if (EVP_DigestFinal_ex(mdContext, hashValue, &hashLength) != 1) {
                cerr << "Error finalizing SHA256 for chunk" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return;
            }

            hashValues.push_back(hashToString(hashValue, hashLength));  
            
            if (EVP_DigestInit_ex(mdContext, md, nullptr) != 1) {
                cerr << "Error reinitializing SHA256 for next chunk" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return;
            }
        }

        if (lseek(fd, 0, SEEK_SET) == -1) {
            cerr << "Error seeking back to the start of the file" << endl;
            EVP_MD_CTX_free(mdContext);
            close(fd);
            return;
        }

        while ((bytesRead = read(fd, chunkBuffer.data(), chunkSize)) > 0) {
            if (EVP_DigestUpdate(mdContext, chunkBuffer.data(), bytesRead) != 1) {
                cerr << "Error updating SHA256 with entire file data" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return;
            }
        }

        if (EVP_DigestFinal_ex(mdContext, hashValue, &hashLength) != 1) {
            cerr << "Error finalizing SHA256 for entire file" << endl;
            EVP_MD_CTX_free(mdContext);
            close(fd);
            return;
        }

        hashValues.push_back(hashToString(hashValue, hashLength));

        EVP_MD_CTX_free(mdContext);
        close(fd);
    }

    static string hashToString(unsigned char* hash, unsigned int hashLength) {
        stringstream ss;
        for (unsigned int i = 0; i < hashLength; i++) {
            ss << hex << setw(2) << setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    vector<string> getChunkHashes() {
        cout<<"Total hashes calculated: "<<hashValues.size()<<endl;
        return hashValues;
    }

    string getCompleteFileHash() {
        if (!hashValues.empty()) {
            return hashValues.back();  
        }
        return "";
    }
};

string sha256(const string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLength;
    EVP_MD_CTX* mdContext = EVP_MD_CTX_new();
    if (mdContext == nullptr) {
        cerr << "Error creating EVP context" << endl;
        return "";
    }

    const EVP_MD* md = EVP_sha256();  
    if (EVP_DigestInit_ex(mdContext, md, nullptr) != 1) {
        cerr << "Error initializing SHA256" << endl;
        EVP_MD_CTX_free(mdContext);
        return "";
    }

    if (EVP_DigestUpdate(mdContext, input.c_str(), input.length()) != 1) {
        cerr << "Error updating SHA256 with input data" << endl;
        EVP_MD_CTX_free(mdContext);
        return "";
    }

    if (EVP_DigestFinal_ex(mdContext, hash, &hashLength) != 1) {
        cerr << "Error finalizing SHA256" << endl;
        EVP_MD_CTX_free(mdContext);
        return "";
    }

    EVP_MD_CTX_free(mdContext);

    return FileHasher::hashToString(hash, hashLength);
}