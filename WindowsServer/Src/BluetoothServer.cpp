#include "BluetoothServer.h"
#include <bluetoothapis.h>

static const GUID SppServiceClassGuid = { 0x00001101, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

BluetoothServer::~BluetoothServer() {
    Stop();
}

bool BluetoothServer::Start() {
    WSADATA WsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0) return false;
    WinsockInitialized = true;

    ListenSocket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (ListenSocket == INVALID_SOCKET) return false;

    ServiceGuid = SppServiceClassGuid;

    SOCKADDR_BTH LocalAddr{};
    LocalAddr.addressFamily = AF_BTH;
    LocalAddr.btAddr = 0;
    LocalAddr.serviceClassId = ServiceGuid;
    LocalAddr.port = BT_PORT_ANY;

    if (bind(ListenSocket, reinterpret_cast<SOCKADDR*>(&LocalAddr), sizeof(LocalAddr)) == SOCKET_ERROR) {
        return false;
    }

    if (listen(ListenSocket, 1) == SOCKET_ERROR) return false;

    return RegisterService();
}

bool BluetoothServer::RegisterService() {
    SOCKADDR_BTH BoundAddr{};
    int AddrLen = sizeof(BoundAddr);
    if (getsockname(ListenSocket, reinterpret_cast<SOCKADDR*>(&BoundAddr), &AddrLen) == SOCKET_ERROR) {
        return false;
    }

    static CSADDR_INFO CsAddr{};
    static SOCKADDR_BTH LocalCopy = BoundAddr;
    CsAddr.LocalAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&LocalCopy);
    CsAddr.LocalAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);
    CsAddr.iSocketType = SOCK_STREAM;
    CsAddr.iProtocol = BTHPROTO_RFCOMM;

    WSAQUERYSETW QuerySet{};
    QuerySet.dwSize = sizeof(WSAQUERYSETW);
    QuerySet.lpServiceClassId = const_cast<GUID*>(&ServiceGuid);
    QuerySet.lpszServiceInstanceName = const_cast<LPWSTR>(L"BluShareServer");
    QuerySet.dwNameSpace = NS_BTH;
    QuerySet.dwNumberOfCsAddrs = 1;
    QuerySet.lpcsaBuffer = &CsAddr;

    int Result = WSASetServiceW(&QuerySet, RNRSERVICE_REGISTER, 0);
    return Result == 0;
}

void BluetoothServer::UnregisterService() {
    WSAQUERYSETW QuerySet{};
    QuerySet.dwSize = sizeof(WSAQUERYSETW);
    QuerySet.lpServiceClassId = const_cast<GUID*>(&ServiceGuid);
    WSASetServiceW(&QuerySet, RNRSERVICE_DELETE, 0);
}

bool BluetoothServer::WaitForClient() {
    ClientSocket = accept(ListenSocket, nullptr, nullptr);
    if (ClientSocket == INVALID_SOCKET) return false;

    int SendBufSize = 4096;
    setsockopt(ClientSocket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&SendBufSize), sizeof(SendBufSize));

    return true;
}

bool BluetoothServer::Send(const uint8_t* Data, uint32_t Size) {
    if (ClientSocket == INVALID_SOCKET) return false;
    uint32_t Sent = 0;
    while (Sent < Size) {
        int Result = send(ClientSocket, reinterpret_cast<const char*>(Data + Sent), static_cast<int>(Size - Sent), 0);
        if (Result <= 0) return false;
        Sent += static_cast<uint32_t>(Result);
    }
    return true;
}

void BluetoothServer::DropClient() {
    if (ClientSocket != INVALID_SOCKET) {
        closesocket(ClientSocket);
        ClientSocket = INVALID_SOCKET;
    }
}

void BluetoothServer::Stop() {
    DropClient();
    UnregisterService();
    if (ListenSocket != INVALID_SOCKET) {
        closesocket(ListenSocket);
        ListenSocket = INVALID_SOCKET;
    }
    if (WinsockInitialized) {
        WSACleanup();
        WinsockInitialized = false;
    }
}
