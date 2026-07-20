#ifndef TESTS_NATIVE_STUBS_TRANSFER_HTTP_H
#define TESTS_NATIVE_STUBS_TRANSFER_HTTP_H

#include <cstddef>
#include <string>

class Http {
public:
    virtual ~Http() = default;
    virtual void SetTimeout(int timeout_ms) = 0;
    virtual void SetHeader(const std::string& key, const std::string& value) = 0;
    virtual void SetContent(std::string&& content) = 0;
    virtual void SetKeepAlive(bool enable) = 0;
    virtual bool Open(const std::string& method, const std::string& url) = 0;
    virtual void Close() = 0;
    virtual int Read(char* buffer, std::size_t buffer_size) = 0;
    virtual int Write(const char* buffer, std::size_t buffer_size) = 0;
    virtual int GetStatusCode() = 0;
    virtual std::string GetResponseHeader(const std::string& key) const = 0;
    virtual std::size_t GetBodyLength() = 0;
    virtual std::string ReadAll() = 0;
    virtual int GetLastError() = 0;
};

#endif  // TESTS_NATIVE_STUBS_TRANSFER_HTTP_H
