#define RAPIDJSON_HAS_STDSTRING 1

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;
using namespace std;

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")

static unsigned short SERVER_PORT = 27015;

vector<string> split(string str, char Delimiter) {
    istringstream iss(str);
    string buffer;

    vector<string> result;

    while (getline(iss, buffer, Delimiter)) {
        result.push_back(buffer);
    }

    return result;
}

int recvThreadProc(SOCKET sock) {
    while (true) {
        int r = 0;
        int dataLenNetByteOrder;
        int offset = 0;
        while (offset < 4) {
            r = recv(sock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
                return 1;
            }
            else if (r == 0) {
                std::cerr << "socket closed while reading length" << std::endl;
                return 1;
            }
            offset += r;
        }
        int dataLen = ntohl(dataLenNetByteOrder);

        shared_ptr<char> buf(new char[dataLen]);
        offset = 0;
        while (offset < dataLen) {
            r = recv(sock, buf.get() + offset, dataLen - offset, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
                return 1;
            }
            else if (r == 0) {
                std::cerr << "socket closed while reading data" << std::endl;
                return 1;
            }
            offset += r;
        }
        Document d;
        d.Parse(buf.get());
        Value& command = d["command"];
        Value& argument = d["argument"];
        if (command.IsString() == true && strcmp(command.GetString(), "printAttack") == 0) {
            if (argument.IsArray() == true && argument.GetArray().Size() == 3) {
                string arg[3];
                int i = 0;
                for (auto& v : argument.GetArray()) {
                    if (v.IsString()) {
                        arg[i] = v.GetString();
                        i++;
                    }
                    else {
                        cout << "data is not string\n";
                    }
                }
                cout << "\"" << arg[0] << "\" 이 \"" << arg[1] << "\" 을 공격해서 데미지 " << arg[2] << "을 가했습니다.\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printGameOver") == 0) {
            if (argument.IsArray() == true && argument.GetArray().Size() == 0) {
                cout << "GameOver\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printGetItem") == 0) {
            if (argument.IsArray() == true && argument.GetArray().Size() == 2) {
                string arg[2];
                int i = 0;
                for (auto& v : argument.GetArray()) {
                    if (v.IsString()) {
                        arg[i] = v.GetString();
                        i++;
                    }
                    else {
                        cout << "data is not string\n";
                    }
                }
                cout << arg[0] << " 포션 을 " << arg[1] << "개 얻었습니다.\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printMonsters") == 0) {
            if (argument.IsArray() == true) {
                for (auto& v : argument.GetArray()) {
                    if (v.IsString()) {
                        cout << v.GetString() << " ";
                    }
                    else {
                        cout << "data is not string\n";
                    }
                }
                cout << "\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printUsers") == 0) {
            if (argument.IsArray() == true) {
                for (auto& v : argument.GetArray()) {
                    if (v.IsArray()) {
                        for (auto& e : v.GetArray()) {
                            cout << e.GetString() << " ";
                        }
                    }
                    else {
                        cout << "data is not array\n";
                    }
                    cout << ", ";
                }
                cout << "\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printNoUser") == 0) {
            if (argument.IsArray() == true) {
                cout << "이러한 이름을 가진 유저는 없습니다.\n";
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "printMessage") == 0) {
            if (argument.IsArray() == true && argument.GetArray().Size() == 1) {
                for (auto& v : argument.GetArray()) {
                    cout << v.GetString() << "\n";
                }
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else if (command.IsString() == true && strcmp(command.GetString(), "print") == 0) {
            if (argument.IsArray() == true && argument.GetArray().Size() == 1) {
                for (auto& v : argument.GetArray()) {
                    cout << v.GetString() << "\n";
                }
            }
            else {
                cout << "argument is invalid\n";
            }
        }
        else {
            cout << "command is wrong\n";
        }
    }
    return 0;
}

int sendJsonString(SOCKET sock, Document& d, string command, Value& argument) {
    // DOM 내용을 생성한다.
    d.SetObject();
    d.AddMember("command", command, d.GetAllocator());
    d.AddMember("argument", argument, d.GetAllocator());

    // DOM 내용을 문자열로 변경한다.
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    d.Accept(writer);

    int r = 0;
    int dataLen = strlen(buffer.GetString()) + 1;

    // 길이를 먼저 보낸다.
    // binary 로 4bytes 를 길이로 encoding 한다.
    // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        r = send(sock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
    }

    // send 로 데이터를 보낸다.
    offset = 0;
    while (offset < dataLen) {
        r = send(sock, buffer.GetString() + offset, dataLen - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
    }

    return 0;
}

int bot(SOCKET sock) {
    while (true) {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<int> dis(0, 5);
        uniform_int_distribution<int> disXY(-29, 3);
        Document d;
        Value a(kArrayType);
        switch (dis(gen)) {
        case 0:
            a.PushBack(disXY(gen), d.GetAllocator());
            a.PushBack(disXY(gen), d.GetAllocator());
            if (sendJsonString(sock, d, "move", a) == 1) {
                return 1;
            }
            break;
        case 1:
            if (sendJsonString(sock, d, "attack", a) == 1) {
                return 1;
            }
            break;
        case 2:
            if (sendJsonString(sock, d, "monsters", a) == 1) {
                return 1;
            }
            break;
        case 3:
            if (sendJsonString(sock, d, "chatBot", a) == 1) {
                return 1;
            }
            break;
        case 4:
            a.PushBack("hp", d.GetAllocator());
            if (sendJsonString(sock, d, "potion", a) == 1) {
                return 1;
            }
            break;
        case 5:
            a.PushBack("str", d.GetAllocator());
            if (sendJsonString(sock, d, "potion", a) == 1) {
                return 1;
            }
            break;
        default:
            cout << "random error\n";
            break;
        }

        Sleep(1000);
    }
    return 0;
}

int main() {
    int r = 0;

    // Winsock 을 초기화한다.
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        std::cerr << "WSAStartup failed with error " << r << std::endl;
        return 1;
    }

    struct sockaddr_in serverAddr;

    // TCP socket 을 만든다.
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TCP 는 연결 기반이다. 서버 주소를 정하고 connect() 로 연결한다.
    // connect 후에는 별도로 서버 주소를 기재하지 않고 send/recv 를 한다.
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    r = connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "connect failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    string userId;
    while (userId == "") {
        cout << "유저 이름을 입력해주세요\n";
        getline(cin, userId);
    }

    thread recvThread(recvThreadProc, sock);

    Document userDoc;
    Value userArrVal(kArrayType);
    Value userIdVal;
    userIdVal.SetString(userId.c_str(), userId.length(), userDoc.GetAllocator());
    userArrVal.PushBack(userIdVal, userDoc.GetAllocator());
    sendJsonString(sock, userDoc, "login", userArrVal);

    while (true) {
        string input;

        getline(cin, input);

        vector<string> command = split(input, ' ');
        if (command.empty() == true) {
            cout << "명령어가 틀렸습니다.\n";
        }
        else if ((command[0] == "move") && (command.size() == 3)) {
            try {
                int x = stoi(command[1]);
                int y = stoi(command[2]);

                if (x > 3 || y > 3) {
                    cout << "명령어가 틀렸습니다.\n";
                    continue;
                }
                Document d;
                Value a(kArrayType);
                a.PushBack(x, d.GetAllocator());
                a.PushBack(y, d.GetAllocator());
                if (sendJsonString(sock, d, command[0], a) == 1) {
                    return 1;
                }
            }
            catch (const std::invalid_argument& e) {
                cout << "명령어가 틀렸습니다.\n";
            }
            catch (const std::out_of_range& e) {
                cout << "명령어가 틀렸습니다.\n";
            }
        }
        else if ((command[0] == "attack") && (command.size() == 1)) {
            Document d;
            Value a(kArrayType);
            if (sendJsonString(sock, d, command[0], a) == 1) {
                return 1;
            }
        }
        else if ((command[0] == "monsters") && (command.size() == 1)) {
            Document d;
            Value a(kArrayType);
            if (sendJsonString(sock, d, command[0], a) == 1) {
                return 1;
            }
        }
        else if ((command[0] == "users") && (command.size() == 1)) {
            Document d;
            Value a(kArrayType);
            if (sendJsonString(sock, d, command[0], a) == 1) {
                return 1;
            }
        }
        else if ((command[0] == "chat") && (command.size() == 3)) {
            Document d;
            Value a(kArrayType);
            Value user;
            user.SetString(command[1].c_str(), command[1].length(), d.GetAllocator());
            Value content;
            content.SetString(command[2].c_str(), command[2].length(), d.GetAllocator());
            a.PushBack(user, d.GetAllocator());
            a.PushBack(content, d.GetAllocator());
            if (sendJsonString(sock, d, command[0], a) == 1) {
                return 1;
            }
        }
        else if ((command[0] == "bot") && (command.size() == 1)) {
            if (bot(sock) == 1) {
                return 1;
            };
        }
        else if ((command[0] == "potion") && (command.size() == 2)) {
            Document d;
            Value a(kArrayType);
            Value kind;
            kind.SetString(command[1].c_str(), command[1].length(), d.GetAllocator());
            a.PushBack(kind, d.GetAllocator());
            if (sendJsonString(sock, d, command[0], a) == 1) {
                return 1;
            }
        }
        else {
            cout << "명령어가 틀렸습니다.\n";
        }
    }

    // thread를 join한다.
    recvThread.join();

    // Socket 을 닫는다.
    r = closesocket(sock);
    if (r == SOCKET_ERROR) {
        std::cerr << "closesocket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}