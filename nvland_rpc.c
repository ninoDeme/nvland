#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>

#include <mpack.h>

#define SOCKET_PATH "/run/user/1000/nvim.29865.0"
#define BUFFER_SIZE 100

struct mp_buffer_t {
  size_t remaining_size;
  size_t total_size;
  char* original_pointer;
  char* buffer;
};

void pack_cstring(struct mp_buffer_t *buffer, mpack_tokbuf_t *tokbuf, const char *str) {
  size_t len = strlen(str);

  mpack_token_t str_header_tok = mpack_pack_str(len);
  mpack_write(tokbuf, &buffer->buffer, &buffer->remaining_size, &str_header_tok);

  mpack_token_t str_chunk_tok = mpack_pack_chunk(str, (mpack_uint32_t) len);
  mpack_write(tokbuf, &buffer->buffer, &buffer->remaining_size, &str_chunk_tok);
}

int main(void)
{
  mpack_rpc_session_t rpc_session = {0};
  mpack_rpc_session_init(&rpc_session, MPACK_RPC_MAX_REQUESTS);

  int sock;
  struct sockaddr_un addr;
  char buffer[BUFFER_SIZE];

  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket");
    return EXIT_FAILURE;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("connect");
    close(sock);
    return EXIT_FAILURE;
  }

  printf("Connected to %s\n", SOCKET_PATH);


  // if (res >= MPACK_RPC_ERROR) {
  //   switch (res) {
  //     case MPACK_RPC_EARRAY:
  //       printf("MPACK_RPC_EARRAY\n");
  //       return 1;
  //     case MPACK_RPC_EARRAYL:
  //       printf("MPACK_RPC_EARRAYL\n");
  //       return 1;
  //     case MPACK_RPC_ETYPE:
  //       printf("MPACK_RPC_ETYPE\n");
  //       return 1;
  //     case MPACK_RPC_EMSGID:
  //       printf("MPACK_RPC_EMSGID\n");
  //       return 1;
  //     case MPACK_RPC_ERESPID:
  //       printf("MPACK_RPC_ERESPID\n");
  //       return 1;
  //   }
  // }
  // printf("%i\n", res);

  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    FD_SET(STDIN_FILENO, &fds);

    int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;

    if (select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) {
      perror("select");
      break;
    }

    if (FD_ISSET(sock, &fds)) {
      ssize_t n = read(sock, buffer, sizeof(buffer) - 1);

      if (n == 0) {
        printf("Server disconnected.\n");
        break;
      }
      if (n < 0) {
        perror("read");
        break;
      }

      const char *read_ptr = buffer;
      size_t read_len = (size_t)n;
      mpack_rpc_session_t *session_ptr = (mpack_rpc_session_t*)&rpc_session;

      while (read_len > 0) {
        mpack_rpc_message_t msg;
        int res = mpack_rpc_receive(session_ptr, &read_ptr, &read_len, &msg);

        if (res == MPACK_RPC_RESPONSE) {
          printf("Success! Received response for Request ID: %u\n", msg.id);
          // Because mpack_rpc_receive successfully parsed the response, 
          // it has now automatically freed the slot, allowing your next request to succeed!
        } else if (res == MPACK_RPC_NOTIFICATION) {
          printf("Received an event notification from Neovim.\n");
        } else if (res == MPACK_EOF) {
          // Need more data from the socket to finish parsing
          break;
        } else if (res < 0) {
          printf("Failed to parse incoming RPC data: %d\n", res);
          break;
        }
      }
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
        printf("EOF on stdin\n");
        break;
      }
      if (strlen(buffer) == 0) continue;

      buffer[strcspn(buffer, "\n")] = '\0';

      mpack_rpc_session_t *session_ptr = (mpack_rpc_session_t*)&rpc_session;

      mpack_tokbuf_t *tokbuf = malloc(sizeof(mpack_tokbuf_t));
      mpack_tokbuf_init(tokbuf);

      char out_buf[1024];
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
          continue;
      }
      printf("%i\n", mp_buffer.remaining_size);

      pack_cstring(&mp_buffer, tokbuf, "nvim_command");

      mpack_token_t array_tok = mpack_pack_array(1);
      mpack_write(tokbuf, &mp_buffer.buffer, &mp_buffer.remaining_size, &array_tok);

      pack_cstring(&mp_buffer, tokbuf, buffer);

      size_t len = mp_buffer.total_size - mp_buffer.remaining_size;
      for (size_t i = 0; i < len; i++) {
        printf("%02x ", (unsigned char)mp_buffer.original_pointer[i]);
      }
      printf("\n");
      if (write(sock, mp_buffer.original_pointer, len) != (ssize_t)len) {
        perror("write");
        break;
      }
      printf("Sent command to Neovim!\n");
    }
  }

  close(sock);
  return EXIT_SUCCESS;
}
