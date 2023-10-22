#define RAPIDJSON_HAS_STDSTRING 1

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <hiredis/hiredis.h>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>

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


static const unsigned short SERVER_PORT = 27015;
static const unsigned short SERVER_PORT_HTTP = 27016;
static const int NUM_WORKER_THREADS = 30;
static const int DUNGEON_HEIGHT = 30;
static const int DUNGEON_WIDTH = 30;
static const int NUM_GEN_MONSTERS = 10;
static const int GEN_TIME = 1000 * 60; // ms 단위
static const int ATTACK_TIME = 1000 * 5; // ms 단위


class Client {
public:
    SOCKET sock;  // 이 클라이언트의 active socket

    unsigned short serverPort; // 이 클라이언트가 접속한 서버의 포트

    atomic<bool> doingRecv;

    bool lenCompleted;
    int packetLen;
    char* packet;
    int offset;

    redisContext* c;
    string userId;

    Client(SOCKET sock, unsigned short serverPort, redisContext* c) : sock(sock), serverPort(serverPort), doingRecv(false), lenCompleted(false), packetLen(0), packet(nullptr), offset(0), c(c), userId("") {

    }

    ~Client() {
        if (packet != nullptr) {
            delete[] packet;
        }
        redisReply* exist = (redisReply*)redisCommand(c, "get USER:%s", userId.c_str());
        if (exist->type == REDIS_REPLY_STRING) {
            freeReplyObject(exist);
            redisReply* logout = (redisReply*)redisCommand(c, "set USER:%s 0", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error1\n";
            }
            freeReplyObject(logout);
            logout = (redisReply*)redisCommand(c, "expire USER:%s 300", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error2\n";
            }
            freeReplyObject(logout);
            logout = (redisReply*)redisCommand(c, "expire USER:%s:pos 300", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error3\n";
            }
            freeReplyObject(logout);
            logout = (redisReply*)redisCommand(c, "expire USER:%s:hp 300", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error4\n";
            }
            freeReplyObject(logout);
            logout = (redisReply*)redisCommand(c, "expire USER:%s:str 300", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error5\n";
            }
            freeReplyObject(logout);
            logout = (redisReply*)redisCommand(c, "expire USER:%s:potions 300", userId.c_str());
            if (logout->type == REDIS_REPLY_ERROR) {
                cout << "logout error6\n";
            }
            freeReplyObject(logout);
        }
        else if (exist->type == REDIS_REPLY_NIL) {
            // nothing to do
        }
        else {
            cout << "logout error1\n";
        }
        cout << "Client destroyed. Socket: " << sock << endl;
    }
};


// 소켓으로부터 Client 객체 포인터를 얻어내기 위한 map
// 소켓을 key 로 Client 객체 포인터를 value 로 집어넣는다. (shared_ptr 을 사용한다.)
// 나중에 key 인 소켓으로 찾으면 연결된 Client 객체 포인터가 나온다.
// key 인 소켓으로 지우면 해당 엔트리는 사라진다.
// key 목록은 소켓 목록이므로 현재 남아있는 소켓들이라고 생각할 수 있다.
map<SOCKET, shared_ptr<Client> > activeClients;
mutex activeClientsMutex;

// 패킷이 도착한 client 들의 큐
queue<shared_ptr<Client> > jobQueue;
mutex jobQueueMutex;
condition_variable jobQueueFilledCv;


SOCKET createPassiveSocket(const unsigned short& port) {
    // TCP socket 을 만든다.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        cerr << "socket failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // socket 을 특정 주소, 포트에 바인딩 한다.
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int r = bind(passiveSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        cerr << "bind failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // TCP 는 연결을 받는 passive socket 과 실제 통신을 할 수 있는 active socket 으로 구분된다.
    // passive socket 은 socket() 뒤에 listen() 을 호출함으로써 만들어진다.
    // active socket 은 passive socket 을 이용해 accept() 를 호출함으로써 만들어진다.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        cerr << "listen faijled with error " << WSAGetLastError() << endl;
        return 1;
    }

    return passiveSock;
}

int sendErrorHttp(SOCKET sock) {
    int r = 0;
    string sendText = "HTTP/1.1 404 Not Found\r\n\r\n";

    // send 로 데이터를 보낸다.
    int offset = 0;
    while (offset < sendText.length()) {
        r = send(sock, sendText.c_str() + offset, sendText.length() - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
    }

    return 0;
}

int sendJsonStringHttp(SOCKET sock, Document& d, string command, Value& argument) {
    // DOM 내용을 생성한다.
    d.SetObject();
    d.AddMember("command", command, d.GetAllocator());
    d.AddMember("argument", argument, d.GetAllocator());

    // DOM 내용을 문자열로 변경한다.
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    d.Accept(writer);

    int r = 0;
    string sendText = "HTTP/1.1 200 OK\r\n";
    sendText += "Content-Type: application/json; charset=utf-8\r\n";
    sendText += "Content-Length: " + to_string(strlen(buffer.GetString())) + "\r\n";
    sendText += "\r\n";
    sendText += buffer.GetString();

    // send 로 데이터를 보낸다.
    int offset = 0;
    while (offset < sendText.length()) {
        r = send(sock, sendText.c_str() + offset, sendText.length() - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
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

int sendJsonStringToAll(Document& d, string command, Value& argument) {
    // DOM 내용을 생성한다.
    d.SetObject();
    d.AddMember("command", command, d.GetAllocator());
    d.AddMember("argument", argument, d.GetAllocator());

    // DOM 내용을 문자열로 변경한다.
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    d.Accept(writer);
    {
        lock_guard<mutex> lg(activeClientsMutex);
        for (auto& entry : activeClients) {
            SOCKET sock = entry.first;
            shared_ptr<Client> client = entry.second;
            if (client->userId != "" && client->serverPort == SERVER_PORT) {
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
            }
        }
    }
    return 0;
}

int printAttack(string& attack, string& hit, string& damage) {
    Document d;
    Value a(kArrayType);
    Value attackV;
    attackV.SetString(attack.c_str(), attack.length(), d.GetAllocator());
    Value hitV;
    hitV.SetString(hit.c_str(), hit.length(), d.GetAllocator());
    Value damageV;
    damageV.SetString(damage.c_str(), damage.length(), d.GetAllocator());
    a.PushBack(attackV, d.GetAllocator());
    a.PushBack(hitV, d.GetAllocator());
    a.PushBack(damageV, d.GetAllocator());
    if (sendJsonStringToAll(d, "printAttack", a) == 1) {
        return 1;
    }

    return 0;
}

vector<string> split(string str, char Delimiter) {
    istringstream iss(str);
    string buffer;

    vector<string> result;

    while (getline(iss, buffer, Delimiter)) {
        result.push_back(buffer);
    }

    return result;
}

// pos size 는 2
bool move(shared_ptr<Client> client, int pos[]) {
    redisReply* move = (redisReply*)redisCommand(client->c, "hincrby USER:%s:pos x %d", client->userId.c_str(), pos[0]);
    if (move->type == REDIS_REPLY_ERROR) {
        cout << "falied to" << client->userId << " move x\n";
        return false;
    }
    freeReplyObject(move);
    move = (redisReply*)redisCommand(client->c, "hincrby USER:%s:pos y %d", client->userId.c_str(), pos[1]);
    if (move->type == REDIS_REPLY_ERROR) {
        cout << "falied to" << client->userId << " move y\n";
        return false;
    }
    freeReplyObject(move);
    move = (redisReply*)redisCommand(client->c, "hget USER:%s:pos x", client->userId.c_str());
    if (move->type == REDIS_REPLY_STRING) {
        try {
            int x = stoi(move->str);
            if (x >= 30) {
                redisReply* replace = (redisReply*)redisCommand(client->c, "hset USER:%s:pos x 29", client->userId.c_str());
                if (replace->type == REDIS_REPLY_ERROR) {
                    cout << "falied to" << client->userId << " move x\n";
                    return false;
                }
                freeReplyObject(replace);
            }
            else if (x < 0) {
                redisReply* replace = (redisReply*)redisCommand(client->c, "hset USER:%s:pos x 0", client->userId.c_str());
                if (replace->type == REDIS_REPLY_ERROR) {
                    cout << "falied to" << client->userId << " move x\n";
                    return false;
                }
                freeReplyObject(replace);
            }
        }
        catch (const std::invalid_argument& e) {
            cout << "falied to" << client->userId << " move y\n";
            return false;
        }
        catch (const std::out_of_range& e) {
            cout << "falied to" << client->userId << " move y\n";
            return false;
        }
    }
    else {
        cout << "falied to" << client->userId << " move x\n";
        return false;
    }
    freeReplyObject(move);
    move = (redisReply*)redisCommand(client->c, "hget USER:%s:pos y", client->userId.c_str());
    if (move->type == REDIS_REPLY_STRING) {
        try {
            int y = stoi(move->str);
            if (y >= 30) {
                redisReply* replace = (redisReply*)redisCommand(client->c, "hset USER:%s:pos y 29", client->userId.c_str());
                if (replace->type == REDIS_REPLY_ERROR) {
                    cout << "falied to" << client->userId << " move y\n";
                    return false;
                }
                freeReplyObject(replace);
            }
            else if (y < 0) {
                redisReply* replace = (redisReply*)redisCommand(client->c, "hset USER:%s:pos y 0", client->userId.c_str());
                if (replace->type == REDIS_REPLY_ERROR) {
                    cout << "falied to" << client->userId << " move y\n";
                    return false;
                }
                freeReplyObject(replace);
            }
        }
        catch (const std::invalid_argument& e) {
            cout << "falied to" << client->userId << " move y\n";
            return false;
        }
        catch (const std::out_of_range& e) {
            cout << "falied to" << client->userId << " move y\n";
            return false;
        }
    }
    else {
        cout << "falied to" << client->userId << " move y\n";
        return false;
    }
    freeReplyObject(move);
    return true;
}

bool attack(shared_ptr<Client> client) {
    int x;
    int y;

    redisReply* attack = (redisReply*)redisCommand(client->c, "hget USER:%s:pos x", client->userId.c_str());
    if (attack->type == REDIS_REPLY_STRING) {
        try {
            x = stoi(attack->str);
        }
        catch (const std::invalid_argument& e) {
            cout << "falied to" << client->userId << " attack\n";
            return false;
        }
        catch (const std::out_of_range& e) {
            cout << "falied to" << client->userId << " attack\n";
            return false;
        }
    }
    else {
        cout << "falied to" << client->userId << " attack\n";
        return false;
    }
    freeReplyObject(attack);
    attack = (redisReply*)redisCommand(client->c, "hget USER:%s:pos y", client->userId.c_str());
    if (attack->type == REDIS_REPLY_STRING) {
        try {
            y = stoi(attack->str);
        }
        catch (const std::invalid_argument& e) {
            cout << "falied to" << client->userId << " attack\n";
            return false;
        }
        catch (const std::out_of_range& e) {
            cout << "falied to" << client->userId << " attack\n";
            return false;
        }
    }
    else {
        cout << "falied to" << client->userId << " attack\n";
        return false;
    }
    freeReplyObject(attack);

    for (int i = 0; i < NUM_GEN_MONSTERS; i++) {
        bool resultX = false;
        bool resultY = false;
        redisReply* isMonster = (redisReply*)redisCommand(client->c, "get MONSTER:%d", i);
        if (isMonster->type == REDIS_REPLY_STRING) {
            if (strcmp(isMonster->str, "1") == 0) {
                attack = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:pos x", i);
                if (attack->type == REDIS_REPLY_STRING) {
                    try {
                        int monsterX = stoi(attack->str);
                        if (monsterX >= x - 1 && monsterX <= x + 1) {
                            resultX = true;
                        }
                    }
                    catch (const std::invalid_argument& e) {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                    catch (const std::out_of_range& e) {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                }
                else {
                    cout << "falied to" << client->userId << " attack\n";
                    return false;
                }
                freeReplyObject(attack);
                attack = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:pos y", i);
                if (attack->type == REDIS_REPLY_STRING) {
                    try {
                        int monsterY = stoi(attack->str);
                        if (monsterY >= y - 1 && monsterY <= y + 1) {
                            resultY = true;
                        }
                    }
                    catch (const std::invalid_argument& e) {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                    catch (const std::out_of_range& e) {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                }
                else {
                    cout << "falied to" << client->userId << " attack\n";
                    return false;
                }
                freeReplyObject(attack);

                if (resultX && resultY) {
                    string attacker = client->userId;
                    string hit = "슬라임";
                    string damage;
                    attack = (redisReply*)redisCommand(client->c, "get USER:%s:str", attacker.c_str());
                    if (attack->type == REDIS_REPLY_STRING) {
                        damage = attack->str;
                    }
                    else if (attack->type == REDIS_REPLY_NIL) {
                        damage = "3";
                    }
                    else {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                    freeReplyObject(attack);
                    attack = (redisReply*)redisCommand(client->c, "decrby MONSTER:%d:hp %s", i, damage.c_str());
                    if (attack->type == REDIS_REPLY_ERROR) {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                    else {
                        printAttack(attacker, hit, damage);
                    }
                    freeReplyObject(attack);
                    attack = (redisReply*)redisCommand(client->c, "get MONSTER:%d:hp", i);
                    if (attack->type == REDIS_REPLY_STRING) {
                        try {
                            if (stoi(attack->str) <= 0) {
                                redisReply* monster = (redisReply*)redisCommand(client->c, "del MONSTER:%d", i);
                                if (monster->type == REDIS_REPLY_ERROR) {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "del MONSTER:%d:pos", i);
                                if (monster->type == REDIS_REPLY_ERROR) {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "del MONSTER:%d:hp", i);
                                if (monster->type == REDIS_REPLY_ERROR) {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "del MONSTER:%d:str", i);
                                if (monster->type == REDIS_REPLY_ERROR) {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:potions hp", i);
                                if (monster->type == REDIS_REPLY_STRING) {
                                    redisReply* user = (redisReply*)redisCommand(client->c, "hincrby USER:%d:potions hp %d", client->userId.c_str(), monster->str);
                                    if (user->type == REDIS_REPLY_ERROR) {
                                        return false;
                                    }
                                    freeReplyObject(user);
                                    string s = monster->str;
                                    Document d;
                                    Value a(kArrayType);
                                    Value num;
                                    num.SetString(s.c_str(), s.length(), d.GetAllocator());
                                    a.PushBack("hp", d.GetAllocator());
                                    a.PushBack(num, d.GetAllocator());
                                    if (client->serverPort == SERVER_PORT) {
                                        if (sendJsonString(client->sock, d, "printGetItem", a) == 1) {
                                            return false;
                                        }
                                    }
                                }
                                else {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:potions str", i);
                                if (monster->type == REDIS_REPLY_STRING) {
                                    redisReply* user = (redisReply*)redisCommand(client->c, "hincrby USER:%d:potions str %d", client->userId.c_str(), monster->str);
                                    if (user->type == REDIS_REPLY_ERROR) {
                                        return false;
                                    }
                                    freeReplyObject(user);
                                    string s = monster->str;
                                    Document d;
                                    Value a(kArrayType);
                                    Value num;
                                    num.SetString(s.c_str(), s.length(), d.GetAllocator());
                                    a.PushBack("str", d.GetAllocator());
                                    a.PushBack(num, d.GetAllocator());
                                    if (client->serverPort == SERVER_PORT) {
                                        if (sendJsonString(client->sock, d, "printGetItem", a) == 1) {
                                            return false;
                                        }
                                    }
                                }
                                else {
                                    return false;
                                }
                                freeReplyObject(monster);
                                monster = (redisReply*)redisCommand(client->c, "del MONSTER:%d:potions", i);
                                if (monster->type == REDIS_REPLY_ERROR) {
                                    return false;
                                }
                                freeReplyObject(monster);
                            }
                        }
                        catch (const std::invalid_argument& e) {
                            cout << "falied to" << client->userId << " attack\n";
                            return false;
                        }
                        catch (const std::out_of_range& e) {
                            cout << "falied to" << client->userId << " attack\n";
                            return false;
                        }
                    }
                    else {
                        cout << "falied to" << client->userId << " attack\n";
                        return false;
                    }
                    freeReplyObject(attack);
                }
            }
        }
        else if (isMonster->type == REDIS_REPLY_NIL) {
            //nothing to do
        }
        else {
            cout << "falied to" << client->userId << " attack\n";
            return false;
        }
        freeReplyObject(isMonster);
    }
    return true;
}

bool monsters(shared_ptr<Client> client) {
    Document d;
    Value a(kArrayType);
    for (int i = 0; i < NUM_GEN_MONSTERS; i++) {
        int x;
        int y;
        redisReply* isMonster = (redisReply*)redisCommand(client->c, "get MONSTER:%d", i);
        if (isMonster->type == REDIS_REPLY_STRING) {
            if (strcmp(isMonster->str, "1") == 0) {
                redisReply* monsters = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:pos x", i);
                if (monsters->type == REDIS_REPLY_STRING) {
                    try {
                        x = stoi(monsters->str);
                    }
                    catch (const std::invalid_argument& e) {
                        cout << "falied to find monsters\n";
                        return false;
                    }
                    catch (const std::out_of_range& e) {
                        cout << "falied to find monsters\n";
                        return false;
                    }
                }
                else {
                    cout << "falied to find monsters\n";
                    return false;
                }
                freeReplyObject(monsters);
                monsters = (redisReply*)redisCommand(client->c, "hget MONSTER:%d:pos y", i);
                if (monsters->type == REDIS_REPLY_STRING) {
                    try {
                        y = stoi(monsters->str);
                    }
                    catch (const std::invalid_argument& e) {
                        cout << "falied to find monsters\n";
                        return false;
                    }
                    catch (const std::out_of_range& e) {
                        cout << "falied to find monsters\n";
                        return false;
                    }
                }
                else {
                    cout << "falied to find monsters\n";
                    return false;
                }
                freeReplyObject(monsters);
                string s = "";
                s.append("[");
                s.append(to_string(x));
                s.append(", ");
                s.append(to_string(y));
                s.append("]");
                Value pos;
                pos.SetString(s.c_str(), s.length(), d.GetAllocator());
                a.PushBack(pos, d.GetAllocator());
            }
        }
        else if (isMonster->type == REDIS_REPLY_NIL) {
            //nothing to do
        }
        else {
            cout << "falied to find monsters\n";
            return false;
        }
        freeReplyObject(isMonster);

    }
    sendJsonString(client->sock, d, "printMonsters", a);
    return true;
}

bool users(shared_ptr<Client> client) {
    Document d;
    Value a(kArrayType);

    list<string> userIdList;
    {
        lock_guard<mutex> lg(activeClientsMutex);
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;
            if (client->userId != "") {
                userIdList.push_back(client->userId);
            }
        }
    }

    for (auto& userId : userIdList) {
        int x;
        int y;
        redisReply* users = (redisReply*)redisCommand(client->c, "hget USER:%s:pos x", userId.c_str());
        if (users->type == REDIS_REPLY_STRING) {
            try {
                x = stoi(users->str);
            }
            catch (const std::invalid_argument& e) {
                cout << "falied to find users\n";
                return false;
            }
            catch (const std::out_of_range& e) {
                cout << "falied to find users\n";
                return false;
            }
        }
        else {
            cout << "falied to find users\n";
            return false;
        }
        freeReplyObject(users);
        users = (redisReply*)redisCommand(client->c, "hget USER:%s:pos y", userId.c_str());
        if (users->type == REDIS_REPLY_STRING) {
            try {
                y = stoi(users->str);
            }
            catch (const std::invalid_argument& e) {
                cout << "falied to find users\n";
                return false;
            }
            catch (const std::out_of_range& e) {
                cout << "falied to find users\n";
                return false;
            }
        }
        else {
            cout << "falied to find users\n";
            return false;
        }
        freeReplyObject(users);
        Value userArr(kArrayType);
        string s = "";
        s.append("[");
        s.append(to_string(x));
        s.append(", ");
        s.append(to_string(y));
        s.append("]");
        Value userIdV;
        userIdV.SetString(userId.c_str(), userId.length(), d.GetAllocator());
        Value pos;
        pos.SetString(s.c_str(), s.length(), d.GetAllocator());
        userArr.PushBack(userIdV, d.GetAllocator());
        userArr.PushBack(pos, d.GetAllocator());
        a.PushBack(userArr, d.GetAllocator());
    }
    sendJsonString(client->sock, d, "printUsers", a);
    return true;
}

// chat size 는 2
bool chat(shared_ptr<Client> client, string chats[]) {
    bool exist = false;
    shared_ptr<Client> toSend;
    {
        lock_guard<mutex> lg(activeClientsMutex);
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> toFindClient = entry.second;

            if (toFindClient->userId == chats[0]) {
                exist = true;
                toSend = toFindClient;
                break;
            }
        }
    }
    if (exist) {
        Document d;
        Value a(kArrayType);
        string chatText = "";
        chatText.append(client->userId);
        chatText.append(": ");
        chatText.append("\"");
        chatText.append(chats[1]);
        chatText.append("\"");
        Value text;
        text.SetString(chatText.c_str(), chatText.length(), d.GetAllocator());
        a.PushBack(text, d.GetAllocator());
        if (sendJsonString(toSend->sock, d, "printMessage", a) == 1) {
            return false;
        }
    }
    else {
        Document d;
        Value a(kArrayType);
        if (sendJsonString(client->sock, d, "printNoUser", a) == 1) {
            return false;
        }
    }
    return true;
}

bool chatBot(shared_ptr<Client> client) {
    string chats[2];
    chats[1] = "저는 bot입니다. 봇봇봇";
    bool exist = false;
    vector<shared_ptr<Client> > toSendList;

    {
        lock_guard<mutex> lg(activeClientsMutex);
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> toFindClient = entry.second;

            if (toFindClient->userId != client->userId && toFindClient->userId != "") {
                toSendList.push_back(toFindClient);
                exist = true;
            }
        }
    }
    if (exist) {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<int> dis(0, toSendList.size() - 1);
        int i = 0;
        int index = dis(gen);
        shared_ptr<Client> toSend = toSendList[index];

        Document d;
        Value a(kArrayType);
        string chatText = "";
        chatText.append(client->userId);
        chatText.append(": ");
        chatText.append("\"");
        chatText.append(chats[1]);
        chatText.append("\"");
        Value text;
        text.SetString(chatText.c_str(), chatText.length(), d.GetAllocator());
        a.PushBack(text, d.GetAllocator());
        if (sendJsonString(toSend->sock, d, "printMessage", a) == 1) {
            return false;
        }
    }
    else {
        Document d;
        Value a(kArrayType);
        if (sendJsonString(client->sock, d, "printNoUser", a) == 1) {
            return false;
        }
    }
    return true;
}

bool login(shared_ptr<Client> client, string userId) {
    redisReply* isLogin = (redisReply*)redisCommand(client->c, "get USER:%s", userId.c_str());
    // int가 아닌 string으로 인식됨
    if (isLogin->type == REDIS_REPLY_STRING) {
        if (strcmp(isLogin->str, "1") == 0) {
            {
                lock_guard<mutex> lg(activeClientsMutex);
                SOCKET toDelete;
                for (auto& entry : activeClients) {
                    SOCKET activeSock = entry.first;
                    shared_ptr<Client> client = entry.second;

                    cout << client->userId << "\n";
                    if (client->userId == userId) {
                        closesocket(activeSock);
                        toDelete = activeSock;
                        break;
                    }
                }
                activeClients.erase(toDelete);
            }
        }
        redisReply* login = (redisReply*)redisCommand(client->c, "set USER:%s 1", userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        client->userId = userId;
        freeReplyObject(login);
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<int> disx(0, DUNGEON_WIDTH - 1);
        uniform_int_distribution<int> disy(0, DUNGEON_HEIGHT - 1);
        login = (redisReply*)redisCommand(client->c, "hget USER:%s:pos x", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        string x = login->str;
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:pos x %s", client->userId.c_str(), x.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hget USER:%s:pos y", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        string y = login->str;
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:pos y %s", client->userId.c_str(), y.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "get USER:%s:hp", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        string hp = login->str;
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "set USER:%s:hp %s", client->userId.c_str(), hp.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "set USER:%s:str 3", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hget USER:%s:potions hp", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        string hpPotion = login->str;
        freeReplyObject(login);        
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:potions hp %s", client->userId.c_str(), hpPotion.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hget USER:%s:potions str", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        string strPotion = login->str;
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:potions str %s", client->userId.c_str(), strPotion.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
    }
    else if (isLogin->type == REDIS_REPLY_NIL) {
        redisReply* login = (redisReply*)redisCommand(client->c, "set USER:%s 1", userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        client->userId = userId;
        freeReplyObject(login);
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<int> disx(0, DUNGEON_WIDTH - 1);
        uniform_int_distribution<int> disy(0, DUNGEON_HEIGHT - 1);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:pos x %d", client->userId.c_str(), disx(gen));
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:pos y %d", client->userId.c_str(), disy(gen));
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "set USER:%s:hp 30", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "set USER:%s:str 3", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:potions hp 1", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
        login = (redisReply*)redisCommand(client->c, "hset USER:%s:potions str 1", client->userId.c_str());
        if (login->type == REDIS_REPLY_ERROR) {
            return false;
        }
        freeReplyObject(login);
    }
    else {
        return false;
    }
    freeReplyObject(isLogin);
    return true;
}

bool potion(shared_ptr<Client> client, string kind) {
    if (kind == "hp" || kind == "str") {
        redisReply* potion = (redisReply*)redisCommand(client->c, "hget USER:%s:potions %s", client->userId.c_str(), kind.c_str());
        if (potion->type == REDIS_REPLY_STRING) {
            try {
                if (stoi(potion->str) <= 0) {
                    Document d;
                    Value a(kArrayType);
                    string print = kind;
                    print.append(" 포션이 없습니다.");
                    Value printV;
                    printV.SetString(print.c_str(), print.length(), d.GetAllocator());
                    a.PushBack(printV, d.GetAllocator());
                    if (client->serverPort == SERVER_PORT) {
                        if (sendJsonString(client->sock, d, "print", a) == 1) {
                            return false;
                        }
                    }
                    else {
                        return false;
                    }
                }
                else {
                    redisReply* usePotion = (redisReply*)redisCommand(client->c, "hincrby USER:%s:potions %s -1", client->userId.c_str(), kind.c_str());
                    if (usePotion->type == REDIS_REPLY_ERROR) {
                        cout << "falied to " << client->userId << "use potion " << kind << "\n";
                        return false;
                    }
                    freeReplyObject(usePotion);
                    string ins;
                    int num = 0;
                    if (kind == "hp") {
                        ins = "incrby";
                        num = 10;
                    }
                    else {
                        ins = "set";
                        num = 6;
                    }
                    usePotion = (redisReply*)redisCommand(client->c, "%s USER:%s:%s %d", ins.c_str(), client->userId.c_str(), kind.c_str(), num);
                    if (usePotion->type == REDIS_REPLY_ERROR) {
                        cout << "falied to " << client->userId << "use potion " << kind << "\n";
                        return false;
                    }
                    freeReplyObject(usePotion);
                    if (kind == "hp") {
                        // nothing to do
                    }
                    else {
                        usePotion = (redisReply*)redisCommand(client->c, "expire USER:%s:%s 60", client->userId.c_str(), kind.c_str());
                        if (usePotion->type == REDIS_REPLY_ERROR) {
                            cout << "falied to " << client->userId << "use potion " << kind << "\n";
                            return false;
                        }
                        freeReplyObject(usePotion);
                    }
                }
            }
            catch (const std::invalid_argument& e) {
                cout << "falied to " << client->userId << "use potion " << kind << "\n";
                return false;
            }
            catch (const std::out_of_range& e) {
                cout << "falied to " << client->userId << "use potion " << kind << "\n";
                return false;
            }
        }
        else {
            cout << "falied to " << client->userId << "use potion " << kind << "\n";
            return false;
        }
        freeReplyObject(potion);
    }
    else {
        Document d;
        Value a(kArrayType);
        a.PushBack("포션 종류가 없습니다.", d.GetAllocator());
        if (client->serverPort == SERVER_PORT) {
            if (sendJsonString(client->sock, d, "print", a) == 1) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    return true;
}

bool processClient(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    // 이전에 어디까지 작업했는지에 따라 다르게 처리한다.
    // 이전에 packetLen 을 완성하지 못했다. 그걸 완성하게 한다.
    if (client->lenCompleted == false) {
        // 길이 정보를 받기 위해서 4바이트를 읽는다.
        // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
        int r = recv(activeSock, (char*)&(client->packetLen) + client->offset, 4 - client->offset, 0);
        if (r == SOCKET_ERROR) {
            cerr << "recv failed with error " << WSAGetLastError() << endl;
            return false;
        }
        else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            cerr << "Socket closed: " << activeSock << endl;
            return false;
        }
        client->offset += r;

        // 완성 못했다면 다음번에 계속 시도할 것이다.
        if (client->offset < 4) {
            return true;
        }

        // network byte order 로 전송했었다.
        // 따라서 이를 host byte order 로 변경한다.
        int dataLen = ntohl(client->packetLen);
        cout << "[" << activeSock << "] Received length info: " << dataLen << endl;
        client->packetLen = dataLen;

        // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
        client->lenCompleted = true;
        client->packet = new char[client->packetLen];
        client->offset = 0;
    }

    // 여기까지 도달했다는 것은 packetLen 을 완성한 경우다. (== lenCompleted 가 true)
    // packetLen 만큼 데이터를 읽으면서 완성한다.
    if (client->lenCompleted == false) {
        return true;
    }

    int r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    if (r == SOCKET_ERROR) {
        cerr << "recv failed with error " << WSAGetLastError() << endl;
        return false;
    }
    else if (r == 0) {
        // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
        // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
        return false;
    }
    client->offset += r;

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (client->offset == client->packetLen) {
        cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << endl;

        cout << client->packet << endl;

        Document d;
        d.Parse(client->packet);
        Value& command = d["command"];
        Value& argument = d["argument"];
        if (command.IsString() == true && argument.IsArray() == true) {
            if (strcmp(command.GetString(), "move") == 0) {
                if (argument.GetArray().Size() == 2) {
                    int pos[2];
                    int cnt = 0;
                    for (auto& v : argument.GetArray()) {
                        if (v.IsInt() && v.GetInt() <= 3) {
                            pos[cnt] = v.GetInt();
                            cnt++;
                        }
                        else {
                            cout << "data is not int\n";
                            return false;
                        }
                    }
                    if (cnt == 2) {
                        //move 명령 실행
                        if (move(client, pos) == false) {
                            return false;
                        }
                    }
                    else {
                        cout << "all data are not int\n";
                        return false;
                    }
                }
                else {
                    cout << "move argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "attack") == 0) {
                if (argument.GetArray().Empty() == true) {
                    // attack 명령 실행
                    if (attack(client) == false) {
                        return false;
                    }
                }
                else {
                    cout << "attack argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "monsters") == 0) {
                if (argument.GetArray().Empty() == true) {
                    // monsters 명령 실행
                    if (monsters(client) == false) {
                        return false;
                    }
                }
                else {
                    cout << "monsters argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "users") == 0) {
                if (argument.GetArray().Empty() == true) {
                    // users 명령 실행
                    if (users(client) == false) {
                        return false;
                    }
                }
                else {
                    cout << "users argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "chat") == 0) {
                if (argument.GetArray().Size() == 2) {
                    string chats[2];
                    int cnt = 0;
                    for (auto& v : argument.GetArray()) {
                        if (v.IsString()) {
                            chats[cnt] = v.GetString();
                            cnt++;
                        }
                        else {
                            cout << "data is not string\n";
                            return false;
                        }
                    }
                    if (cnt == 2) {
                        // chat 명령 실행
                        if (chat(client, chats) == false) {
                            return false;
                        }
                    }
                    else {
                        cout << "all data are not string\n";
                        return false;
                    }
                }
                else {
                    cout << "chat argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "chatBot") == 0) {
                if (argument.GetArray().Empty() == true) {
                    // chatBot 명령 실행
                    if (chatBot(client) == false) {
                        return false;
                    }
                }
                else {
                    cout << "chatBot argument is wrong\n";
                    return false;
                }
            }
            else if (strcmp(command.GetString(), "login") == 0) {
                if (argument.GetArray().Size() == 1 && argument.GetArray()[0].IsString()) {
                    // login 명령 실행
                    string userId = argument.GetArray()[0].GetString();
                    if (login(client, userId) == false) {
                        return false;
                    }
                }
            }
            else if (strcmp(command.GetString(), "potion") == 0) {
                if (argument.GetArray().Size() == 1) {
                    string kind;
                    for (auto& v : argument.GetArray()) {
                        if (v.IsString()) {
                            kind = v.GetString();
                        }
                        else {
                            cout << "data is not string\n";
                            return false;
                        }
                    }
                    // potion 명령 실행
                    if (potion(client, kind) == false) {
                        return false;
                    }
                }
                else {
                    cout << "potion argument is wrong\n";
                    return false;
                }
            }
            else {
                cout << "command is wrong\n";
                return false;
            }
        }
        else {
            cout << "command is wrong or argument is wrong\n";
            return false;
        }

        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;
        delete[] client->packet;
        client->packet = nullptr;
    }
    else {
        cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << endl;
    }

    return true;
}

//processClient의 http 버전
bool processClientHttp(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    if (client->lenCompleted == false) {
        int dataLen = 65535;
        client->packetLen = dataLen;

        client->lenCompleted = true;
        client->packet = new char[client->packetLen];
        memset(client->packet, 0, client->packetLen);
        client->offset = 0;
    }

    if (client->lenCompleted == false) {
        return true;
    }

    int r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    if (r == SOCKET_ERROR) {
        cerr << "recv failed with error " << WSAGetLastError() << endl;
        return false;
    }
    else if (r == 0) {
        return false;
    }
    client->offset += r;

    cout << "[" << activeSock << "] Received " << client->offset << " bytes" << endl;
    cout << client->packet << endl;
    string input = client->packet;
    vector<string> inputVector = split(input, ' ');
    if (inputVector[0] == "GET") {
        if (inputVector[1] == "/monsters") {
            if (monsters(client) == false) {
                sendErrorHttp(activeSock);
            }
            return false;
        }
        else if (inputVector[1] == "/users") {
            if (users(client) == false) {
                sendErrorHttp(activeSock);
            }
            return false;
        }
        else {
            sendErrorHttp(activeSock);
            return false;
        }
    }
    else if (inputVector[0] == "POST") {
        vector<string> http = split(input, '\n');
        string body;
        if (!http.empty()) {
            body = http.back();
        }
        vector<string> bodyArgument = split(body, '&');
        map<string, string> arguments;
        for (const auto element : bodyArgument) {
            vector<string> tmp = split(element, '=');
            if (tmp.size() == 2) {
                arguments.emplace(tmp[0], tmp[1]);
            }
        }
        if (inputVector[1] == "/attack") {
            string username;
            for (const auto element : arguments) {
                if (element.first == "username") {
                    username = element.second;
                }
                else {
                    sendErrorHttp(activeSock);
                    return false;
                }
            }
            if (username != "") {
                if (login(client, username) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                if (attack(client) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                Document d;
                Value a(kArrayType);
                a.PushBack("Attack is success", d.GetAllocator());
                sendJsonStringHttp(client->sock, d, "print", a);
                return false;
            }
            else {
                sendErrorHttp(activeSock);
                return false;
            }
        }
        else if (inputVector[1] == "/move") {
            string username;
            string pos[2];
            for (const auto element : arguments) {
                if (element.first == "username") {
                    username = element.second;
                }
                else if (element.first == "x") {
                    pos[0] = element.second;
                }
                else if (element.first == "y") {
                    pos[1] = element.second;
                }
                else {
                    sendErrorHttp(activeSock);
                    return false;
                }
            }
            if (username != "" && pos[0] != "" && pos[1] != "") {
                try {
                    int xy[2] = { stoi(pos[0]), stoi(pos[1]) };
                    if (login(client, username) == false) {
                        sendErrorHttp(activeSock);
                        return false;
                    }
                    if (move(client, xy) == false) {
                        sendErrorHttp(activeSock);
                        return false;
                    }
                    Document d;
                    Value a(kArrayType);
                    a.PushBack("Move is success", d.GetAllocator());
                    sendJsonStringHttp(client->sock, d, "print", a);
                    return false;
                }
                catch (const invalid_argument& e) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                catch (const out_of_range& e) {
                    sendErrorHttp(activeSock);
                    return false;
                }
            }
            else {
                sendErrorHttp(activeSock);
                return false;
            }
        }
        else if (inputVector[1] == "/chat") {
            string username;
            string chats[2];
            for (const auto element : arguments) {
                if (element.first == "username") {
                    username = element.second;
                }
                else if (element.first == "to") {
                    chats[0] = element.second;
                }
                else if (element.first == "text") {
                    chats[1] = element.second;
                }
                else {
                    sendErrorHttp(activeSock);
                    return false;
                }
            }
            if (username != "" && chats[0] != "" && chats[1] != "") {
                if (login(client, username) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                if (chat(client, chats) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                Document d;
                Value a(kArrayType);
                a.PushBack("Chat is success", d.GetAllocator());
                sendJsonStringHttp(client->sock, d, "print", a);
                return false;
            }
            else {
                sendErrorHttp(activeSock);
                return false;
            }
        }
        else if (inputVector[1] == "/potion") {
            string username;
            string kind;
            for (const auto element : arguments) {
                if (element.first == "username") {
                    username = element.second;
                }
                else if (element.first == "kind") {
                    kind = element.second;
                }
                else {
                    sendErrorHttp(activeSock);
                    return false;
                }
            }
            if (username != "" && kind != "") {
                if (login(client, username) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                if (potion(client, kind) == false) {
                    sendErrorHttp(activeSock);
                    return false;
                }
                Document d;
                Value a(kArrayType);
                a.PushBack("Potion is success", d.GetAllocator());
                sendJsonStringHttp(client->sock, d, "print", a);
                return false;
            }
            else {
                sendErrorHttp(activeSock);
                return false;
            }
        }
    }
    else {
        sendErrorHttp(activeSock);
        return false;
    }

    // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
    client->lenCompleted = false;
    client->offset = 0;
    client->packetLen = 0;
    delete[] client->packet;
    client->packet = nullptr;

    return true;
}

void workerThreadProc(int workerId) {
    cout << "Worker thread is starting. WorkerId: " << workerId << endl;

    while (true) {
        // lock_guard 혹은 unique_lock 의 경우 scope 단위로 lock 범위가 지정되므로,
        // 아래처럼 새로 scope 을 열고 lock 을 잡는 것이 좋다.
        shared_ptr<Client> client;
        {
            unique_lock<mutex> ul(jobQueueMutex);

            // job queue 에 이벤트가 발생할 때까지 condition variable 을 잡을 것이다.
            while (jobQueue.empty()) {
                jobQueueFilledCv.wait(ul);
            }

            // while loop 을 나왔다는 것은 job queue 에 작업이 있다는 것이다.
            // queue 의 front 를 기억하고 front 를 pop 해서 큐에서 뺀다.
            client = jobQueue.front();
            jobQueue.pop();

        }

        // 위의 block 을 나왔으면 client 는 존재할 것이다.
        // 그러나 혹시 나중에 코드가 변경될 수도 있고 그러니 client 가 null 이 아닌지를 확인 후 처리하도록 하자.
        // shared_ptr 은 boolean 이 필요한 곳에 쓰일 때면 null 인지 여부를 확인해준다.
        if (client) {
            SOCKET activeSock = client->sock;
            bool successful = false;
            if (client->serverPort == SERVER_PORT) {
                // 기본값으로 처리
                successful = processClient(client);
            }
            else if (client->serverPort == SERVER_PORT_HTTP) {
                // https로 처리
                successful = processClientHttp(client);
            }
            else {
                // nothing to do
            }

            if (successful == false) {
                closesocket(activeSock);

                // 전체 동접 클라이언트 목록인 activeClients 에서 삭제한다.
                // activeClients 는 메인 쓰레드에서도 접근한다. 따라서 mutex 으로 보호해야될 대상이다.
                // lock_guard 가 scope 단위로 동작하므로 lock 잡히는 영역을 최소화하기 위해서 새로 scope 을 연다.
                {
                    lock_guard<mutex> lg(activeClientsMutex);

                    // activeClients 는 key 가 SOCKET 타입이고, value 가 shared_ptr<Client> 이므로 socket 으로 지운다.
                    activeClients.erase(activeSock);
                }
            }
            else {
                // 다시 select 대상이 될 수 있도록 플래그를 꺼준다.
                // 참고로 오직 성공한 경우만 이 flag 를 다루고 있다.
                // 그 이유는 오류가 발생한 경우는 어차피 동접 리스트에서 빼버릴 것이고 select 를 할 일이 없기 때문이다.
                client->doingRecv.store(false);
            }
        }
    }

    cout << "Worker thread is quitting. Worker id: " << workerId << endl;
}

int genThreadProc() {
    // redis와 연결한다.
    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            printf("Error: %s\n", c->errstr);
        }
        else {
            printf("Cant't allocate redis context\n");
        }
        return 1;
    }
    while (true) {
        for (int i = 0; i < NUM_GEN_MONSTERS; i++) {
            redisReply* isMonster = (redisReply*)redisCommand(c, "get MONSTER:%d", i);
            if (isMonster->type == REDIS_REPLY_STRING) {
                // nothing to do
            }
            else if (isMonster->type == REDIS_REPLY_NIL) {
                redisReply* monster = (redisReply*)redisCommand(c, "set MONSTER:%d 1", i);
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                random_device rd;
                mt19937 gen(rd());
                uniform_int_distribution<int> disx(0, DUNGEON_WIDTH - 1);
                uniform_int_distribution<int> disy(0, DUNGEON_HEIGHT - 1);
                uniform_int_distribution<int> dishp(5, 10);
                uniform_int_distribution<int> disstr(3, 5);
                uniform_int_distribution<int> dispostionhp(0, 1);
                uniform_int_distribution<int> dispostionstr(0, 1);
                monster = (redisReply*)redisCommand(c, "hset MONSTER:%d:pos x %d", i, disx(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                monster = (redisReply*)redisCommand(c, "hset MONSTER:%d:pos y %d", i, disy(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                monster = (redisReply*)redisCommand(c, "set MONSTER:%d:hp %d", i, dishp(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                monster = (redisReply*)redisCommand(c, "set MONSTER:%d:str %d", i, disstr(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                monster = (redisReply*)redisCommand(c, "hset MONSTER:%d:potions hp %d", i, dispostionhp(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
                monster = (redisReply*)redisCommand(c, "hset MONSTER:%d:potions str %d", i, dispostionstr(gen));
                if (monster->type == REDIS_REPLY_ERROR) {
                    return 1;
                }
                freeReplyObject(monster);
            }
            else {
                return 1;
            }
            freeReplyObject(isMonster);

        }

        // GEN_TIME당 한번씩 작동시킴
        Sleep(GEN_TIME);
    }

    redisFree(c);
}

void attackThreadProc() {
    // redis와 연결한다.
    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            printf("Error: %s\n", c->errstr);
        }
        else {
            printf("Cant't allocate redis context\n");
        }
    }
    while (true) {
        for (int i = 0; i < NUM_GEN_MONSTERS; i++) {
            redisReply* isMonster = (redisReply*)redisCommand(c, "get MONSTER:%d", i);
            if (isMonster->type == REDIS_REPLY_STRING) {
                // 공격 명령 실행
                if (strcmp(isMonster->str, "1") == 0) {
                    int x;
                    int y;
                    redisReply* attack = (redisReply*)redisCommand(c, "hget MONSTER:%d:pos x", i);
                    if (attack->type == REDIS_REPLY_STRING) {
                        try {
                            x = stoi(attack->str);
                        }
                        catch (const std::invalid_argument& e) {
                            cout << "MONSTER:" << i << ":pos x is invalid argument\n";
                        }
                        catch (const std::out_of_range& e) {
                            cout << "MONSTER:" << i << ":pos x is out of range\n";
                        }
                    }
                    else {
                        cout << "Can't get Monster:" << i << " : pos x\n";
                    }
                    freeReplyObject(attack);
                    attack = (redisReply*)redisCommand(c, "hget MONSTER:%d:pos y", i);
                    if (attack->type == REDIS_REPLY_STRING) {
                        try {
                            y = stoi(attack->str);
                        }
                        catch (const std::invalid_argument& e) {
                            cout << "MONSTER:" << i << ":pos y is invalid argument\n";
                        }
                        catch (const std::out_of_range& e) {
                            cout << "MONSTER:" << i << ":pos y is out of range\n";
                        }
                    }
                    else {
                        cout << "Can't get Monster:" << i << " : pos y\n";
                    }
                    freeReplyObject(attack);

                    list<string> userIdList;
                    {
                        lock_guard<mutex> lg(activeClientsMutex);
                        for (auto& entry : activeClients) {
                            SOCKET activeSock = entry.first;
                            shared_ptr<Client> client = entry.second;
                            if (client->userId != "") {
                                userIdList.push_back(client->userId);
                            }
                        }
                    }

                    for (auto& userId : userIdList) {
                        redisReply* isLogin = (redisReply*)redisCommand(c, "get USER:%s", userId.c_str());
                        if (isLogin->type == REDIS_REPLY_STRING) {
                            if (strcmp(isLogin->str, "1") == 0) {
                                bool resultX = false;
                                bool resultY = false;
                                redisReply* user = (redisReply*)redisCommand(c, "hget USER:%s:pos x", userId.c_str());
                                if (user->type == REDIS_REPLY_STRING) {
                                    try {
                                        int userX = stoi(user->str);
                                        if (userX >= x - 1 && userX <= x + 1) {
                                            resultX = true;
                                        }
                                    }
                                    catch (const std::invalid_argument& e) {
                                        cout << "USER:" << i << ":pos x is invalid argument\n";
                                    }
                                    catch (const std::out_of_range& e) {
                                        cout << "USER:" << i << ":pos x is out of range\n";
                                    }
                                }
                                else {
                                    cout << "failed to get USER:" << userId << ":pos x\n";
                                }
                                freeReplyObject(user);
                                user = (redisReply*)redisCommand(c, "hget USER:%s:pos y", userId.c_str());
                                if (user->type == REDIS_REPLY_STRING) {
                                    try {
                                        int userY = stoi(user->str);
                                        if (userY >= y - 1 && userY <= y + 1) {
                                            resultY = true;
                                        }
                                    }
                                    catch (const std::invalid_argument& e) {
                                        cout << "USER:" << i << ":pos y is invalid argument\n";
                                    }
                                    catch (const std::out_of_range& e) {
                                        cout << "USER:" << i << ":pos y is out of range\n";
                                    }
                                }
                                else {
                                    cout << "failed to get USER:" << userId << ":pos y\n";
                                }
                                freeReplyObject(user);
                                if (resultX && resultY) {
                                    // 공격함.
                                    redisReply* str = (redisReply*)redisCommand(c, "get MONSTER:%d:str", i);
                                    if (str->type == REDIS_REPLY_STRING) {
                                        string attack = "슬라임";
                                        string hit = userId;
                                        string damage = str->str;
                                        redisReply* apply = (redisReply*)redisCommand(c, "Decrby USER:%s:hp %s", hit.c_str(), damage.c_str());
                                        if (apply->type == REDIS_REPLY_ERROR) {
                                            cout << "failed to apply MONSTER:" << i << ":str to " << userId << "\n";
                                        }
                                        else {
                                            printAttack(attack, hit, damage);
                                            redisReply* hp = (redisReply*)redisCommand(c, "get USER:%s:hp", hit.c_str());
                                            if (hp->type == REDIS_REPLY_ERROR) {
                                                cout << "failed to get USER:" << hit << ":hp\n";
                                            }
                                            else {
                                                try {
                                                    if (stoi(hp->str) <= 0) {
                                                        {
                                                            lock_guard<mutex> lg(activeClientsMutex);
                                                            SOCKET toDelete;
                                                            for (auto& entry : activeClients) {
                                                                SOCKET activeSock = entry.first;
                                                                shared_ptr<Client> client = entry.second;



                                                                if (client->userId == hit) {
                                                                    // DOM 내용을 생성한다.
                                                                    Document d;
                                                                    Value a(kArrayType);
                                                                    sendJsonString(activeSock, d, "printGameOver", a);

                                                                    //죽으면 데이터 삭제
                                                                    redisReply* del = (redisReply*)redisCommand(c, "del USER:%s", client->userId.c_str());
                                                                    if (del->type == REDIS_REPLY_ERROR) {
                                                                        cout << "del error1\n";
                                                                    }
                                                                    freeReplyObject(del);
                                                                    del = (redisReply*)redisCommand(c, "del USER:%s:pos", client->userId.c_str());
                                                                    if (del->type == REDIS_REPLY_ERROR) {
                                                                        cout << "del error2\n";
                                                                    }
                                                                    freeReplyObject(del);
                                                                    del = (redisReply*)redisCommand(c, "del USER:%s:hp", client->userId.c_str());
                                                                    if (del->type == REDIS_REPLY_ERROR) {
                                                                        cout << "del error3\n";
                                                                    }
                                                                    freeReplyObject(del);
                                                                    del = (redisReply*)redisCommand(c, "del USER:%s:str", client->userId.c_str());
                                                                    if (del->type == REDIS_REPLY_ERROR) {
                                                                        cout << "del error4\n";
                                                                    }
                                                                    freeReplyObject(del);
                                                                    del = (redisReply*)redisCommand(c, "del USER:%s:potions", client->userId.c_str());
                                                                    if (del->type == REDIS_REPLY_ERROR) {
                                                                        cout << "del error5\n";
                                                                    }
                                                                    freeReplyObject(del);

                                                                    closesocket(activeSock);
                                                                    toDelete = activeSock;
                                                                    break;
                                                                }
                                                            }
                                                            activeClients.erase(toDelete);
                                                        }
                                                    }
                                                }
                                                catch (const std::invalid_argument& e) {
                                                    cout << "USER:" << i << ":hp is invalid argument\n";
                                                }
                                                catch (const std::out_of_range& e) {
                                                    cout << "USER:" << i << ":hp is out of range\n";
                                                }
                                            }
                                            freeReplyObject(hp);
                                        }
                                        freeReplyObject(apply);
                                    }
                                    else {
                                        cout << "failed to get MONSTER:" << i << ":str\n";
                                    }
                                    freeReplyObject(str);
                                }
                            }
                        }
                        else if (isLogin->type == REDIS_REPLY_NIL) {
                            cout << "No data\n";
                        }
                        else {
                            cout << "failed to get USER:" << userId << "\n";
                        }
                        freeReplyObject(isLogin);
                    }
                }
                else {
                }
            }
            else if (isMonster->type == REDIS_REPLY_NIL) {
                // nothing to do
            }
            else {
                cout << "failed to get MONSTER:" << i << "\n";
            }
            freeReplyObject(isMonster);

        }

        // ATTACK_TIME당 한번씩 작동시킴
        Sleep(ATTACK_TIME);
    }

    redisFree(c);
}

int main() {
    int r = 0;

    // Winsock 을 초기화한다.
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        cerr << "WSAStartup failed with error " << r << endl;
        return 1;
    }

    // passive socket 을 만들어준다.
    SOCKET passiveSock = createPassiveSocket(SERVER_PORT);
    SOCKET httpPassiveSock = createPassiveSocket(SERVER_PORT_HTTP);

    // redis 서버에 연결한다.
    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            printf("Error: %s\n", c->errstr);
            return 1;
        }
        else {
            printf("Cant't allocate redis context\n");
            return 1;
        }
    }
    //redis 서버를 초기화 한다.
    redisReply* reply = (redisReply*)redisCommand(c, "flushall");
    if (reply->type == REDIS_REPLY_ERROR) {
        cout << "failed to flushall\n";
        return 1;
    }
    freeReplyObject(reply);

    // workerThread의 thread pool과 genThread 그리고 attackThread를 생성한다.
    list<shared_ptr<thread> > threads;
    for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
        shared_ptr<thread> workerThread(new thread(workerThreadProc, i));
        threads.push_back(workerThread);
    }

    shared_ptr<thread> genThread(new thread(genThreadProc));
    shared_ptr<thread> attackThread(new thread(attackThreadProc));

    // 서버는 사용자가 중단할 때까지 프로그램이 계속 동작해야된다.
    // 따라서 loop 으로 반복 처리한다.
    while (true) {
        // select 를 이용해 읽기 이벤트와 예외 이벤트가 발생하는 소켓을 알아낼 것이다.
        fd_set readSet, exceptionSet;

        // 위의 socket set 을 초기화한다.
        FD_ZERO(&readSet);
        FD_ZERO(&exceptionSet);

        // select 의 첫번째 인자는 max socket 번호에 1을 더한 값이다.
        // 따라서 max socket 번호를 계산한다.
        SOCKET maxSock = -1;

        // passive socket 은 기본으로 각 socket set 에 포함되어야 한다.
        FD_SET(passiveSock, &readSet);
        FD_SET(passiveSock, &exceptionSet);
        maxSock = max(maxSock, passiveSock);

        FD_SET(httpPassiveSock, &readSet);
        FD_SET(httpPassiveSock, &exceptionSet);
        maxSock = max(maxSock, httpPassiveSock);

        // 현재 남아있는 active socket 들에 대해서도 모두 set 에 넣어준다.
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            // 이미 readable 하다고 해서 job queue 에 넣은 경우 다시 select 를 하면 다시 readable 하게 나온다.
            // 이렇게 되면 job queue 안에 중복으로 client 가 들어가게 되므로,
            // 아직 job queue 안에 안들어간 클라이언트만 select 확인 대상으로 한다.
            if (client->doingRecv.load() == false) {
                FD_SET(activeSock, &readSet);
                FD_SET(activeSock, &exceptionSet);
                maxSock = max(maxSock, activeSock);
            }
        }

        // select 를 해준다. 동접이 있더라도 doingRecv 가 켜진 것들은 포함하지 않았었다.
        // 이런 것들은 worker thread 가 처리 후 doingRecv 를 끄면 다시 select 대상이 되어야 하는데,
        // 아래는 timeout 없이 한정 없이 select 를 기다리므로 doingRecv 변경으로 다시 select 되어야 하는 것들이
        // 굉장히 오래 걸릴 수 있다. 그런 문제를 해결하기 위해서 select 의 timeout 을 100 msec 정도로 제한한다.
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100;
        r = select(maxSock + 1, &readSet, NULL, &exceptionSet, &timeout);

        // 회복할 수 없는 오류이다. 서버를 중단한다.
        if (r == SOCKET_ERROR) {
            cerr << "select failed: " << WSAGetLastError() << endl;
            break;
        }

        // select 의 반환값은 오류일 때 SOCKET_ERROR, 그 외의 경우 이벤트가 발생한 소켓 갯수이다.
        // 따라서 반환값 r 이 0인 경우는 아래를 스킵하게 한다.
        if (r == 0) {
            continue;
        }

        // passive socket 이 readable 하다면 이는 새 연결이 들어왔다는 것이다.
        // 새 클라이언트 객체를 동적으로 만들고 
        if (FD_ISSET(passiveSock, &readSet)) {
            // passive socket 을 이용해 accept() 를 한다.
            // accept() 는 blocking 이지만 우리는 이미 select() 를 통해 새 연결이 있음을 알고 accept() 를 호출한다.
            // 따라서 여기서는 blocking 되지 않는다.
            // 연결이 완료되고 만들어지는 소켓은 active socket 이다.
            cout << "Waiting for a connection" << endl;
            struct sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(passiveSock, (sockaddr*)&clientAddr, &clientAddrSize);

            // accpet() 가 실패하면 해당 연결은 이루어지지 않았음을 의미한다.
            // 그 여결이 잘못된다고 하더라도 다른 연결들을 처리해야되므로 에러가 발생했다고 하더라도 계속 진행한다.
            if (activeSock == INVALID_SOCKET) {
                cerr << "accept failed with error " << WSAGetLastError() << endl;
                return 1;
            }
            else {
                // redis와 연결한다.
                redisContext* c = redisConnect("127.0.0.1", 6379);
                if (c == NULL || c->err) {
                    if (c) {
                        printf("Error: %s\n", c->errstr);
                    }
                    else {
                        printf("Cant't allocate redis context\n");
                    }
                    closesocket(activeSock);
                }
                else {
                    // 새로 client 객체를 만든다.
                    shared_ptr<Client> newClient(new Client(activeSock, SERVER_PORT, c));

                    // socket 을 key 로 하고 해당 객체 포인터를 value 로 하는 map 에 집어 넣는다.
                    {
                        lock_guard<mutex> lg(activeClientsMutex);
                        activeClients.insert(make_pair(activeSock, newClient));
                    }

                    // 로그를 찍는다.
                    char strBuf[1024];
                    inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                    cout << "New client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                        << "Socket: " << activeSock << endl;
                }
            }
        }

        if (FD_ISSET(httpPassiveSock, &readSet)) {
            cout << "Waiting for a connection" << endl;
            struct sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(httpPassiveSock, (sockaddr*)&clientAddr, &clientAddrSize);

            if (activeSock == INVALID_SOCKET) {
                cerr << "accept failed with error " << WSAGetLastError() << endl;
                return 1;
            }
            else {
                redisContext* c = redisConnect("127.0.0.1", 6379);
                if (c == NULL || c->err) {
                    if (c) {
                        printf("Error: %s\n", c->errstr);
                    }
                    else {
                        printf("Cant't allocate redis context\n");
                    }
                    closesocket(activeSock);
                }
                else {
                    shared_ptr<Client> newClient(new Client(activeSock, SERVER_PORT_HTTP, c));
                    {
                        lock_guard<mutex> lg(activeClientsMutex);
                        activeClients.insert(make_pair(activeSock, newClient));
                    }

                    char strBuf[1024];
                    inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                    cout << "New client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                        << "Socket: " << activeSock << endl;
                }
            }
        }

        // 오류 이벤트가 발생하는 소켓의 클라이언트는 제거한다.
        // activeClients 를 순회하는 동안 그 내용을 변경하면 안되니 지우는 경우를 위해 별도로 list 를 쓴다.
        list<SOCKET> toDelete;
        {
            lock_guard<mutex> lg(activeClientsMutex);
            for (auto& entry : activeClients) {
                SOCKET activeSock = entry.first;
                shared_ptr<Client> client = entry.second;

                if (FD_ISSET(activeSock, &exceptionSet)) {
                    cerr << "Exception on socket " << activeSock << endl;

                    // 소켓을 닫는다.
                    closesocket(activeSock);

                    // 지울 대상에 포함시킨다.
                    // 여기서 activeClients 에서 바로 지우지 않는 이유는 현재 activeClients 를 순회중이기 때문이다.
                    toDelete.push_back(activeSock);

                    // 소켓을 닫은 경우 더 이상 처리할 필요가 없으니 아래 read 작업은 하지 않는다.
                    continue;
                }

                // 읽기 이벤트가 발생하는 소켓의 경우 recv() 를 처리한다.
                // 주의: 아래는 여전히 recv() 에 의해 blocking 이 발생할 수 있다.
                //       우리는 이를 producer-consumer 형태로 바꿀 것이다.
                if (FD_ISSET(activeSock, &readSet)) {
                    // 이제 다시 select 대상이 되지 않도록 client 의 flag 를 켜준다.
                    client->doingRecv.store(true);

                    // 해당 client 를 job queue 에 넣자. lock_guard 를 써도 되고 unique_lock 을 써도 된다.
                    // lock 걸리는 범위를 명시적으로 제어하기 위해서 새로 scope 을 열어준다.
                    {
                        lock_guard<mutex> lg(jobQueueMutex);

                        bool wasEmpty = jobQueue.empty();
                        jobQueue.push(client);

                        // 그리고 worker thread 를 깨워준다.
                        // 무조건 condition variable 을 notify 해도 되는데,
                        // 해당 condition variable 은 queue 에 뭔가가 들어가서 더 이상 빈 큐가 아닐 때 쓰이므로
                        // 여기서는 무의미하게 CV 를 notify하지 않도록 큐의 길이가 0에서 1이 되는 순간 notify 를 하도록 하자.
                        if (wasEmpty) {
                            jobQueueFilledCv.notify_one();
                        }

                        // lock_guard 는 scope 이 벗어날 때 풀릴 것이다.
                    }
                }
            }
        }
        // 이제 지울 것이 있었다면 지운다.
        for (auto& closedSock : toDelete) {
            {
                lock_guard<mutex> lg(activeClientsMutex);
                // 맵에서 지우고 객체도 지워준다.
                // shared_ptr 을 썼기 때문에 맵에서 지워서 더 이상 사용하는 곳이 없어지면 객체도 지워진다.
                activeClients.erase(closedSock);
            }
        }
    }

    // 이제 threads 들을 join 한다.
    for (shared_ptr<thread>& workerThread : threads) {
        workerThread->join();
    }

    genThread->join();
    attackThread->join();

    redisFree(c);

    // 연결을 기다리는 passive socket 을 닫는다.
    r = closesocket(passiveSock);
    if (r == SOCKET_ERROR) {
        cerr << "closesocket(passive) failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    r = closesocket(httpPassiveSock);
    if (r == SOCKET_ERROR) {
        cerr << "closesocket(httpsPassive) failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}