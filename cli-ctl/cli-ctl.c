#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <signal.h>
#include <ctype.h>

#define SOCKET_PATH "/tmp/command_socket"
#define BUFFER_SIZE 1024
#define PROMPT "prompt > "

// 全局变量
struct termios original_term;
int sock_fd = -1;

// 信号处理函数
void handle_signal(int sig) {
    // 恢复终端原始设置
    tcsetattr(STDIN_FILENO, TCSANOW, &original_term);
    
    // 关闭套接字（如果已打开）
    if (sock_fd >= 0) {
        close(sock_fd);
    }
    
    printf("\nProgram terminated by signal %d\n", sig);
    exit(EXIT_FAILURE);
}

// 设置终端为非规范模式
void set_terminal_raw() {
    struct termios term_settings;
    tcgetattr(STDIN_FILENO, &term_settings);
    term_settings.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term_settings);
}

// 显示当前行（带光标位置）
void redraw_line(const char *input, int cursor_pos) {
    printf("\r%s", PROMPT); // 回到行首并打印提示符
    printf("%s", input);     // 打印当前输入内容
    printf("\x1b[0K");      // 清除从光标到行尾的内容
    
    // 定位光标到正确位置
    int total_pos = strlen(PROMPT) + cursor_pos;
    if (total_pos > 0) {
        printf("\x1b[%dG", total_pos + 1); // ANSI转义序列定位光标
    }
    fflush(stdout);
}

// 处理转义序列（仅支持左右方向键）
int handle_escape_sequence() {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 0;
    if (read(STDIN_FILENO, &seq[1], 1) <= 0) return 0;
    
    // 仅处理左右方向键
    if (seq[0] == '[') {
        switch (seq[1]) {
            case 'C': // 右箭头
                return 1; // 表示需要向右移动光标
                
            case 'D': // 左箭头
                return -1; // 表示需要向左移动光标
        }
    }
    return 0; // 不是方向键或未处理
}

int main() {
    struct sockaddr_un server_addr;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    // 注册信号处理函数
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 保存原始终端设置并配置为非规范模式
    tcgetattr(STDIN_FILENO, &original_term);
    set_terminal_raw();

    // 创建Unix域套接字
    if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        tcsetattr(STDIN_FILENO, TCSANOW, &original_term);
        exit(EXIT_FAILURE);
    }

    // 配置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    // 连接到服务器
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connection failed");
        close(sock_fd);
        tcsetattr(STDIN_FILENO, TCSANOW, &original_term);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server. Type commands after '%s' prompt.\n", PROMPT);
    printf("Type 'quit' to exit.\n");
    printf("Press ←/→ to move cursor.\n");
    printf("Press Ctrl+C to terminate at any time.\n");

    // 初始提示符
    printf("%s", PROMPT);
    fflush(stdout);

    // 交互循环
    while (1) {
        int pos = 0;        // 输入缓冲区位置
        int cursor_pos = 0; // 光标位置
        buffer[0] = '\0';   // 清空缓冲区
        
        // 逐字符读取输入
        while (1) {
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0) break;

            // 处理ESC序列（可能是方向键）
            if (c == '\x1b') {
                int direction = handle_escape_sequence();
                if (direction == 1 && cursor_pos < pos) { // 右箭头
                    cursor_pos++;
                    printf("\x1b[%dG", strlen(PROMPT) + cursor_pos + 1);
                    fflush(stdout);
                } 
                else if (direction == -1 && cursor_pos > 0) { // 左箭头
                    cursor_pos--;
                    printf("\x1b[%dG", strlen(PROMPT) + cursor_pos + 1);
                    fflush(stdout);
                }
                continue;
            }
            
            // 处理回车键
            if (c == '\n') {
                printf("\n");
                buffer[pos] = '\0'; // 确保字符串终止
                
                // 检查是否为退出命令
                if (strcmp(buffer, "quit") == 0) {
                    goto disconnect;
                }

                // 发送命令到服务器
                if (pos > 0) {
                    write(sock_fd, buffer, pos);
                    
                    // 读取服务器响应
                    bytes_read = read(sock_fd, buffer, BUFFER_SIZE - 1);
                    if (bytes_read <= 0) {
                        // 服务器断开连接
                        if (bytes_read == 0) {
                            printf("Server disconnected\n");
                        } else {
                            perror("read from server failed");
                        }
                        goto disconnect;
                    }
                    
                    if (bytes_read == 1) {
                        /* 只有占位符 */
                        printf("\n%s", PROMPT);
                    } else {
                        buffer[bytes_read] = '\0';
                        printf("%s\n%s", buffer, PROMPT);
                    }
                    fflush(stdout);
                } else {
                    // 没有输入命令，只打印新提示符
                    printf("%s", PROMPT);
                    fflush(stdout);
                }

                break;
            }
            // 处理退格键
            else if (c == 127 || c == '\b') { // 退格键
                if (cursor_pos > 0) {
                    // 删除光标前的字符
                    memmove(&buffer[cursor_pos - 1], &buffer[cursor_pos], pos - cursor_pos + 1);
                    pos--;
                    cursor_pos--;
                    
                    // 重绘整行
                    redraw_line(buffer, cursor_pos);
                }
            }
            // 处理普通字符
            else if (isprint(c) && pos < BUFFER_SIZE - 1) {
                // 在光标位置插入字符
                if (cursor_pos < pos) {
                    memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], pos - cursor_pos);
                }
                buffer[cursor_pos] = c;
                pos++;
                cursor_pos++;
                buffer[pos] = '\0';
                
                // 重绘整行
                redraw_line(buffer, cursor_pos);
            }
            // 其他按键（包括Tab）忽略
            else {
                // 忽略所有其他按键
            }
        }
    }

disconnect:
    printf("Disconnecting from server...\n");
    close(sock_fd);
    tcsetattr(STDIN_FILENO, TCSANOW, &original_term);
    return 0;
}