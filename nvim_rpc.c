#include "nvim_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <poll.h>

// #define STB_DS_IMPLEMENTATION
// #include "stb_ds.h"

#define BUFFER_SIZE 100
#define NVL_MP_MAX_DEPTH 32

struct mp_buffer_t {
  size_t remaining_size;
  size_t total_size;
  char* original_pointer;
  char* buffer;
};

typedef struct {
  mpack_token_type_t type;
  uint32_t items_left;
} nvl_mp_frame_t;

static void pack_cstring(struct mp_buffer_t *buffer, mpack_tokbuf_t *tokbuf, const char *str);
static void nvl_log_msgpack(const char *buf, size_t len);
static int consume_value(const mpack_token_t* data, size_t data_length, size_t* index);
static ssize_t count_top_level_values(const mpack_token_t* data, size_t data_length);
static int consume_one_value_from_socket(int sock, mpack_rpc_session_t *session, char *rx_buf, size_t *rx_len, size_t rx_buf_capacity, mpack_token_t *first_tok);
static int consume_value_content_from_socket(int sock, mpack_rpc_session_t *session, char *rx_buf, size_t *rx_len, size_t rx_buf_capacity, mpack_token_t tok);

/* --- Public API Implementation --- */

void nvl_rpc_init(struct nvl_rpc_t *rpc_connection, char* socket_path) {
  mpack_rpc_session_t *rpc_session = malloc(sizeof(mpack_rpc_session_t));
  mpack_rpc_session_init(rpc_session, MPACK_RPC_MAX_REQUESTS);

  rpc_connection->rpc_session = rpc_session;

  struct sockaddr_un addr;

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);

  rpc_connection->socket = sock;

  if (sock == -1) {
    perror("socket");
    exit(1); // TODO: error handling
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("connect");
    close(sock);
    exit(1); // TODO: error handling
  }

  printf("Connected to %s\n", socket_path);
}

void nvl_rpc_drop(struct nvl_rpc_t *rpc_connection) {
  close(rpc_connection->socket);
  free(rpc_connection->rpc_session);
  rpc_connection->status = NVL_RPC_DROPPED;
}

NVL_POLL nvl_rpc_poll(struct nvl_rpc_t *rpc_connection, int timeout, char *out_result_buf, size_t *out_len) {
  int sock = rpc_connection->socket;

  struct pollfd fds = {
    .fd = sock,
    .events = POLLIN | POLLHUP | POLLERR | POLLNVAL,
    .revents = 0
  };

  int res = poll(&fds, 1, timeout);
  if (res == -1) {
    perror("poll");
    return NVL_POLL_ERROR;
  }

  if (res == 0) { 
    printf("Timeout.\n");
    return NVL_POLL_TIMEOUT;
  }

  if (fds.revents & POLLHUP) {
    printf("POLLHUP.\n");
    return NVL_POLL_ERROR;
  }
  if (fds.revents & POLLERR) {
    printf("POLLERR.\n");
    return NVL_POLL_ERROR;
  }
  if (fds.revents & POLLNVAL) {
    printf("POLLNVAL.\n");
    return NVL_POLL_ERROR;
  }

  if (!(fds.revents & POLLIN)) {
    printf("Not POLLIN.\n");
    return NVL_POLL_ERROR;
  }

  char rx_buf[4096];
  mpack_rpc_session_t *session_ptr = rpc_connection->rpc_session;

  while (1) {
    size_t rx_len = 0;
    mpack_rpc_message_t msg;
    int parse_res = MPACK_EOF;

    while (parse_res == MPACK_EOF) {
      char c;
      ssize_t n = read(sock, &c, 1);
      if (n == 0) {
        printf("Server disconnected.\n");
        return NVL_POLL_ERROR;
      }
      if (n < 0) {
        perror("read");
        return NVL_POLL_ERROR;
      }

      if (rx_len < sizeof(rx_buf)) {
        rx_buf[rx_len++] = c;
      }

      const char *read_ptr = &c;
      size_t read_len = 1;

      parse_res = mpack_rpc_receive(session_ptr, &read_ptr, &read_len, &msg);
      if (parse_res < 0) {
        printf("Failed to parse incoming RPC data: %d\n", parse_res);
        exit(1); // TODO: error handling
      }
    }

    if (parse_res == MPACK_RPC_RESPONSE) {
      printf("Received response for Request ID: %u\n", msg.id);

      mpack_token_t error_tok;
      if (consume_one_value_from_socket(sock, session_ptr, rx_buf, &rx_len, sizeof(rx_buf), &error_tok) != 0) {
        printf("Failed to parse error value\n");
        exit(1); // TODO: error handling
      }

      if (error_tok.type != MPACK_TOKEN_NIL) {
        printf("RPC error token type: %d\n", error_tok.type);
        printf("RPC error dump:\n");
        nvl_log_msgpack(rx_buf, rx_len);
        exit(1); // TODO: error handling
      }

      size_t result_start = rx_len;
      if (consume_one_value_from_socket(sock, session_ptr, rx_buf, &rx_len, sizeof(rx_buf), NULL) != 0) {
        printf("Failed to parse result value\n");
        exit(1);
      }

      if (out_result_buf && out_len) {
        size_t actual_length = rx_len - result_start;
        if (*out_len < actual_length) {
          printf("Caller buffer too small (provided %zu, need %zu)\n", *out_len, actual_length);
          *out_len = actual_length;
          return NVL_POLL_ERROR;
        }
        memcpy(out_result_buf, &rx_buf[result_start], actual_length);
        *out_len = actual_length;
      }

      nvl_log_msgpack(rx_buf, rx_len);

      return NVL_POLL_SUCCESS;

    } else if (parse_res == MPACK_RPC_NOTIFICATION) {
      printf("Received notification ID: %u\n", msg.id);

      mpack_token_t error_tok;
      if (consume_one_value_from_socket(sock, session_ptr, rx_buf, &rx_len, sizeof(rx_buf), &error_tok) != 0) {
        printf("Failed to parse error value\n");
        exit(1); // TODO: error handling
      }

      if (error_tok.type != MPACK_TOKEN_NIL) {
        printf("RPC error token type: %d\n", error_tok.type);
        printf("RPC error dump:\n");
        nvl_log_msgpack(rx_buf, rx_len);
        exit(1); // TODO: error handling
      }

      size_t result_start = rx_len;
      if (consume_one_value_from_socket(sock, session_ptr, rx_buf, &rx_len, sizeof(rx_buf), NULL) != 0) {
        printf("Failed to parse result value\n");
        exit(1);
      }

      if (out_result_buf && out_len) {
        size_t actual_length = rx_len - result_start;
        if (*out_len < actual_length) {
          printf("Caller buffer too small (provided %zu, need %zu)\n", *out_len, actual_length);
          *out_len = actual_length;
          return NVL_POLL_ERROR;
        }
        memcpy(out_result_buf, &rx_buf[result_start], actual_length);
        *out_len = actual_length;
      }

      printf("Received asynchronous Neovim notification!\n");
      nvl_log_msgpack(rx_buf, rx_len);

      continue;

    } else if (parse_res == MPACK_RPC_REQUEST) {
      while (session_ptr->reader.passthrough > 0) {
        char c;
        ssize_t n = read(sock, &c, 1);
        if (n == 0) return NVL_POLL_ERROR;
        if (n < 0) { perror("read"); return NVL_POLL_ERROR; }

        if (rx_len < sizeof(rx_buf)) {
          rx_buf[rx_len++] = c;
        }

        const char *read_ptr = &c;
        size_t read_len = 1;

        mpack_token_t chunk_tok;
        int chunk_res = mpack_read(&session_ptr->reader, &read_ptr, &read_len, &chunk_tok);
        if (chunk_res == MPACK_ERROR) return NVL_POLL_ERROR;
      }

      if (consume_one_value_from_socket(sock, session_ptr, rx_buf, &rx_len, sizeof(rx_buf), NULL) != 0) {
        printf("Failed to parse request params\n");
        exit(1);
      }

      printf("Received Neovim request!\n");
      nvl_log_msgpack(rx_buf, rx_len);

      continue;

    } else {
      printf("Unexpected parsed message type: %d\n", parse_res);
      exit(1);
    }
  }
}

void nvl_rpc_send(struct nvl_rpc_t *rpc_connection, const char* command, const mpack_token_t* data, ssize_t data_length) {
  mpack_rpc_session_t *session_ptr = rpc_connection->rpc_session;

  mpack_tokbuf_t *tokbuf = malloc(sizeof(mpack_tokbuf_t));
  mpack_tokbuf_init(tokbuf);

  char out_buf[4096];
  struct mp_buffer_t mp_buffer = {
    .remaining_size = sizeof(out_buf),
    .total_size = sizeof(out_buf),
    .original_pointer = out_buf,
    .buffer = out_buf 
  };

  mp_buffer.original_pointer = mp_buffer.buffer;

  mpack_data_t request_metadata = { .i = 1 };
  int res = mpack_rpc_request(session_ptr, &mp_buffer.buffer, &mp_buffer.remaining_size, request_metadata);
  if (res != MPACK_OK) {
    fprintf(stderr, "Failed to start RPC Request! Error code: %d\n", res);
    fprintf(stderr, "(This usually means your session capacity is full because a previous response wasn't read).\n");
    exit(1);
  }

  pack_cstring(&mp_buffer, tokbuf, command);

  ssize_t n_values = count_top_level_values(data, data_length);
  if (n_values < 0) {
    fprintf(stderr, "Invalid token array sequence\n");
    exit(1);
  }
  mpack_token_t array_tok = mpack_pack_array(n_values);

  mpack_write(tokbuf, &mp_buffer.buffer, &mp_buffer.remaining_size, &array_tok);

  for (ssize_t i = 0; i < data_length; i++) {
    mpack_write(tokbuf, &mp_buffer.buffer, &mp_buffer.remaining_size, &data[i]);
  }

  size_t len = mp_buffer.total_size - mp_buffer.remaining_size;
  if (write(rpc_connection->socket, mp_buffer.original_pointer, len) != (ssize_t)len) {
    perror("write");
    exit(1);
  }
  printf("Sent command to Neovim!\n");

  free(tokbuf);
}

void nvl_rpc_eval(struct nvl_rpc_t *rpc_connection, const char* command) {
  ssize_t len = strlen(command);
  mpack_token_t data[2] = {
    mpack_pack_str(len),
    mpack_pack_chunk(command, (mpack_uint32_t) len)
  };
  nvl_rpc_send(rpc_connection, "nvim_command", data, 2);
  if (nvl_rpc_poll(rpc_connection, 100, NULL, NULL)) {
    printf("poll error\n");
    exit(1);
  }
}

void nvl_rpc_command(struct nvl_rpc_t *rpc_connection, const char* command) {
  ssize_t len = strlen(command);
  mpack_token_t data[2] = {
    mpack_pack_str(len),
    mpack_pack_chunk(command, (mpack_uint32_t) len)
  };
  nvl_rpc_send(rpc_connection, "nvim_command", data, 2);
  if (nvl_rpc_poll(rpc_connection, 100, NULL, NULL)) {
    printf("poll error\n");
    exit(1);
  }
}

#define NVL_PACK_CSTRING(str) \
  mpack_pack_str(strlen(str)), \
  mpack_pack_chunk(str, strlen(str))


void nvl_rpc_ui_attach(struct nvl_rpc_t *rpc_connection, unsigned int width, unsigned int height) {
  mpack_token_t data[10] = {
    mpack_pack_uint((mpack_uintmax_t) width),
    mpack_pack_uint((mpack_uintmax_t) height),
    mpack_pack_map(2),
    NVL_PACK_CSTRING("ext_linegrid"),
    mpack_pack_boolean(1),
    NVL_PACK_CSTRING("term_name"),
    NVL_PACK_CSTRING("nvland"),
  };

  nvl_rpc_send(rpc_connection, "nvim_ui_attach", data, 10);
  if (nvl_rpc_poll(rpc_connection, 100, NULL, NULL)) {
    printf("poll error\n");
    exit(1);
  }
}

void nvl_rpc_ui_detach(struct nvl_rpc_t *rpc_connection) {
  nvl_rpc_send(rpc_connection, "nvim_ui_detach", NULL, 0);
  if (nvl_rpc_poll(rpc_connection, 100, NULL, NULL)) {
    printf("poll error\n");
    exit(1);
  }
}

// ======================== Helper functions =================================

static void pack_cstring(struct mp_buffer_t *buffer, mpack_tokbuf_t *tokbuf, const char *str) {
  size_t len = strlen(str);

  mpack_token_t str_header_tok = mpack_pack_str(len);
  mpack_write(tokbuf, &buffer->buffer, &buffer->remaining_size, &str_header_tok);

  mpack_token_t str_chunk_tok = mpack_pack_chunk(str, (mpack_uint32_t) len);
  mpack_write(tokbuf, &buffer->buffer, &buffer->remaining_size, &str_chunk_tok);
}

static void nvl_log_msgpack(const char *buf, size_t len) {
  mpack_tokbuf_t tb;
  mpack_tokbuf_init(&tb);

  const char *read_ptr = buf;
  size_t read_len = len;

  nvl_mp_frame_t stack[NVL_MP_MAX_DEPTH];
  int depth = 0;

  printf("--- Msgpack Debug Dump (%zu bytes) ---\n", len);

  while (read_len > 0) {
    mpack_token_t tok;
    int res = mpack_read(&tb, &read_ptr, &read_len, &tok);
    if (res == MPACK_EOF) {
      printf("[EOF - incomplete stream]\n");
      break;
    }
    if (res == MPACK_ERROR) {
      printf("[ERROR - corrupt msgpack]\n");
      break;
    }

    for (int i = 0; i < depth; i++) printf("  ");

    int complete = 0;

    switch (tok.type) {
      case MPACK_TOKEN_NIL:
        printf("nil\n");
        complete = 1;
        break;
      case MPACK_TOKEN_BOOLEAN:
        printf("bool: %s\n", mpack_unpack_boolean(tok) ? "true" : "false");
        complete = 1;
        break;
      case MPACK_TOKEN_UINT:
        printf("uint: %lu\n", (unsigned long)mpack_unpack_uint(tok));
        complete = 1;
        break;
      case MPACK_TOKEN_SINT:
        printf("sint: %ld\n", (long)mpack_unpack_sint(tok));
        complete = 1;
        break;
      case MPACK_TOKEN_FLOAT:
        printf("float: %f\n", mpack_unpack_float(tok));
        complete = 1;
        break;
      case MPACK_TOKEN_ARRAY:
        printf("array[%u] [\n", tok.length);
        if (depth < NVL_MP_MAX_DEPTH) {
          stack[depth++] = (nvl_mp_frame_t){ MPACK_TOKEN_ARRAY, tok.length };
        }
        break;
      case MPACK_TOKEN_MAP:
        printf("map[%u] {\n", tok.length);
        if (depth < NVL_MP_MAX_DEPTH) {
          stack[depth++] = (nvl_mp_frame_t){ MPACK_TOKEN_MAP, tok.length * 2 };
        }
        break;
      case MPACK_TOKEN_STR:
      case MPACK_TOKEN_BIN:
        if (tok.length == 0) {
          printf("\"\"\n");
          complete = 1;
        } else {
          printf("%s[%u]: ", tok.type == MPACK_TOKEN_STR ? "str" : "bin", tok.length);
        }
        break;
      case MPACK_TOKEN_EXT:
        if (tok.length == 0) {
          printf("ext[type=%d, len=0]\n", tok.data.ext_type);
          complete = 1;
        } else {
          printf("ext[type=%d, len=%u]: ", tok.data.ext_type, tok.length);
        }
        break;
      case MPACK_TOKEN_CHUNK:
        printf("\"%.*s\"\n", (int)tok.length, tok.data.chunk_ptr);
        complete = 1;
        break;
    }

    if (complete) {
      while (depth > 0) {
        stack[depth - 1].items_left--;
        if (stack[depth - 1].items_left == 0) {
          depth--;
          for (int i = 0; i < depth; i++) printf("  ");
          printf("%s\n", stack[depth].type == MPACK_TOKEN_ARRAY ? "]" : "}");
        } else {
          break;
        }
      }
    }
  }
  printf("--- End Dump ---\n");
}

static int consume_value(const mpack_token_t* data, size_t data_length, size_t* index) {
  if (*index >= data_length) return -1;
  
  mpack_token_t tok = data[(*index)++];
  
  switch (tok.type) {
    case MPACK_TOKEN_NIL:
    case MPACK_TOKEN_BOOLEAN:
    case MPACK_TOKEN_UINT:
    case MPACK_TOKEN_SINT:
    case MPACK_TOKEN_FLOAT:
      return 0;
      
    case MPACK_TOKEN_STR:
    case MPACK_TOKEN_BIN:
    case MPACK_TOKEN_EXT:
      if (tok.length > 0) {
        if (*index >= data_length || data[*index].type != MPACK_TOKEN_CHUNK) return -1;
        (*index)++;
      }
      return 0;
      
    case MPACK_TOKEN_ARRAY: {
      uint32_t len = tok.length;
      for (uint32_t i = 0; i < len; i++) {
        if (consume_value(data, data_length, index) != 0) return -1;
      }
      return 0;
    }
    
    case MPACK_TOKEN_MAP: {
      uint32_t len = tok.length * 2;
      for (uint32_t i = 0; i < len; i++) {
        if (consume_value(data, data_length, index) != 0) return -1;
      }
      return 0;
    }
    
    default:
      return -1;
  }
}

static ssize_t count_top_level_values(const mpack_token_t* data, size_t data_length) {
  size_t index = 0;
  ssize_t count = 0;
  while (index < data_length) {
    if (consume_value(data, data_length, &index) != 0) return -1;
    count++;
  }
  return count;
}

static int consume_one_value_from_socket(int sock, mpack_rpc_session_t *session, char *rx_buf, size_t *rx_len, size_t rx_buf_capacity, mpack_token_t *first_tok) {
  mpack_token_t tok;
  int read_res = MPACK_EOF;

  while (read_res == MPACK_EOF) {
    char c;
    ssize_t n = read(sock, &c, 1);
    if (n == 0) {
      printf("Server disconnected.\n");
      return -1;
    }
    if (n < 0) {
      perror("read");
      return -1;
    }

    if (*rx_len < rx_buf_capacity) {
      rx_buf[(*rx_len)++] = c;
    }

    const char *read_ptr = &c;
    size_t read_len = 1;

    read_res = mpack_read(&session->reader, &read_ptr, &read_len, &tok);
    if (read_res == MPACK_ERROR) {
      printf("Failed to parse token\n");
      return -1;
    }
  }

  if (first_tok) {
    *first_tok = tok;
  }

  return consume_value_content_from_socket(sock, session, rx_buf, rx_len, rx_buf_capacity, tok);
}

static int consume_value_content_from_socket(int sock, mpack_rpc_session_t *session, char *rx_buf, size_t *rx_len, size_t rx_buf_capacity, mpack_token_t tok) {
  switch (tok.type) {
    case MPACK_TOKEN_NIL:
    case MPACK_TOKEN_BOOLEAN:
    case MPACK_TOKEN_UINT:
    case MPACK_TOKEN_SINT:
    case MPACK_TOKEN_FLOAT:
      return 0;

    case MPACK_TOKEN_STR:
    case MPACK_TOKEN_BIN:
    case MPACK_TOKEN_EXT: {
      if (tok.length > 0) {
        while (session->reader.passthrough > 0) {
          char c;
          ssize_t n = read(sock, &c, 1);
          if (n == 0) return -1;
          if (n < 0) { perror("read"); return -1; }

          if (*rx_len < rx_buf_capacity) {
            rx_buf[(*rx_len)++] = c;
          }

          const char *read_ptr = &c;
          size_t read_len = 1;

          mpack_token_t chunk_tok;
          int chunk_res = mpack_read(&session->reader, &read_ptr, &read_len, &chunk_tok);
          if (chunk_res == MPACK_ERROR) return -1;
        }
      }
      return 0;
    }

    case MPACK_TOKEN_ARRAY: {
      uint32_t len = tok.length;
      for (uint32_t i = 0; i < len; i++) {
        if (consume_one_value_from_socket(sock, session, rx_buf, rx_len, rx_buf_capacity, NULL) != 0) {
          return -1;
        }
      }
      return 0;
    }

    case MPACK_TOKEN_MAP: {
      uint32_t len = tok.length * 2;
      for (uint32_t i = 0; i < len; i++) {
        if (consume_one_value_from_socket(sock, session, rx_buf, rx_len, rx_buf_capacity, NULL) != 0) {
          return -1;
        }
      }
      return 0;
    }

    default:
      return -1;
  }
}
