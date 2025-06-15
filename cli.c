#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include "cli.h"

/* 主要控制结构 */
cli_main_t cm;

/* 初始命令数量 */
#define INITIAL_COMMAND_NUM 10

/* 每个注册命令有这么一个结构  */
typedef struct cmd_node {
    char* path;
    int cmd_index;         /* 在 cm. commands 中的位置 */
    struct cmd_node* next;
} cmd_node_t;

typedef struct {
    cmd_node_t** buckets;
    int size;
    int count;
} cmd_hash_table_t;

// 哈希函数（djb2算法）
unsigned long hash(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

void* hash_table_create() {
    cmd_hash_table_t* table = (cmd_hash_table_t*)malloc(sizeof(cmd_hash_table_t));
    if (!table) return 0;
    
    table->size = 32;
    table->count = 0;
    table->buckets = (cmd_node_t**)calloc(table->size, sizeof(cmd_node_t*));
    if (!table->buckets) {
        free(table);
        return 0;
    }
    return (void*)table;
}

// 动态调整哈希表大小
void hash_table_resize(cmd_hash_table_t* table, int new_size) {
    cmd_node_t** new_buckets = (cmd_node_t**)calloc(new_size, sizeof(cmd_node_t*));
    if (!new_buckets) return;

    // 迁移所有节点到新桶数组
    for (int i = 0; i < table->size; i++) {
        cmd_node_t* node = table->buckets[i];
        while (node) {
            cmd_node_t* next = node->next;
            // 在新桶中的位置
            unsigned long idx = hash(node->path) % new_size;
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }

    free(table->buckets);
    table->buckets = new_buckets;
    table->size = new_size;
}


// 插入键值对（如果键存在则更新值）
static void hash_table_set(void* t, const char* path, int cmd_index) {
    cmd_hash_table_t* table = (cmd_hash_table_t*)t;
    // 检查是否需要扩容
    if ((float)table->count / table->size >= 0.75) {
        hash_table_resize(table, table->size * 2);
    }

    unsigned long idx = hash(path) % table->size;
    cmd_node_t* node = table->buckets[idx];

    // 检查键是否已存在
    while (node) {
        if (strcmp(node->path, path) == 0) {
            node->cmd_index = cmd_index; // 更新值
            return;
        }
        node = node->next;
    }

    // 创建新节点
    cmd_node_t* new_node = (cmd_node_t*)malloc(sizeof(cmd_node_t));
    if (!new_node) return;
    
    new_node->path = strdup(path); // 复制字符串键
    if (!new_node->path) {
        free(new_node);
        return;
    }
    new_node->cmd_index = cmd_index;
    new_node->next = table->buckets[idx]; // 头插法
    table->buckets[idx] = new_node;
    table->count++;
}

// 查找键对应的值（成功返回1，失败返回0）
static int hash_table_get(void* t, const char* path, int* cmd_index) {
    cmd_hash_table_t* table = (cmd_hash_table_t*)t;
    unsigned long idx = hash(path) % table->size;
    cmd_node_t* node = table->buckets[idx];
    
    while (node) {
        if (strcmp(node->path, path) == 0) {
            *cmd_index = node->cmd_index;
            return 1; // 查找成功
        }
        node = node->next;
    }
    return 0; // 键不存在
}

// 清理哈希表内存
static void hash_table_destroy(cmd_hash_table_t* t) {
    cmd_hash_table_t* table = (cmd_hash_table_t*)t;
    for (int i = 0; i < table->size; i++) {
        cmd_node_t* node = table->buckets[i];
        while (node) {
            cmd_node_t* next = node->next;
            free(node->path); // 释放键内存
            free(node);
            node = next;
        }
    }
    free(table->buckets);
    free(table);
}

/* bitmap 相关 */
#define _(name, body)			\
 inline __attribute__((always_inline)) bitmap_t *	\
 cli_bitmap_##name (bitmap_t* ai, uint32_t i)		\
 {							\
    int i0 = i / 32;		\
    int i1 = i % 32;		\
    uint32_t a, b;						\
    if (ai->max_valid_index < i0) {                         \
        ai->bitmap = realloc(ai->bitmap, (i0 + 1) * sizeof(uint32_t)); \
        for (int j = i0; j > ai->max_valid_index; j--)                 \
           ai->bitmap[j] = 0;                                          \
        ai->max_valid_index = i0;                                      \
    }                                                             \
    a = ai->bitmap[i0];						        \
    b = (uint32_t) 1 << i1;					\
    do { body; } while (0);				    \
    ai->bitmap[i0] = a;						\
    return ai;						\
 }
 
 /* ALU functions immediate: */
 _(andi, a = a & b)
 _(andnoti, a = a & ~b)
 _(ori, a = a | b)
 _(xori, a = a ^ b)
#undef _

/* ALU function definition macro for functions taking two bitmaps. */
#define _(name, body)                                             \
inline __attribute__((always_inline)) bitmap_t *cli_bitmap_##name (bitmap_t *ai, bitmap_t *bi)  \
  {                                                                           \
    int i;                                                                    \
    uint32_t a, b;                                                            \
    int ai_len, bi_len;                                                       \
    ai_len = ai->max_valid_index + 1;                                         \
    bi_len = bi->max_valid_index + 1;                                         \
    if (bi_len > 0 && ai_len < bi_len) {                                       \
        ai->bitmap = realloc(ai->bitmap, (bi_len) * sizeof(uint32_t));        \
        ai->max_valid_index = bi_len - 1;                                     \
        ai_len = ai->max_valid_index + 1;                                     \
    }                                                                         \
    for (i = 0; i < ai_len; i++) {                                            \
        a = ai->bitmap[i];                                                    \
	    b = i < bi_len ? bi->bitmap[i] : 0;                                   \
	    do {                                                                  \
	        body;                                                             \
	    } while (0);                                                          \
	    ai->bitmap[i] = a;                                                    \
    }                                                                         \
    return ai;                                                                \
}                                                                         

/* ALU functions: */
_(and, a = a & b)
_(andnot, a = a & ~b)
_(or, a = a | b)
_(xor, a = a ^ b)
#undef _

/** Return the number of set bits in a bitmap
    @param ai - pointer to the bitmap
    @returns the number of set bits in the bitmap
*/
inline __attribute__((always_inline)) int cli_bitmap_count_set_bits (bitmap_t * ai)
{
    int i;
    int n_set = 0;

    if (ai) {
        for (i = 0; i < ai->max_valid_index + 1; i++)
            n_set += __builtin_popcount (ai->bitmap[i]);
    }
    
    return n_set;
}

inline __attribute__((always_inline)) void cli_bitmap_free (bitmap_t * ai)
{
    if (ai) {
        free(ai->bitmap);
        free(ai);
    }
}

inline __attribute__((always_inline)) bitmap_t* cli_bitmap_dup (bitmap_t * ai)
{
    bitmap_t* result = 0;
    if (ai) {
        result = (bitmap_t*)malloc(sizeof(bitmap_t));
        result->bitmap = (uint32_t*)malloc((ai->max_valid_index + 1) * sizeof(uint32_t));
        result->max_valid_index = ai->max_valid_index;
        for (int i = 0; i <= result->max_valid_index; i++) {
            result->bitmap[i] = ai->bitmap[i];
        }
    }

    return result;
}

inline __attribute__((always_inline)) int cli_bitmap_is_zero (bitmap_t * ai)
{
    int i;
    for (i = 0; i < (ai->max_valid_index + 1); i++)
        if (ai->bitmap[i] != 0)
            return 0;
    return 1;
}

/* 获取 bitmap 中首个置位的 bit, 如果没有的话, 则返回 ~0 */
inline __attribute__((always_inline)) int 
cli_bitmap_first_set (bitmap_t * ai)
{
    for (int i = 0; i < (ai->max_valid_index + 1); i++) {
        uint32_t x = ai->bitmap[i];
        if (x != 0)
            return i * 32 + __builtin_ctz(x);
    }
    return ~0;
}

inline __attribute__((always_inline)) char unformat_get_input (cli_ctx_t * ctx)
{
    char i = -1;
    if (ctx->index < ctx->len) {
        i = ctx->buffer[ctx->index];
        ctx->index++;
    }
    return i;
}

/* Back up ctx pointer by one. */
inline __attribute__((always_inline)) void unformat_put_input (cli_ctx_t * ctx)
{
  ctx->index -= 1;
}

inline __attribute__((always_inline)) char unformat_peek_input (cli_ctx_t * ctx)
{
  char c = unformat_get_input (ctx);
  if (c != -1)
    unformat_put_input (ctx);
  return c;
}

inline __attribute__((always_inline)) int is_white_space (char c)
{
  switch (c)
    {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
      return 1;

    default:
      return 0;
    }
}

/* Values for is_signed. */
#define UNFORMAT_INTEGER_SIGNED		1
#define UNFORMAT_INTEGER_UNSIGNED	0

/*
 *  unformat 一个整数
 */
static int
unformat_integer (cli_ctx_t* ctx, 
                  va_list* va, int base, int is_signed, int data_bytes)
{
    char c, digit;
    int value = 0;
    int n_digits = 0;
    int n_input = 0;
    int sign = 0;

    /* We only support bases <= 64. */
    if (base < 2 || base > 64)
        goto error;

    while ((c = unformat_get_input (ctx)) != -1) {
        switch (c) {
            case '-':
                if (n_input == 0) {
                    if (is_signed) {
                        sign = 1;
                        goto next_digit;
                    }
                    else
                        /* Leading sign for unsigned number. */
                        goto error;
                }
                /* Sign after input (e.g. 100-200). */
                goto put_input_done;

            case '+':
                if (n_input > 0)
                    goto put_input_done;
                sign = 0;
                goto next_digit;

            case '0' ... '9':
                digit = c - '0';
                break;

            case 'a' ... 'z':
                digit = 10 + (c - 'a');
                break;

            case 'A' ... 'Z':
                digit = 10 + (base >= 36 ? 26 : 0) + (c - 'A');
                break;

            case '/':
                digit = 62;
                break;

            case '?':
                digit = 63;
                break;

	        default:
	            goto put_input_done;
	    }

        if (digit >= base)
        {
            put_input_done:
            unformat_put_input (ctx);
            goto done;
        }

        {
            int new_value = base * value + digit;

            /* Check for overflow. */
            if (new_value < value)
                goto error;
            value = new_value;
        }
        n_digits += 1;

        next_digit:
            n_input++;
    }

done:
    if (sign)
        value = -value;

    if (n_digits > 0)
    {
        void *v = va_arg (*va, void *);
        if (data_bytes == ~0)
	        data_bytes = sizeof (int);

        switch (data_bytes) {
            case 1:
            *(uint8_t *) v = value;
            break;
            case 2:
            *(uint16_t *) v = value;
            break;
            case 4:
            *(uint32_t *) v = value;
            break;
            case 8:
            *(uint64_t *) v = value;
            break;
            default:
            goto error;
        }

        return 1;
    }

error:
  return 0;
}

static const char *
match_input_with_format (cli_ctx_t * ctx, const char *f)
{
    char cf, ci;

    while (1)
    {
        cf = *f;
        if (cf == 0 || cf == '%' || cf == ' ')
            break;
        f++;

        ci = unformat_get_input (ctx); // 从输入数据中获取当前字符，并推进索引

        if (cf != ci)
            return 0;
    }
    return f;
}

static const char *
do_percent (cli_ctx_t * ctx, va_list * va, const char *f)
{
    char cf;
    int n;
    uint32_t data_bytes = ~0;

    cf = *f++;

    switch (cf) {
        default:
            break;

        case 'w':
            /* Word types. */
            cf = *f++;
            data_bytes = sizeof (uint32_t);
            break;

        case 'l':
            cf = *f++;
            if (cf == 'l') {
	            cf = *f++;
	            data_bytes = sizeof (long long);
	        } else {
	            data_bytes = sizeof (long);
	        }
            break;
        case 'L':
            cf = *f++;
            data_bytes = sizeof (long long);
            break;
    }

    n = 0;
    switch (cf) {
        case 'D':
            data_bytes = va_arg (*va, int);
        case 'd':
            n = unformat_integer (ctx, va, 10,
			    UNFORMAT_INTEGER_SIGNED, data_bytes);
            break;

        case 'u':
            n = unformat_integer (ctx, va, 10,
			    UNFORMAT_INTEGER_UNSIGNED, data_bytes);
            break;

        case 'b':
            n = unformat_integer (ctx, va, 2,
			    UNFORMAT_INTEGER_UNSIGNED, data_bytes);
            break;

        case 'o':
            n = unformat_integer (ctx, va, 8,
			    UNFORMAT_INTEGER_UNSIGNED, data_bytes);
            break;

        case 'X':
            data_bytes = va_arg (*va, int);
        case 'x':
            n = unformat_integer (ctx, va, 16,
			    UNFORMAT_INTEGER_UNSIGNED, data_bytes);
            break;

        // case 'f':
        //     n = unformat_float (ctx, va);
        //     break;

        // case 's':
        // case 'v':
        //     n = unformat_string (ctx, f[0], cf, va);
        //     break;
    //     case 'U':
    //   {
	// unformat_function_t *f = va_arg (*va, unformat_function_t *);
	// n = f (ctx, va);
    //   }
    //   break;
    // case '=':
    // case '|':
    //   {
	// int *var = va_arg (*va, int *);
	// uword val = va_arg (*va, int);

	// if (cf == '|')
	//   val |= *var;
	// *var = val;
	// n = 1;
    //   }
    //   break;
    }

    return n ? f : 0;
}

/*
 * 跳过空格, 并返回跳过的空格数目
 */
int unformat_skip_white_space (cli_ctx_t * ctx)
{
    int n = 0;
    char c;

    while ((c = unformat_get_input (ctx)) != -1) {
    if (!is_white_space (c))
    {
        unformat_put_input (ctx);
        break;
    }
    n++;
  }
  return n;
}

static int va_unformat (cli_ctx_t* ctx, const char *fmt, va_list * va)
{
    const char *f;
    int input_matches_format;
    int input_index_save;
    int n_input_white_space_skipped;

    f = fmt;                                 // 格式化字符串
    input_matches_format = 0;
    input_index_save = ctx->index;
    while (1)  // 主循环处理格式字符串
    {
        char cf;
        int is_percent, skip_input_white_space;

        cf = *f;
        is_percent = 0;
        skip_input_white_space = f == fmt;

        /* Spaces in format request skipping input white space. */
        if (is_white_space (cf)) {
	        skip_input_white_space = 1;

	        /* Multiple format spaces are equivalent to a single white space. */
	        while (is_white_space (*++f))  // 移动 f 跳过多余的空格
	            ;
	    } else if (cf == '%') {
	        /* %_ toggles whether or not to skip input white space. */
	        switch (*++f) {
                case '%':
                    break;

                /* % at end of format string. */
                case 0:
                    goto parse_fail;

                default:
                    is_percent = 1;
                    break;
	        }
	    }

        n_input_white_space_skipped = 0;
        if (skip_input_white_space)  // 如果在这之前跳过了空格
	        n_input_white_space_skipped = unformat_skip_white_space (ctx);  // 返回得到跳过的空白字符个数

      /* End of format string. */
      if (cf == 0)
      {
        /* Force parse error when format string ends and input is
            not white or at end.  As an example, this is to prevent
            format "foo" from matching input "food".
            The last_non_white_space_match_percent is to make
            "foo %d" match input "foo 10,bletch" with %d matching 10. */
        if (skip_input_white_space
        //    && !last_non_white_space_match_percent
        //    && !last_non_white_space_match_format
            && n_input_white_space_skipped == 0
            && ctx->index != -1)
          goto parse_fail;
        break;
      }

      if (is_percent)  // 如果是百分号符号
      {
        if (!(f = do_percent (ctx, va, f)))
          goto parse_fail;
      }
      else
      // 下面这是最平常的情况, 也就是真的拿 fmt 去匹配输入
      {
        const char *g = match_input_with_format (ctx, f);
        if (!g)
          goto parse_fail;
        f = g;
      }
    }

  input_matches_format = 1;
parse_fail:
    if (!input_matches_format) {
        ctx->index = input_index_save;
    }
  return input_matches_format;
}

/*
 * 使用 fmt 尝试格式化 ctx 输入 
 */
int unformat (cli_ctx_t* ctx, const char *fmt, ...)
{
    va_list va;
    int result;
    va_start (va, fmt);
    result = va_unformat (ctx, fmt, &va);
    va_end (va);
    return result;
}

// static void cli_output(cli_ctx_t* ctx, char* output) {
void cli_output(cli_ctx_t* ctx, int new_line, char* fmt, ...) 
{
    va_list args;
    int needed;

    va_start(args, fmt);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (ctx->output_index == 0) {
        /* 如果没有输出过任何内容, 则不必设置为新行 */
        new_line = 0;
    }

    if (new_line) {
        needed++;
    }
    int new_size = ctx->output_index + 1 + needed;
    while (new_size > ctx->output_capacity) {
        ctx->output_capacity << 1;
        ctx->output_buffer = (char*)realloc(ctx->output_buffer, ctx->output_capacity);
    }

    if (new_line) {
        ctx->output_buffer[ctx->output_index] = '\n';
        ctx->output_index++;
    }

    va_start(args, fmt);
    vsnprintf(ctx->output_buffer + ctx->output_index, 
              ctx->output_capacity - ctx->output_index,
              fmt, args);
    va_end(args);

    ctx->output_index += needed;
    if (new_line) {
        ctx->output_index--;
    }

    return;
}

void cli_normalize_str(char *input, char **result) {
    // 处理输入为NULL或空字符串的情况
    if (input == NULL || *input == '\0') {
        *result = strdup("");
        return;
    }

    // 分配足够的内存
    char *output = (char *)malloc(strlen(input) + 1);
    if (output == NULL) {
        *result = NULL;
        return;
    }

    char *src = input;   // 源指针遍历输入
    char *dst = output;  // 目标指针写入输出
    int in_space = 0;    // 标记是否在空格序列中
    int has_content = 0; // 标记是否有非空格内容

    // 跳过开头的空白字符
    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }

    // 处理字符串主体
    while (*src != '\0') {
        // 检查回车符 - 替换为字符串终止符并结束处理
        if (*src == '\r') {
            *dst = '\0';  // 添加字符串终止符
            *result = output;
            return;      // 立即返回，不再处理后续字符
        }
        
        // 检查是否为空白字符（空格/制表符/换行）
        if (isspace((unsigned char)*src)) {
            in_space = 1;  // 标记遇到空白字符
        } else {
            // 遇到非空白字符
            if (in_space && has_content) {
                *dst++ = ' ';  // 添加单个空格
                in_space = 0;
            }
            *dst++ = *src;     // 复制当前字符
            has_content = 1;   // 标记已有内容
        }
        src++;
    }

    // 终止字符串
    *dst = '\0';
    
    *result = output;
}

static int cli_command_is_empty (cli_command_t * c)
{
  return (c->help == 0  && c->function == 0);
}

/*
 * 获取一个完整 path 的 parent 路径的长度.
 * eg. path = "show interface"  返回 4
 */
static int parent_path_len (char *path)
{
    int i;
    for (i = strlen(path) - 1; i >= 0; i--)
    {
        if (path[i] == ' ')
	        return i;
    }
    return ~0;
}

/*
 * 添加一个父子命令关系
 * @parent_index: 父命令索引(在cli_main_t.commands中的位置)
 * @child_index:  子命令索引(在cli_main_t.commands中的位置)
 */
static void add_sub_command(int parent_index, int child_index)
{
    cli_command_t *p, *c;
    cli_sub_command_t *sub_c;
    char *sub_name;
    int l, si;
    int i, j;

    p = &cm.commands[parent_index];
    c = &cm.commands[child_index];

    l = parent_path_len (c->path);
    if (l == ~0)
        sub_name = strdup(c->path);
    else {
        sub_name = strdup(c->path + l + 1);
    }

    if (!p->sub_command_index_by_name)
        p->sub_command_index_by_name = hash_table_create();

    /* Check if sub-command has already been created. */
    if (hash_table_get(p->sub_command_index_by_name, sub_name, &si)) {
        free (sub_name);
        return;
    }

    if (!p->sub_commands) {
        p->sub_commands = (cli_sub_command_t*)calloc(INITIAL_COMMAND_NUM, sizeof(cli_sub_command_t));
        p->sub_commands_count = 0;
        p->sub_commands_capacity = INITIAL_COMMAND_NUM;
    } else if (p->sub_commands_count == p->sub_commands_capacity) {
        p->sub_commands = (cli_sub_command_t*)realloc(p->sub_commands, (p->sub_commands_capacity << 1) * sizeof(cli_sub_command_t));
        p->sub_commands_capacity = p->sub_commands_capacity << 1;
    }

    /* si 为 child 命令在 parent 命令的 sub command 的索引 */
    si = p->sub_commands_count;
    p->sub_commands_count ++;
    sub_c = &p->sub_commands[si];
    // p->sub_commands[si] = (cli_sub_command_t *)malloc(sizeof(cli_sub_command_t));
    sub_c->index = child_index;
    sub_c->name = sub_name;
    hash_table_set(p->sub_command_index_by_name,sub_c->name, si);

    /* 接下来开始构建子命令的搜索位图 
     * sub_command_positions 的长度需要保持和最长的 sub command 想通
     */
    int sub_name_len = strlen(sub_name);
    if (!p->sub_command_positions) {
        p->sub_command_positions = (cli_parse_position_t*)calloc(sub_name_len, sizeof(cli_parse_position_t));
        p->sub_command_positions_capacity = sub_name_len;
        for (i = 0; i < sub_name_len; i++) {
            p->sub_command_positions[i].min_char = sub_c->name[i];
            p->sub_command_positions[i].bitmaps = 0;
            p->sub_command_positions[i].bitmaps_max_valid_index = -1;
        } 
    } else if (sub_name_len > p->sub_command_positions_capacity) {
        p->sub_command_positions = (cli_parse_position_t*)realloc(p->sub_command_positions, sub_name_len * sizeof(cli_parse_position_t));
        for (i = p->sub_command_positions_capacity; i < sub_name_len; i++) {
            p->sub_command_positions[i].min_char = sub_c->name[i];
            p->sub_command_positions[i].bitmaps = 0;
            p->sub_command_positions[i].bitmaps_max_valid_index = -1;
        }
        p->sub_command_positions_capacity = sub_name_len;
    }

    /* 接下来需要每个位置.*/
    for (i = 0; i < sub_name_len; i++) {
        /* 子命令有多长, 就要初始化多少 cli_parse_position_t */
        int n;
        cli_parse_position_t *pos;

        pos = &p->sub_command_positions[i];

        /* 计算要插入的 sub command 位置 i 字符和原来记录的 min_char 的差距 
           如果比原来的小, 则需要移动 min_char, 并将 bitmaps 整体往后移动
           如果比原来的大, 则需要看是否要扩大 bitmaps 范围.
         */
        n = sub_c->name[i] - pos->min_char;
        if (n < 0) {
            /* 这个位置插入了更小 char 的 child 命令, 更新这个位置新的 min_char */
	        pos->min_char = sub_c->name[i];
            /* 数组扩张 n 个 */
            pos->bitmaps = (bitmap_t*) realloc (pos->bitmaps, (pos->bitmaps_max_valid_index + 1 - n) * sizeof(bitmap_t));
            /*
             *   进行迁移, 搬移次数
             *      0 -----+       0          0 ---------+
             *      1 ---+ |       1          1 -------+ +--> 1
             *           | |       2          2 -----+ +----> 2
             *           | |       3          3 ---+ +------> 3
             *           | +-----> 4               +--------> 4
             *           |-------> 5
             */
            for (j = pos->bitmaps_max_valid_index; j >= 0; j--) {
                memcpy(&pos->bitmaps[j - n], &pos->bitmaps[j], sizeof(bitmap_t));
            }
            pos->bitmaps_max_valid_index = pos->bitmaps_max_valid_index - n;
            for (j = 0; j < -n; j++) {
                pos->bitmaps[j].bitmap = 0;
                pos->bitmaps[j].max_valid_index = -1;
            }
	        n = 0;
	    }

        if (n > pos->bitmaps_max_valid_index) {
            if (pos->bitmaps_max_valid_index == -1) {
                pos->bitmaps = (bitmap_t*) realloc (pos->bitmaps, 1 * sizeof(bitmap_t));
                pos->bitmaps_max_valid_index = 0;
                pos->bitmaps[0].bitmap = 0;
                pos->bitmaps[0].max_valid_index = -1;
            } else {
                /* 创建下标 [pos->bitmaps_max_valid_index + 1, n] 的 bitmap */
                pos->bitmaps = (bitmap_t*) realloc (pos->bitmaps, (n + pos->bitmaps_max_valid_index + 1) * sizeof(bitmap_t));
                for (j = pos->bitmaps_max_valid_index + 1; j <= n; j++) {
                    pos->bitmaps[j].bitmap = 0;
                    pos->bitmaps[j].max_valid_index = -1;
                }
                pos->bitmaps_max_valid_index += n;
            }
        }
        
        cli_bitmap_ori (&pos->bitmaps[n], si);
    }
    
}

/*
 * 为命令创建 parent 命令
 * @ci: Child command Index
 */
static void cli_make_parent(int ci)
{
    int p_len, pi;
    char *p_path;
    cli_command_t *c, *parent;

    c = &cm.commands[ci];
    p_len = parent_path_len (c->path);

    /* No space?  Parent is root command. */
    if (p_len == ~0) {
        add_sub_command (0, ci);
        return;
    }

    p_path = strndup(c->path, p_len);
    int found = hash_table_get(cm.command_index_by_path, p_path, &pi);
    if(found) {
         /* Parent exists */
        free(p_path);
    } else {
        /* Parent does not exist; create it. */
        if (cm.commands_count == cm.commands_capacity) {
            /* 如果此时已经达到最大容量, 则进行扩容 */
            cm.commands = (cli_command_t*)realloc(cm.commands, (cm.commands_capacity << 1) * sizeof(cli_command_t));
            cm.commands_capacity = cm.commands_capacity << 1;
        }

        pi = cm.commands_count;
        cm.commands_count++;
        hash_table_set (cm.command_index_by_path, p_path, pi); // 将命令的路径和索引存入哈希表
        // cm.commands[pi] = malloc(sizeof(cli_command_t));
        cm.commands[pi].path = p_path;
    }

    /* 记录父子命令关系 */
    add_sub_command (pi, ci);

    /* 继续创建父命令的父命令 */
    if (!found)
        cli_make_parent (pi);
}

/*
 * 注册一个命令
 */
int cli_register(cli_command_t* c)
{
    int error = 0;
    int ci = -1;
    char *normalized_path;

    /* 去掉 command 中多余的空格 */
    cli_normalize_str (c->path, &normalized_path); // normalized_path 结果为标准化后的字符串

    /* See if command already exists with given path. */
    if(hash_table_get(cm.command_index_by_path, normalized_path, &ci)) {
        cli_command_t *d;
        d = &cm.commands[ci];

        /* 如果已存在的命令是创建子命令时自动创建的 */
        if (cli_command_is_empty (d)) {
            cli_command_t save = d[0];

            /* Copy callers fields. */
            d[0] = c[0];

            /* Save internal fields. */
            d->path = save.path;
            d->sub_commands = save.sub_commands;
            d->sub_command_index_by_name = save.sub_command_index_by_name;
            d->sub_command_positions = save.sub_command_positions;
            //d->sub_rules = save.sub_rules;
        }
        else
	        error = -1;

      free (normalized_path);
      if (error)
	      return error;
    } else {
        /* Command does not exist: create it. */
        if (cm.commands_count == cm.commands_capacity) {
            /* 如果此时已经达到最大容量, 则进行扩容 */
            cm.commands = (cli_command_t*) realloc(cm.commands, (cm.commands_capacity << 1) * sizeof(cli_command_t));
            cm.commands_capacity = cm.commands_capacity << 1;
        }

        ci = cm.commands_count;
        cm.commands_count++;
        hash_table_set (cm.command_index_by_path, normalized_path, ci); // 将命令的路径和索引存入哈希表
        // cm.commands[ci] = malloc(sizeof(cli_command_t));
        cm.commands[ci] = c[0];
        cm.commands[ci].path = normalized_path;

        /* Don't inherit from registration. */
        cm.commands[ci].sub_commands = 0;
        cm.commands[ci].sub_commands_count = 0;
        cm.commands[ci].sub_commands_capacity = 0;
        cm.commands[ci].sub_command_index_by_name = 0;
        cm.commands[ci].sub_command_positions = 0;
        cm.commands[ci].sub_command_positions_capacity = 0;
    }

    /* 为命令创建 parent 命令 */
    cli_make_parent(ci);

    return 0;
}

/* Returns bitmap of commands which match key.  
 * 返回匹配 command 中匹配 key 的 bitmap, bitmap 中置 1 的 bit 为符合的 sub command 的索引(在sub_commands中)
 */
static bitmap_t* cli_sub_command_match(cli_command_t * c, cli_ctx_t* ctx)
{
    int i, n;
    bitmap_t *match = 0;
    cli_parse_position_t *p;

    unformat_skip_white_space (ctx);

    for (i = 0;; i++) {
        char k;
        k = unformat_get_input (ctx);  // 从 ctx 读取下一个字符
        switch (k) {
            case 'a' ... 'z':
            case 'A' ... 'Z':
            case '0' ... '9':
            case '-':
            case '_':
                break;

            case ' ':
            // case '\t':
            // case '\r':
            // case '\n':
            case -1:
                if (i < c->sub_command_positions_capacity && cli_bitmap_count_set_bits (match) > 1) {
                    p = &c->sub_command_positions[i];
                    for (n = 0; n < p->bitmaps_max_valid_index + 1; n++)
                        match = cli_bitmap_andnot (match, &p->bitmaps[n]);
                }
                goto done;

            default:
                unformat_put_input (ctx);
            goto done;
	    }

        /* 如果读取的位置已经超过了c的最大sub command 长度, 则说明匹配失败 */
        if (i >= c->sub_command_positions_capacity) {
no_match:
            cli_bitmap_free (match);
            return 0;
        }

        /* 下面开始考虑命令 c 的位置 i */
        p = &c->sub_command_positions[i];

        n = k - p->min_char;
        if (n < 0 || n >= (p->bitmaps_max_valid_index + 1))
	        goto no_match;

        if (i == 0)
	        match = cli_bitmap_dup (&p->bitmaps[n]);
        else
	        match = cli_bitmap_and (match, &p->bitmaps[n]);

        if (cli_bitmap_is_zero (match))
	        goto no_match;
    }

done:
  return match;
}

/*
 * 获得一个 parent 命令在 index 为 si 的 child 命令
 */
static cli_command_t *get_sub_command (cli_command_t* parent, uint32_t si)
{
    cli_sub_command_t *s = &parent->sub_commands[si];
    return &cm.commands[s->index];
}

/*
 * 以 parent 命令为基础, 根据输入的字符串, 查询满足条件的 sub command, 返回是否唯一匹配. 
 * 如果是unique匹配, 会将唯一匹配的 sub command 保存到 result,
 */
static int parse_cli_sub_command(cli_ctx_t* i, cli_command_t *parent,  cli_command_t **result) {
    bitmap_t *match_bitmap;
    int is_unique, index, match_count;

    match_bitmap = cli_sub_command_match (parent, i);  // 根据 输入的命令行字符串，返回匹配的子命令位图
    match_count = cli_bitmap_count_set_bits (match_bitmap);
    is_unique = match_count == 1;
    index = ~0;
    if (is_unique) {
        index = cli_bitmap_first_set (match_bitmap);
        *result = get_sub_command (parent, index);
    }
    cli_bitmap_free (match_bitmap);

    return match_count;
}

static void cli_output_sub_commands(cli_ctx_t* ctx, cli_command_t* parent) 
{
    int index, i;
    cli_command_t *cmd;

    for (i = 0; i < parent->sub_commands_count; i++) {
        index = parent->sub_commands[i].index;
        cmd = &cm.commands[index];
        cli_output(ctx, NEW_LINE, "%s", parent->sub_commands[i].name);
    }
}

static int cli_dispatch_sub_commands (cli_ctx_t* ctx, int parent_command_index)  // 最开始进来为 0
{
    cli_command_t *parent, *c;
    int error = 0, match_count = 0, i = 0;
    cli_ctx_t sub_input;

    parent = &cm.commands[parent_command_index];
    if (unformat (ctx, "help") || unformat (ctx, "?")) {
        int help_at_end_of_line;
        help_at_end_of_line = unformat_peek_input (ctx) == -1;
        if (!help_at_end_of_line) {
            cli_output(ctx, NEW_LINE, " help must appear in line end");
        } else {
            if (parent->help)
                cli_output(ctx, NEW_LINE, parent->help);
            else if (parent->sub_commands_count > 0) {
                cli_output_sub_commands(ctx, parent);
            } else {
                cli_output(ctx, NEW_LINE, "no sub command");
            }
        }

    } else {
        match_count = parse_cli_sub_command(ctx, parent, &c);
        if (match_count == 1) {
            /* 如果精确匹配,  */
            cli_ctx_t *si;
            int has_sub_commands = c->sub_commands_count;

            si = ctx;
            if (has_sub_commands)
                /* 如果还有子命令, 则递归进行 dispatch */
                error = cli_dispatch_sub_commands (si, c - cm.commands);

           
            if (!error && c->function) {
                int error;
                unformat_skip_white_space (si);

                if (unformat (si, "?") || unformat (si, "help")) {
                    if (c->help) {
                        cli_output(ctx, CUR_LINE, c->help);
                    }
                } else {
                    error = c->function (si);
                }
            }
        } else if (match_count > 1) {
            cli_output(ctx, NEW_LINE, " ambiguous commands");
            error = -1;
        } else {
            cli_output(ctx, NEW_LINE, " command not found");
            error = -1;
        }
    } 

    return error;
}

cli_main_t* get_cli_main() {
    return &cm;
}

int cli_input(int client_fd, char* user_input) {
    char* normalize_str = 0;
    cli_normalize_str(user_input, &normalize_str);
    int len = strlen(normalize_str);
    
    cli_ctx_t ctx;    
    ctx.buffer = normalize_str;
    ctx.len = len;
    ctx.index = 0;
    ctx.fd = client_fd;
    ctx.output_buffer = (char*) malloc(256);
    ctx.output_buffer[0] = '#';
    ctx.output_index = 0;
    ctx.output_capacity = 256;
    cli_dispatch_sub_commands (&ctx, /* parent */ 0);
    write(client_fd, ctx.output_buffer, ctx.output_index > 0 ? ctx.output_index:1);

    free(normalize_str);
    free(ctx.output_buffer);
    return 0;
}

int cli_init()
{
    int error = 0;
    cli_command_t *cmd;

    if (!cm.commands) {
        cm.commands = (cli_command_t*)calloc(INITIAL_COMMAND_NUM, sizeof(cli_command_t));
        cm.commands_count = 0;
        cm.commands_capacity = INITIAL_COMMAND_NUM;
    }

    if (!cm.command_index_by_path) 
        cm.command_index_by_path = hash_table_create();

    /* Add root command (index 0) */
    // cm.commands[0] = malloc(sizeof(cli_command_t));
    cm.commands[0].path = "";
    cm.commands[0].sub_commands = 0;
    cm.commands[0].sub_command_index_by_name = 0;
    cm.commands_count++;

    cmd = cm.cli_command_registrations;       // 包含所有预注册命令
    while (cmd) {
        error = cli_register (cmd);      // 注册所有命令
        if (error)
            return error;
        cmd = cmd->next_cli_command;
    }

    return error;
}