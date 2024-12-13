#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include<arpa/inet.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {

    struct hostent *h;
    /* The struct hostent (host entry) with its terms documented

    struct hostent {
        char *h_name;    // Official name of the host.
        char **h_aliases;    // A NULL-terminated array of alternate names for the host.
        int h_addrtype;    // The type of address being returned; usually AF_INET.
        int h_length;    // The length of the address in bytes.
        char **h_addr_list;    // A zero-terminated array of network addresses for the host.
        // Host addresses are in Network Byte Order.
    };*/

    char buffer[512], user[50], password[50], host[100], url_path[512];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s should be as follows: ftp://[<user>:<password>@]<host>/<url-path>\n", argv[0]);
        exit(-1);
    }

    sscanf(argv[1], "ftp://%511s", buffer);

    // parsing the URL to retrieve the information for the host, user and password
    {   // In this case, we have a user and password
        size_t distance = 0, length_buffer = strlen(buffer);
        distance = strcspn(buffer, ":");

        if(distance < length_buffer){
            size_t distance2 = strcspn(buffer, "@");

            if(distance2 == length_buffer){
                fprintf(stderr, "Usage: %s ftp://[<user>:<password>@]<host>/<url-path>\n", argv[0]);
                exit(-1);
            }

            strncpy(user, buffer, distance);
            user[distance] = '\0';
            strncpy(password, buffer + distance + 1, distance2 - distance - 1);
            password[distance2-distance-1] = '\0';
            distance = strcspn(buffer + distance2 + 1, "/");
            strncpy(host, buffer + distance2 + 1, distance);
            host[distance] = '\0';
            strcpy(url_path, buffer + distance2 + distance + 1);
        } else
        {   // In this case, we have not
            distance = strcspn(buffer, "/");
            strncpy(host, buffer, distance);
            host[distance] = '\0';
            strcpy(url_path, buffer + distance + 1);
            sprintf(user,"anonymous");
            sprintf(password,"password");
        }
    }

    printf("user:%s password:%s host:%s path:%s|\n", user, password, host, url_path);
    //ftp://rcom:rcom@netlab1.fe.up.pt

    // If we don´t have a host name then we get an error
    if ((h = gethostbyname(host)) == NULL) {
        herror("Error on gethostbyname()");
        printf("%s\n",host);
        exit(-1);
    }

    //Getting the ip address
    // Address, for backward compatibility.
    printf("Host name  : %s\n", h->h_name);
    printf("IP Address : %s\n", inet_ntoa(*((struct in_addr *) h->h_addr_list[0])));

    int sockfd_control;
    struct sockaddr_in server_address;
    char buffer2[520];
    size_t bytes;

    // Server address handling
    bzero((char *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = inet_addr(inet_ntoa(*((struct in_addr *) h->h_addr_list[0])));    //32 bit Internet address network byte ordered
    server_address.sin_port = htons(21);        // Server TCP port must be network byte ordered 

    // Opening a TCP socket
    if ((sockfd_control = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error on socket()");
        exit(-1);
    }
    // Connecting to the server (establishing control channel)
    if (connect(sockfd_control,
                (struct sockaddr *) &server_address,
                sizeof(server_address)) < 0) {
                    printf("%s\n",h->h_addr_list[0]);
        perror("Error on connect()");
        exit(-1);
    }

    // Sending string to the server
    usleep(100000);
    int bytes_read = read(sockfd_control, buffer, 512);
    buffer[bytes_read] = '\0';
    printf("%s\n", buffer);
    if(0 != strncmp(buffer,"220",3)){
        printf("Error: Unexpected reply from connection.\n");
        exit(-1);
    }
    
    // Sending the user 
    sprintf(buffer2, "user %s\r\n", user);
    bytes = write(sockfd_control, buffer2, strlen(buffer2));

    if (bytes > 0)
        printf("Bytes written %ld\n", bytes);
    else {
        perror("Error on write()");
        exit(-1);
    }
    
    // Reading the response to specify the password
    bytes_read = read(sockfd_control, buffer, 512);
    buffer[bytes_read] = '\0';
    printf("%s\n", buffer);
    if(0 != strncmp(buffer,"331 Please specify the password.",32)){
        printf("Error: Unexpected reply from connection.\n");
        exit(-1);
    }
    
    // Sending any password and reading the response of successful login
    sprintf(buffer2, "pass %s\r\n", password);
    write(sockfd_control, buffer2, strlen(buffer2));
    bytes_read = read(sockfd_control, buffer, 512);
    buffer[bytes_read] = '\0';
    printf("%s", buffer);
    if(0 != strncmp(buffer,"230",3)){
        printf("Error: Login failed.\n");
        exit(-1);
    }
    
    // Sending pasv to enter passive mode and reading the response of entering said mode
    sprintf(buffer2, "pasv\r\n");
    write(sockfd_control, buffer2, strlen(buffer2));
    bytes_read=read(sockfd_control, buffer, 512);
    buffer[bytes_read] = '\0';
    printf("%s", buffer);
    if(0 != strncmp(buffer,"227",3)){
        printf("Error: Unexpected reply from connection.\n");
        exit(-1);
    }

    // Going into passive mode (client connecting to the server)
    int h1=0, h2=0, h3=0, h4=0, p1=0, p2=0, pasvport;
    sscanf(buffer, "227 Entering Passive Mode (%i,%i,%i,%i,%i,%i).", &h1, &h2, &h3, &h4, &p1, &p2);
    pasvport = p1*256+p2;

    int sockfd_data;
    struct sockaddr_in server_address2;

    // Server address handling
    sprintf(buffer, "%i.%i.%i.%i", h1, h2, h3, h4);
    bzero((char *) &server_address2, sizeof(server_address2));
    server_address2.sin_family = AF_INET;
    server_address2.sin_addr.s_addr = inet_addr(buffer);  /*32 bit Internet address network byte ordered*/
    server_address2.sin_port = htons(pasvport);        /*server TCP port must be network byte ordered */

    if ((sockfd_data = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error on socket()");
        exit(-1);
    }

    // Connecting to the server (establishing data channel)
    if (connect(sockfd_data,
                (struct sockaddr *) &server_address2,
                sizeof(server_address2)) < 0) {
        perror("Error on connect()");
        exit(-1);

    }

    // Sending a retr so we can download a copy of a file on the server
    sprintf(buffer2, "retr %s\r\n", url_path);
    bytes = write(sockfd_control, buffer2, strlen(buffer2));

    if (bytes > 0)
        printf("Bytes written %ld\n", bytes);
    else {
        perror("Error on write()");
        exit(-1);
    }

    // Receiving the file
    bytes_read = read(sockfd_control, buffer, 512);
    buffer[bytes_read] = '\0';
    if(0 != strncmp(buffer,"150",3)){
        printf("%sClosing\n",buffer);    
        if (close(sockfd_control)<0) {
            perror("close()");
            exit(-1);
        }
        if (close(sockfd_data)<0) {
            perror("close()");
            exit(-1);
        }
        return -1;
    }
    printf("%s\nReceiving File....\n", buffer);
    FILE *f = fopen("file","w");

    // File not created, exiting with error
    if(f == NULL){
        printf("Unable to create file locally.\n");
        exit(EXIT_FAILURE);
    }
    // Creating the file
    while((bytes_read = read(sockfd_data, buffer, 512)) > 0){
        fwrite(buffer, 1, bytes_read, f);
    }
    printf("The file was received!\n");

    // Closing the socketsS
    if (close(sockfd_control) < 0) {
        perror("Error on close()");
        exit(-1);
    }
    if (close(sockfd_data) < 0) {
        perror("Error on close()");
        exit(-1);
    }
    return 0;

}
