#ifndef CLI_H_
#define CLI_H_

#define NEW_LINE 1
#define CUR_LINE 0


typedef struct
{
  char *name;
  unsigned int index;
} cli_sub_command_t;

typedef struct
{
  uint32_t *bitmap;
  int max_valid_index;
} bitmap_t;

typedef struct
{
  int min_char;
  bitmap_t *bitmaps;
  int bitmaps_max_valid_index;
} cli_parse_position_t;

typedef struct _cli_cxt_t
{
    /* Input buffer */
    char *buffer;
    int len;
    /* Current index in input buffer. */
    int index;
    int fd;

    char *output_buffer;
    int output_index;
    int output_capacity;
} cli_ctx_t;

struct cli_command_t;

/* CLI command callback function. */
typedef int (*cli_command_function_t) (cli_ctx_t* user_input);

/* 定义一个命令 */
typedef struct cli_command_t
{
  char *path;
  char *help;
  /* Callback function. */
  cli_command_function_t function;

  /* Sub commands for this command. */
  cli_sub_command_t *sub_commands;
  int sub_commands_count;
  int sub_commands_capacity;

  /* Hash table mapping name (e.g. last path element) to sub command index. */
  void *sub_command_index_by_name;

  cli_parse_position_t *sub_command_positions;
  int sub_command_positions_capacity;
  struct cli_command_t *next_cli_command;
} cli_command_t;

typedef struct cli_main_t
{
    cli_command_t *commands;
    int commands_count;
    int commands_capacity;
    void *command_index_by_path;
    cli_command_t *cli_command_registrations;
} cli_main_t;

cli_main_t* get_cli_main();

#define CLI_COMMAND(x)                                                                  \
  static cli_command_t x;                                                               \
  static void _cli_command_registration_##x (void)  __attribute__ ((__constructor__));  \
  static void _cli_command_registration_##x (void)                                      \
  {                                                                                     \
    cli_main_t *cm = get_cli_main();                                     \
    x.next_cli_command = cm->cli_command_registrations;                       \
    cm->cli_command_registrations = &x;                                       \
  }                                                                           \
  static cli_command_t x

int cli_init();

int cli_input(int client_fd, char* user_input);

void cli_output(cli_ctx_t* input, int new_line, char* fmt, ...);

int unformat (cli_ctx_t* input, const char *fmt, ...);

#endif