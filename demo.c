#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include "cli.h"

#define SOCKET_PATH "/tmp/command_socket"
#define MAX_EVENTS 10
#define BUFFER_SIZE 1024
#define PROMPT "> "

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int unix_socket_init(char* unix_sock_path) {
    struct sockaddr_un server_addr;
    int server_fd = -1;

    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return server_fd;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, unix_sock_path, sizeof(server_addr.sun_path) - 1);

    unlink(unix_sock_path);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return -1;
    }

    // 设置服务器套接字为非阻塞
    set_nonblocking(server_fd);

    // 开始监听
    if (listen(server_fd, SOMAXCONN) < 0) {
        close(server_fd);
        return -2;
    }

    return server_fd;
}

int main() {
    int epoll_fd, server_fd, nfds;
    struct epoll_event ev, events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];

    cli_init();

    // 创建 epoll 实例
    if ((epoll_fd = epoll_create1(0)) == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    
    server_fd = unix_socket_init(SOCKET_PATH);
    if (server_fd < 0) {
        perror("unix_socket_init");
        exit(EXIT_FAILURE);
    }
    // 监听标准输入 (文件描述符 0)
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        exit(EXIT_FAILURE);
    }

    // 事件循环
    while (1) {
        nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100); // 100ms timeout
        
        if (nfds < 0) {
            if (errno == EINTR) continue; // 被信号中断
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // 处理新连接
            if (events[i].data.fd == server_fd) {
                struct sockaddr_un client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd < 0) {
                    perror("accept failed");
                    continue;
                }
                
                printf("New client connected (fd=%d)\n", client_fd);
                
                // 设置客户端套接字为非阻塞
                if (set_nonblocking(client_fd)) {
                    perror("client nonblock failed");
                    close(client_fd);
                    continue;
                }
                
                // 添加客户端到epoll
                ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl: client_fd");
                    close(client_fd);
                }
            } 
            // 处理客户端数据
            else {
                int client_fd = events[i].data.fd;
                ssize_t bytes_read;
                
                // 读取客户端数据
                while ((bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) {
                    cli_input(client_fd, buffer);

                    memset(buffer, 0, sizeof(buffer));
                }

                if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    printf("Client (fd=%d) disconnected\n", client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                }
            }
        }
    }

    close(epoll_fd);
    return 0;
}

static int
test_show_instance_command_fn(cli_ctx_t* input)
{
    int id = -1;

    while (input->index < input->len)
    {
      if (unformat (input, "id %d", &id))
	    ;
      else
        {
            break;
        }
    }

    if (id > 0)
        cli_output(input, NEW_LINE, "id %d is shown", id);
    return 0;
}

CLI_COMMAND (test_show_instance_command) = {
    .path = "show instance",
    .help = "Usage: show instance [id INDEX]",
    .function = test_show_instance_command_fn,
};

static int
test_show_link_command_fn(cli_ctx_t* input)
{
  return 0;
}

CLI_COMMAND (test_show_link_command) = {
    .path = "show link",
    .help = "Usage: show link <link-id>",
    .function = test_show_link_command_fn,
};

static int
test_reload_config_command_fn(cli_ctx_t* input)
{
  return 0;
}

CLI_COMMAND (test_reload_config_command) = {
    .path = "reload config",
    .help = "Usage: reload config",
    .function = test_reload_config_command_fn,
};

