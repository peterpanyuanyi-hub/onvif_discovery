#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Link with ws2_32.lib
#pragma comment(lib, "ws2_32.lib")

#define MULTICAST_IP "239.255.255.250"
#define MULTICAST_PORT 3702
#define RCV_TIMEOUT_SEC 5
#define MAX_BUF_SIZE 4096

// Generate a random UUID-like string
void generate_uuid(char *buffer, size_t size) {
    srand((unsigned int)time(NULL));
    snprintf(buffer, size, "urn:uuid:%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
             rand() & 0xFFFF, rand() & 0xFFFF,
             rand() & 0xFFFF,
             ((rand() & 0x0FFF) | 0x4000),
             ((rand() & 0x3FFF) | 0x8000),
             rand() & 0xFFFF, rand() & 0xFFFF, rand() & 0xFFFF);
}

// Simple XML tag extractor (not a full XML parser)
// Finds content between <tag> and </tag> or <tag ...> and </tag>
// Returns 1 if found, 0 otherwise. result buffer is filled with content.
int extract_xml_tag(const char *xml, const char *tag_name, char *result, size_t result_size) {
    const char *start_pos = NULL;
    char prefix[64] = {0};
    int has_prefix = 0;

    // 1. Try <TagName
    char search_tag[128];
    snprintf(search_tag, sizeof(search_tag), "<%s", tag_name);
    start_pos = strstr(xml, search_tag);
    
    if (start_pos) {
        // Check if it's followed by space, >, or /
        char next_char = start_pos[strlen(search_tag)];
        if (next_char != ' ' && next_char != '>' && next_char != '/') {
            start_pos = NULL; // False match like <TagNameSuffix
        }
    }

    if (!start_pos) {
        // 2. Try :TagName
        snprintf(search_tag, sizeof(search_tag), ":%s", tag_name);
        const char *p = strstr(xml, search_tag);
        while (p) {
            // Check backward for <
            const char *check = p;
            while (check > xml && *check != '<' && *check != ' ') check--;
            if (*check == '<') {
                // Found <Prefix:TagName
                start_pos = check;
                size_t prefix_len = p - check - 1;
                if (prefix_len < sizeof(prefix)) {
                    strncpy(prefix, check + 1, prefix_len);
                    prefix[prefix_len] = '\0';
                    has_prefix = 1;
                }
                break;
            }
            // Continue searching if this wasn't it
            p = strstr(p + 1, search_tag);
        }
    }

    if (!start_pos) return 0;

    // Check for self-closing or content start
    const char *tag_end = strchr(start_pos, '>');
    if (!tag_end) return 0;

    // Check if self-closing: ends with />
    if (tag_end > start_pos && *(tag_end - 1) == '/') {
        // Self-closing, empty content
        if (result_size > 0) result[0] = '\0';
        return 1;
    }

    const char *content_start = tag_end + 1;

    // Construct end tag
    char end_tag_str[128];
    if (has_prefix) {
        snprintf(end_tag_str, sizeof(end_tag_str), "</%s:%s>", prefix, tag_name);
    } else {
        snprintf(end_tag_str, sizeof(end_tag_str), "</%s>", tag_name);
    }

    const char *end_pos = strstr(content_start, end_tag_str);
    if (!end_pos) return 0;

    size_t len = end_pos - content_start;
    if (len >= result_size) len = result_size - 1;
    
    strncpy(result, content_start, len);
    result[len] = '\0';
    return 1;
}

// Helper to decode HTML entities (basic + numeric)
void decode_html_entities(char *str) {
    char *p = str;
    char *w = str;
    while (*p) {
        if (strncmp(p, "&lt;", 4) == 0) { *w++ = '<'; p += 4; }
        else if (strncmp(p, "&gt;", 4) == 0) { *w++ = '>'; p += 4; }
        else if (strncmp(p, "&amp;", 5) == 0) { *w++ = '&'; p += 5; }
        else if (strncmp(p, "&quot;", 6) == 0) { *w++ = '"'; p += 6; }
        else if (strncmp(p, "&apos;", 6) == 0) { *w++ = '\''; p += 6; }
        else if (strncmp(p, "&#", 2) == 0) {
            // Handle numeric entities &#...;
            char *end_ent = strchr(p, ';');
            if (end_ent) {
                int code = 0;
                if (p[2] == 'x' || p[2] == 'X') {
                    sscanf(p + 3, "%x", &code);
                } else {
                    sscanf(p + 2, "%d", &code);
                }
                if (code > 0 && code < 256) {
                    *w++ = (char)code;
                    p = end_ent + 1;
                } else {
                    *w++ = *p++; // fallback
                }
            } else {
                *w++ = *p++;
            }
        }
        else { *w++ = *p++; }
    }
    *w = '\0';
}

int main() {
    WSADATA wsaData;
    SOCKET sock;
    struct sockaddr_in multicast_addr, local_addr;
    char buffer[MAX_BUF_SIZE];
    char uuid[64];
    int reuse = 1;
    DWORD timeout = 1000; // 1 second for recv timeout
    struct ip_mreq mreq;

    // 1. Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed with error: %d\n", WSAGetLastError());
        return 1;
    }

    // 2. Create UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("socket failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // 3. Set reuse address
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
        printf("setsockopt(SO_REUSEADDR) failed: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 4. Bind to local port
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(0); // Let OS pick a port

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 5. Set Receive Timeout
    // On Windows, SO_RCVTIMEO takes DWORD in milliseconds
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) < 0) {
        printf("setsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
    }

    // 6. Join Multicast Group (Optional for sending, but needed if we want to listen to multicast traffic, 
    //    though for discovery we usually just send TO multicast and receive unicast/multicast responses)
    //    We'll skip joining for simple discovery unless we expect multicast responses.
    
    // 7. Prepare Multicast Address
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    multicast_addr.sin_port = htons(MULTICAST_PORT);

    // 8. Build Probe Message
    generate_uuid(uuid, sizeof(uuid));
    const char *probe_template = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>%s</w:MessageID>"
        "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header>"
        "<e:Body>"
        "<d:Probe>"
        "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
        "</d:Probe>"
        "</e:Body>"
        "</e:Envelope>";

    snprintf(buffer, sizeof(buffer), probe_template, uuid);

    // 9. Send Probe
    printf("Sending ONVIF Probe to %s:%d...\n", MULTICAST_IP, MULTICAST_PORT);
    if (sendto(sock, buffer, (int)strlen(buffer), 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr)) == SOCKET_ERROR) {
        printf("sendto failed with error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 10. Receive Loop
    printf("Listening for responses (Timeout: %d seconds)...\n", RCV_TIMEOUT_SEC);
    time_t start_time = time(NULL);
    
    while (time(NULL) - start_time < RCV_TIMEOUT_SEC) {
        struct sockaddr_in sender_addr;
        int sender_len = sizeof(sender_addr);
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        struct timeval select_tv;
        select_tv.tv_sec = 1;
        select_tv.tv_usec = 0;
        
        int ret = select(0, &fds, NULL, NULL, &select_tv); // First arg ignored in Windows
        
        if (ret > 0) {
            int n = recvfrom(sock, buffer, MAX_BUF_SIZE - 1, 0, (struct sockaddr *)&sender_addr, &sender_len);
            if (n > 0) {
                buffer[n] = '\0';
                
                // Basic check if it's a ProbeMatch
                if (strstr(buffer, "ProbeMatch") || strstr(buffer, "d:ProbeMatch") || strstr(buffer, ":ProbeMatch")) {
                    char xaddrs[1024] = {0};
                    char scopes[2048] = {0};
                    char sender_ip[INET_ADDRSTRLEN];
                    
                    // Windows inet_ntop requires Vista or later, else use inet_ntoa
                    inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, INET_ADDRSTRLEN);
                    
                    printf("\n[Device Found] IP: %s\n", sender_ip);
                    
                    if (extract_xml_tag(buffer, "XAddrs", xaddrs, sizeof(xaddrs))) {
                        printf("  Service URL: %s\n", xaddrs);
                    }
                    
                    if (extract_xml_tag(buffer, "Scopes", scopes, sizeof(scopes))) {
                        decode_html_entities(scopes);
                        // Try to find Name in Scopes
                        char *token;
                        char *next_token = NULL;
                        
                        // strtok_s is safer on Windows
                        token = strtok_s(scopes, " \t\r\n", &next_token);
                        while (token) {
                            if (strstr(token, "onvif://www.onvif.org/name/")) {
                                printf("  Name: %s\n", token + strlen("onvif://www.onvif.org/name/"));
                            } else if (strstr(token, "onvif://www.onvif.org/location/")) {
                                printf("  Location: %s\n", token + strlen("onvif://www.onvif.org/location/"));
                            } else if (strstr(token, "onvif://www.onvif.org/hardware/")) {
                                printf("  Hardware: %s\n", token + strlen("onvif://www.onvif.org/hardware/"));
                            }
                            token = strtok_s(NULL, " \t\r\n", &next_token);
                        }
                    }
                }
            }
        }
    }

    printf("\nDiscovery finished.\n");
    closesocket(sock);
    WSACleanup();
    return 0;
}
