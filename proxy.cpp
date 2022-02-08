// Built-in library
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <signal.h>
#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <sys/mman.h>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

// Reference library
#include "string.cc"
#include "color.hpp"

// Constants
#define MAX_CONTENT_SIZE 99999
#define ERROR_RESPONSE "HTTP/1.1 200 OK\nDate: Wed, 29 Sep 2021 08:22:51 GMT\nServer: Apache/2.2.15 (Scientific Linux)\nLast-Modified: Wed, 29 Sep 2021 01:30:37 GMT\nETag: \"442483e-14e-5cd1846fcaa4b\"\nAccept-Ranges: bytes\nContent-Length: 334\nConnection: close\nContent-Type: text/html; charset=UTF-8\n\n<html><title>Simple URL Censorship Error Page for CPSC 441 Assignment 1</title><body><h1>NO!!</h1><p>Sorry, but the Web page that you were trying to access has a <b>URL</b> that is inappropriate.</p><p>The page has been blocked to avoid insulting your intelligence.</p><p>Web Censorship Proxy</p></body></html>"

// Using namespace
using namespace std;
using namespace rang;
using namespace boost::interprocess;


// Get Ip address by given hostname
string getHostIp(string hostname)
{
    struct addrinfo hints, * res;
    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(hostname.c_str(), NULL, &hints, &res);

    sockaddr_in* address = (sockaddr_in*)res->ai_addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(res->ai_family, &(address->sin_addr), ip_str, INET_ADDRSTRLEN);
    cout << "IP Address: " << ip_str << endl;

    return string(ip_str);
}

// Initialize the server socket
int server_socket_initialize(string hostname = "pages.cpsc.ucalgary.ca") {
    string ip = getHostIp(hostname);

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(80);
    server.sin_addr.s_addr = inet_addr(ip.c_str());

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        printf("proxy-server: socket() call failed");
        cout << fg::red << "Proxy-server: socket() call failed." << fg::reset << endl;
        return -1;
    }

    int server_connection = connect(server_socket, (struct sockaddr*)&server, sizeof(struct sockaddr_in));
    if (server_connection == -1) {
        cout << fg::red << "Proxy-server: connect() call failed." << fg::reset << endl;
        return -1;
    }

    return server_socket;
}

// Initialize client socket with designated port 
int client_socket_initialize(int port) {
    struct sockaddr_in client;
    memset(&client, 0, sizeof(client));
    client.sin_family = AF_INET;
    client.sin_port = htons(port);
    client.sin_addr.s_addr = htonl(INADDR_ANY);

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        cout << fg::red << "Proxy-client: socket() call failed." << fg::reset << endl;
        return -1;
    }

    int client_bind = bind(client_socket, (struct sockaddr*)&client, sizeof(struct sockaddr_in));
    if (client_bind == -1) {
        cout << fg::red << "Proxy-client: bind() call failed." << fg::reset << endl;
        return -1;
    }

    int client_listen = listen(client_socket, 5);
    if (client_listen == -1)
    {
        cout << fg::red << "Proxy-client: listen() call failed." << fg::reset << endl;
        return -1;
    }

    return client_socket;
}


// Get the full array of block words list from shared memory
vector<string> get_block_list() {
    vector<string> block_list;

    shared_memory_object shm(open_only, "block_list", read_only);
    mapped_region region(shm, read_only);

    string word("");
    char* mem = static_cast<char*>(region.get_address());
    for (std::size_t i = 0; i < region.get_size(); ++i) {
        char character = *mem;
        if (character == '\n') {
            block_list.push_back(word);
            word = "";
        } else {
            word += character;
        }
        *mem++;
    }
    return block_list;
}

// Set the full array of block words list to shared memory
void set_block_list(vector<string> block_list) {
    shared_memory_object shm(open_only, "block_list", read_write);
    mapped_region region(shm, read_write);
    memset(region.get_address(), 0, region.get_size()); // Reset memory

    char* mem = static_cast<char*>(region.get_address());
    for (string word : block_list) {
        word += '\n';
        for (char const& character : word) {
            memset(mem, character, sizeof(char));
            *mem++;
        }
    }
}


// HeaderParsable class
class HeaderParsable {
public:
    unordered_map<string, string> headers;

protected:
    virtual void parseHeaders(string& str) {
        vector<string> lines = StringExtension::split(str, '\n');
        for (auto& line : lines) {
            if (line == "") break;
            vector<string> pair = StringExtension::split(line, ':');
            if (pair.size() < 2) continue;

            string key(StringExtension::trim(pair[0]));
            string value(StringExtension::trim(pair[1]));
            value.erase(remove(value.begin(), value.end(), '\n'), value.end()); // Remove trailing newline
            value.erase(remove(value.begin(), value.end(), '\r'), value.end()); // Remove trailing newline
            headers[StringExtension::trim(pair[0])] = value;
        }
    }

    virtual string generateHeaders() {
        string headers_string = "";
        for (const auto& [key, value] : headers) {
            headers_string += key + ": " + value + "\n";
        }
        return headers_string;
    }
};

// Request object class
class Request : public HeaderParsable {
public:
    string method;
    string url;
    string httpVersion;
    string host;

    void parseRequest(string& str) {
        vector<string> lines = StringExtension::split(str, '\n');
        vector<string> triple = StringExtension::split(lines[0], ' ');
        if (triple.size() < 3) return;

        method = StringExtension::trim(triple[0]);
        url = StringExtension::trim(triple[1]);
        httpVersion = StringExtension::trim(triple[2]);
        HeaderParsable::parseHeaders(str);
        host = headers["Host"];
    }

    string generateRequest() {
        string request_string = "";
        request_string = method + " " + url + " HTTP/1.1\n";
        request_string += HeaderParsable::generateHeaders() + "\n";
        return request_string;
    }


    bool hasBlockedWord(string word) {
        return url.find(word) != string::npos;
    }

    bool hasBlockedWords() {
        vector<string> block_list = get_block_list();
        for (string word : block_list) {
            if (hasBlockedWord(word)) {
                return true;
            }
        }
        return false;
    }
};


// Response object class
class Response : public HeaderParsable {
public:
    int statusCode;
    string message;
    string httpVersion;

    void parseResponse(string& str) {
        vector<string> lines = StringExtension::split(str, '\n');
        vector<string> pair = StringExtension::split(lines[0], ' ');
        if (pair.size() < 3) return;

        httpVersion = StringExtension::trim(pair[0]);
        statusCode = stoi(StringExtension::trim(pair[1]));
        message = "";
        for (long unsigned int t = 2; t < pair.size(); t++) {
            message += StringExtension::trim(pair[t]) + " ";
        }
        HeaderParsable::parseHeaders(str);
    }

    string generateResponse() {
        string response_string = "";
        response_string = httpVersion + " " + to_string(statusCode) + " " + message + "\n";
        response_string += HeaderParsable::generateHeaders() + "\n";
        return response_string;
    }

    bool isContentText() {
        return headers["Content-Type"].find("text/html") != string::npos;
    }

    bool isContentImage() {
        return headers["Content-Type"].find("image") != string::npos;
    }

    bool hasBlockedWord(string word) {
        if (!isContentText() || isContentImage()) return false;
        return false;
    }
};

// Close all the sockets by their id and exit the process
void close_sockets(int exit_code = 0, int client_socket = 0, int server_socket = 0) {
    if (client_socket != 0) close(client_socket);
    if (server_socket != 0) close(server_socket);
    exit(exit_code);
}


// Service function handling all regular requests
void service() {
    // Proxy <-> Client socket connection
    int client_socket = -1;
    int client_port = 9941;
    while (client_socket == -1) {
        client_port++;
        client_socket = client_socket_initialize(client_port);
    }

    cout << "CONNECT at PORT: " << style::bold << fg::green << client_port << style::reset << fg::reset << endl;



    // Accepting new socket connections by clients
    int c = sizeof(struct sockaddr_in);
    int request = 0;
    struct sockaddr_in browser;
    while (true) {
        int child_client_socket = accept(client_socket, (struct sockaddr*)&browser, (socklen_t*)&c);
        if (child_client_socket == -1) {
            cout << fg::red << "Proxy-client: accept() call failed." << fg::reset << endl;
            close_sockets(1, child_client_socket);
        }

        request++;
        int request_no = request;

        // Fork new child process to handle the new connection
        int pid = fork();
        if (pid < 0) { // Child process creation failed
            cout << fg::red << "Proxy-client: fork() call failed." << fg::reset << endl;
            close_sockets(1, child_client_socket);
        } else if (pid == 0) { // Child process
            unsigned char message_in[MAX_CONTENT_SIZE];
            unsigned char message_out[MAX_CONTENT_SIZE];
            bzero(message_in, MAX_CONTENT_SIZE);
            bzero(message_out, MAX_CONTENT_SIZE);

            cout << style::bold << fg::gray << "======================================" << fg::reset << style::reset << endl;

            // Receive request from client
            int client_recv_bytes = recv(child_client_socket, message_in, MAX_CONTENT_SIZE, 0);
            if (client_recv_bytes == -1) {
                cout << fg::red << "Proxy-client: recv() call failed." << fg::reset << endl;
                close_sockets(1, child_client_socket);
            }
            cout << fg::cyan << style::bold << "REQUEST" << fg::reset << style::reset << " #" << request_no << " (" << client_recv_bytes << " bytes) " << endl;
            cout << message_in << endl << endl;

            // Parse request
            string message_in_string(reinterpret_cast<char const*>(message_in));
            Request request;
            request.parseRequest(message_in_string);
            bool request_blocked = request.hasBlockedWords();

            // Blocked request
            if (request_blocked) {
                char error_out[MAX_CONTENT_SIZE];
                bzero(error_out, MAX_CONTENT_SIZE);
                strcpy(error_out, ERROR_RESPONSE);

                // Response with block message pages
                int client_send = send(child_client_socket, error_out, MAX_CONTENT_SIZE, 0);
                if (client_send == -1) {
                    cout << "Proxy-client: send() call failed." << endl;
                    close_sockets(1, child_client_socket);
                }

                cout << fg::red << style::bold << "RESPONSE (blocked)" << fg::reset << style::reset << " #" << request_no << endl;
                cout << error_out << endl;
                cout << style::bold << fg::gray << "======================================" << fg::reset << style::reset << endl;

                close_sockets(1, child_client_socket);
            }


            // Proxy <-> Server socket connection
            int server_socket = server_socket_initialize(request.host);
            if (server_socket == -1) close_sockets(1, child_client_socket, server_socket);


            // Sends to the original server
            int server_send = send(server_socket, message_in, client_recv_bytes, 0);
            if (server_send == -1) {
                cout << fg::red << "Proxy-server: send() call failed." << fg::reset << endl;
                close_sockets(1, child_client_socket, server_socket);
            }

            // Receive response from server
            cout << fg::green << style::bold << "RESPONSE" << fg::reset << style::reset << " #" << request_no << endl;
            int server_recv_bytes = 0;
            int response_parts = 0;
            Response response;
            while (true) { // For large response, such as images
                // Receive parts by parts
                server_recv_bytes = recv(server_socket, message_out, MAX_CONTENT_SIZE, 0);
                if (server_recv_bytes == -1) {
                    cout << fg::red << "Proxy-server: recv() call failed." << fg::reset << endl;
                    break;
                } else if (server_recv_bytes == 0) { // Connection closed (nothing more to receive)
                    break;
                }

                cout << message_out << endl;
                if (response_parts < 1) {
                    string message_out_string(reinterpret_cast<char const*>(message_out));
                    response.parseResponse(message_out_string);
                }
                response_parts++;

                // Send back to client browser
                int client_send = send(child_client_socket, message_out, server_recv_bytes, 0);
                if (client_send < 0) {
                    cout << fg::red << "Proxy-client: send() call failed." << fg::reset << endl;
                    break;
                }

                bzero(message_out, MAX_CONTENT_SIZE); // Clear message_out content
            }

            cout << style::bold << fg::gray << "======================================" << fg::reset << style::reset << endl;

            bzero(message_in, MAX_CONTENT_SIZE);
            bzero(message_out, MAX_CONTENT_SIZE);
            close_sockets(0, child_client_socket, server_socket);
        } else { // Parent process
            printf("proxy-client: Created child client process %d\n", pid);
        }
    }
}

// Block function handling all blocking operations
void block() {
    // Proxy <-> Admin socket connection
    int block_socket = -1;
    int block_port = 9951;
    while (block_socket == -1) {
        block_port++;
        block_socket = client_socket_initialize(block_port);
    }
    cout << "BLOCK at PORT: " << style::bold << fg::green << block_port << style::reset << fg::reset << endl;


    // Accepting new socket connections by admins
    int c = sizeof(struct sockaddr_in);
    struct sockaddr_in browser;
    while (true) {
        int child_block_socket = accept(block_socket, (struct sockaddr*)&browser, (socklen_t*)&c);
        if (child_block_socket == -1) {
            cout << fg::red << "Proxy-block: accept() call failed." << fg::reset << endl;
            exit(1);
        }


        // Fork new child process to handle the new connection
        int pid = fork();
        if (pid < 0) { // Child process creation failed
            cout << fg::red << "Proxy-block: fork() call failed." << fg::reset << endl;
            close_sockets(1, child_block_socket);
        } else if (pid == 0) { // Child process
            char message_in[MAX_CONTENT_SIZE];

            // Keep receiving commands
            while (true) {
                bzero(message_in, MAX_CONTENT_SIZE);
                int client_recv_bytes = recv(child_block_socket, message_in, MAX_CONTENT_SIZE, 0);
                if (client_recv_bytes == -1) {
                    cout << fg::red << "Proxy-client: recv() call failed." << fg::reset << endl;
                }

                // Process command string
                string command(reinterpret_cast<char const*>(message_in));
                vector<string> pair = StringExtension::split(command, ' ');
                if (pair.size() < 2) continue;

                // Parse word
                string word(StringExtension::trim(pair[1]));
                word.erase(remove(word.begin(), word.end(), '\n'), word.end()); // Remove trailing newline
                word.erase(remove(word.begin(), word.end(), '\r'), word.end()); // Remove trailing newline

                // Retreive lastest block list
                vector<string> block_list = get_block_list();

                // Add or remove block word
                if (pair[0] == "BLOCK") {
                    block_list.push_back(word);
                    cout << "Blocked: " << style::bold << fg::red << word << fg::reset << style::reset << endl;
                } else if (pair[0] == "UNBLOCK") {
                    block_list.erase(remove(block_list.begin(), block_list.end(), word), block_list.end());
                    cout << "Unlocked: " << style::bold << fg::green << word << fg::reset << style::reset << endl;
                }

                // Write to shared memory and verify
                set_block_list(block_list);
                block_list = get_block_list();

                // Prints block list
                if (block_list.empty()) cout << "  (block list empty)" << endl;
                for (string word : block_list) {
                    cout << "  -" << word << endl;
                }
            }

            close_sockets(0, child_block_socket);
        }
    }

}

// Main
int main() {
    // Create shared memory
    shared_memory_object::remove("block_list"); // Remove existing one
    shared_memory_object shm(create_only, "block_list", read_write);
    shm.truncate(1000); // Maximum 1000 characters

    // Fork for two ports
    int pid = fork();
    if (pid == 0) {
        block();
    } else {
        service();
    }
    return 0;
}