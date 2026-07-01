#pragma once
#include <winsock2.h>
#include <ws2bth.h>
#include <cstdint>

class BluetoothServer {
public:
    BluetoothServer() = default;
    ~BluetoothServer();

    bool Start();
    void Stop();
    bool WaitForClient();
    bool Send(const uint8_t* Data, uint32_t Size);
    void DropClient();

private:
    bool RegisterService();
    void UnregisterService();

    SOCKET ListenSocket = INVALID_SOCKET;
    SOCKET ClientSocket = INVALID_SOCKET;
    GUID ServiceGuid{};
    HANDLE ServiceLookupHandle = nullptr;
    bool WinsockInitialized = false;
};
