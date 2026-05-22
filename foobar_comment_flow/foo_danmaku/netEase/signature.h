#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <windows.h>
#include <string>

class Signature {
public:
    static std::string encrypt(const std::string& data);
    static std::string md5(const std::string& input);
    static std::string generateNonce(int length = 16);
};

#endif // SIGNATURE_H