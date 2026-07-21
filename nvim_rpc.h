#ifndef NVIM_RPC_H
#define NVIM_RPC_H

#include <stdint.h>
#include <sys/types.h>
#include <mpack.h>

typedef enum {
  NVL_RPC_DROPPED = -1,
  NVL_RPC_DISCONNECTED = 0,
  NVL_RPC_CONNECTED = 1
} nvl_rpc_status_t;

struct nvl_rpc_t {
  nvl_rpc_status_t status;
  mpack_rpc_session_t* rpc_session;
  int socket;
};

typedef enum {
  NVL_POLL_TIMEOUT = -1,
  NVL_POLL_SUCCESS = 0,
  NVL_POLL_ERROR = 1,
} NVL_POLL;

void nvl_rpc_init(struct nvl_rpc_t *rpc_connection, char* socket_path);
NVL_POLL nvl_rpc_poll(struct nvl_rpc_t *rpc_connection, int timeout, char *out_result_buf, size_t *out_len);
void nvl_rpc_send(struct nvl_rpc_t *rpc_connection, const char* command, const mpack_token_t* data, ssize_t data_length);
void nvl_rpc_drop(struct nvl_rpc_t *rpc_connection);
void nvl_rpc_command(struct nvl_rpc_t *rpc_connection, const char* command);
void nvl_rpc_eval(struct nvl_rpc_t *rpc_connection, const char* command);
void nvl_rpc_ui_attach(struct nvl_rpc_t *rpc_connection, unsigned int width, unsigned int height);
void nvl_rpc_ui_detach(struct nvl_rpc_t *rpc_connection);

#endif /* NVIM_RPC_H */
