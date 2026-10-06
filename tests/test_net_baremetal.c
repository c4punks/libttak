#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <ttak/net/core/port.h>
#include <ttak/phys/mem/buddy.h>

int main(void) {
    ttak_net_driver_ops_t ops;
    memset(&ops, 0, sizeof(ops));

    ttak_net_baremetal_spec_t spec;
    memset(&spec, 0, sizeof(spec));

    ttak_net_driver_get_baremetal(&ops, &spec);

    assert(ops.socket_open != NULL);
    assert(ops.socket_close != NULL);
    assert(ops.socket_bind != NULL);
    assert(ops.socket_listen != NULL);
    assert(ops.socket_connect != NULL);
    assert(ops.socket_send != NULL);
    assert(ops.socket_recv != NULL);
    assert(ops.socket_setopt != NULL);
    assert(ops.poll_wait != NULL);

    /* 1. Open two virtual sockets (client and server) */
    int server_fd = ops.socket_open(2, 1, 0); /* AF_INET, SOCK_STREAM */
    assert(server_fd >= 0);

    int client_fd = ops.socket_open(2, 1, 0);
    assert(client_fd >= 0);
    assert(client_fd != server_fd);

    /* 2. Bind server */
    uint16_t port = 8080;
    int rc = ops.socket_bind(server_fd, &port, sizeof(port));
    assert(rc == 0);

    /* Binding another socket to same port should fail */
    rc = ops.socket_bind(client_fd, &port, sizeof(port));
    assert(rc != 0);
    (void)rc;

    /* 3. Listen on server */
    rc = ops.socket_listen(server_fd, 5);
    assert(rc == 0);

    /* 4. Connect client to server */
    rc = ops.socket_connect(client_fd, &port, sizeof(port));
    assert(rc == 0);

    /* 5. Send data from client to server */
    const char *msg = "hello baremetal net!";
    size_t msg_len = strlen(msg) + 1;
    int sent = ops.socket_send(client_fd, msg, msg_len);
    assert(sent == (int)msg_len);
    (void)sent;

    /* Poll server for readable data */
    int poll_rc = ops.poll_wait(server_fd, 0x0001 /* POLLIN */, 0);
    assert(poll_rc > 0);
    (void)poll_rc;

    /* 6. Recv on server */
    char recv_buf[64];
    memset(recv_buf, 0, sizeof(recv_buf));
    int recvd = ops.socket_recv(server_fd, recv_buf, sizeof(recv_buf));
    assert(recvd == (int)msg_len);
    assert(strcmp(recv_buf, msg) == 0);
    (void)recvd;

    /* 7. Send reply from server to client */
    const char *reply = "baremetal net ack";
    size_t reply_len = strlen(reply) + 1;
    sent = ops.socket_send(server_fd, reply, reply_len);
    assert(sent == (int)reply_len);

    memset(recv_buf, 0, sizeof(recv_buf));
    recvd = ops.socket_recv(client_fd, recv_buf, sizeof(recv_buf));
    assert(recvd == (int)reply_len);
    assert(strcmp(recv_buf, reply) == 0);

    /* 8. Close sockets */
    assert(ops.socket_close(client_fd) == 0);
    assert(ops.socket_close(server_fd) == 0);

    puts("test_net_baremetal passed");
    return 0;
}
