/*
 * kernel/ipc.c - Zero-Copy Single-Producer Single-Consumer (SPSC) Ring Buffer
 *
 * Provides safe, lock-free communication in internal SRAM without dynamic allocations.
 */

#include <radix/ipc.h>
#include <radix/atomic.h>

void ipc_channel_init(ipc_channel_t *ch) {
    if (!ch) {
        return;
    }

    ch->head = 0;
    ch->tail = 0;

    for (int i = 0; i < IPC_MAX_SLOTS; i++) {
        ch->slots[i].msg_id = 0;
        ch->slots[i].len = 0;
        ch->slots[i].flags = 0;
        for (int j = 0; j < IPC_SLOT_PAYLOAD_SIZE; j++) {
            ch->slots[i].data[j] = 0;
        }
    }

    sem_init(&ch->sem_items, 0, IPC_MAX_SLOTS);
    sem_init(&ch->sem_spaces, IPC_MAX_SLOTS, IPC_MAX_SLOTS);
    arch_memory_barrier();
}

ipc_msg_t *ipc_claim_slot(ipc_channel_t *ch) {
    if (!ch) {
        return NULL;
    }

    /* Check if full */
    if ((ch->head - ch->tail) >= IPC_MAX_SLOTS) {
        return NULL;
    }

    return &ch->slots[ch->head % IPC_MAX_SLOTS];
}

void ipc_commit_slot(ipc_channel_t *ch) {
    if (!ch) {
        return;
    }

    /* Ensure producer's payload writes are committed before sequence update */
    arch_memory_barrier();
    ch->head++;

    /* Signal any waiting consumer */
    sem_post(&ch->sem_items);
}

const ipc_msg_t *ipc_peek_slot(const ipc_channel_t *ch) {
    if (!ch) {
        return NULL;
    }

    /* Check if empty */
    if (ch->head == ch->tail) {
        return NULL;
    }

    /* Ensure consumer observes all writes made before producer incremented head */
    arch_memory_barrier();
    return &ch->slots[ch->tail % IPC_MAX_SLOTS];
}

void ipc_release_slot(ipc_channel_t *ch) {
    if (!ch) {
        return;
    }

    /* Ensure consumer finishes reading slot before sequence update */
    arch_memory_barrier();
    ch->tail++;

    /* Signal any waiting producer */
    sem_post(&ch->sem_spaces);
}

bool ipc_send_noblock(ipc_channel_t *ch, uint32_t msg_id, const void *payload, uint16_t len) {
    ipc_msg_t *slot = ipc_claim_slot(ch);
    if (!slot) {
        return false;
    }

    slot->msg_id = msg_id;
    slot->flags = 0;

    if (len > IPC_SLOT_PAYLOAD_SIZE) {
        len = IPC_SLOT_PAYLOAD_SIZE;
    }
    slot->len = len;

    if (payload && len > 0) {
        const uint8_t *src = (const uint8_t *)payload;
        for (uint16_t i = 0; i < len; i++) {
            slot->data[i] = src[i];
        }
    }

    ipc_commit_slot(ch);
    return true;
}

bool ipc_recv_noblock(ipc_channel_t *ch, uint32_t *msg_id, void *payload_out, uint16_t *len_out) {
    const ipc_msg_t *slot = ipc_peek_slot(ch);
    if (!slot) {
        return false;
    }

    if (msg_id) {
        *msg_id = slot->msg_id;
    }

    if (len_out) {
        *len_out = slot->len;
    }

    if (payload_out && slot->len > 0) {
        uint8_t *dst = (uint8_t *)payload_out;
        for (uint16_t i = 0; i < slot->len; i++) {
            dst[i] = slot->data[i];
        }
    }

    ipc_release_slot(ch);
    return true;
}

uint32_t ipc_available_count(const ipc_channel_t *ch) {
    if (!ch) {
        return 0;
    }
    return (ch->head - ch->tail);
}

bool ipc_is_empty(const ipc_channel_t *ch) {
    if (!ch) {
        return true;
    }
    return (ch->head == ch->tail);
}

bool ipc_is_full(const ipc_channel_t *ch) {
    if (!ch) {
        return false;
    }
    return ((ch->head - ch->tail) >= IPC_MAX_SLOTS);
}

ipc_msg_t *ipc_claim_slot_blocking(ipc_channel_t *ch) {
    if (!ch) {
        return NULL;
    }
    sem_wait(&ch->sem_spaces);
    return ipc_claim_slot(ch);
}

const ipc_msg_t *ipc_peek_slot_blocking(ipc_channel_t *ch) {
    if (!ch) {
        return NULL;
    }
    sem_wait(&ch->sem_items);
    return ipc_peek_slot(ch);
}
