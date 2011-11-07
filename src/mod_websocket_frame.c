/**
 * $Id$
 * a part of mod_websocket
 */

#include <string.h>

#include "mod_websocket.h"

#ifdef	_MOD_WEBSOCKET_SPEC_IETF_00_
int
mod_websocket_frame_send(handler_ctx *hctx,
                         mod_websocket_frame_type_t type,
                         char *payload, size_t siz) {
    const char additional = 0x00;
    const unsigned char head = 0x00;
    const unsigned char tail = 0xff;
    const unsigned char cfrm[2] = { 0xff, 0x00 };
    int ret = -1;
    buffer *b = NULL;
    char *enc = NULL;
    size_t encsiz = siz * 3; // XXX

    if (!hctx || (!payload && type != MOD_WEBSOCKET_FRAME_TYPE_CLOSE)) {
        return -1;
    }
    if (!siz && type != MOD_WEBSOCKET_FRAME_TYPE_CLOSE) {
        return 0;
    }
    b = chunkqueue_get_append_buffer(hctx->tocli);
    if (!b) {
        DEBUG_LOG("s", "no memory");
        return -1;
    }
    switch (type) {
    case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
        ret = buffer_append_memory(b, (const char *)&head, 1);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            break;
        }
        enc = (char *)malloc(sizeof(char) * encsiz);
        if (!enc) {
            DEBUG_LOG("s", "no memory");
            ret = -1;
            break;
        }
        memset(enc, 0, encsiz);
        ret = mod_websocket_conv_to_client(hctx->cnv,
                                           enc, &encsiz, payload, siz);
        if (ret != 0) {
            DEBUG_LOG("s", "failed to convert char encodings");
            break;
        }
        ret = buffer_append_memory(b, enc, encsiz);
        free(enc);
        enc = NULL;
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            break;
        }
        ret = buffer_append_memory(b, (const char *)&tail, 1);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
        }
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_CLOSE:
        ret = buffer_append_memory(b, (const char *)cfrm, sizeof(cfrm));
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
        }
        break;
    default:
        ret = -1;
        DEBUG_LOG("s", "not support type");
        break;
    }
    if (ret != 0) {
        chunkqueue_reset(hctx->tocli);
        return ret;
    }
    /* lighty needs additional char to send */
    ret = buffer_append_memory(b, &additional, 1);
    if (ret != 0) {
        DEBUG_LOG("s", "no memory");
        chunkqueue_reset(hctx->tocli);
    }
    return ret;
}

int
mod_websocket_frame_recv(handler_ctx *hctx) {
    const char additional = 0x00;
    chunk *c = NULL;
    buffer *frame = NULL;
    buffer *payload = NULL, *b = NULL;
    int ret;
    char *enc = NULL;
    size_t i, encsiz;

    if (!hctx || !hctx->con || !hctx->con->read_queue) {
        return -1;
    }
    if (chunkqueue_is_empty(hctx->con->read_queue)) {
        return 0;
    }
    /* serialize data */
    for (c = hctx->con->read_queue->first; c; c = c->next) {
        if (NULL == frame) {
            frame = buffer_init();
            if (!frame) {
                DEBUG_LOG("s", "no memory");
                chunkqueue_reset(hctx->con->read_queue);
                return -1;
            }
        }
        ret = buffer_append_memory(frame, c->mem->ptr, c->mem->used - 1);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            buffer_free(frame);
            chunkqueue_reset(hctx->con->read_queue);
            return -1;
        }
    }
    chunkqueue_reset(hctx->con->read_queue);

    /* get payload from frame */
    payload = hctx->frame.payload;
    for (i = 0; i < frame->used; i++) {
        if (hctx->frame.state == MOD_WEBSOCKET_FRAME_STATE_INIT) {
            if (0x00 == frame->ptr[i]) {
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_PAYLOAD;
            } else {
                DEBUG_LOG("s", "recv closing or invalid frame");
                buffer_free(frame);
                buffer_reset(payload);
                return -1;
            }
        } else if (hctx->frame.state ==
                   MOD_WEBSOCKET_FRAME_STATE_READ_PAYLOAD) {
            if (0xff == frame->ptr[i]) { // XXX: equal to tail flag(0xff)
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                if (mod_websocket_conv_isUTF8(payload->ptr,
                                              payload->used) !=
                    MOD_WEBSOCKET_TRUE) {
                    DEBUG_LOG("s", "recv not UTF-8");
                    buffer_free(frame);
                    buffer_reset(payload);
                    return -1;
                }
                encsiz = (payload->used) * 3; // XXX
                enc = (char *)malloc(sizeof(char) * encsiz);
                if (!enc) {
                    DEBUG_LOG("s", "no memory");
                    buffer_free(frame);
                    buffer_reset(payload);
                    return -1;
                }
                memset(enc, 0, encsiz);
                ret = mod_websocket_conv_to_server(hctx->cnv,
                                                   enc, &encsiz,
                                                   payload->ptr,
                                                   payload->used);
                buffer_reset(payload);
                if (ret != 0) {
                    DEBUG_LOG("s", "fail to convert chars");
                    buffer_free(frame);
                    free(enc);
                    return -1;
                }
                b = chunkqueue_get_append_buffer(hctx->tosrv);
                if (!b) {
                    DEBUG_LOG("s", "no memory");
                    buffer_free(frame);
                    free(enc);
                    return -1;
                }
                ret = buffer_append_memory(b, enc, encsiz);
                if (ret != 0) {
                    DEBUG_LOG("s", "no memory");
                    buffer_free(frame);
                    free(enc);
                    return -1;
                }
                free(enc);
                /* lighty needs additional char to send */
                ret = buffer_append_memory(b, &additional, 1);
                if (ret != 0) {
                    DEBUG_LOG("s", "no memory");
                    buffer_free(frame);
                    chunkqueue_reset(hctx->tosrv);
                    return -1;
                }
            } else {
                ret = buffer_append_memory(payload, &frame->ptr[i], 1);
                if (ret != 0) {
                    DEBUG_LOG("s", "no memory");
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                    buffer_free(frame);
                    buffer_reset(payload);
                    return -1;
                }
            }
        } else { /* never reach */
            DEBUG_LOG("s", "BUG: unknown state");
            buffer_free(frame);
            buffer_reset(payload);
            return -1;
        }
    }
    buffer_free(frame);
    return 0;
}
#endif	/* _MOD_WEBSOCKET_SPEC_IETF_00_ */

#ifdef	_MOD_WEBSOCKET_SPEC_IETF_08_
int
mod_websocket_frame_send(handler_ctx *hctx,
                         mod_websocket_frame_type_t type,
                         char *payload, size_t siz) {
    const char additional = 0x00;
    int ret = -1;
    char c, len[MOD_WEBSOCKET_FRAME_LEN63 + 1];
    buffer *b = NULL;
    char *enc = NULL;
    size_t encsiz = siz * 3; // XXX

    if (!hctx ||
        (!payload &&
         (type == MOD_WEBSOCKET_FRAME_TYPE_TEXT ||
          type == MOD_WEBSOCKET_FRAME_TYPE_BIN))) {
        return -1;
    }
    b = chunkqueue_get_append_buffer(hctx->tocli);
    if (!b) {
        DEBUG_LOG("s", "no memory");
        return -1;
    }

    switch (type) {
    case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
    case MOD_WEBSOCKET_FRAME_TYPE_PING:
    case MOD_WEBSOCKET_FRAME_TYPE_PONG:
        if (siz == 0) {
            break;
        }
        enc = (char *)malloc(sizeof(char) * encsiz);
        if (!enc) {
            DEBUG_LOG("s", "no memory");
            buffer_reset(b);
            return -1;
        }
        memset(enc, 0, encsiz);
        ret = mod_websocket_conv_to_client(hctx->cnv,
                                           enc, &encsiz, payload, siz);
        if (ret != 0) {
            DEBUG_LOG("s", "failed to convert char encodings");
            buffer_reset(b);
            free(enc);
            return -1;
        }
        siz = encsiz;
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_BIN:
    case MOD_WEBSOCKET_FRAME_TYPE_CLOSE:
    default:
        /* nothing to do */
        break;
    }

    switch (type) {
    case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
        c = (char)(0x80 | MOD_WEBSOCKET_OPCODE_TEXT);
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_BIN:
        c = (char)(0x80 | MOD_WEBSOCKET_OPCODE_BIN);
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_PING:
        c = (char) (0x80 | MOD_WEBSOCKET_OPCODE_PING);
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_PONG:
        c = (char)(0x80 | MOD_WEBSOCKET_OPCODE_PONG);
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_CLOSE:
    default:
        c = (char)(0x80 | MOD_WEBSOCKET_OPCODE_CLOSE);
        break;
    }
    ret = buffer_append_memory(b, &c, 1);
    if (ret != 0) {
        DEBUG_LOG("s", "no memory");
        if (enc) {
            free(enc);
            enc = NULL;
        }
        buffer_reset(b);
        return -1;
    }

    memset(len, 0, sizeof(len));
    if (siz < MOD_WEBSOCKET_FRAME_LEN16) {
        len[0] = siz;
        ret = buffer_append_memory(b, len, 1);
#if (SIZEOF_SIZE_T == 4)
    } else {
        len[0] = MOD_WEBSOCKET_FRAME_LEN16;
        len[1] = (siz >> 8) & 0xff;
        len[2] = siz & 0xff;
        ret = buffer_append_memory(b, len, MOD_WEBSOCKET_FRAME_LEN16_CNT + 1);
#elif (SIZEOF_SIZE_T == 8)
    } else if (siz <= UINT16_MAX) {
        len[0] = MOD_WEBSOCKET_FRAME_LEN16;
        len[1] = (siz >> 8) & 0xff;
        len[2] = siz & 0xff;
        ret = buffer_append_memory(b, len, MOD_WEBSOCKET_FRAME_LEN16_CNT + 1);
    } else {
        len[0] = MOD_WEBSOCKET_FRAME_LEN63;
        len[1] = (siz >> 56) & 0xff;
        len[2] = (siz >> 48) & 0xff;
        len[3] = (siz >> 40) & 0xff;
        len[4] = (siz >> 32) & 0xff;
        len[5] = (siz >> 24) & 0xff;
        len[6] = (siz >> 16) & 0xff;
        len[7] = (siz >> 8) & 0xff;
        len[8] = siz & 0xff;
        ret = buffer_append_memory(b, len, MOD_WEBSOCKET_FRAME_LEN63 + 1);
#endif
    }
    if (ret != 0) {
        DEBUG_LOG("s", "no memory");
        if (enc) {
            free(enc);
            enc = NULL;
        }
        buffer_reset(b);
        return -1;
    }
    if (siz == 0) {
        return ret;
    }

    switch (type) {
    case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
    case MOD_WEBSOCKET_FRAME_TYPE_PING:
    case MOD_WEBSOCKET_FRAME_TYPE_PONG:
        ret = buffer_append_memory(b, enc, siz);
        free(enc);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            buffer_reset(b);
        }
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_BIN:
        ret = buffer_append_memory(b, payload, siz);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            buffer_reset(b);
        }
        break;
    case MOD_WEBSOCKET_FRAME_TYPE_CLOSE:
    default:
        /* nothing to do */
        break;
    }
    if (ret != 0) {
        return ret;
    }
    /* lighty needs additional char to send */
    ret = buffer_append_memory(b, &additional, 1);
    if (ret != 0) {
        DEBUG_LOG("s", "no memory");
        buffer_reset(b);
    }
    return ret;
}

void
unmask_payload(handler_ctx *hctx) {
    size_t i;

    for (i = 0; i < hctx->frame.payload->used; i++) {
        hctx->frame.payload->ptr[i] =
            hctx->frame.payload->ptr[i] ^ hctx->frame.ctl.mask[i % 4];
    }
    return;
}

int
mod_websocket_frame_recv(handler_ctx *hctx) {
    const char additional = 0x00;
    chunk *c = NULL;
    buffer *frame = NULL;
    buffer *payload = NULL, *b = NULL;
    int ret;
    char *enc = NULL;
    size_t i, encsiz;

    if (!hctx || !hctx->con || !hctx->con->read_queue) {
        return -1;
    }
    if (chunkqueue_is_empty(hctx->con->read_queue)) {
        return 0;
    }
    /* serialize data */
    for (c = hctx->con->read_queue->first; c; c = c->next) {
        if (NULL == frame) {
            frame = buffer_init();
            if (!frame) {
                DEBUG_LOG("s", "no memory");
                chunkqueue_reset(hctx->con->read_queue);
                return -1;
            }
        }
        ret = buffer_append_memory(frame, c->mem->ptr, c->mem->used - 1);
        if (ret != 0) {
            DEBUG_LOG("s", "no memory");
            buffer_free(frame);
            chunkqueue_reset(hctx->con->read_queue);
            return -1;
        }
    }
    chunkqueue_reset(hctx->con->read_queue);

    /* get payload from frame */
    payload = hctx->frame.payload;
    for (i = 0; i < frame->used; i++) {
        switch (hctx->frame.state) {
        case MOD_WEBSOCKET_FRAME_STATE_INIT:
            hctx->frame.ctl.rsv = frame->ptr[i] & 0x70;
            hctx->frame.ctl.opcode = frame->ptr[i] & 0x0f;
            if (hctx->frame.ctl.opcode == MOD_WEBSOCKET_OPCODE_TEXT) {
                hctx->frame.type = MOD_WEBSOCKET_FRAME_TYPE_TEXT;
            }
            if (hctx->frame.ctl.opcode == MOD_WEBSOCKET_OPCODE_BIN) {
                hctx->frame.type = MOD_WEBSOCKET_FRAME_TYPE_BIN;
            }
            if (hctx->frame.ctl.opcode == MOD_WEBSOCKET_OPCODE_PING) {
                hctx->frame.type = MOD_WEBSOCKET_FRAME_TYPE_PING;
            }
            if (hctx->frame.ctl.opcode == MOD_WEBSOCKET_OPCODE_PONG) {
                hctx->frame.type = MOD_WEBSOCKET_FRAME_TYPE_PONG;
            }
            if (hctx->frame.ctl.opcode == MOD_WEBSOCKET_OPCODE_CLOSE) {
                buffer_free(frame);
                buffer_reset(payload);
                return -1;
            }
            hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_LENGTH;
            break;
        case MOD_WEBSOCKET_FRAME_STATE_READ_LENGTH:
            hctx->frame.ctl.mask_flag = ((frame->ptr[i] & 0x80) == 0x80);
            hctx->frame.ctl.siz = frame->ptr[i] & 0x7f;
            if (hctx->frame.ctl.siz == 0) {
                switch (hctx->frame.type) {
                case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
                case MOD_WEBSOCKET_FRAME_TYPE_BIN:
                    hctx->frame.ctl.mask_cnt = 0;
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_MASK;
                    break;
                case MOD_WEBSOCKET_FRAME_TYPE_PING:
                    mod_websocket_frame_send(hctx,
                                             MOD_WEBSOCKET_FRAME_TYPE_PONG,
                                             NULL, 0);
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                    buffer_free(frame);
                    buffer_reset(payload);
                    return 0;
                case MOD_WEBSOCKET_FRAME_TYPE_PONG:
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                    buffer_free(frame);
                    buffer_reset(payload);
                    return 0;
                default:
                    DEBUG_LOG("s", "BUG: unknown state");
                    buffer_free(frame);
                    buffer_reset(payload);
                    return -1;
                }

            } else if (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN16) {
                hctx->frame.ctl.ex_siz = 0;
                hctx->frame.ctl.ex_siz_cnt = 0;
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_EX_LENGTH;
#if (SIZEOF_SIZE_T == 4)
            } else if (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN63) {
                /* too large */
                DEBUG_LOG("s", "cant handle 63bits length frame");
                buffer_free(frame);
                buffer_reset(payload);
                return -1;
#elif (SIZEOF_SIZE_T == 8)
            } else if (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN63) {
                hctx->frame.ctl.ex_siz = 0;
                hctx->frame.ctl.ex_siz_cnt = 0;
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_EX_LENGTH;
#endif
            }else {
                if (hctx->frame.ctl.mask_flag) {
                    hctx->frame.ctl.mask_cnt = 0;
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_MASK;
                } else { // TODO: disc, client must mask payload
                    hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_PAYLOAD;
                }
            }
            break;
        case MOD_WEBSOCKET_FRAME_STATE_READ_EX_LENGTH:
            hctx->frame.ctl.ex_siz =
                (hctx->frame.ctl.ex_siz << 8) + (frame->ptr[i] & 0xff);
            hctx->frame.ctl.ex_siz_cnt++;
            if ((hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN16 &&
                 hctx->frame.ctl.ex_siz_cnt == MOD_WEBSOCKET_FRAME_LEN16_CNT) ||
                (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN63 &&
                 hctx->frame.ctl.ex_siz_cnt == MOD_WEBSOCKET_FRAME_LEN63_CNT)) {
                hctx->frame.ctl.mask_cnt = 0;
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_MASK;
            }
            break;
        case MOD_WEBSOCKET_FRAME_STATE_READ_MASK:
            hctx->frame.ctl.mask[hctx->frame.ctl.mask_cnt] = frame->ptr[i];
            hctx->frame.ctl.mask_cnt++;
            if (hctx->frame.ctl.mask_cnt >= MOD_WEBSOCKET_MASK_CNT) {
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_READ_PAYLOAD;
            }
            break;
        case MOD_WEBSOCKET_FRAME_STATE_READ_PAYLOAD:
            ret = buffer_append_memory(payload, &frame->ptr[i], 1);
            if (ret != 0) {
                DEBUG_LOG("s", "no memory");
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                buffer_free(frame);
                buffer_reset(payload);
                return -1;
            }
            if ((hctx->frame.ctl.siz != MOD_WEBSOCKET_FRAME_LEN16 &&
                 hctx->frame.ctl.siz != MOD_WEBSOCKET_FRAME_LEN63 &&
                 hctx->frame.ctl.siz == payload->used) ||
                (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN16 &&
                 hctx->frame.ctl.ex_siz == payload->used) ||
                (hctx->frame.ctl.siz == MOD_WEBSOCKET_FRAME_LEN63 &&
                 hctx->frame.ctl.ex_siz == payload->used)) {
                hctx->frame.state = MOD_WEBSOCKET_FRAME_STATE_INIT;
                if (hctx->frame.ctl.mask_flag) { // XXX
                    unmask_payload(hctx);
                }
                switch (hctx->frame.type) {
                case MOD_WEBSOCKET_FRAME_TYPE_TEXT:
#if (SIZEOF_SIZE_T == 4)
                    if (UINT32_MAX / 3 < payload->used) { // XXX
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        buffer_reset(payload);
                        return -1;
                    }
#elif (SIZEOF_SIZE_T == 8)
                    if (UINT64_MAX / 3 < payload->used) { // XXX
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        buffer_reset(payload);
                        return -1;
                    }
#endif
                    if (mod_websocket_conv_isUTF8(payload->ptr,
                                                  payload->used) !=
                        MOD_WEBSOCKET_TRUE) {
                        DEBUG_LOG("s", "recv not UTF-8");
                        buffer_free(frame);
                        buffer_reset(payload);
                        return -1;
                    }
                    encsiz = (payload->used) * 3; // XXX
                    enc = (char *)malloc(sizeof(char) * encsiz);
                    if (!enc) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        buffer_reset(payload);
                        return -1;
                    }
                    memset(enc, 0, encsiz);
                    ret = mod_websocket_conv_to_server(hctx->cnv,
                                                       enc, &encsiz,
                                                       payload->ptr,
                                                       payload->used);
                    buffer_reset(payload);
                    if (ret != 0) {
                        DEBUG_LOG("s", "fail to convert chars");
                        buffer_free(frame);
                        free(enc);
                        return -1;
                    }
                    b = chunkqueue_get_append_buffer(hctx->tosrv);
                    if (!b) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        free(enc);
                        return -1;
                    }
                    ret = buffer_append_memory(b, enc, encsiz);
                    if (ret != 0) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        free(enc);
                        return -1;
                    }
                    free(enc);
                    /* lighty needs additional char to send */
                    ret = buffer_append_memory(b, &additional, 1);
                    if (ret != 0) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        chunkqueue_reset(hctx->tosrv);
                        return -1;
                    }
                    break;
                case MOD_WEBSOCKET_FRAME_TYPE_BIN:
                    b = chunkqueue_get_append_buffer(hctx->tosrv);
                    if (!b) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        buffer_reset(payload);
                        return -1;
                    }
                    ret = buffer_append_memory(b, payload->ptr, payload->used);
                    if (ret != 0) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        return -1;
                    }
                    buffer_reset(payload);
                    /* lighty needs additional char to send */
                    ret = buffer_append_memory(b, &additional, 1);
                    if (ret != 0) {
                        DEBUG_LOG("s", "no memory");
                        buffer_free(frame);
                        chunkqueue_reset(hctx->tosrv);
                        return -1;
                    }
                    break;
                case MOD_WEBSOCKET_FRAME_TYPE_PING:
                    mod_websocket_frame_send(hctx,
                                             MOD_WEBSOCKET_FRAME_TYPE_PONG,
                                             payload->ptr, payload->used);
                    buffer_reset(payload);
                    break;
                case MOD_WEBSOCKET_FRAME_TYPE_PONG:
                case MOD_WEBSOCKET_FRAME_TYPE_CLOSE:
                default:
                    buffer_reset(payload);
                    break;
                }
            }
            break;
        default:
            DEBUG_LOG("s", "BUG: unknown state");
            buffer_free(frame);
            buffer_reset(payload);
            return -1;
        }
    }
    buffer_free(frame);
    return 0;
}
#endif	/* _MOD_WEBSOCKET_SPEC_IETF_08_ */

/* EOF */
