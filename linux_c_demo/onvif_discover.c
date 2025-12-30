#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>

#define MULTICAST_IP "239.255.255.250"
#define MULTICAST_PORT 3702
#define RCV_TIMEOUT_SEC 5
#define MAX_BUF_SIZE 4096

// Generate a random UUID-like string
void generate_uuid(char *buffer, size_t size) {
    srand(time(NULL));
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
    char start_tag[128];
    char end_tag[128];
    
    // Construct simple start tag <TagName>
    snprintf(start_tag, sizeof(start_tag), "<%s", tag_name); // Just match <TagName...
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag_name);

    // Find start
    const char *start_pos = strstr(xml, start_tag);
    if (!start_pos) {
        // Try searching with namespace prefix (naive approach, just search for :TagName)
        char ns_tag[128];
        snprintf(ns_tag, sizeof(ns_tag), ":%s", tag_name);
        start_pos = strstr(xml, ns_tag);
        if (start_pos) {
             // Verify it is an opening tag
             const char *check = start_pos;
             while (check > xml && *check != '<' && *check != ' ') check--;
             if (*check == '<') start_pos = check;
             else start_pos = NULL;
        }
    }

    if (!start_pos) return 0;

    // Find the closing bracket of the start tag
    const char *content_start = strchr(start_pos, '>');
    if (!content_start) return 0;
    content_start++; // Skip '>'

    // Find end tag
    // We need to be careful about matching the correct end tag if possible, 
    // but for simple extraction we just look for </...:TagName> or </TagName>
    const char *end_pos = strstr(content_start, end_tag);
    if (!end_pos) {
        // Try with namespace
        char ns_end_tag[128];
        snprintf(ns_end_tag, sizeof(ns_end_tag), ":%s>", tag_name);
        end_pos = strstr(content_start, ns_end_tag);
        // We need to make sure it is </Prefix:TagName>
        if (end_pos) {
            const char *check = end_pos;
            while (check > content_start && *check != '/') check--;
            if (check > content_start && *(check-1) == '<') end_pos = check - 1;
            else end_pos = NULL;
        }
    }

    if (!end_pos) return 0;

    size_t len = end_pos - content_start;
    if (len >= result_size) len = result_size - 1;
    
    strncpy(result, content_start, len);
    result[len] = '\0';
    return 1;
}

// Helper to decode HTML entities (basic)
void decode_html_entities(char *str) {
    char *p = str;
    char *w = str;
    while (*p) {
        if (strncmp(p, "&lt;", 4) == 0) { *w++ = '<'; p += 4; }
        else if (strncmp(p, "&gt;", 4) == 0) { *w++ = '>'; p += 4; }
        else if (strncmp(p, "&amp;", 5) == 0) { *w++ = '&'; p += 5; }
        else if (strncmp(p, "&quot;", 6) == 0) { *w++ = '"'; p += 6; }
        else if (strncmp(p, "&apos;", 6) == 0) { *w++ = '\''; p += 6; }
        else { *w++ = *p++; }
    }
    *w = '\0';
}

int main() {
    int sock;
    struct sockaddr_in multicast_addr, local_addr;
    char buffer[MAX_BUF_SIZE];
    char uuid[64];
    
    // 1. Create UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // 2. Set reuse address
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(sock);
        return 1;
    }

    // 3. Bind to local port (optional, but good for receiving responses)
    // We bind to ANY and let OS pick a port, or we can bind to 0
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(0); // Random port

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    // 4. Set Receive Timeout
    struct timeval tv;
    tv.tv_sec = 1; // 1 second timeout for select loop
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    // 5. Prepare Multicast Address
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    multicast_addr.sin_port = htons(MULTICAST_PORT);

    // 6. Build Probe Message
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

    // 7. Send Probe
    printf("Sending ONVIF Probe to %s:%d...\n", MULTICAST_IP, MULTICAST_PORT);
    if (sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr)) < 0) {
        perror("sendto");
        close(sock);
        return 1;
    }

    // 8. Receive Loop
    printf("Listening for responses (Timeout: %d seconds)...\n", RCV_TIMEOUT_SEC);
    time_t start_time = time(NULL);
    
    while (time(NULL) - start_time < RCV_TIMEOUT_SEC) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        struct timeval select_tv;
        select_tv.tv_sec = 1;
        select_tv.tv_usec = 0;
        
        int ret = select(sock + 1, &fds, NULL, NULL, &select_tv);
        
        if (ret > 0) {
            int n = recvfrom(sock, buffer, MAX_BUF_SIZE - 1, 0, (struct sockaddr *)&sender_addr, &sender_len);
            if (n > 0) {
                buffer[n] = '\0';
                
                // Basic check if it's a ProbeMatch
                if (strstr(buffer, "ProbeMatch") || strstr(buffer, "d:ProbeMatch") || strstr(buffer, ":ProbeMatch")) {
                    char xaddrs[1024] = {0};
                    char scopes[2048] = {0};
                    char sender_ip[INET_ADDRSTRLEN];
                    
                    inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, INET_ADDRSTRLEN);
                    
                    printf("\n[Device Found] IP: %s\n", sender_ip);
                    
                    if (extract_xml_tag(buffer, "XAddrs", xaddrs, sizeof(xaddrs))) {
                        printf("  Service URL: %s\n", xaddrs);
                    }
                    
                    if (extract_xml_tag(buffer, "Scopes", scopes, sizeof(scopes))) {
                        decode_html_entities(scopes);
                        // Try to find Name in Scopes
                        char *token;
                        char *saveptr;
                        
                        // Note: strtok modifies the string, so we work on a copy if needed, 
                        // but here we just want to print info
                        token = strtok_r(scopes, " \t\r\n", &saveptr);
                        while (token) {
                            if (strstr(token, "onvif://www.onvif.org/name/")) {
                                printf("  Name: %s\n", token + strlen("onvif://www.onvif.org/name/"));
                            } else if (strstr(token, "onvif://www.onvif.org/location/")) {
                                printf("  Location: %s\n", token + strlen("onvif://www.onvif.org/location/"));
                            } else if (strstr(token, "onvif://www.onvif.org/hardware/")) {
                                printf("  Hardware: %s\n", token + strlen("onvif://www.onvif.org/hardware/"));
                            }
                            token = strtok_r(NULL, " \t\r\n", &saveptr);
                        }
                    }
                }
            }
        }
    }

    printf("\nDiscovery finished.\n");
    close(sock);
    return 0;
}
