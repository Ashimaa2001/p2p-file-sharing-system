#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>

using namespace std;

// Compute SHA256 per 1024-byte chunk and the overall file hash
class FileHasher {
public:
    string filePath; 
    vector<string> hashValues;

    FileHasher(const string& file) : filePath(file) {}

    void calculateHashValues() {
        int fd = open(filePath.c_str(), O_RDONLY);
        if (fd < 0) {
            cerr << "Wrong file path" << endl;
            return;
        }

        size_t chunkSize = 1024;  // chunk size in bytes
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

            // finalize chunk hash
            if (EVP_DigestFinal_ex(mdContext, hashValue, &hashLength) != 1) {
                cerr << "Error finalizing SHA256 for chunk" << endl;
                EVP_MD_CTX_free(mdContext);
                close(fd);
                return;
            }

            hashValues.push_back(hashToString(hashValue, hashLength));  
        }

        if (EVP_DigestFinal_ex(mdContext, hashValue, &hashLength) != 1) {
            cerr << "Error finalizing SHA256 for entire file" << endl;
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
