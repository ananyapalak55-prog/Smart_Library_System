//cd SmartLibraryProject; g++ -std=c++17 smart_library_network.cpp -o smart_library.exe -lws2_32; .\smart_library.exe
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <queue>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <cstring>

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #ifndef ENABLE_QUICK_EDIT_MODE
        #define ENABLE_QUICK_EDIT_MODE 0x0040
    #endif
    #ifndef ENABLE_EXTENDED_FLAGS
        #define ENABLE_EXTENDED_FLAGS 0x0080
    #endif
    typedef SOCKET SocketType;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
    #define SHUT_SEND SD_SEND
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <ifaddrs.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <thread>
    #include <mutex>
    #include <cerrno>
    typedef int SocketType;
    #define INVALID_SOCK -1
    #define CLOSE_SOCKET close
    #define SHUT_SEND SHUT_WR
#endif

using namespace std;

void disableConsoleQuickEdit() {
#ifdef _WIN32
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hInput, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hInput, mode);
    }
#endif
}

class CrossMutex {
private:
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    std::mutex mtx;
#endif
public:
    CrossMutex() {
#ifdef _WIN32
        InitializeCriticalSection(&cs);
#endif
    }
    ~CrossMutex() {
#ifdef _WIN32
        DeleteCriticalSection(&cs);
#endif
    }
    void lock() {
#ifdef _WIN32
        EnterCriticalSection(&cs);
#else
        mtx.lock();
#endif
    }
    void unlock() {
#ifdef _WIN32
        LeaveCriticalSection(&cs);
#else
        mtx.unlock();
#endif
    }
};

class CrossLock {
private:
    CrossMutex& m;
public:
    explicit CrossLock(CrossMutex& mtx) : m(mtx) { m.lock(); }
    ~CrossLock() { m.unlock(); }
};

const int SERVER_PORT          = 8080;
const int DISCOVERY_PORT       = 8081;
const vector<int> VALID_FLOORS = {0, 1, 2};

const string BOOKS_EXCEL = "books_catalog.xls";
const string HELP_EXCEL  = "help_records.xls";
const string ISSUED_CSV  = "issued_records.csv";

struct Book {
    string bookID;
    string isbn;
    string title;
    string author;
    string subject;
    int floor;
    int shelf;
    int rack;
    int slot;
    int totalStock;
    int availableStock;
};

struct DeskTerminal {
    string terminalID;
    string deskLabel;
    string password;
    int floor;
    int zone;
};

struct StudentKiosk {
    string kioskID;
    int floor;
    int compNo;
    int sideZone;
};

struct IssueRecord {
    int issueID;
    string bookID;
    string bookTitle;
    string studentRollNo;
    string issueDate;
    string dueDate;
    string status;
};

struct HelpRecord {
    int ticketID;
    string dateTime;
    string studentKioskID;
    string bookID;
    string bookTitle;
    int targetFloor;
    int targetShelf;
    string targetTerminalID;
    string status;
    string resolvedTime;
    string librarianRemark;
};

void initNetwork() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

void cleanupNetwork() {
#ifdef _WIN32
    WSACleanup();
#endif
}

vector<string> getLocalIPs() {
    vector<string> ips;
#ifdef _WIN32
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
        struct hostent* phe = gethostbyname(host);
        if (phe != nullptr) {
            for (int i = 0; phe->h_addr_list[i] != nullptr; ++i) {
                struct in_addr addr;
                memcpy(&addr, phe->h_addr_list[i], sizeof(struct in_addr));
                string ip = inet_ntoa(addr);
                if (ip != "127.0.0.1") ips.push_back(ip);
            }
        }
    }
#else
    struct ifaddrs* ifAddrStruct = nullptr;
    if (getifaddrs(&ifAddrStruct) == 0) {
        for (struct ifaddrs* ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                void* tmpAddrPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                char addressBuffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
                string ip(addressBuffer);
                if (ip != "127.0.0.1") ips.push_back(ip);
            }
        }
        freeifaddrs(ifAddrStruct);
    }
#endif
    return ips;
}

void setSocketTimeout(SocketType sock, int seconds) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(seconds * 1000);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

SocketType connectWithTimeout(const string& ip, int port, int timeoutSec, string& errOut) {
    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) {
        errOut = "Socket creation failed";
        return INVALID_SOCK;
    }

    sockaddr_in servAddr{};
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);

    unsigned long ipBin = inet_addr(ip.c_str());
    if (ipBin == INADDR_NONE && ip != "255.255.255.255") {
        errOut = "Invalid IP address";
        CLOSE_SOCKET(sock);
        return INVALID_SOCK;
    }
    servAddr.sin_addr.s_addr = ipBin;

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    int res = connect(sock, (struct sockaddr*)&servAddr, sizeof(servAddr));
    if (res < 0) {
        fd_set writeSet, exceptSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&exceptSet);
        FD_SET(sock, &writeSet);
        FD_SET(sock, &exceptSet);

        timeval tv{};
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(sock) + 1, nullptr, &writeSet, &exceptSet, &tv);
        if (sel <= 0) {
            errOut = "Timeout (Check Laptop 1 Firewall / Wi-Fi)";
            CLOSE_SOCKET(sock);
            return INVALID_SOCK;
        }

        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &len);
        if (soError != 0 || FD_ISSET(sock, &exceptSet)) {
            errOut = "Server stopped on Laptop 1 (Code " + to_string(soError) + ")";
            CLOSE_SOCKET(sock);
            return INVALID_SOCK;
        }
    }

#ifdef _WIN32
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
#else
    fcntl(sock, F_SETFL, flags);
#endif

    int keepAlive = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));
    setSocketTimeout(sock, 8);
    return sock;
}

bool sendLine(SocketType sock, const string& msg) {
    string data = msg + "\n";
    int totalSent = 0;
    int len = static_cast<int>(data.size());
    while (totalSent < len) {
        int sent = send(sock, data.c_str() + totalSent, len - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

string recvLine(SocketType sock) {
    string line;
    char ch;
    while (true) {
        int bytes = recv(sock, &ch, 1, 0);
        if (bytes <= 0) break;
        if (ch == '\n') break;
        if (ch != '\r') line += ch;
    }
    return line;
}

string getCurrentDate() {
    time_t now = time(nullptr);
    tm* ltm = localtime(&now);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", ltm);
    return string(buf);
}

string getCurrentDateTime() {
    time_t now = time(nullptr);
    tm* ltm = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ltm);
    return string(buf);
}

string getActiveShift() {
    time_t now = time(nullptr);
    tm* ltm = localtime(&now);
    return (ltm->tm_hour < 14) ? "Morning Shift (8 AM - 2 PM)" : "Evening Shift (2 PM - 8 PM)";
}

string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

string toLowerStr(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

string toUpperStr(string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

int safeStoi(const string& s, int fallback = 0) {
    try {
        string t = trim(s);
        return t.empty() ? fallback : stoi(t);
    } catch (...) {
        return fallback;
    }
}

int readInt(const string& prompt, int fallback = 0) {
    cout << prompt << flush;
    string line;
    getline(cin, line);
    return safeStoi(line, fallback);
}

vector<string> splitStr(const string& line, char delim) {
    vector<string> tokens;
    string token;
    stringstream ss(line);
    while (getline(ss, token, delim)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}

string escapeXML(const string& data) {
    string out;
    out.reserve(data.size());
    for (char c : data) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '\"': out += "&quot;"; break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            default:   out += c;        break;
        }
    }
    return out;
}

void replaceAll(string& str, const string& from, const string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

string unescapeXML(string data) {
    replaceAll(data, "&quot;", "\"");
    replaceAll(data, "&gt;",   ">");
    replaceAll(data, "&lt;",   "<");
    replaceAll(data, "&amp;",  "&");
    return data;
}

vector<string> extractExcelCells(const string& rowXml) {
    vector<string> cells;
    size_t pos = 0;
    while (true) {
        size_t dataStart = rowXml.find("<Data", pos);
        if (dataStart == string::npos) break;
        size_t contentStart = rowXml.find('>', dataStart);
        if (contentStart == string::npos) break;
        contentStart++;
        size_t dataEnd = rowXml.find("</Data>", contentStart);
        if (dataEnd == string::npos) break;

        string rawVal = rowXml.substr(contentStart, dataEnd - contentStart);
        cells.push_back(unescapeXML(trim(rawVal)));
        pos = dataEnd + 7;
    }
    return cells;
}

int getZoneFromShelf(int shelf) {
    return (shelf >= 1 && shelf <= 15) ? 1 : 2;
}

string getZoneText(int zone) {
    return (zone == 1) ? "Shelves 1-15" : "Shelves 16-30";
}

bool isValidDate(const string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (!isdigit(static_cast<unsigned char>(d[i]))) return false;
    }
    return true;
}

class LibraryNavigator {
private:
    unordered_map<string, vector<string>> adj;

public:
    LibraryNavigator() {
        adj["Floor 0 Lobby"] = {"Floor 0 Zone A", "Floor 0 Zone B", "Stairs / Lift"};
        adj["Floor 0 Zone A"] = {"Floor 0 Lobby"};
        adj["Floor 0 Zone B"] = {"Floor 0 Lobby"};

        adj["Stairs / Lift"] = {"Floor 0 Lobby", "Floor 1 Lobby", "Floor 2 Lobby"};

        adj["Floor 1 Lobby"] = {"Stairs / Lift", "Floor 1 Zone A", "Floor 1 Zone B"};
        adj["Floor 1 Zone A"] = {"Floor 1 Lobby"};
        adj["Floor 1 Zone B"] = {"Floor 1 Lobby"};

        adj["Floor 2 Lobby"] = {"Stairs / Lift", "Floor 2 Zone A", "Floor 2 Zone B"};
        adj["Floor 2 Zone A"] = {"Floor 2 Lobby"};
        adj["Floor 2 Zone B"] = {"Floor 2 Lobby"};
    }

    void printRoute(const StudentKiosk& kiosk, int targetFloor, int targetShelf, int rack, int slot) {
        string startZone = (kiosk.sideZone == 1) ? "Zone A" : "Zone B";
        string startNode = "Floor " + to_string(kiosk.floor) + " " + startZone;

        string targetZone = (targetShelf <= 15) ? "Zone A" : "Zone B";
        string targetNode = "Floor " + to_string(targetFloor) + " " + targetZone;

        queue<string> q;
        unordered_map<string, string> parent;
        unordered_map<string, bool> visited;

        q.push(startNode);
        visited[startNode] = true;
        parent[startNode] = "";

        while (!q.empty()) {
            string curr = q.front();
            q.pop();
            if (curr == targetNode) break;

            for (const string& next : adj[curr]) {
                if (!visited[next]) {
                    visited[next] = true;
                    parent[next] = curr;
                    q.push(next);
                }
            }
        }

        vector<string> path;
        for (string at = targetNode; !at.empty(); at = parent[at]) {
            path.push_back(at);
        }
        reverse(path.begin(), path.end());

        cout << "  Route    : " << kiosk.kioskID << " -> ";
        for (size_t i = 0; i < path.size(); ++i) {
            cout << path[i] << " -> ";
        }
        cout << "Shelf " << targetShelf << ", Rack " << rack << ", Slot " << slot << "\n";
    }
};

class LibrarianServer {
private:
    CrossMutex dataMutex;

    unordered_map<string, Book> bookByID;
    unordered_map<string, string> isbnToBookID;

    unordered_map<string, DeskTerminal> terminalsByID;
    map<pair<int, int>, DeskTerminal> terminalByFloorZone;

    string todayDate;
    bool running;

    vector<IssueRecord> issueRecords;
    vector<HelpRecord> helpRecords;

    struct ClientArgs {
        LibrarianServer* server;
        SocketType sock;
    };

#ifdef _WIN32
    static DWORD WINAPI serverThreadProc(LPVOID param) {
        static_cast<LibrarianServer*>(param)->acceptLoop();
        return 0;
    }

    static DWORD WINAPI udpThreadProc(LPVOID param) {
        static_cast<LibrarianServer*>(param)->udpDiscoveryLoop();
        return 0;
    }

    static DWORD WINAPI clientThreadProc(LPVOID param) {
        ClientArgs* args = static_cast<ClientArgs*>(param);
        args->server->handleClient(args->sock);
        delete args;
        return 0;
    }
#endif

public:
    LibrarianServer() : running(true) {
        todayDate = getCurrentDate();
        initTerminals();
        initDefaultFiles();
        loadBooks();
        loadHelpRecords();
        loadIssuedRecords();
    }

    void initTerminals() {
        vector<DeskTerminal> desks = {
            {"F0_LIB1", "Floor 0, Shelves 1-15",  "1234", 0, 1},
            {"F0_LIB2", "Floor 0, Shelves 16-30", "1234", 0, 2},
            {"F1_LIB1", "Floor 1, Shelves 1-15",  "1234", 1, 1},
            {"F1_LIB2", "Floor 1, Shelves 16-30", "1234", 1, 2},
            {"F2_LIB1", "Floor 2, Shelves 1-15",  "1234", 2, 1},
            {"F2_LIB2", "Floor 2, Shelves 16-30", "1234", 2, 2}
        };
        for (const auto& d : desks) {
            terminalsByID[d.terminalID] = d;
            terminalByFloorZone[{d.floor, d.zone}] = d;
        }
    }

    DeskTerminal getTerminal(int floor, int zone) {
        auto key = make_pair(floor, zone);
        if (terminalByFloorZone.find(key) != terminalByFloorZone.end()) {
            return terminalByFloorZone[key];
        }
        return terminalsByID["F0_LIB1"];
    }

    bool writeBooksExcel(const vector<Book>& list) {
        ofstream out(BOOKS_EXCEL);
        if (!out.is_open()) {
            cout << "  Error: Close " << BOOKS_EXCEL << " in Excel first.\n";
            return false;
        }
        out << "<?xml version=\"1.0\"?>\n<?mso-application progid=\"Excel.Sheet\"?>\n";
        out << "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"\n";
        out << " xmlns:o=\"urn:schemas-microsoft-com:office:office\"\n";
        out << " xmlns:x=\"urn:schemas-microsoft-com:office:excel\"\n";
        out << " xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n";
        out << " <Styles><Style ss:ID=\"HeaderStyle\"><Font ss:Bold=\"1\" ss:Color=\"#FFFFFF\"/>";
        out << "<Interior ss:Color=\"#1E3A8A\" ss:Pattern=\"Solid\"/></Style></Styles>\n";
        out << " <Worksheet ss:Name=\"BooksCatalog\"><Table>\n";
        out << "   <Column ss:Width=\"70\"/><Column ss:Width=\"110\"/><Column ss:Width=\"260\"/>\n";
        out << "   <Column ss:Width=\"160\"/><Column ss:Width=\"170\"/><Column ss:Width=\"55\"/>\n";
        out << "   <Column ss:Width=\"55\"/><Column ss:Width=\"55\"/><Column ss:Width=\"55\"/>\n";
        out << "   <Column ss:Width=\"80\"/><Column ss:Width=\"95\"/>\n";

        vector<string> headers = {
            "BookID", "ISBN", "Title", "Author", "Subject",
            "Floor", "Shelf", "Rack", "Slot", "TotalStock", "AvailableStock"
        };
        out << "   <Row>\n";
        for (const string& h : headers) {
            out << "    <Cell ss:StyleID=\"HeaderStyle\"><Data ss:Type=\"String\">" << escapeXML(h) << "</Data></Cell>\n";
        }
        out << "   </Row>\n";

        for (const Book& b : list) {
            out << "   <Row>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(b.bookID)  << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(b.isbn)    << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(b.title)   << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(b.author)  << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(b.subject) << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.floor               << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.shelf               << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.rack                << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.slot                << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.totalStock          << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << b.availableStock      << "</Data></Cell>\n";
            out << "   </Row>\n";
        }
        out << "  </Table></Worksheet></Workbook>\n";
        out.close();
        return true;
    }

    bool saveBooksLocked() {
        vector<Book> list;
        for (const auto& p : bookByID) list.push_back(p.second);
        sort(list.begin(), list.end(), [](const Book& a, const Book& b) {
            return a.bookID < b.bookID;
        });
        return writeBooksExcel(list);
    }

    void loadBooks() {
        CrossLock lock(dataMutex);
        loadBooksLocked();
    }

    void loadBooksLocked() {
        bookByID.clear();
        isbnToBookID.clear();

        ifstream file(BOOKS_EXCEL);
        if (!file.good()) return;

        stringstream buffer;
        buffer << file.rdbuf();
        string content = buffer.str();

        size_t pos = 0;
        int rowIdx = 0;
        while (true) {
            size_t rowStart = content.find("<Row", pos);
            if (rowStart == string::npos) break;
            size_t tagEnd = content.find('>', rowStart);
            if (tagEnd == string::npos) break;
            size_t rowEnd = content.find("</Row>", tagEnd);
            if (rowEnd == string::npos) break;

            string body = content.substr(tagEnd + 1, rowEnd - (tagEnd + 1));
            pos = rowEnd + 6;
            if (rowIdx++ == 0) continue;

            vector<string> cols = extractExcelCells(body);
            if (cols.size() < 11) continue;

            Book b;
            b.bookID         = cols[0];
            b.isbn           = cols[1];
            b.title          = cols[2];
            b.author         = cols[3];
            b.subject        = cols[4];
            b.floor          = safeStoi(cols[5], 0);
            b.shelf          = safeStoi(cols[6], 1);
            b.rack           = safeStoi(cols[7], 1);
            b.slot           = safeStoi(cols[8], 1);
            b.totalStock     = safeStoi(cols[9], 1);
            b.availableStock = safeStoi(cols[10], 0);

            if (!b.bookID.empty()) {
                bookByID[b.bookID] = b;
                isbnToBookID[b.isbn] = b.bookID;
            }
        }
    }

    bool saveHelpLocked() {
        ofstream out(HELP_EXCEL);
        if (!out.is_open()) {
            cout << "  Error: Close " << HELP_EXCEL << " in Excel first.\n";
            return false;
        }
        out << "<?xml version=\"1.0\"?>\n<?mso-application progid=\"Excel.Sheet\"?>\n";
        out << "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"\n";
        out << " xmlns:o=\"urn:schemas-microsoft-com:office:office\"\n";
        out << " xmlns:x=\"urn:schemas-microsoft-com:office:excel\"\n";
        out << " xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n";
        out << " <Styles><Style ss:ID=\"HeaderStyle\"><Font ss:Bold=\"1\" ss:Color=\"#FFFFFF\"/>";
        out << "<Interior ss:Color=\"#065F46\" ss:Pattern=\"Solid\"/></Style></Styles>\n";
        out << " <Worksheet ss:Name=\"HelpRecords\"><Table>\n";
        out << "   <Column ss:Width=\"70\"/><Column ss:Width=\"140\"/><Column ss:Width=\"135\"/>\n";
        out << "   <Column ss:Width=\"95\"/><Column ss:Width=\"220\"/><Column ss:Width=\"75\"/>\n";
        out << "   <Column ss:Width=\"75\"/><Column ss:Width=\"110\"/><Column ss:Width=\"85\"/>\n";
        out << "   <Column ss:Width=\"140\"/><Column ss:Width=\"180\"/>\n";

        vector<string> headers = {
            "TicketID", "DateTime", "StudentKiosk", "BookID", "BookTitle",
            "TargetFloor", "TargetShelf", "TargetLibrarianPC", "Status", "ResolvedTime", "LibrarianRemark"
        };
        out << "   <Row>\n";
        for (const string& h : headers) {
            out << "    <Cell ss:StyleID=\"HeaderStyle\"><Data ss:Type=\"String\">" << escapeXML(h) << "</Data></Cell>\n";
        }
        out << "   </Row>\n";

        for (const HelpRecord& r : helpRecords) {
            out << "   <Row>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << r.ticketID                   << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.dateTime)        << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.studentKioskID)  << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.bookID)          << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.bookTitle)       << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << r.targetFloor                << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"Number\">" << r.targetShelf                << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.targetTerminalID)<< "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.status)          << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.resolvedTime)    << "</Data></Cell>\n";
            out << "    <Cell><Data ss:Type=\"String\">" << escapeXML(r.librarianRemark) << "</Data></Cell>\n";
            out << "   </Row>\n";
        }
        out << "  </Table></Worksheet></Workbook>\n";
        out.close();
        return true;
    }

    void loadHelpRecords() {
        CrossLock lock(dataMutex);
        loadHelpLocked();
    }

    void loadHelpLocked() {
        helpRecords.clear();
        ifstream file(HELP_EXCEL);
        if (!file.good()) return;

        stringstream buffer;
        buffer << file.rdbuf();
        string content = buffer.str();

        size_t pos = 0;
        int rowIdx = 0;
        while (true) {
            size_t rowStart = content.find("<Row", pos);
            if (rowStart == string::npos) break;
            size_t tagEnd = content.find('>', rowStart);
            if (tagEnd == string::npos) break;
            size_t rowEnd = content.find("</Row>", tagEnd);
            if (rowEnd == string::npos) break;

            string body = content.substr(tagEnd + 1, rowEnd - (tagEnd + 1));
            pos = rowEnd + 6;
            if (rowIdx++ == 0) continue;

            vector<string> cols = extractExcelCells(body);
            if (cols.size() < 11) continue;

            HelpRecord r;
            r.ticketID         = safeStoi(cols[0], 1001);
            r.dateTime         = cols[1];
            r.studentKioskID   = cols[2];
            r.bookID           = cols[3];
            r.bookTitle        = cols[4];
            r.targetFloor      = safeStoi(cols[5], 0);
            r.targetShelf      = safeStoi(cols[6], 1);
            r.targetTerminalID = cols[7];
            r.status           = cols[8];
            r.resolvedTime     = cols[9];
            r.librarianRemark  = cols[10];
            helpRecords.push_back(r);
        }
    }

    void loadIssuedRecords() {
        issueRecords.clear();
        ifstream file(ISSUED_CSV);
        if (!file.good()) return;
        string line;
        getline(file, line);

        while (getline(file, line)) {
            if (trim(line).empty()) continue;
            auto cols = splitStr(line, ',');
            if (cols.size() < 7) continue;

            IssueRecord r;
            r.issueID       = safeStoi(cols[0]);
            r.bookID        = cols[1];
            r.bookTitle     = cols[2];
            r.studentRollNo = cols[3];
            r.issueDate     = cols[4];
            r.dueDate       = cols[5];
            r.status        = cols[6];
            issueRecords.push_back(r);
        }
    }

    bool saveIssuedRecords() {
        ofstream out(ISSUED_CSV);
        if (!out.is_open()) {
            cout << "  Error: Close " << ISSUED_CSV << " in Excel first.\n";
            return false;
        }
        out << "IssueID,BookID,BookTitle,StudentRollNo,IssueDate,DueDate,Status\n";
        for (const auto& r : issueRecords) {
            out << r.issueID << "," << r.bookID << "," << r.bookTitle << ","
                << r.studentRollNo << "," << r.issueDate << "," << r.dueDate << ","
                << r.status << "\n";
        }
        return true;
    }

    void initDefaultFiles() {
        ifstream excelCheck(BOOKS_EXCEL);
        if (!excelCheck.good()) {
            vector<Book> books = {
                {"BK101", "9780131103627", "The C Programming Language", "Dennis Ritchie", "Programming (1st Yr)", 0, 1, 1, 10, 8, 8},
                {"BK102", "9780262033848", "Introduction to Algorithms (CLRS)", "Thomas H. Cormen", "Data Structures & Algo", 0, 2, 2, 5, 10, 9},
                {"BK103", "9788126556762", "Higher Engineering Mathematics", "B.S. Grewal", "Engineering Maths (1st Yr)", 0, 3, 1, 12, 12, 12},
                {"BK104", "9780070151284", "Concepts of Modern Physics", "Arthur Beiser", "Engineering Physics (1st Yr)", 0, 4, 3, 18, 7, 7},
                {"BK105", "9788126519101", "Engineering Chemistry", "Jain & Jain", "Engineering Chemistry (1st Yr)", 0, 5, 2, 9, 7, 7},
                {"BK106", "9780070634558", "Basic Electrical Engineering", "D.C. Kulshreshtha", "Electrical Engg (1st Yr)", 0, 6, 4, 14, 9, 9},
                {"BK107", "9781118063330", "Operating System Concepts", "Abraham Silberschatz", "Operating Systems (2nd Yr)", 0, 7, 1, 22, 8, 7},
                {"BK108", "9780073523323", "Database System Concepts", "Henry F. Korth", "DBMS (2nd Yr)", 0, 8, 5, 11, 8, 8},
                {"BK109", "9780321563842", "The C++ Programming Language", "Bjarne Stroustrup", "OOP in C++ (2nd Yr)", 0, 9, 2, 27, 6, 6},
                {"BK110", "9781260440232", "Java: The Complete Reference", "Herbert Schildt", "Java Programming (2nd Yr)", 0, 10, 3, 15, 7, 7},
                {"BK111", "9780073383095", "Discrete Mathematics and Its Applications", "Kenneth H. Rosen", "Discrete Maths (2nd Yr)", 0, 12, 4, 8, 8, 8},
                {"BK112", "9780134549897", "Digital Design", "M. Morris Mano", "Digital Electronics (2nd Yr)", 0, 14, 1, 19, 9, 9},
                {"BK113", "9780132126953", "Computer Networks", "Andrew S. Tanenbaum", "Computer Networks (2nd Yr)", 0, 16, 2, 4, 8, 8},
                {"BK114", "9780124077263", "Computer Organization and Design", "David A. Patterson", "Computer Architecture (2nd Yr)", 0, 18, 3, 25, 6, 6},
                {"BK115", "9788131732090", "Engineering Mechanics: Statics & Dynamics", "R.C. Hibbeler", "Engineering Mechanics (1st Yr)", 0, 20, 5, 13, 7, 7},
                {"BK116", "9780134444321", "Electronic Devices and Circuit Theory", "Robert L. Boylestad", "Basic Electronics (1st Yr)", 0, 22, 1, 17, 8, 8},
                {"BK117", "9780132350884", "Clean Code", "Robert C. Martin", "Software Craftsmanship", 0, 25, 4, 6, 5, 5},
                {"BK118", "9788122434972", "Engineering Drawing and Graphics", "N.D. Bhatt", "Engineering Graphics (1st Yr)", 0, 28, 2, 30, 10, 10},

                {"BK119", "9781133187790", "Introduction to the Theory of Computation", "Michael Sipser", "Theory of Computation (3rd Yr)", 1, 1, 3, 11, 7, 7},
                {"BK120", "9780321486813", "Compilers: Principles, Techniques & Tools", "Alfred V. Aho", "Compiler Design (3rd Yr)", 1, 3, 2, 19, 6, 6},
                {"BK121", "9780078022159", "Software Engineering: A Practitioner's Approach", "Roger S. Pressman", "Software Engineering (3rd Yr)", 1, 5, 1, 7, 8, 8},
                {"BK122", "9780133594140", "Computer Networking: A Top-Down Approach", "James F. Kurose", "Computer Networks (3rd Yr)", 1, 7, 4, 24, 7, 7},
                {"BK123", "9788131701713", "Microprocessor Architecture 8085", "Ramesh Gaonkar", "Microprocessors (3rd Yr)", 1, 9, 5, 14, 9, 9},
                {"BK124", "9780137156528", "Modern Control Engineering", "Katsuhiko Ogata", "Control Systems (3rd Yr)", 1, 11, 2, 3, 6, 6},
                {"BK125", "9780138147570", "Signals and Systems", "Alan V. Oppenheim", "Signals & Systems (2nd Yr)", 1, 13, 3, 28, 7, 7},
                {"BK126", "9788173715273", "Fundamentals of Computer Algorithms", "Ellis Horowitz & Sahni", "Design & Analysis of Algo (3rd Yr)", 1, 15, 1, 16, 8, 8},
                {"BK127", "9780136042594", "Computer System Architecture", "M. Morris Mano", "COA (2nd Yr)", 1, 16, 4, 9, 7, 7},
                {"BK128", "9780070648227", "Principles of Communication Systems", "Herbert Taub", "Analog & Digital Comm (3rd Yr)", 1, 17, 2, 21, 6, 6},
                {"BK129", "9780321982384", "Linear Algebra and Its Applications", "David C. Lay", "Applied Mathematics (2nd Yr)", 1, 19, 5, 12, 8, 8},
                {"BK130", "9780201633610", "Design Patterns: Elements of Reusable OO Software", "Erich Gamma", "Object Oriented Design (3rd Yr)", 1, 21, 3, 5, 5, 5},
                {"BK131", "9780070636767", "Electromagnetic Waves and Radiating Systems", "Edward C. Jordan", "Electromagnetics (3rd Yr)", 1, 23, 1, 18, 5, 5},
                {"BK132", "9788120340077", "Object Oriented Programming with C++", "E. Balagurusamy", "OOP (2nd Yr)", 1, 25, 4, 26, 10, 10},
                {"BK133", "9781449355739", "Learning Python", "Mark Lutz", "Python Programming (2nd Yr)", 1, 27, 2, 15, 8, 8},
                {"BK134", "9780131989269", "Discrete-Time Signal Processing", "Alan V. Oppenheim", "DSP (3rd Yr)", 1, 29, 3, 8, 6, 6},
                {"BK135", "9788126538027", "Probability and Statistics for Engineers", "Richard A. Johnson", "Probability & Stats (2nd Yr)", 1, 30, 5, 20, 7, 7},

                {"BK136", "9780134610993", "Artificial Intelligence: A Modern Approach", "Stuart Russell & Norvig", "Artificial Intelligence (4th Yr)", 2, 2, 1, 14, 8, 8},
                {"BK137", "9780070428072", "Machine Learning", "Tom M. Mitchell", "Machine Learning (4th Yr)", 2, 4, 3, 7, 8, 8},
                {"BK138", "9780262035613", "Deep Learning", "Ian Goodfellow", "Deep Learning (4th Yr)", 2, 6, 2, 23, 6, 6},
                {"BK139", "9780134444284", "Cryptography and Network Security", "William Stallings", "Network Security (4th Yr)", 2, 8, 4, 11, 7, 7},
                {"BK140", "9780123858801", "Distributed and Cloud Computing", "Kai Hwang", "Cloud Computing (4th Yr)", 2, 10, 5, 19, 6, 6},
                {"BK141", "9780132143011", "Distributed Systems: Concepts and Design", "George Coulouris", "Distributed Systems (4th Yr)", 2, 12, 1, 29, 5, 5},
                {"BK142", "9780123814791", "Data Mining: Concepts and Techniques", "Jiawei Han", "Data Mining & Warehousing (4th Yr)", 2, 14, 2, 4, 7, 7},
                {"BK143", "9781491901427", "Hadoop: The Definitive Guide", "Tom White", "Big Data Analytics (4th Yr)", 2, 15, 3, 16, 5, 5},
                {"BK144", "9788173719547", "Internet of Things: A Hands-On Approach", "Arshdeep Bahga", "Internet of Things (4th Yr)", 2, 16, 4, 10, 7, 7},
                {"BK145", "9780070671119", "CMOS Digital Integrated Circuits", "Sung-Mo Kang", "VLSI Design (4th Yr)", 2, 18, 1, 25, 6, 6},
                {"BK146", "9780070667648", "Embedded Systems: Architecture & Programming", "Raj Kamal", "Embedded Systems (4th Yr)", 2, 20, 5, 13, 7, 7},
                {"BK147", "9780133354690", "Digital Image Processing", "Rafael C. Gonzalez", "Image Processing (4th Yr)", 2, 22, 2, 8, 6, 6},
                {"BK148", "9788126557127", "Principles of Soft Computing", "S.N. Sivanandam", "Soft Computing (4th Yr)", 2, 24, 3, 21, 5, 5},
                {"BK149", "9780130422323", "Wireless Communications: Principles & Practice", "Theodore S. Rappaport", "Wireless Comm (4th Yr)", 2, 26, 4, 17, 6, 6},
                {"BK150", "9781491957660", "Python for Data Analysis", "Wes McKinney", "Data Science (4th Yr)", 2, 30, 1, 2, 8, 8}
            };
            writeBooksExcel(books);
        }

        ifstream helpCheck(HELP_EXCEL);
        if (!helpCheck.good()) {
            saveHelpLocked();
        }

        ifstream issueCheck(ISSUED_CSV);
        if (!issueCheck.good()) {
            ofstream out(ISSUED_CSV);
            out << "IssueID,BookID,BookTitle,StudentRollNo,IssueDate,DueDate,Status\n";
            out << "5001,BK102,Introduction to Algorithms (CLRS),CS2024015,2026-11-10,2026-11-25,ISSUED\n";
            out << "5002,BK107,Operating System Concepts,EC2024088,2026-11-01,2026-11-12,ISSUED\n";
            out.close();
        }
    }

    void handleClient(SocketType clientSock) {
        setSocketTimeout(clientSock, 300);

        while (running) {
            string req = recvLine(clientSock);
            if (req.empty() || req == "QUIT") break;

            vector<string> parts = splitStr(req, '|');
            string cmd = parts.empty() ? "" : parts[0];

            if (cmd == "PING") {
                if (!sendLine(clientSock, "PONG|SmartLibraryServer")) break;
            }
            else if (cmd == "SEARCH" && parts.size() >= 2) {
                string query = parts[1];
                string upperQ = toUpperStr(query);

                Book foundCopy;
                bool isFound = false;

                {
                    CrossLock lock(dataMutex);
                    if (bookByID.find(upperQ) != bookByID.end()) {
                        foundCopy = bookByID[upperQ];
                        isFound = true;
                    } else if (isbnToBookID.find(query) != isbnToBookID.end()) {
                        foundCopy = bookByID[isbnToBookID[query]];
                        isFound = true;
                    } else {
                        string lowerQ = toLowerStr(query);
                        for (const auto& pair : bookByID) {
                            if (toLowerStr(pair.second.title).find(lowerQ) != string::npos ||
                                toLowerStr(pair.second.subject).find(lowerQ) != string::npos ||
                                toLowerStr(pair.second.bookID).find(lowerQ) != string::npos) {
                                foundCopy = pair.second;
                                isFound = true;
                                break;
                            }
                        }
                    }
                }

                if (!isFound) {
                    if (!sendLine(clientSock, "NOT_FOUND")) break;
                } else {
                    DeskTerminal desk = getTerminal(foundCopy.floor, getZoneFromShelf(foundCopy.shelf));
                    string resp = "FOUND|" + foundCopy.bookID + "|" + foundCopy.isbn + "|" + foundCopy.title + "|" +
                                  foundCopy.author + "|" + foundCopy.subject + "|" + to_string(foundCopy.floor) + "|" +
                                  to_string(foundCopy.shelf) + "|" + to_string(foundCopy.rack) + "|" +
                                  to_string(foundCopy.slot) + "|" + to_string(foundCopy.totalStock) + "|" +
                                  to_string(foundCopy.availableStock) + "|" + desk.terminalID + "|" + desk.deskLabel;
                    if (!sendLine(clientSock, resp)) break;
                }
            }
            else if (cmd == "HELP" && parts.size() >= 8) {
                string kioskID    = parts[1];
                int kioskFloor    = safeStoi(parts[2], 0);
                int kioskSide     = safeStoi(parts[3], 1);
                string bookID     = parts[4];
                string bookTitle  = parts[5];
                int targetFloor   = safeStoi(parts[6], kioskFloor);
                int targetShelf   = safeStoi(parts[7], 1);

                if (find(VALID_FLOORS.begin(), VALID_FLOORS.end(), targetFloor) == VALID_FLOORS.end()) {
                    targetFloor = kioskFloor;
                }
                int zone = (targetShelf >= 1 && targetShelf <= 30) ? getZoneFromShelf(targetShelf) : kioskSide;
                DeskTerminal targetDesk = getTerminal(targetFloor, zone);

                HelpRecord rec;
                {
                    CrossLock lock(dataMutex);
                    rec.ticketID         = static_cast<int>(helpRecords.size()) + 1001;
                    rec.dateTime         = getCurrentDateTime();
                    rec.studentKioskID   = kioskID;
                    rec.bookID           = bookID;
                    rec.bookTitle        = bookTitle;
                    rec.targetFloor      = targetFloor;
                    rec.targetShelf      = targetShelf;
                    rec.targetTerminalID = targetDesk.terminalID;
                    rec.status           = "PENDING";
                    rec.resolvedTime     = "-";
                    rec.librarianRemark  = "-";

                    helpRecords.push_back(rec);
                    saveHelpLocked();
                }

                cout << "\n\a  [New Help Alert] Ticket #" << rec.ticketID
                     << " | " << rec.dateTime
                     << " | From: " << rec.studentKioskID
                     << " -> " << rec.targetTerminalID
                     << " | Book: " << rec.bookID << " (" << rec.bookTitle << ")\n";
                cout << "  Choice: " << flush;

                string resp = "HELP_OK|" + to_string(rec.ticketID) + "|" + rec.dateTime + "|" +
                              targetDesk.terminalID + "|" + targetDesk.deskLabel;
                if (!sendLine(clientSock, resp)) break;
            }
        }

        shutdown(clientSock, SHUT_SEND);
        CLOSE_SOCKET(clientSock);
    }

    void udpDiscoveryLoop() {
        SocketType udpSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSock == INVALID_SOCK) return;

        int opt = 1;
        setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(DISCOVERY_PORT);

        if (::bind(udpSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            CLOSE_SOCKET(udpSock);
            return;
        }

        char buf[128];
        while (running) {
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            int n = recvfrom(udpSock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&clientAddr, &len);
            if (n > 0) {
                buf[n] = '\0';
                if (string(buf).find("DISCOVER_LIB_SERVER") != string::npos) {
                    string reply = "LIB_SERVER_HERE";
                    sendto(udpSock, reply.c_str(), static_cast<int>(reply.size()), 0,
                           (struct sockaddr*)&clientAddr, len);
                }
            }
        }
        CLOSE_SOCKET(udpSock);
    }

    void acceptLoop() {
        SocketType serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock == INVALID_SOCK) return;

        int opt = 1;
        setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(SERVER_PORT);

        if (::bind(serverSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            cout << "\n  ERROR: Port " << SERVER_PORT << " is already in use by an old instance!\n";
            cout << "  Close any other smart_library.exe windows and restart.\n" << flush;
            CLOSE_SOCKET(serverSock);
            return;
        }

        listen(serverSock, 10);

        while (running) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            SocketType clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSock != INVALID_SOCK) {
#ifdef _WIN32
                ClientArgs* args = new ClientArgs{this, clientSock};
                HANDLE hClient = CreateThread(nullptr, 0, clientThreadProc, args, 0, nullptr);
                if (hClient != nullptr) CloseHandle(hClient);
                else {
                    handleClient(clientSock);
                    delete args;
                }
#else
                std::thread(&LibrarianServer::handleClient, this, clientSock).detach();
#endif
            }
        }
        CLOSE_SOCKET(serverSock);
    }

    void startListener() {
#ifdef _WIN32
        HANDLE hThread = CreateThread(nullptr, 0, serverThreadProc, this, 0, nullptr);
        if (hThread != nullptr) CloseHandle(hThread);

        HANDLE hUdp = CreateThread(nullptr, 0, udpThreadProc, this, 0, nullptr);
        if (hUdp != nullptr) CloseHandle(hUdp);
#else
        std::thread(&LibrarianServer::acceptLoop, this).detach();
        std::thread(&LibrarianServer::udpDiscoveryLoop, this).detach();
#endif
    }

    void showOverdueBooks() {
        CrossLock lock(dataMutex);
        bool any = false;
        for (const auto& r : issueRecords) {
            if (r.status == "ISSUED" && todayDate > r.dueDate) {
                if (!any) {
                    cout << "\n  Overdue Books:\n";
                    any = true;
                }
                cout << "    - Issue #" << r.issueID << " | " << r.bookID << " (" << r.bookTitle << ")"
                     << " | Roll: " << r.studentRollNo << " | Due: " << r.dueDate << "\n";
            }
        }
    }

    void issueBook() {
        cout << "\n  Book ID: " << flush;
        string bID;
        getline(cin, bID);
        bID = toUpperStr(trim(bID));

        cout << "  Student Roll No: " << flush;
        string rollNo;
        getline(cin, rollNo);
        rollNo = toUpperStr(trim(rollNo));

        cout << "  Due Date (YYYY-MM-DD): " << flush;
        string dueDate;
        getline(cin, dueDate);
        dueDate = trim(dueDate);

        if (!isValidDate(dueDate)) {
            cout << "  Invalid date format.\n";
            return;
        }

        CrossLock lock(dataMutex);
        if (bookByID.find(bID) == bookByID.end()) {
            cout << "  Book ID not found.\n";
            return;
        }

        Book& b = bookByID[bID];
        if (b.availableStock <= 0) {
            cout << "  Out of stock: " << b.title << "\n";
            return;
        }

        b.availableStock -= 1;
        if (!saveBooksLocked()) {
            b.availableStock += 1;
            return;
        }

        IssueRecord rec;
        rec.issueID       = static_cast<int>(issueRecords.size()) + 5001;
        rec.bookID        = b.bookID;
        rec.bookTitle     = b.title;
        rec.studentRollNo = rollNo;
        rec.issueDate     = todayDate;
        rec.dueDate       = dueDate;
        rec.status        = "ISSUED";

        issueRecords.push_back(rec);
        saveIssuedRecords();

        cout << "  Issued #" << rec.issueID << " | " << rec.bookID << " to " << rec.studentRollNo
             << " (Due: " << rec.dueDate << ", Stock left: " << b.availableStock << ")\n";
    }

    void returnBook() {
        cout << "\n  Book ID: " << flush;
        string bID;
        getline(cin, bID);
        bID = toUpperStr(trim(bID));

        cout << "  Student Roll No: " << flush;
        string rollNo;
        getline(cin, rollNo);
        rollNo = toUpperStr(trim(rollNo));

        CrossLock lock(dataMutex);
        for (auto& r : issueRecords) {
            if (r.bookID == bID && toUpperStr(r.studentRollNo) == rollNo && r.status == "ISSUED") {
                if (bookByID.find(bID) != bookByID.end()) {
                    Book& b = bookByID[bID];
                    int oldStock = b.availableStock;
                    if (b.availableStock < b.totalStock) b.availableStock += 1;
                    if (!saveBooksLocked()) {
                        b.availableStock = oldStock;
                        return;
                    }
                    r.status = "RETURNED";
                    saveIssuedRecords();
                    cout << "  Returned: " << b.bookID << " (" << b.title << ") | Stock: "
                         << b.availableStock << "/" << b.totalStock
                         << (todayDate > r.dueDate ? " [Late Return]" : " [On Time]") << "\n";
                } else {
                    r.status = "RETURNED";
                    saveIssuedRecords();
                    cout << "  Returned: " << bID << "\n";
                }
                return;
            }
        }
        cout << "  No active issue record found.\n";
    }

    void viewMyZoneBooks(const DeskTerminal& desk) {
        CrossLock lock(dataMutex);
        int s1 = (desk.zone == 1) ? 1 : 16;
        int s2 = (desk.zone == 1) ? 15 : 30;

        cout << "\n  --- Books in " << desk.deskLabel << " ---\n";
        cout << "  " << left << setw(8) << "ID" << setw(32) << "Title" << setw(22) << "Subject"
             << setw(16) << "Location" << "Stock\n";
        cout << "  ------------------------------------------------------------------------------------\n";

        int count = 0;
        for (const auto& pair : bookByID) {
            const Book& b = pair.second;
            if (b.floor == desk.floor && b.shelf >= s1 && b.shelf <= s2) {
                string loc = "S" + to_string(b.shelf) + "-R" + to_string(b.rack) + "-P" + to_string(b.slot);
                string stk = to_string(b.availableStock) + "/" + to_string(b.totalStock);
                cout << "  " << left << setw(8) << b.bookID << setw(32) << b.title.substr(0, 30)
                     << setw(22) << b.subject.substr(0, 20) << setw(16) << loc << stk << "\n";
                count++;
            }
        }
        if (count == 0) cout << "  No books in this zone.\n";
    }

    void searchBooks(const DeskTerminal& desk) {
        cout << "\n  Scope: 1. My Floor (" << desk.floor << ")  2. Pick Floor (0/1/2)  3. All Floors: " << flush;
        string scope;
        getline(cin, scope);

        int filterFloor = 999;
        if (scope == "1") filterFloor = desk.floor;
        else if (scope == "2") filterFloor = readInt("  Floor (0/1/2): ", desk.floor);

        cout << "  Search (ID, Name, Subject, or Enter for all): " << flush;
        string query;
        getline(cin, query);
        string lowerQ = toLowerStr(trim(query));

        CrossLock lock(dataMutex);
        cout << "\n  " << left << setw(8) << "ID" << setw(32) << "Title" << setw(22) << "Subject"
             << setw(18) << "Location" << "Stock\n";
        cout << "  --------------------------------------------------------------------------------------\n";

        int matches = 0;
        for (const auto& pair : bookByID) {
            const Book& b = pair.second;
            if (filterFloor != 999 && b.floor != filterFloor) continue;

            if (lowerQ.empty() ||
                toLowerStr(b.bookID).find(lowerQ) != string::npos ||
                toLowerStr(b.title).find(lowerQ) != string::npos ||
                toLowerStr(b.subject).find(lowerQ) != string::npos) {
                string loc = "F" + to_string(b.floor) + "-S" + to_string(b.shelf) +
                             "-R" + to_string(b.rack) + "-P" + to_string(b.slot);
                string stk = to_string(b.availableStock) + "/" + to_string(b.totalStock);
                cout << "  " << left << setw(8) << b.bookID << setw(32) << b.title.substr(0, 30)
                     << setw(22) << b.subject.substr(0, 20) << setw(18) << loc << stk << "\n";
                matches++;
            }
        }
        if (matches == 0) cout << "  No matching books found.\n";
    }

    void viewHelpHistory() {
        CrossLock lock(dataMutex);
        cout << "\n  --- Help Records (" << HELP_EXCEL << ") ---\n";
        if (helpRecords.empty()) {
            cout << "  No records yet.\n";
            return;
        }
        for (const auto& r : helpRecords) {
            cout << "  #" << r.ticketID << " [" << r.status << "] | " << r.dateTime
                 << " | " << r.studentKioskID << " -> " << r.targetTerminalID
                 << " | " << r.bookID << " (" << r.bookTitle << ")\n";
            if (r.status == "RESOLVED") {
                cout << "     Resolved: " << r.resolvedTime << " (" << r.librarianRemark << ")\n";
            }
        }
    }

    void run() {
        startListener();

        cout << "\n  Server listening on port " << SERVER_PORT << "\n";
        vector<string> localIPs = getLocalIPs();
        if (!localIPs.empty()) {
            cout << "  This Server IP(s): ";
            for (size_t i = 0; i < localIPs.size(); ++i) {
                cout << localIPs[i] << (i + 1 < localIPs.size() ? " | " : "");
            }
            cout << "\n";
        }

        cout << "  Terminals: F0_LIB1, F0_LIB2, F1_LIB1, F1_LIB2, F2_LIB1, F2_LIB2\n";
        cout << "  Terminal ID: " << flush;
        string id;
        getline(cin, id);
        id = toUpperStr(trim(id));

        cout << "  Password: " << flush;
        string pass;
        getline(cin, pass);
        pass = trim(pass);

        if (terminalsByID.find(id) == terminalsByID.end() || terminalsByID[id].password != pass) {
            cout << "  Invalid login, defaulting to F0_LIB1.\n";
            id = "F0_LIB1";
        }

        DeskTerminal desk = terminalsByID[id];

        while (true) {
            cout << "\n  -----------------------------------------------------------------\n";
            cout << "  Desk: " << desk.terminalID << " (" << desk.deskLabel << ") | "
                 << todayDate << " | " << getActiveShift() << "\n";
            cout << "  -----------------------------------------------------------------\n";

            showOverdueBooks();

            {
                CrossLock lock(dataMutex);
                bool hasPending = false;
                for (const auto& r : helpRecords) {
                    if (r.targetTerminalID == desk.terminalID && r.status == "PENDING") {
                        if (!hasPending) {
                            cout << "\n  Pending Help Requests:\n";
                            hasPending = true;
                        }
                        cout << "    - Ticket #" << r.ticketID << " | " << r.dateTime
                             << " | " << r.studentKioskID << " | " << r.bookID << " (" << r.bookTitle << ")\n";
                    }
                }
            }

            cout << "\n  1. Issue Book\n";
            cout << "  2. Return Book\n";
            cout << "  3. My Zone Books (" << getZoneText(desk.zone) << ")\n";
            cout << "  4. Search Books\n";
            cout << "  5. Resolve Help Ticket\n";
            cout << "  6. Help History\n";
            cout << "  7. Refresh\n";
            cout << "  0. Exit\n";
            cout << "  Choice: " << flush;

            string choice;
            getline(cin, choice);
            choice = trim(choice);

            if (choice == "0") {
                running = false;
                break;
            }
            else if (choice == "1") issueBook();
            else if (choice == "2") returnBook();
            else if (choice == "3") viewMyZoneBooks(desk);
            else if (choice == "4") searchBooks(desk);
            else if (choice == "5") {
                int tID = readInt("  Ticket ID to resolve: ", -1);
                cout << "  Remark (or Enter): " << flush;
                string remark;
                getline(cin, remark);
                if (trim(remark).empty()) remark = "Assisted";

                CrossLock lock(dataMutex);
                bool found = false;
                for (auto& r : helpRecords) {
                    if (r.ticketID == tID) {
                        r.status = "RESOLVED";
                        r.resolvedTime = getCurrentDateTime();
                        r.librarianRemark = trim(remark);
                        found = true;
                        if (saveHelpLocked()) {
                            cout << "  Ticket #" << tID << " marked resolved.\n";
                        }
                        break;
                    }
                }
                if (!found) cout << "  Ticket ID not found.\n";
            }
            else if (choice == "6") viewHelpHistory();
            else if (choice == "7") continue;
        }
    }
};

class StudentKioskApp {
private:
    string serverIP;
    string lastNetError;
    StudentKiosk kiosk;
    LibraryNavigator nav;
    SocketType sessionSock = INVALID_SOCK;

    void closeSession() {
        if (sessionSock != INVALID_SOCK) {
            sendLine(sessionSock, "QUIT");
            shutdown(sessionSock, SHUT_SEND);
            CLOSE_SOCKET(sessionSock);
            sessionSock = INVALID_SOCK;
        }
    }

    bool ensureConnected() {
        if (sessionSock != INVALID_SOCK) return true;
        sessionSock = connectWithTimeout(serverIP, SERVER_PORT, 5, lastNetError);
        return sessionSock != INVALID_SOCK;
    }

    string autoDiscoverServer() {
        SocketType udpSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSock == INVALID_SOCK) return "";

        int bcast = 1;
        setsockopt(udpSock, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));
        setSocketTimeout(udpSock, 2);

        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(DISCOVERY_PORT);
        bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

        string msg = "DISCOVER_LIB_SERVER";
        sendto(udpSock, msg.c_str(), static_cast<int>(msg.size()), 0,
               (struct sockaddr*)&bcastAddr, sizeof(bcastAddr));

        char buf[128];
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        int n = recvfrom(udpSock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&fromAddr, &fromLen);
        CLOSE_SOCKET(udpSock);

        if (n > 0) {
            buf[n] = '\0';
            if (string(buf).find("LIB_SERVER_HERE") != string::npos) {
                return string(inet_ntoa(fromAddr.sin_addr));
            }
        }
        return "";
    }

    string sendRequest(const string& payload) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensureConnected()) {
                string rediscovered = autoDiscoverServer();
                if (!rediscovered.empty() && rediscovered != serverIP) {
                    serverIP = rediscovered;
                    sessionSock = connectWithTimeout(serverIP, SERVER_PORT, 5, lastNetError);
                }
                if (sessionSock == INVALID_SOCK) return "ERR";
            }

            if (sendLine(sessionSock, payload)) {
                string reply = recvLine(sessionSock);
                if (!reply.empty()) return reply;
                lastNetError = "Server closed connection";
            } else {
                lastNetError = "Failed sending request";
            }

            CLOSE_SOCKET(sessionSock);
            sessionSock = INVALID_SOCK;
        }
        return "ERR";
    }

    void requestHelp(const string& bookID, const string& bookTitle, int targetFloor, int targetShelf) {
        string payload = "HELP|" + kiosk.kioskID + "|" + to_string(kiosk.floor) + "|" +
                         to_string(kiosk.sideZone) + "|" + bookID + "|" + bookTitle + "|" +
                         to_string(targetFloor) + "|" + to_string(targetShelf);

        string resp = sendRequest(payload);
        vector<string> parts = splitStr(resp, '|');

        if (parts.size() >= 5 && parts[0] == "HELP_OK") {
            cout << "\n  Help sent to " << parts[3] << " | Ticket #" << parts[1]
                 << " | " << parts[2] << " | Book: " << bookID << "\n";
        } else {
            cout << "\n  Connection lost to server (" << serverIP << "): " << lastNetError << "\n";
        }
    }

public:
    ~StudentKioskApp() {
        closeSession();
    }

    void run() {
        cout << "\n  Searching for Librarian Server on Wi-Fi...\n" << flush;
        serverIP = autoDiscoverServer();
        if (!serverIP.empty()) {
            cout << "  Auto-detected Server IP: " << serverIP << "\n";
        } else {
            cout << "  Enter Server IP shown on Laptop 1: " << flush;
            getline(cin, serverIP);
            serverIP = trim(serverIP);
            if (serverIP.empty()) serverIP = "127.0.0.1";
        }

        if (sendRequest("PING").find("PONG") == string::npos) {
            cout << "  Could not connect to server at " << serverIP << ":" << SERVER_PORT
                 << " (" << lastNetError << ")\n";
            return;
        }
        cout << "  Connected to server.\n";

        int fl = readInt("  Floor (0 / 1 / 2): ", 0);
        if (find(VALID_FLOORS.begin(), VALID_FLOORS.end(), fl) == VALID_FLOORS.end()) fl = 0;

        int comp = readInt("  Computer No (1-6): ", 1);
        if (comp < 1 || comp > 6) comp = 1;

        int side = (comp <= 3) ? 1 : 2;
        kiosk = {"UserFloor" + to_string(fl) + "_Comp" + to_string(comp), fl, comp, side};

        while (true) {
            cout << "\n  -------------------------------------------------------\n";
            cout << "  " << kiosk.kioskID << " | Floor " << kiosk.floor << " | " << getZoneText(kiosk.sideZone) << "\n";
            cout << "  -------------------------------------------------------\n";
            cout << "  1. Search Book\n";
            cout << "  H. Request Help\n";
            cout << "  0. Exit\n";
            cout << "  Choice: " << flush;

            string choice;
            getline(cin, choice);
            choice = trim(choice);

            if (choice == "0") {
                closeSession();
                break;
            }
            else if (choice == "H" || choice == "h") {
                cout << "  Book ID or Title (or Enter for general help): " << flush;
                string q;
                getline(cin, q);
                q = trim(q);

                if (q.empty()) {
                    requestHelp("GENERAL", "Floor Assistance", kiosk.floor, (kiosk.sideZone == 1 ? 1 : 16));
                } else {
                    string sResp = sendRequest("SEARCH|" + q);
                    vector<string> sp = splitStr(sResp, '|');
                    if (sp.size() >= 12 && sp[0] == "FOUND") {
                        requestHelp(sp[1], sp[3], safeStoi(sp[6], kiosk.floor), safeStoi(sp[7], 1));
                    } else {
                        requestHelp("UNLISTED", q, kiosk.floor, (kiosk.sideZone == 1 ? 1 : 16));
                    }
                }
            }
            else if (choice == "1") {
                cout << "  Search (Book ID, ISBN, Title, or Subject): " << flush;
                string query;
                getline(cin, query);
                query = trim(query);
                if (query.empty()) continue;

                string resp = sendRequest("SEARCH|" + query);
                if (resp == "ERR") {
                    cout << "  Server not responding (" << lastNetError << ").\n";
                    continue;
                }

                vector<string> p = splitStr(resp, '|');
                if (p.size() < 14 || p[0] != "FOUND") {
                    cout << "  Book not found. Press [H] for help or Enter to continue: " << flush;
                    string hOpt;
                    getline(cin, hOpt);
                    if (trim(hOpt) == "H" || trim(hOpt) == "h") {
                        requestHelp("UNLISTED", query, kiosk.floor, (kiosk.sideZone == 1 ? 1 : 16));
                    }
                    continue;
                }

                string bID    = p[1];
                string isbn   = p[2];
                string title  = p[3];
                string author = p[4];
                string subj   = p[5];
                int bFloor    = safeStoi(p[6]);
                int bShelf    = safeStoi(p[7]);
                int bRack     = safeStoi(p[8]);
                int bSlot     = safeStoi(p[9]);
                int totStk    = safeStoi(p[10]);
                int avlStk    = safeStoi(p[11]);
                string deskID = p[12];

                cout << "\n  Book ID  : " << bID << " (ISBN: " << isbn << ")\n";
                cout << "  Title    : " << title << "\n";
                cout << "  Author   : " << author << " | " << subj << "\n";
                cout << "  Status   : " << (avlStk > 0 ? "Available" : "Out of Stock")
                     << " (" << avlStk << "/" << totStk << ")\n";
                cout << "  Location : Floor " << bFloor << " -> Shelf " << bShelf
                     << " -> Rack " << bRack << " -> Slot " << bSlot << "\n";

                nav.printRoute(kiosk, bFloor, bShelf, bRack, bSlot);

                cout << "\n  Press [H] for help (" << deskID << ") or Enter to continue: " << flush;
                string act;
                getline(cin, act);
                if (trim(act) == "H" || trim(act) == "h") {
                    requestHelp(bID, title, bFloor, bShelf);
                }
            }
        }
    }
};

int main() {
    disableConsoleQuickEdit();
    initNetwork();

    cout << "  Smart Library System\n";
    cout << "  1. Librarian Server\n";
    cout << "  2. Student Kiosk\n";
    cout << "  Choice: " << flush;

    string role;
    getline(cin, role);
    role = trim(role);

    if (role == "1") {
        LibrarianServer server;
        server.run();
    } else if (role == "2") {
        StudentKioskApp client;
        client.run();
    }

    cleanupNetwork();
    return 0;
}