#ifndef RADIX_IPC_H
#define RADIX_IPC_H

#include <radix/types.h>
#include <radix/atomic.h>
#include <radix/sync.h>

#define IPC_MAX_SLOTS           8
#define IPC_SLOT_PAYLOAD_SIZE   64

/* IPC Message Container */
typedef struct ipc_msg {
    uint32_t msg_id;
    uint16_t len;
    uint16_t flags;
    uint8_t  data[IPC_SLOT_PAYLOAD_SIZE];
} __attribute__((aligned(4))) ipc_msg_t;

/* Single-Producer Single-Consumer (SPSC) Lock-Free Zero-Copy Channel */
typedef struct ipc_channel {
    ipc_msg_t           slots[IPC_MAX_SLOTS];
    volatile uint32_t   head;       /* Advanced by Producer */
    volatile uint32_t   tail;       /* Advanced by Consumer */
    semaphore_t         sem_items;  /* Available readable messages */
    semaphore_t         sem_spaces; /* Available writable slots */
} ipc_channel_t;

/* Channel Initialization */
void ipc_channel_init(ipc_channel_t *ch);

/* Zero-Copy Producer API */
ipc_msg_t *ipc_claim_slot(ipc_channel_t *ch);
void       ipc_commit_slot(ipc_channel_t *ch);

/* Zero-Copy Consumer API */
const ipc_msg_t *ipc_peek_slot(const ipc_channel_t *ch);
void             ipc_release_slot(ipc_channel_t *ch);

/* Non-blocking convenience helpers */
bool     ipc_send_noblock(ipc_channel_t *ch, uint32_t msg_id, const void *payload, uint16_t len);
bool     ipc_recv_noblock(ipc_channel_t *ch, uint32_t *msg_id, void *payload_out, uint16_t *len_out);

/* Status queries */
uint32_t ipc_available_count(const ipc_channel_t *ch);
bool     ipc_is_empty(const ipc_channel_t *ch);
bool     ipc_is_full(const ipc_channel_t *ch);

/* Optional blocking wrappers for tasks */
ipc_msg_t       *ipc_claim_slot_blocking(ipc_channel_t *ch);
const ipc_msg_t *ipc_peek_slot_blocking(ipc_channel_t *ch);

#endif /* RADIX_IPC_H */
