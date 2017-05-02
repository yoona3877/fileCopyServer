#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "hash.h"
#include "ftree.h"

#ifndef PORT
#define PORT 30000
#endif

void rcopy_server(unsigned short port);
int rcopy_client(char *source, char *host, unsigned short port);

static int path_index = 0;
static int first_run = 1;

int writeFile(int connfd, char* path);
int sendFile(char* source, int sock_fd);
int checkFile(struct request fromClient);

int rcopy_client(char *source, char *host, unsigned short port){
    struct request fileRequest;
    char *src = malloc(sizeof(char) * MAXPATH);
    strncpy(src, source,MAXPATH);

    int len_basename, len_src;
    len_basename = strlen(basename(strdup(src)));
    len_src = strlen(src);

    if (path_index == 0){
        if (len_src!= len_basename){
            if (first_run == 1){
                path_index = len_src - len_basename;
            }
        }
    }

    if(first_run == 1){
        first_run =0;
    }

    struct stat file_info;

    int sockfd;
    struct sockaddr_in serv_addr;
    if((sockfd = socket(AF_INET, SOCK_STREAM, 0))< 0){
        perror("socket \n");
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port); 
    serv_addr.sin_addr.s_addr = inet_addr(host);

    if(connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0){
        perror("connect\n");
        close(sockfd);
        return 1;
    }

    FILE *fp ;
    fp = fopen(src,"rb");
    
    if(fp == NULL){
        perror("fopen\n");
        return 1;
    }
    if (lstat(src, &file_info) == -1){
        fprintf(stderr,"lstat: No such file or directory\n");
        return 1;
    }
    
    if(path_index == 0){
        strcpy(fileRequest.path,(src));
    }else{
        char serverPath[MAXPATH];
        strncpy(serverPath, (src + path_index), MAXPATH);
        strcpy(fileRequest.path,(serverPath));
    }

    fileRequest.mode = file_info.st_mode ;
    fileRequest.size = (int)file_info.st_size;
    
    int size = htonl(fileRequest.size);
    int mode = htonl(fileRequest.mode);

    int serverSignal = -1;
    
    if ((S_ISREG(file_info.st_mode))){
        fileRequest.type = REGFILE;
        char *hash_copy = hash(fp);
        strncpy(fileRequest.hash, hash_copy, BLOCKSIZE);
        
        int status_file;

        for (status_file = 0; status_file <5; status_file++){
            if (status_file == AWAITING_TYPE){
                write(sockfd, &(fileRequest.type),sizeof(int));
            } else if (status_file == AWAITING_PATH){
                write(sockfd, fileRequest.path, MAXPATH);
            } else if (status_file == AWAITING_SIZE){
                write(sockfd, &size, sizeof(int));
            } else if (status_file == AWAITING_PERM){
                write(sockfd, &mode, sizeof(int));
            } else if (status_file == AWAITING_HASH){
                write(sockfd, fileRequest.hash, BLOCKSIZE);
            }
        }
        
        read(sockfd,&serverSignal,sizeof(int));

        if(serverSignal == SENDFILE){
            int new_proc;
            new_proc = fork();
            if (new_proc == 0){
                close(sockfd);
                int trans_fd;
                if((trans_fd = socket(AF_INET, SOCK_STREAM, 0))< 0){
                    printf("\n Error : Could not create socket \n");
                    return 1;
                }

                if(connect(trans_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0){
                    printf("\n Error : Connect Failed \n");
                    close(trans_fd);
                    return 1;
                }
                for (status_file = 0; status_file <5; status_file++){
                    fileRequest.type = TRANSFILE;
                    if (status_file == AWAITING_TYPE){
                        write(trans_fd, &(fileRequest.type),sizeof(int));
                    } else if (status_file == AWAITING_PATH){
                        write(trans_fd, fileRequest.path, MAXPATH);
                    } else if (status_file == AWAITING_SIZE){
                        write(trans_fd, &size, sizeof(int));
                    } else if (status_file == AWAITING_PERM){
                        write(trans_fd, &mode, sizeof(int));
                    } else if (status_file == AWAITING_HASH){
                        write(trans_fd, fileRequest.hash, BLOCKSIZE);
                    }
                }
                sendFile(src, trans_fd);
                close(trans_fd);
            }
            int status;
            pid_t pid;
            if ((pid = wait(&status)) < 0){
                perror("wait");
            } else{
                printf("Child %d terminates.\n", pid);
            }
            return 0;
        }else if(serverSignal == OK){
            close(sockfd);
            return 0;
        } else{ //error 
            close(sockfd);
            return 1;
        }
    }
    else if ((S_ISDIR(file_info.st_mode))){
        fileRequest.type = REGDIR;
        int status;
        for (status = 0; status <5; status++){
            if (status == AWAITING_TYPE){
                write(sockfd, &(fileRequest.type),sizeof(int));
            } else if (status == AWAITING_PATH){
                write(sockfd, fileRequest.path, MAXPATH);
            } else if (status == AWAITING_SIZE){
                write(sockfd, &size, sizeof(int));
            } else if (status == AWAITING_PERM){
                write(sockfd, &mode, sizeof(int));
            } else if (status == AWAITING_HASH){
                write(sockfd, fileRequest.hash, BLOCKSIZE);
            }
        }
        DIR *dirp  = opendir(src);
        struct dirent *cf;
        while ((cf = readdir(dirp)) != NULL){
            char newPath[(strlen(src))+1+(strlen(cf->d_name))];
            strcpy(newPath, src);
            strcat(newPath,"/");
            strcat(newPath, cf->d_name);
            if (cf -> d_name[0] != '.'){
                struct stat file_info;
                if (lstat(newPath, &file_info) == -1){
                    fprintf(stderr,"lstat: No such file or directory\n");
                    return 1;
                }
                rcopy_client(newPath, host, port);
            }
        } 
    }
    close(sockfd);
    return 0;
}

int setup(unsigned short port);

void rcopy_server(unsigned short port){
    int listenfd, maxfd, nready;
    fd_set allset, rset;

    listenfd = setup(port);
    printf("listening socket set\n");
    
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    maxfd = listenfd;
    
    while (1){
        rset = allset;
        
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }
        
        if (FD_ISSET(listenfd, &rset)){
            printf("New client is connecting ... \n");

            int connfd;
            struct sockaddr_in peer;
            socklen_t socklen;
            
            socklen = sizeof(peer);
            if ((connfd = accept(listenfd, (struct sockaddr *)&peer, &socklen)) < 0) {
                perror("accept");
                exit(1);
            }
            printf("New connection on port %d\n", ntohs(peer.sin_port));
            if (connfd > maxfd) {
                maxfd = connfd;
            }
            FD_SET(connfd, &allset);
        } else{
            for (int i = 0; i <= maxfd; i++){
                if (FD_ISSET(i, &rset)){
                    struct request fromClient;
                    
                    char *destPath= malloc(MAXPATH);
                    char *hash = malloc(BLOCKSIZE);
                    int size,mode, type;
                    
                    int status;
                    int read_num;

                    for (status = 0; status <5; status++){
                        if (status == AWAITING_TYPE){
                            read_num = read(i, &type, sizeof(int));
                        } else if(status == AWAITING_PATH){
                            read_num = read(i, destPath, MAXPATH);
                        } else if (status == AWAITING_SIZE){
                            read_num = read(i, &size, sizeof(int));
                        } else if (status == AWAITING_PERM){
                            read_num = read(i, &mode, sizeof(int));
                        } else if (status == AWAITING_HASH){
                            read_num = read(i, hash, BLOCKSIZE);
                        }
                        if (read_num == 0){
                                FD_CLR(i, &allset);
                                close(i);
                        }
                    }

                    fromClient.type = type;
                    strncpy(fromClient.path, destPath,MAXPATH);
                    fromClient.size = ntohl(size);
                    fromClient.mode = ntohl(mode);
                    strncpy(fromClient.hash, hash,BLOCKSIZE);

                    if (fromClient.type != TRANSFILE){ // file type is REGFILE/REGDIR
                        int check;
                        int fileCheck = checkFile(fromClient);
                        if(fileCheck == 0){
                            if (fromClient.type == REGDIR){
                                chmod(fromClient.path, fromClient.mode);
                                FD_CLR(i, &allset);
                                close(i);
                            } else if (fromClient.type == REGFILE){
                                chmod(fromClient.path, fromClient.mode);
                                check = OK;
                                write(i,&check,sizeof(OK));
                                FD_CLR(i, &allset);
                                close(i);
                            }
                        }
                        else if (fileCheck == 1){
                            if(fromClient.type == REGDIR){  
                                printf("Directory is created\n");
                                mkdir(fromClient.path, fromClient.mode&0777);
                                FD_CLR(i, &allset);
                                close(i);
                            }
                            else if(fromClient.type == REGFILE){
                                check = SENDFILE;
                                write(i,&check,sizeof(check));
                                FD_CLR(i, &allset);
                                close(i);
                            }
                        }
                    } else{ // file type is TRANSFILE
                        writeFile(i, destPath);
                        chmod(fromClient.path, fromClient.mode);
                        FD_CLR(i, &allset);
                        close(i);
                    }
                } 
            }
        }
    }
    FD_CLR(listenfd, &allset);
    close(listenfd);
}



int checkFile(struct request fromClient){
    if (fromClient.type == REGDIR){
        if( access( fromClient.path, F_OK ) != -1 ) {
            return 0; 
        }else{
            return 1; 
        }
    }else if (fromClient.type == REGFILE){
        if( access( fromClient.path, F_OK ) != -1 ) {
            printf("FILE EXIST\n");
            struct stat fromDest;
            char* dhashed = malloc(8);
            char *shashed = malloc(8);
            stat(fromClient.path,&fromDest);
            
            if(fromClient.size != fromDest.st_size){
                printf("DIFFERENT SIZE\n");
                return 1;
            }
            FILE *dfile = fopen(fromClient.path,"rb");
            
            strncpy(dhashed,hash(dfile),BLOCKSIZE);
            strncpy(shashed, fromClient.hash, BLOCKSIZE);
            if (strcmp(shashed,dhashed) != 0){
                printf("DIFFERNET HASH. COPY FILE\n");
                free(dhashed);
                free(shashed);
                return 1;
            }else{
                printf("SAME HASH.\n");
                return 0;
            }
        } else {
            printf("COPY FILE\n");
            return 1;
        }
    }
    return 0;
}

int setup(unsigned short port) {
    int on = 1, status;
    struct sockaddr_in self;
    int listenfd;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    
    status = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                        (const char *) &on, sizeof(on));
    if(status == -1) {
        perror("setsockopt");
    }
    
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = INADDR_ANY;
    self.sin_port = htons(port);
    memset(&self.sin_zero, 0, sizeof(self.sin_zero));
    
    printf("Listening on %d\n", port);
    
    if (bind(listenfd, (struct sockaddr *)&self, sizeof(self)) == -1) {
        perror("bind");
        exit(1);
    }
    
    if (listen(listenfd, 5) == -1) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

int writeFile(int connfd, char* path){
    int fp;
    int bytesReceived = 0;
    char recvBuff[256];
    memset(recvBuff, '0', sizeof(recvBuff));
    fp = open(path, O_WRONLY|O_CREAT, 0644);
    if (fp == -1) {
        perror ("open");
        return 1;
    }
    while((bytesReceived = read(connfd, recvBuff, 256)) > 0){
        write(fp,recvBuff,bytesReceived);
    }
    if(bytesReceived < 0){
        printf("\n Read Error \n");
        return 1;
    }
    return 0;
}

int sendFile(char* source, int sock_fd){
    FILE *f = fopen(source,"rb");

    while(1){
        unsigned char buffer[MAXDATA]={0};
        int num_read = fread(buffer,1,MAXDATA,f);

        if(num_read > 0){
            write(sock_fd, buffer, num_read);
        }
        if (num_read < MAXDATA)
        {
            if (feof(f)){
                return 0;
            }
            if (ferror(f)){
                perror("fread : ferror\n");
                return 1;
            }
        }
    }
    return 0;
}