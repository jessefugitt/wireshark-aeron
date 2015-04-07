/* packet-aeron.c
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#ifdef HAVE_ARPA_INET_H
    #include <arpa/inet.h>
#endif
#if HAVE_WINSOCK2_H
    #include <winsock2.h>
#endif
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/expert.h>
#include <epan/uat.h>
#include <epan/tap.h>
#include <epan/conversation.h>
#include <epan/to_str.h>
#ifndef HAVE_INET_ATON
    #include <wsutil/inet_aton.h>
#endif
#include <wsutil/pint.h>

#define AERON_REASSEMBLY 0

void proto_register_aeron(void);
void proto_reg_handoff_aeron(void);

/* Protocol handle */
static int proto_aeron = -1;

/* Dissector handle */
static dissector_handle_t aeron_dissector_handle;

/* TODO:
static int aeron_tap_handle = -1;
*/

/*----------------------------------------------------------------------------*/
/* Aeron transport management.                                                */
/*----------------------------------------------------------------------------*/

static guint64 aeron_channel = 0;

typedef struct
{
    address * addr1;
    address * addr2;
    port_type ptype;
    guint16 port1;
    guint16 port2;
} aeron_conversation_info_t;

struct aeron_transport_t_stct;
typedef struct aeron_transport_t_stct aeron_transport_t;

struct aeron_stream_t_stct;
typedef struct aeron_stream_t_stct aeron_stream_t;

struct aeron_term_t_stct;
typedef struct aeron_term_t_stct aeron_term_t;

struct aeron_fragment_t_stct;
typedef struct aeron_fragment_t_stct aeron_fragment_t;

typedef struct
{
    guint32 term_id;
    guint32 term_offset;
} aeron_pos_t;

static int aeron_pos_roundup(int offset)
{
    return ((offset+7) & 0xfffffff8);
}

static int aeron_pos_compare(const aeron_pos_t * pos1, const aeron_pos_t * pos2)
{
    /* Returns:
        < 0  if pos1 < pos2
        == 0 if pos1 == pos2
        > 0  if pos1 > pos2
    */
    if (pos1->term_id == pos2->term_id)
    {
        if (pos1->term_offset == pos2->term_offset)
        {
            return (0);
        }
        else
        {
            return ((pos1->term_offset < pos2->term_offset) ? -1 : 1);
        }
    }
    else
    {
        return ((pos1->term_id < pos2->term_id) ? -1 : 1);
    }
}

static guint32 aeron_pos_delta(const aeron_pos_t * pos1, const aeron_pos_t * pos2, guint32 term_size)
{
    const aeron_pos_t * p1;
    const aeron_pos_t * p2;
    guint64 p1_val;
    guint64 p2_val;
    guint64 delta;
    int rc;

    rc = aeron_pos_compare(pos1, pos2);
    if (rc >= 0)
    {
        p1 = pos1;
        p2 = pos2;
    }
    else
    {
        p1 = pos2;
        p2 = pos1;
    }
    p1_val = (guint64) (p1->term_id * term_size) + ((guint64) p1->term_offset);
    p2_val = (guint64) (p2->term_id * term_size) + ((guint64) p2->term_offset);
    delta = p1_val - p2_val;
    return ((guint32) (delta & 0x00000000ffffffff));
}

static void aeron_pos_add_length(aeron_pos_t * pos, guint32 length, guint32 term_length)
{
    guint32 next_offset = aeron_pos_roundup(pos->term_offset + length);

    if (next_offset >= term_length)
    {
        pos->term_offset = 0;
        pos->term_id++;
    }
    else
    {
        pos->term_offset = next_offset;
    }
}

typedef struct
{
    guint32 frame;
    guint32 previous_frame;
    guint32 next_frame;
    gboolean retransmission;
} aeron_frame_t;

struct aeron_fragment_t_stct
{
    aeron_term_t * term;                    /* Parent term */
    wmem_tree_t * frame;                    /* Tree of all frames (aeron_frame_t) in which this fragment occurs, keyed by frame number */
    aeron_frame_t * first_frame;
    aeron_frame_t * last_frame;
    guint32 offset;
    guint32 length;
    guint32 data_length;
    guint32 frame_count;
    gboolean is_data_frame;
    gboolean is_begin_msg;
    gboolean is_end_msg;
};

#if AERON_REASSEMBLY
struct aeron_msg_t_stct;
typedef struct aeron_msg_t_stct aeron_msg_t;

struct aeron_msg_fragment_list_t_stct;
typedef struct aeron_msg_fragment_list_t_stct aeron_msg_fragment_list_t;
#endif

struct aeron_term_t_stct
{
    aeron_stream_t * stream;                /* Parent stream */
    wmem_tree_t * fragment;                 /* Tree of all fragments (aeron_fragment_t) in this term, keyed by term offset */
    wmem_tree_t * frame;                    /* Tree of all frames (aeron_frame_t) in this term, keyed by frame number */
#if AERON_REASSEMBLY
    wmem_tree_t * message;                  /* Tree of all fragmented messages (aeron_msg_t) in this term, keyed by lowest term offset */
    aeron_msg_fragment_list_t * orphan_fragment;
#endif
    aeron_frame_t * last_frame;             /* Pointer to last frame seen for this term */
    guint32 term_id;
};

typedef struct
{
    guint32 flags;
    guint32 frame;
    aeron_pos_t high;
    aeron_pos_t completed;
    guint32 receiver_window;
    guint32 outstanding_bytes;
} aeron_stream_frame_analysis_t;
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_FULL      0x00000001
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_IDLE_RX          0x00000002
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_PACING_RX        0x00000004
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO              0x00000008
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_GAP          0x00000010
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE        0x00000020
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_RESIZE    0x00000040
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_SM           0x00000080
#define AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE_SM     0x00000100

struct aeron_stream_t_stct
{
    aeron_transport_t * transport;          /* Parent transprt */
    wmem_tree_t * term;                     /* Tree of all terms (aeron_term_t) in this stream, keyed by term ID */
    wmem_tree_t * frame;                    /* Tree of all frames (aeron_frame_t) in this term, keyed by frame number */
    wmem_tree_t * analysis;                 /* Tree of analysis items (aeron_stream_frame_analysis_t) in this stream, keyed by frame number */
    aeron_frame_t * last_frame;
    guint32 stream_id;
    guint32 term_length;
    guint32 mtu;
    guint32 fragment_stride;
    guint32 flags;
    aeron_pos_t high;
    aeron_pos_t completed;
    guint32 receiver_window;
};
#define AERON_STREAM_FLAGS_HIGH_VALID 0x1
#define AERON_STREAM_FLAGS_COMPLETED_VALID 0x2
#define AERON_STREAM_FLAGS_RECEIVER_WINDOW_VALID 0x4

struct aeron_transport_t_stct
{
    guint64 channel;
    wmem_tree_t * stream;                   /* Tree of all streams (aeron_stream_t) in this transport, keyed by stream ID */
    wmem_tree_t * frame;                    /* Tree of all frames (aeron_frame_t) in this transport, keyed by frame number */
    aeron_frame_t * last_frame;
    address address1;
    address address2;
    guint32 session_id;
    guint16 port1;
    guint16 port2;
};

static guint64 aeron_channel_assign(void)
{
    return (aeron_channel++);
}

static gboolean aeron_is_address_multicast(const address * addr)
{
    guint8 * addr_data = (guint8 *) addr->data;

    switch (addr->type)
    {
        case AT_IPv4:
            if ((addr_data[0] & 0xf0) == 0xe0)
            {
                return (TRUE);
            }
            break;
        case AT_IPv6:
            if (addr_data[0] == 0xff)
            {
                return (TRUE);
            }
            break;
        default:
            break;
    }
    return (FALSE);
}

static char * aeron_format_transport_uri(const aeron_conversation_info_t * cinfo)
{
    wmem_strbuf_t * uri = NULL;

    uri = wmem_strbuf_new(wmem_file_scope(), "aeron:");
    switch (cinfo->ptype)
    {
        case PT_UDP:
            wmem_strbuf_append(uri, "udp");
            break;
        default:
            wmem_strbuf_append(uri, "unknown");
            break;
    }
    wmem_strbuf_append_c(uri, '?');
    if (aeron_is_address_multicast(cinfo->addr2))
    {
        switch (cinfo->addr2->type)
        {
            case AT_IPv6:
                wmem_strbuf_append_printf(uri, "group=[%s]:%" G_GUINT16_FORMAT, address_to_str(wmem_packet_scope(), cinfo->addr2), cinfo->port2);
                break;
            case AT_IPv4:
            default:
                wmem_strbuf_append_printf(uri, "group=%s:%" G_GUINT16_FORMAT, address_to_str(wmem_packet_scope(), cinfo->addr2), cinfo->port2);
                break;
        }
    }
    else
    {
        switch (cinfo->addr2->type)
        {
            case AT_IPv6:
                wmem_strbuf_append_printf(uri, "remote=[%s]:%" G_GUINT16_FORMAT, address_to_str(wmem_packet_scope(), cinfo->addr2), cinfo->port2);
                break;
            case AT_IPv4:
            default:
                wmem_strbuf_append_printf(uri, "remote=%s:%" G_GUINT16_FORMAT, address_to_str(wmem_packet_scope(), cinfo->addr2), cinfo->port2);
                break;
        }
    }
    return (wmem_strbuf_finalize(uri));
}

static aeron_transport_t * aeron_transport_add(const aeron_conversation_info_t * cinfo, guint32 session_id, guint32 frame)
{
    aeron_transport_t * transport;
    conversation_t * conv = NULL;
    wmem_tree_t * session_tree = NULL;

    conv = find_conversation(frame, cinfo->addr1, cinfo->addr2, cinfo->ptype, cinfo->port1, cinfo->port2, 0);
    if (conv == NULL)
    {
        conv = conversation_new(frame, cinfo->addr1, cinfo->addr2, cinfo->ptype, cinfo->port1, cinfo->port2, 0);
    }
    if (frame > conv->last_frame)
    {
        conv->last_frame = frame;
    }
    session_tree = (wmem_tree_t *) conversation_get_proto_data(conv, proto_aeron);
    if (session_tree == NULL)
    {
        session_tree = wmem_tree_new(wmem_file_scope());
        conversation_add_proto_data(conv, proto_aeron, (void *) session_tree);
    }
    transport = (aeron_transport_t *) wmem_tree_lookup32(session_tree, session_id);
    if (transport != NULL)
    {
        return (transport);
    }
    transport = wmem_new(wmem_file_scope(), aeron_transport_t);
    transport->channel = aeron_channel_assign();
    transport->stream = wmem_tree_new(wmem_file_scope());
    transport->frame = wmem_tree_new(wmem_file_scope());
    transport->last_frame = NULL;
    WMEM_COPY_ADDRESS(wmem_file_scope(), &(transport->address1), cinfo->addr1);
    WMEM_COPY_ADDRESS(wmem_file_scope(), &(transport->address2), cinfo->addr2);
    transport->session_id = session_id;
    transport->port1 = cinfo->port1;
    transport->port2 = cinfo->port2;
    wmem_tree_insert32(session_tree, session_id, (void *) transport);
    return (transport);
}

static aeron_stream_t * aeron_transport_stream_find(aeron_transport_t * transport, guint32 stream_id)
{
    aeron_stream_t * stream = NULL;

    stream = (aeron_stream_t *) wmem_tree_lookup32(transport->stream, stream_id);
    return (stream);
}

static aeron_stream_t * aeron_transport_stream_add(aeron_transport_t * transport, guint32 stream_id)
{
    aeron_stream_t * stream = NULL;

    stream = aeron_transport_stream_find(transport, stream_id);
    if (stream == NULL)
    {
        stream = wmem_new(wmem_file_scope(), aeron_stream_t);
        stream->transport = transport;
        stream->term = wmem_tree_new(wmem_file_scope());
        stream->frame = wmem_tree_new(wmem_file_scope());
        stream->analysis = wmem_tree_new(wmem_file_scope());
        stream->last_frame = NULL;
        stream->stream_id = stream_id;
        stream->term_length = 0;
        stream->mtu = 0;
        stream->fragment_stride = 0;
        stream->flags = 0;
        stream->high.term_id = 0;
        stream->high.term_offset = 0;
        stream->completed.term_id = 0;
        stream->completed.term_offset = 0;
        stream->receiver_window = 0;
        wmem_tree_insert32(transport->stream, stream_id, (void *) stream);
    }
    return (stream);
}

static aeron_stream_frame_analysis_t * aeron_stream_frame_analysis_find(aeron_stream_t * stream, guint32 frame)
{
    return ((aeron_stream_frame_analysis_t *) wmem_tree_lookup32(stream->analysis, frame));
}

static aeron_stream_frame_analysis_t * aeron_stream_frame_analysis_add(aeron_stream_t * stream, guint32 frame)
{
    aeron_stream_frame_analysis_t * sfa = NULL;

    sfa = aeron_stream_frame_analysis_find(stream, frame);
    if (sfa != NULL)
    {
        return (sfa);
    }
    sfa = wmem_new(wmem_file_scope(), aeron_stream_frame_analysis_t);
    sfa->flags = 0;
    sfa->frame = frame;
    sfa->high.term_id = 0;
    sfa->high.term_offset = 0;
    sfa->completed.term_id = 0;
    sfa->completed.term_offset = 0;
    sfa->receiver_window = 0;
    sfa->outstanding_bytes = 0;
    wmem_tree_insert32(stream->analysis, frame, (void *) sfa);
    return (sfa);
}

static aeron_term_t * aeron_stream_term_find(aeron_stream_t * stream, guint32 term_id)
{
    aeron_term_t * term = NULL;

    term = (aeron_term_t *) wmem_tree_lookup32(stream->term, term_id);
    return (term);
}

static aeron_term_t * aeron_stream_term_add(aeron_stream_t * stream, guint32 term_id)
{
    aeron_term_t * term = NULL;

    term = aeron_stream_term_find(stream, term_id);
    if (term == NULL)
    {
        term = wmem_new(wmem_file_scope(), aeron_term_t);
        term->stream = stream;
        term->fragment = wmem_tree_new(wmem_file_scope());
        term->frame = wmem_tree_new(wmem_file_scope());
#if AERON_REASSEMBLY
        term->message = wmem_tree_new(wmem_file_scope());
#endif
        term->last_frame = NULL;
        term->term_id = term_id;
        wmem_tree_insert32(stream->term, term_id, (void *) term);
    }
    return (term);
}

static aeron_fragment_t * aeron_term_fragment_find(aeron_term_t * term, guint32 offset)
{
    aeron_fragment_t * fragment = NULL;

    fragment = (aeron_fragment_t *) wmem_tree_lookup32(term->fragment, offset);
    return (fragment);
}

static aeron_fragment_t * aeron_term_fragment_add(aeron_term_t * term, guint32 offset, guint32 length, guint32 data_length)
{
    aeron_fragment_t * fragment = NULL;

    fragment = (aeron_fragment_t *) wmem_tree_lookup32(term->fragment, offset);
    if (fragment == NULL)
    {
        fragment = wmem_new(wmem_file_scope(), aeron_fragment_t);
        memset((void *) fragment, 0, sizeof(aeron_fragment_t));
        fragment->term = term;
        fragment->frame = wmem_tree_new(wmem_file_scope());
        fragment->first_frame = NULL;
        fragment->last_frame = NULL;
        fragment->offset = offset;
        fragment->length = length;
        fragment->data_length = data_length;
        fragment->frame_count = 0;
        wmem_tree_insert32(term->fragment, offset, (void *) fragment);
    }
    return (fragment);
}

static void aeron_transport_frame_add(aeron_transport_t * transport, guint32 frame)
{
    aeron_frame_t * entry;

    entry = wmem_new(wmem_file_scope(), aeron_frame_t);
    memset((void *) entry, 0, sizeof(aeron_frame_t));
    entry->frame = frame;
    if (transport->last_frame != NULL)
    {
        entry->previous_frame = transport->last_frame->frame;
        transport->last_frame->next_frame = frame;
    }
    else
    {
        entry->previous_frame = 0;
    }
    entry->next_frame = 0;
    entry->retransmission = FALSE;
    transport->last_frame = entry;
    wmem_tree_insert32(transport->frame, frame, (void *) entry);
}

static aeron_frame_t * aeron_transport_frame_find(aeron_transport_t * transport, guint32 frame)
{
    return ((aeron_frame_t *) wmem_tree_lookup32(transport->frame, frame));
}

static void aeron_stream_frame_add(aeron_stream_t * stream, guint32 frame)
{
    aeron_frame_t * entry;

    entry = wmem_new(wmem_file_scope(), aeron_frame_t);
    memset((void *) entry, 0, sizeof(aeron_frame_t));
    entry->frame = frame;
    if (stream->last_frame != NULL)
    {
        entry->previous_frame = stream->last_frame->frame;
        stream->last_frame->next_frame = frame;
    }
    else
    {
        entry->previous_frame = 0;
    }
    entry->next_frame = 0;
    entry->retransmission = FALSE;
    stream->last_frame = entry;
    wmem_tree_insert32(stream->frame, frame, (void *) entry);
    aeron_transport_frame_add(stream->transport, frame);
}

static aeron_frame_t * aeron_stream_frame_find(aeron_stream_t * stream, guint32 frame)
{
    return ((aeron_frame_t *) wmem_tree_lookup32(stream->frame, frame));
}

static void aeron_term_frame_add(aeron_term_t * term, guint32 frame)
{
    aeron_frame_t * entry;

    entry = wmem_new(wmem_file_scope(), aeron_frame_t);
    memset((void *) entry, 0, sizeof(aeron_frame_t));
    entry->frame = frame;
    if (term->last_frame != NULL)
    {
        entry->previous_frame = term->last_frame->frame;
        term->last_frame->next_frame = frame;
    }
    else
    {
        entry->previous_frame = 0;
    }
    entry->next_frame = 0;
    entry->retransmission = FALSE;
    term->last_frame = entry;
    wmem_tree_insert32(term->frame, frame, (void *) entry);
    aeron_stream_frame_add(term->stream, frame);
}

static aeron_frame_t * aeron_term_frame_find(aeron_term_t * term, guint32 frame)
{
    return ((aeron_frame_t *) wmem_tree_lookup32(term->frame, frame));
}

static aeron_frame_t * aeron_fragment_frame_find(aeron_fragment_t * fragment, guint32 frame)
{
    aeron_frame_t * entry = NULL;

    entry = (aeron_frame_t *) wmem_tree_lookup32(fragment->frame, frame);
    return (entry);
}

static aeron_frame_t * aeron_fragment_frame_add(aeron_fragment_t * fragment, guint32 frame)
{
    aeron_frame_t * entry = NULL;

    entry = aeron_fragment_frame_find(fragment, frame);
    if (entry == NULL)
    {
        entry = wmem_new(wmem_file_scope(), aeron_frame_t);
        memset((void *) entry, 0, sizeof(aeron_frame_t));
        entry->frame = frame;
        if (fragment->first_frame == NULL)
        {
            fragment->first_frame = entry;
        }
        if (fragment->last_frame == NULL)
        {
            entry->previous_frame = 0;
            entry->retransmission = FALSE;
        }
        else
        {
            entry->previous_frame = fragment->last_frame->frame;
            entry->retransmission = TRUE;
        }
        entry->next_frame = 0;
        fragment->last_frame = entry;
        wmem_tree_insert32(fragment->frame, frame, (void *) entry);
        aeron_term_frame_add(fragment->term, frame);
        fragment->frame_count++;
    }
    return (entry);
}

/*----------------------------------------------------------------------------*/
/* Payload reassembly.                                                        */
/*----------------------------------------------------------------------------*/
#if AERON_REASSEMBLY
struct aeron_msg_fragment_t_stct;
typedef struct aeron_msg_fragment_t_stct aeron_msg_fragment_t;

struct aeron_msg_t_stct
{
    aeron_msg_t * next;
    aeron_msg_t * prev;
    aeron_msg_fragment_t * fragment;
    aeron_msg_fragment_t * fragment_cursor;
    aeron_term_t * term;
    guint32 first_fragment_term_offset;
    guint32 next_expected_term_offset;
    guint32 length;                 /* Total message payload length */
    guint32 frame_length;           /* Total length of all message frames accumulated */
    guint32 fragment_count;
    guint32 contiguous_length;      /* Number of contiguous frame bytes accumulated */
    guint32 begin_frame;            /* Data frame in which the B flag was set */
    guint32 first_frame;            /* Lowest-numbered frame which is part of this message */
    guint32 end_frame;              /* Data frame in which the E flag was set */
    guint32 last_frame;             /* Highest-numbered frame which is part of this message */
    gboolean complete;
};

struct aeron_msg_fragment_t_stct
{
    aeron_msg_fragment_t * prev;
    aeron_msg_fragment_t * next;
    gchar * data;
    guint32 term_offset;            /* Term offset for entire fragment */
    guint32 frame_length;           /* Length of entire frame/fragment */
    guint32 data_length;            /* Payload length */
    guint32 frame;                  /* Frame in which the fragment resides */
    gint frame_offset;              /* Offset into the frame */
};

static aeron_msg_fragment_t * aeron_msg_fragment_create(tvbuff_t * tvb, int offset, packet_info * pinfo, guint32 term_offset, guint32 frame_length)
{
    aeron_msg_fragment_t * frag = NULL;

    frag = wmem_new(wmem_file_scope(), aeron_msg_fragment_t);
    frag->prev = NULL;
    frag->next = NULL;
    frag->term_offset = term_offset;
    frag->frame_length = frame_length;
    frag->data_length = frame_length - L_AERON_DATA;
    frag->frame = pinfo->fd->num;
    frag->frame_offset = offset + L_AERON_DATA;
    frag->data = (gchar *) tvb_memdup(wmem_file_scope(), tvb, frag->frame_offset, (size_t) frag->data_length);
    return (frag);
}

static aeron_msg_fragment_t * aeron_msg_fragment_add(aeron_msg_t * message, tvbuff_t * tvb, int offset, guint32 term_offset, guint32 frame_length)
{
    aeron_msg_fragment_t * frag = NULL;

    if ((tvb == NULL) || (message == NULL))
    {
        return (NULL);
    }
    if (message->fragment == NULL)
    {
        frag = wmem_new(wmem_file_scope(), aeron_msg_fragment_t);
        frag->prev = NULL;
        frag->next = NULL;
        message->fragment = frag;
    }
    else
    {
        aeron_msg_fragment_t * cur = message->fragment;

        while (cur != NULL)
        {
            if (term_offset == cur->term_offset)
            {
                /* Already have it */
                return (cur);
            }
            if (term_offset < cur->term_offset)
            {
                /* Fragment goes after cur->prev */
                cur = cur->prev;
                break;
            }
            if (cur->next == NULL)
            {
                /* Fragment goes after cur */
                break;
            }
            cur = cur->next;
        }
        frag = wmem_new(wmem_file_scope(), aeron_msg_fragment_t);
        if (cur == NULL)
        {
            frag->prev = NULL;
            frag->next = message->fragment;
            message->fragment->prev = frag;
            message->fragment = frag;
        }
        else
        {
            frag->prev = cur;
            frag->next = cur->next;
            cur->next = frag;
            if (frag->next != NULL)
            {
                frag->next->prev = frag;
            }
        }
    }
    frag->term_offset = term_offset;

    frag->data = NULL; /* TODO */
    frag->reassembled_data = NULL; /* TODO */
    frag->data_length = length;
    frag->frame = frame;
    frag->frame_offset = offset;
}

static aeron_msg_t * aeron_term_msg_find_le(aeron_term_t * term, guint32 term_offset)
{
    /* Return the last aeron_msg_t with starting_fragment_term_offset <= offset */
    aeron_msg_t * msg = (aeron_msg_t *) wmem_tree_lookup32_le(term->message, term_offset);
    return (msg);
}

static aeron_msg_t * aeron_term_msg_add(aeron_term_t * term, tvbuff_t * tvb, packet_info * pinfo, int offset, guint32 term_offset, guint32 frame_length)
{
    aeron_msg_t * pos = NULL;
    aeron_msg_t * msg = NULL;

    pos = aeron_msg_find_le(term, term_offset);
    if ((pos != NULL) && (pos->first_fragment_term_offset == term_offset))
    {
        return (pos);
    }
    msg = wmem_new(wmem_file_scope(), aeron_msg_t);
    if (cur == NULL)
    {
        /* Goes at the head of the list */
        msg->prev = NULL;
        if (term->message != NULL)
        {
            msg->next = term->message;
            msg->next->prev = msg;
        }
        else
        {
            msg->next = NULL;
        }
        term->message = msg;
    }
    else
    {
        if (cur->next != NULL)
        {
            cur->next->prev = msg;
            msg->next = cur->next;
            cur->next = msg;
            msg->prev = cur;
        }
    }
    msg->fragment = NULL;
    msg->fragment_cursor = NULL;
    msg->term = term;
    msg->first_fragment_term_offset = offset;
    msg->length = length - L_AERON_DATA;
    msg->frame_length = frame_length;
    msg->fragment_count = 0;
    msg->contiguous_length = 0;
    msg->begin_frame = frame;
    msg->first_frame = frame;
    msg->end_frame = 0;
    msg->last_frame = 0;
    /* TODO: should also add the B fragment */
    return (msg);
}
#endif

/*----------------------------------------------------------------------------*/
/* Packet definitions.                                                        */
/*----------------------------------------------------------------------------*/

/* Aeron protocol is defined at https://github.com/real-logic/Aeron/wiki/Protocol-Specification */

/* Padding frame */
#define O_AERON_PAD_VERSION 0
#define O_AERON_PAD_FLAGS 1
#define O_AERON_PAD_TYPE 2
#define O_AERON_PAD_FRAME_LENGTH 4
#define O_AERON_PAD_TERM_OFFSET 8
#define O_AERON_PAD_SESSION_ID 12
#define O_AERON_PAD_STREAM_ID 16
#define O_AERON_PAD_TERM_ID 20
#define L_AERON_PAD 24

/* Data frame */
#define O_AERON_DATA_VERSION 0
#define O_AERON_DATA_FLAGS 1
#define O_AERON_DATA_TYPE 2
#define O_AERON_DATA_FRAME_LENGTH 4
#define O_AERON_DATA_TERM_OFFSET 8
#define O_AERON_DATA_SESSION_ID 12
#define O_AERON_DATA_STREAM_ID 16
#define O_AERON_DATA_TERM_ID 20
#define O_AERON_DATA_DATA 24
#define L_AERON_DATA 24

/* NAK frame */
#define O_AERON_NAK_VERSION 0
#define O_AERON_NAK_FLAGS 1
#define O_AERON_NAK_TYPE 2
#define O_AERON_NAK_FRAME_LENGTH 4
#define O_AERON_NAK_SESSION_ID 8
#define O_AERON_NAK_STREAM_ID 12
#define O_AERON_NAK_TERM_ID 16
#define O_AERON_NAK_TERM_OFFSET 20
#define O_AERON_NAK_LENGTH 24

/* Status message */
#define O_AERON_SM_VERSION 0
#define O_AERON_SM_FLAGS 1
#define O_AERON_SM_TYPE 2
#define O_AERON_SM_FRAME_LENGTH 4
#define O_AERON_SM_SESSION_ID 8
#define O_AERON_SM_STREAM_ID 12
#define O_AERON_SM_TERM_ID 16
#define O_AERON_SM_COMPLETED_TERM_OFFSET 20
#define O_AERON_SM_RECEIVER_WINDOW 24
#define O_AERON_SM_FEEDBACK 28

/* Error header */
#define O_AERON_ERR_VERSION 0
#define O_AERON_ERR_CODE 1
#define O_AERON_ERR_TYPE 2
#define O_AERON_ERR_FRAME_LENGTH 4
#define O_AERON_ERR_OFFENDING_FRAME_LENGTH 8
#define O_AERON_ERR_OFFENDING_HEADER 12
#define O_AERON_ERR_TERM_ID 16
#define O_AERON_ERR_COMPLETED_TERM_OFFSET 20
#define O_AERON_ERR_RECEIVER_WINDOW 24
#define O_AERON_ERR_FEEDBACK 28

/* Setup frame */
#define O_AERON_SETUP_VERSION 0
#define O_AERON_SETUP_FLAGS 1
#define O_AERON_SETUP_TYPE 2
#define O_AERON_SETUP_FRAME_LENGTH 4
#define O_AERON_SETUP_TERM_OFFSET 8
#define O_AERON_SETUP_SESSION_ID 12
#define O_AERON_SETUP_STREAM_ID 16
#define O_AERON_SETUP_INITIAL_TERM_ID 20
#define O_AERON_SETUP_ACTIVE_TERM_ID 24
#define O_AERON_SETUP_TERM_LENGTH 28
#define O_AERON_SETUP_MTU 32

#define HDR_LENGTH_MIN 12

#define HDR_TYPE_PAD 0x0000
#define HDR_TYPE_DATA 0x0001
#define HDR_TYPE_NAK 0x0002
#define HDR_TYPE_SM 0x0003
#define HDR_TYPE_ERR 0x0004
#define HDR_TYPE_SETUP 0x0005
#define HDR_TYPE_EXT 0xFFFF

#define DATA_FLAGS_BEGIN 0x80
#define DATA_FLAGS_END 0x40
#define DATA_FLAGS_COMPLETE (DATA_FLAGS_BEGIN | DATA_FLAGS_END)

#define STATUS_FLAGS_SETUP 0x80

/*----------------------------------------------------------------------------*/
/* Value translation tables.                                                  */
/*----------------------------------------------------------------------------*/

static const value_string aeron_frame_type[] =
{
    { HDR_TYPE_PAD, "Pad" },
    { HDR_TYPE_DATA, "Data" },
    { HDR_TYPE_NAK, "NAK" },
    { HDR_TYPE_SM, "Status" },
    { HDR_TYPE_ERR, "Error" },
    { HDR_TYPE_SETUP, "Setup" },
    { HDR_TYPE_EXT, "Extension" },
    { 0x0, NULL }
};

/*----------------------------------------------------------------------------*/
/* Preferences.                                                               */
/*----------------------------------------------------------------------------*/

static gboolean global_aeron_sequence_analysis = FALSE;
static gboolean global_aeron_window_analysis = FALSE;
#if AERON_REASSEMBLY
static gboolean global_aeron_reassemble_fragments = FALSE;
#endif
static gboolean aeron_sequence_analysis = FALSE;
static gboolean aeron_window_analysis = FALSE;
#if AERON_REASSEMBLY
static gboolean aeron_reassemble_fragments = FALSE;
#endif

/*
    Aeron conversations:

    UDP unicast:
    - The URL specifies the subscriber address and UDP port, and the publisher "connects" to the single subscriber.
    - The publisher sends Pad, Data, and Setup frames to the subscriber address and port.
    - The subscriber sends NAK and SM frames to the publisher, using as the destination the address and port from
      which the Setup and Data frames were received
    - So the conversation is defined by [A(publisher),A(subscriber),P(publisher),P(subscriber),PT_UDP]

    UDP multicast:
    - The URL specifies the data multicast group and UDP port, and must be an odd-numbered address. The control multicast
      group is automatically set to be one greater than the data multicast group, and the same port is used.
    - The publisher sends Pad, Data, and Setup frames to the data multicast group and port.
    - The subscriber sends NAK and SM frames to the control multicast group and port.
    - So the conversation is defined by [ControlGroup,DataGroup,port,port,PT_UDP]

*/

static aeron_conversation_info_t * aeron_setup_conversation_info(const packet_info * pinfo, guint16 type)
{
    aeron_conversation_info_t * cinfo;
    int addr_len = pinfo->dst.len;

    cinfo = wmem_new(wmem_packet_scope(), aeron_conversation_info_t);
    memset((void *) cinfo, 0, sizeof(aeron_conversation_info_t));
    cinfo->ptype = pinfo->ptype;
    switch (pinfo->dst.type)
    {
        case AT_IPv4:
            {
                guint8 * dst_addr = (guint8 *) pinfo->dst.data;

                cinfo->addr1 = wmem_new(wmem_packet_scope(), address);
                cinfo->addr2 = wmem_new(wmem_packet_scope(), address);
                if (aeron_is_address_multicast(&(pinfo->dst)))
                {
                    guint8 * addr1 = NULL;
                    guint8 * addr2 = NULL;

                    addr1 = (guint8 *) wmem_alloc(wmem_packet_scope(), (size_t) addr_len);
                    addr2 = (guint8 *) wmem_alloc(wmem_packet_scope(), (size_t) addr_len);
                    memcpy((void *) addr1, (void *) dst_addr, (size_t) addr_len);
                    memcpy((void *) addr2, (void *) dst_addr, (size_t) addr_len);
                    if ((dst_addr[addr_len - 1] & 0x1) != 0)
                    {
                        /* Address is odd, so it's the data group (in addr2). Increment the last byte of addr1 for the control group. */
                        addr1[addr_len - 1]++;
                    }
                    else
                    {
                        /* Address is even, so it's the control group (in addr1). Decrement the last byte of addr2 for the data group. */
                        addr2[addr_len - 1]--;
                    }
                    SET_ADDRESS(cinfo->addr1, AT_IPv4, addr_len, (void *) addr1);
                    SET_ADDRESS(cinfo->addr2, AT_IPv4, addr_len, (void *) addr2);
                    cinfo->port1 = pinfo->destport;
                    cinfo->port2 = cinfo->port1;
                }
                else
                {
                    switch (type)
                    {
                        case HDR_TYPE_PAD:
                        case HDR_TYPE_DATA:
                        case HDR_TYPE_SETUP:
                            /* Destination is a receiver */
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr1, &(pinfo->src));
                            cinfo->port1 = pinfo->srcport;
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr2, &(pinfo->dst));
                            cinfo->port2 = pinfo->destport;
                            break;
                        case HDR_TYPE_NAK:
                        case HDR_TYPE_SM:
                            /* Destination is the source */
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr1, &(pinfo->dst));
                            cinfo->port1 = pinfo->destport;
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr2, &(pinfo->src));
                            cinfo->port2 = pinfo->srcport;
                            break;
                        default:
                            break;
                    }
                }
            }
            break;
        case AT_IPv6:
            {
                guint8 * dst_addr = (guint8 *) pinfo->dst.data;

                cinfo->addr1 = wmem_new(wmem_packet_scope(), address);
                cinfo->addr2 = wmem_new(wmem_packet_scope(), address);
                if (aeron_is_address_multicast(&(pinfo->dst)))
                {
                    guint8 * addr1 = NULL;
                    guint8 * addr2 = NULL;

                    addr1 = (guint8 *) wmem_alloc(wmem_packet_scope(), (size_t) addr_len);
                    addr2 = (guint8 *) wmem_alloc(wmem_packet_scope(), (size_t) addr_len);
                    memcpy((void *) addr1, (void *) dst_addr, (size_t) addr_len);
                    memcpy((void *) addr2, (void *) dst_addr, (size_t) addr_len);
                    if ((dst_addr[addr_len - 1] & 0x1) != 0)
                    {
                        /* Address is odd, so it's the data group (in addr2). Increment the last byte of addr1 for the control group. */
                        addr1[addr_len - 1]++;
                    }
                    else
                    {
                        /* Address is even, so it's the control group (in addr1). Decrement the last byte of addr2 for the data group. */
                        addr2[addr_len - 1]--;
                    }
                    SET_ADDRESS(cinfo->addr1, AT_IPv6, addr_len, (void *) addr1);
                    SET_ADDRESS(cinfo->addr2, AT_IPv6, addr_len, (void *) addr2);
                    cinfo->port1 = pinfo->destport;
                    cinfo->port2 = cinfo->port1;
                }
                else
                {
                    switch (type)
                    {
                        case HDR_TYPE_PAD:
                        case HDR_TYPE_DATA:
                        case HDR_TYPE_SETUP:
                            /* Destination is a receiver */
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr1, &(pinfo->src));
                            cinfo->port1 = pinfo->srcport;
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr2, &(pinfo->dst));
                            cinfo->port2 = pinfo->destport;
                            break;
                        case HDR_TYPE_NAK:
                        case HDR_TYPE_SM:
                            /* Destination is the source */
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr1, &(pinfo->dst));
                            cinfo->port1 = pinfo->destport;
                            WMEM_COPY_ADDRESS(wmem_packet_scope(), cinfo->addr2, &(pinfo->src));
                            cinfo->port2 = pinfo->srcport;
                            break;
                        default:
                            break;
                    }
                }
            }
            break;
        default:
            return (NULL);
    }
    return (cinfo);
}

/*----------------------------------------------------------------------------*/
/* Handles of all types.                                                      */
/*----------------------------------------------------------------------------*/

/* Dissector tree handles */
static gint ett_aeron = -1;
static gint ett_aeron_pad = -1;
static gint ett_aeron_data = -1;
static gint ett_aeron_data_flags = -1;
static gint ett_aeron_nak = -1;
static gint ett_aeron_sm = -1;
static gint ett_aeron_sm_flags = -1;
static gint ett_aeron_err = -1;
static gint ett_aeron_setup = -1;
static gint ett_aeron_ext = -1;
static gint ett_aeron_sequence_analysis = -1;
static gint ett_aeron_sequence_analysis_term_offset = -1;
static gint ett_aeron_window_analysis = -1;

/* Dissector field handles */
static int hf_aeron_channel = -1;
static int hf_aeron_pad = -1;
static int hf_aeron_pad_version = -1;
static int hf_aeron_pad_flags = -1;
static int hf_aeron_pad_type = -1;
static int hf_aeron_pad_frame_length = -1;
static int hf_aeron_pad_term_offset = -1;
static int hf_aeron_pad_session_id = -1;
static int hf_aeron_pad_stream_id = -1;
static int hf_aeron_pad_term_id = -1;
static int hf_aeron_data = -1;
static int hf_aeron_data_version = -1;
static int hf_aeron_data_flags = -1;
static int hf_aeron_data_flags_b = -1;
static int hf_aeron_data_flags_e = -1;
static int hf_aeron_data_type = -1;
static int hf_aeron_data_frame_length = -1;
static int hf_aeron_data_term_offset = -1;
static int hf_aeron_data_next_offset = -1;
static int hf_aeron_data_next_offset_term = -1;
static int hf_aeron_data_next_offset_first_frame = -1;
static int hf_aeron_data_session_id = -1;
static int hf_aeron_data_stream_id = -1;
static int hf_aeron_data_term_id = -1;
static int hf_aeron_data_data = -1;
static int hf_aeron_nak = -1;
static int hf_aeron_nak_version = -1;
static int hf_aeron_nak_flags = -1;
static int hf_aeron_nak_type = -1;
static int hf_aeron_nak_frame_length = -1;
static int hf_aeron_nak_session_id = -1;
static int hf_aeron_nak_stream_id = -1;
static int hf_aeron_nak_term_id = -1;
static int hf_aeron_nak_term_offset = -1;
static int hf_aeron_nak_length = -1;
static int hf_aeron_sm = -1;
static int hf_aeron_sm_version = -1;
static int hf_aeron_sm_flags = -1;
static int hf_aeron_sm_flags_s = -1;
static int hf_aeron_sm_type = -1;
static int hf_aeron_sm_frame_length = -1;
static int hf_aeron_sm_session_id = -1;
static int hf_aeron_sm_stream_id = -1;
static int hf_aeron_sm_term_id = -1;
static int hf_aeron_sm_completed_term_offset = -1;
static int hf_aeron_sm_receiver_window = -1;
static int hf_aeron_sm_feedback = -1;
static int hf_aeron_err = -1;
static int hf_aeron_err_version = -1;
static int hf_aeron_err_code = -1;
static int hf_aeron_err_type = -1;
static int hf_aeron_err_frame_length = -1;
static int hf_aeron_err_off_frame_length = -1;
static int hf_aeron_err_off_hdr = -1;
static int hf_aeron_err_string = -1;
static int hf_aeron_setup = -1;
static int hf_aeron_setup_version = -1;
static int hf_aeron_setup_flags = -1;
static int hf_aeron_setup_type = -1;
static int hf_aeron_setup_frame_length = -1;
static int hf_aeron_setup_term_offset = -1;
static int hf_aeron_setup_session_id = -1;
static int hf_aeron_setup_stream_id = -1;
static int hf_aeron_setup_initial_term_id = -1;
static int hf_aeron_setup_active_term_id = -1;
static int hf_aeron_setup_term_length = -1;
static int hf_aeron_setup_mtu = -1;
static int hf_aeron_sequence_analysis = -1;
static int hf_aeron_sequence_analysis_channel_prev_frame = -1;
static int hf_aeron_sequence_analysis_channel_next_frame = -1;
static int hf_aeron_sequence_analysis_stream_prev_frame = -1;
static int hf_aeron_sequence_analysis_stream_next_frame = -1;
static int hf_aeron_sequence_analysis_term_prev_frame = -1;
static int hf_aeron_sequence_analysis_term_next_frame = -1;
static int hf_aeron_sequence_analysis_term_offset = -1;
static int hf_aeron_sequence_analysis_term_offset_frame = -1;
static int hf_aeron_sequence_analysis_retransmission = -1;
static int hf_aeron_window_analysis = -1;
static int hf_aeron_window_analysis_high_term_id = -1;
static int hf_aeron_window_analysis_high_term_offset = -1;
static int hf_aeron_window_analysis_completed_term_id = -1;
static int hf_aeron_window_analysis_completed_term_offset = -1;
static int hf_aeron_window_analysis_outstanding_bytes = -1;

/* Expert info handles */
static expert_field ei_aeron_analysis_nak = EI_INIT;
static expert_field ei_aeron_analysis_window_full = EI_INIT;
static expert_field ei_aeron_analysis_idle_rx = EI_INIT;
static expert_field ei_aeron_analysis_pacing_rx = EI_INIT;
static expert_field ei_aeron_analysis_ooo = EI_INIT;
static expert_field ei_aeron_analysis_ooo_gap = EI_INIT;
static expert_field ei_aeron_analysis_keepalive = EI_INIT;
static expert_field ei_aeron_analysis_ooo_sm = EI_INIT;
static expert_field ei_aeron_analysis_keepalive_sm = EI_INIT;
static expert_field ei_aeron_analysis_window_resize = EI_INIT;

/*----------------------------------------------------------------------------*/
/* Setup sequence information                                                 */
/*----------------------------------------------------------------------------*/
typedef struct
{
    guint32 * stream_id;
    guint32 * term_id;
    guint32 * offset;
    guint32 len;
    guint32 data_len;
    guint32 receiver_window;
    guint16 type;
    guint8 flags;
} aeron_sequence_info_t;

static void aeron_sequence_setup(packet_info * pinfo, aeron_transport_t * transport, aeron_sequence_info_t * info)
{
    if (transport != NULL)
    {
        if (aeron_sequence_analysis || aeron_window_analysis)
        {
            if (PINFO_FD_VISITED(pinfo) == 0)
            {
                if (info->stream_id != NULL)
                {
                    aeron_stream_t * stream = NULL;

                    stream = aeron_transport_stream_find(transport, *(info->stream_id));
                    if (stream == NULL)
                    {
                        stream = aeron_transport_stream_add(transport, *(info->stream_id));
                    }
                    if (info->term_id != NULL)
                    {
                        aeron_term_t * term = NULL;

                        term = aeron_stream_term_find(stream, *(info->term_id));
                        if (term == NULL)
                        {
                            term = aeron_stream_term_add(stream, *(info->term_id));
                        }
                        if (info->offset != NULL)
                        {
                            aeron_stream_frame_analysis_t * sfa = NULL;
                            /*
                                dp is the current data position (from this frame).
                                dpv is TRUE for data frames.
                            */
                            aeron_pos_t dp;
                            gboolean dpv = FALSE;
                            /*
                                pdp is the previous (high) data position (from the stream).
                                pdpv is TRUE if pdp is valid (meaning we previously saw a data message).
                            */
                            aeron_pos_t pdp = stream->high;
                            gboolean pdpv = ((stream->flags & AERON_STREAM_FLAGS_HIGH_VALID) != 0);
                            /*
                                rp is the current receiver position (from this frame).
                                rpv is TRUE for status frames.
                            */
                            aeron_pos_t rp;
                            gboolean rpv = FALSE;
                            /*
                                prp is the previous (high) receiver completed position (from the stream).
                                prpv is TRUE if prp is valid (meaning we previously saw a status message).
                            */
                            aeron_pos_t prp = stream->completed;
                            gboolean prpv = ((stream->flags & AERON_STREAM_FLAGS_COMPLETED_VALID) != 0);
                            guint32 cur_receiver_window = stream->receiver_window;

                            switch (info->type)
                            {
                                case HDR_TYPE_DATA:
                                case HDR_TYPE_PAD:
                                    dp.term_id = *(info->term_id);
                                    dp.term_offset = aeron_pos_roundup(*(info->offset) + info->len);
                                    dpv = TRUE;
                                    if (pdpv)
                                    {
                                        if (dp.term_id > stream->high.term_id)
                                        {
                                            stream->high.term_id = dp.term_id;
                                            stream->high.term_offset = dp.term_offset;
                                        }
                                        else if (dp.term_offset > stream->high.term_offset)
                                        {
                                            stream->high.term_offset = dp.term_offset;
                                        }
                                    }
                                    else
                                    {
                                        stream->flags |= AERON_STREAM_FLAGS_HIGH_VALID;
                                        stream->high.term_id = dp.term_id;
                                        stream->high.term_offset = dp.term_offset;
                                    }
                                    break;
                                case HDR_TYPE_SM:
                                    rp.term_id = *(info->term_id);
                                    rp.term_offset = *(info->offset);
                                    rpv = TRUE;
                                    if (prpv)
                                    {
                                        if (rp.term_id > stream->completed.term_id)
                                        {
                                            stream->completed.term_id = rp.term_id;
                                            stream->completed.term_offset = rp.term_offset;
                                        }
                                        else if (rp.term_offset > stream->completed.term_offset)
                                        {
                                            stream->completed.term_offset = rp.term_offset;
                                        }
                                    }
                                    else
                                    {
                                        stream->flags |= AERON_STREAM_FLAGS_COMPLETED_VALID;
                                        stream->completed.term_id = rp.term_id;
                                        stream->completed.term_offset = rp.term_offset;
                                    }
                                    stream->receiver_window = info->receiver_window;
                                    stream->flags |= AERON_STREAM_FLAGS_RECEIVER_WINDOW_VALID;
                                    break;
                                default:
                                    break;
                            }
                            if (aeron_window_analysis)
                            {
                                if ((stream->flags & (AERON_STREAM_FLAGS_HIGH_VALID | AERON_STREAM_FLAGS_COMPLETED_VALID)) == (AERON_STREAM_FLAGS_HIGH_VALID | AERON_STREAM_FLAGS_COMPLETED_VALID))
                                {
                                    sfa = aeron_stream_frame_analysis_add(stream, pinfo->fd->num);
                                }
                            }
                            if (info->type == HDR_TYPE_DATA)
                            {
                                aeron_fragment_t * fragment = NULL;

                                fragment = aeron_term_fragment_find(term, *(info->offset));
                                if (fragment == NULL)
                                {
                                    fragment = aeron_term_fragment_add(term, *(info->offset), info->len, info->data_len);
                                }
                                fragment->is_data_frame = TRUE;
                                fragment->is_begin_msg = FALSE;
                                fragment->is_end_msg = FALSE;
                                if ((info->flags & DATA_FLAGS_BEGIN) != 0)
                                {
                                    fragment->is_begin_msg = TRUE;
                                }
                                if ((info->flags & DATA_FLAGS_END) != 0)
                                {
                                    fragment->is_end_msg = TRUE;
                                }
                                aeron_fragment_frame_add(fragment, pinfo->fd->num);
                            }
                            if (sfa != NULL)
                            {
                                switch (info->type)
                                {
                                    case HDR_TYPE_DATA:
                                    case HDR_TYPE_SM:
                                    case HDR_TYPE_PAD:
                                        sfa->high.term_id = stream->high.term_id;
                                        sfa->high.term_offset = stream->high.term_offset;
                                        sfa->completed.term_id = stream->completed.term_id;
                                        sfa->completed.term_offset = stream->completed.term_offset;
                                        sfa->receiver_window = stream->receiver_window;
                                        sfa->outstanding_bytes = aeron_pos_delta(&(sfa->high), &(sfa->completed), stream->term_length);
                                        if ((sfa->outstanding_bytes >= sfa->receiver_window) && ((stream->flags & AERON_STREAM_FLAGS_RECEIVER_WINDOW_VALID) != 0))
                                        {
                                            sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_FULL;
                                        }
                                        break;
                                    default:
                                        break;
                                }
                                switch (info->type)
                                {
                                    case HDR_TYPE_DATA:
                                    case HDR_TYPE_PAD:
                                        if (pdpv)
                                        {
                                            /* We have a previous data position. */
                                            int rc = aeron_pos_compare(&dp, &pdp);
                                            if (rc == 0)
                                            {
                                                /* Data position is the same as previous data position. */
                                                if (info->len == 0)
                                                {
                                                    sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE;
                                                }
                                                else
                                                {
                                                    if (prpv)
                                                    {
                                                        /* Previous receiver position is valid */
                                                        if (aeron_pos_compare(&dp, &prp) == 0)
                                                        {
                                                            sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_IDLE_RX;
                                                        }
                                                        else
                                                        {
                                                            sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_PACING_RX;
                                                        }
                                                    }
                                                    else
                                                    {
                                                        sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_IDLE_RX;
                                                    }
                                                }
                                            }
                                            else
                                            {
                                                aeron_pos_t expected_dp;
                                                int erc;

                                                expected_dp.term_id = pdp.term_id;
                                                expected_dp.term_offset = pdp.term_offset;
                                                aeron_pos_add_length(&expected_dp, info->len, stream->term_length);
                                                erc = aeron_pos_compare(&expected_dp, &dp);
                                                if (erc < 0)
                                                {
                                                    sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO;
                                                }
                                                else if (erc > 0)
                                                {
                                                    sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_GAP;
                                                }
                                            }
                                        }
                                        break;
                                    case HDR_TYPE_SM:
                                        if (prpv)
                                        {
                                            int rc = aeron_pos_compare(&rp, &prp);
                                            if (rc == 0)
                                            {
                                                /* Completed term ID and offset stayed the same. */
                                                if (pdpv)
                                                {
                                                    if (aeron_pos_compare(&pdp, &rp) == 0)
                                                    {
                                                        sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE_SM;
                                                    }
                                                }
                                            }
                                            else if (rc < 0)
                                            {
                                                sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_SM;
                                            }
                                            if (cur_receiver_window != sfa->receiver_window)
                                            {
                                                sfa->flags |= AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_RESIZE;
                                            }
                                        }
                                        break;
                                    default:
                                        break;
                                }
                            }
                        }
                        else
                        {
                            aeron_term_frame_add(term, pinfo->fd->num);
                        }
                    }
                    else
                    {
                        aeron_stream_frame_add(stream, pinfo->fd->num);
                    }
                }
                else
                {
                    aeron_transport_frame_add(transport, pinfo->fd->num);
                }
            }
        }
    }
}

typedef struct
{
    proto_tree * tree;
    tvbuff_t * tvb;
    guint32 current_frame;
} aeron_sequence_report_frame_callback_data_t;

static gboolean aeron_sequence_report_frame_callback(void * frame, void * user_data)
{
    aeron_sequence_report_frame_callback_data_t * cb_data = (aeron_sequence_report_frame_callback_data_t *) user_data;
    proto_item * item = NULL;
    aeron_frame_t * offset_frame = (aeron_frame_t *) frame;

    if (offset_frame->frame != cb_data->current_frame)
    {
        if (offset_frame->retransmission)
        {
            item = proto_tree_add_uint_format_value(cb_data->tree, hf_aeron_sequence_analysis_term_offset_frame, cb_data->tvb, 0, 0, offset_frame->frame, "%" G_GUINT32_FORMAT " (RX)", offset_frame->frame);
        }
        else
        {
            item = proto_tree_add_uint(cb_data->tree, hf_aeron_sequence_analysis_term_offset_frame, cb_data->tvb, 0, 0, offset_frame->frame);
        }
        PROTO_ITEM_SET_GENERATED(item);
    }
    return (FALSE);
}

static void aeron_sequence_report(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, aeron_transport_t * transport, aeron_sequence_info_t * sinfo)
{
    if (transport != NULL)
    {
        if (aeron_sequence_analysis)
        {
            proto_tree * subtree = NULL;
            proto_item * item = NULL;
            aeron_frame_t * frame = NULL;

            item = proto_tree_add_item(tree, hf_aeron_sequence_analysis, tvb, 0, 0, ENC_NA);
            PROTO_ITEM_SET_GENERATED(item);
            subtree = proto_item_add_subtree(item, ett_aeron_sequence_analysis);
            frame = aeron_transport_frame_find(transport, pinfo->fd->num);
            if (frame != NULL)
            {
                if (frame->previous_frame != 0)
                {
                    item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_channel_prev_frame, tvb, 0, 0, frame->previous_frame);
                    PROTO_ITEM_SET_GENERATED(item);
                }
                if (frame->next_frame != 0)
                {
                    item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_channel_next_frame, tvb, 0, 0, frame->next_frame);
                    PROTO_ITEM_SET_GENERATED(item);
                }
            }
            if (sinfo->stream_id != NULL)
            {
                aeron_stream_t * stream = NULL;

                stream = aeron_transport_stream_find(transport, *(sinfo->stream_id));
                if (stream != NULL)
                {
                    frame = aeron_stream_frame_find(stream, pinfo->fd->num);
                    if (frame != NULL)
                    {
                        if (frame->previous_frame != 0)
                        {
                            item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_stream_prev_frame, tvb, 0, 0, frame->previous_frame);
                            PROTO_ITEM_SET_GENERATED(item);
                        }
                        if (frame->next_frame != 0)
                        {
                            item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_stream_next_frame, tvb, 0, 0, frame->next_frame);
                            PROTO_ITEM_SET_GENERATED(item);
                        }
                    }
                    if (sinfo->term_id != NULL)
                    {
                        aeron_term_t * term = NULL;

                        term = aeron_stream_term_find(stream, *(sinfo->term_id));
                        if (term != NULL)
                        {
                            frame = aeron_term_frame_find(term, pinfo->fd->num);
                            if (frame != NULL)
                            {
                                if (frame->previous_frame != 0)
                                {
                                    item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_term_prev_frame, tvb, 0, 0, frame->previous_frame);
                                    PROTO_ITEM_SET_GENERATED(item);
                                }
                                if (frame->next_frame != 0)
                                {
                                    item = proto_tree_add_uint(subtree, hf_aeron_sequence_analysis_term_next_frame, tvb, 0, 0, frame->next_frame);
                                    PROTO_ITEM_SET_GENERATED(item);
                                }
                            }
                            if (sinfo->offset != NULL)
                            {
                                aeron_fragment_t * fragment = NULL;

                                fragment = aeron_term_fragment_find(term, *(sinfo->offset));
                                if (fragment != NULL)
                                {
                                    if (fragment->frame_count > 1)
                                    {
                                        proto_tree * frame_tree = NULL;
                                        proto_item * frame_item = NULL;
                                        aeron_sequence_report_frame_callback_data_t cb_data;

                                        frame_item = proto_tree_add_item(subtree, hf_aeron_sequence_analysis_term_offset, tvb, 0, 0, ENC_NA);
                                        PROTO_ITEM_SET_GENERATED(frame_item);
                                        frame_tree = proto_item_add_subtree(frame_item, ett_aeron_sequence_analysis_term_offset);
                                        cb_data.tree = frame_tree;
                                        cb_data.tvb = tvb;
                                        cb_data.current_frame = pinfo->fd->num;
                                        wmem_tree_foreach(fragment->frame, aeron_sequence_report_frame_callback, (void *) &cb_data);
                                    }
                                    frame = aeron_fragment_frame_find(fragment, pinfo->fd->num);
                                    if (frame != NULL)
                                    {
                                        proto_item * rx_item = NULL;
                                        rx_item = proto_tree_add_boolean(subtree, hf_aeron_sequence_analysis_retransmission, tvb, 0, 0, frame->retransmission);
                                        PROTO_ITEM_SET_GENERATED(rx_item);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void aeron_window_report(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, aeron_transport_t * transport, guint32 stream_id)
{
    if (transport != NULL)
    {
        if (aeron_window_analysis)
        {
            aeron_stream_t * stream = aeron_transport_stream_find(transport, stream_id);
            if (stream != NULL)
            {
                aeron_stream_frame_analysis_t * sfa = aeron_stream_frame_analysis_find(stream, pinfo->fd->num);
                if (sfa != NULL)
                {
                    proto_tree * subtree = NULL;
                    proto_item * item = NULL;

                    item = proto_tree_add_item(tree, hf_aeron_window_analysis, tvb, 0, 0, ENC_NA);
                    PROTO_ITEM_SET_GENERATED(item);
                    subtree = proto_item_add_subtree(item, ett_aeron_window_analysis);
                    item = proto_tree_add_uint(subtree, hf_aeron_window_analysis_high_term_id, tvb, 0, 0, sfa->high.term_id);
                    PROTO_ITEM_SET_GENERATED(item);
                    item = proto_tree_add_uint(subtree, hf_aeron_window_analysis_high_term_offset, tvb, 0, 0, sfa->high.term_offset);
                    PROTO_ITEM_SET_GENERATED(item);
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_IDLE_RX) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_idle_rx);
                    }
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_PACING_RX) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_pacing_rx);
                    }
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_ooo);
                    }
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_GAP) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_ooo_gap);
                    }
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_keepalive);
                    }
                    item = proto_tree_add_uint(subtree, hf_aeron_window_analysis_completed_term_id, tvb, 0, 0, sfa->completed.term_id);
                    PROTO_ITEM_SET_GENERATED(item);
                    item = proto_tree_add_uint(subtree, hf_aeron_window_analysis_completed_term_offset, tvb, 0, 0, sfa->completed.term_offset);
                    PROTO_ITEM_SET_GENERATED(item);
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_OOO_SM) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_ooo_sm);
                    }
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_KEEPALIVE_SM) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_keepalive_sm);
                    }
                    item = proto_tree_add_uint(subtree, hf_aeron_window_analysis_outstanding_bytes, tvb, 0, 0, sfa->outstanding_bytes);
                    PROTO_ITEM_SET_GENERATED(item);
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_FULL) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_window_full);
                    }
                }
            }
        }
    }
}

static void aeron_next_offset_report(tvbuff_t * tvb, proto_tree * tree, aeron_transport_t * transport, guint32 stream_id, guint32 term_id, guint32 term_offset, guint32 length)
{
    aeron_stream_t * stream = NULL;
    proto_item * item = NULL;

    stream = aeron_transport_stream_find(transport, stream_id);
    if (stream != NULL)
    {
        aeron_term_t * term = NULL;
        if (stream->term_length == 0)
        {
            stream->term_length = length;
        }
        term = aeron_stream_term_find(stream, term_id);
        if (term != NULL)
        {
            aeron_fragment_t * fragment = aeron_term_fragment_find(term, term_offset);
            if (fragment != NULL)
            {
                guint32 next_offset = term_offset + length;
                guint32 next_offset_term_id = term_id;
                guint32 next_offset_first_frame = 0;
                aeron_fragment_t * next_offset_fragment = NULL;
                aeron_term_t * next_offset_term = NULL;

                if (next_offset >= stream->term_length)
                {
                    next_offset = 0;
                    next_offset_term_id++;
                }
                item = proto_tree_add_uint(tree, hf_aeron_data_next_offset, tvb, 0, 0, next_offset);
                PROTO_ITEM_SET_GENERATED(item);
                if (next_offset_term_id != term_id)
                {
                    next_offset_term = aeron_stream_term_find(stream, next_offset_term_id);
                    item = proto_tree_add_uint(tree, hf_aeron_data_next_offset_term, tvb, 0, 0, next_offset_term_id);
                    PROTO_ITEM_SET_GENERATED(item);
                }
                else
                {
                    next_offset_term = term;
                }
                if (next_offset_term != NULL)
                {
                    next_offset_fragment = aeron_term_fragment_find(next_offset_term, next_offset);
                    if (next_offset_fragment != NULL)
                    {
                        if (next_offset_fragment->first_frame != NULL)
                        {
                            next_offset_first_frame = next_offset_fragment->first_frame->frame;
                            item = proto_tree_add_uint(tree, hf_aeron_data_next_offset_first_frame, tvb, 0, 0, next_offset_first_frame);
                            PROTO_ITEM_SET_GENERATED(item);
                        }
                    }
                }
            }
        }
    }
}

static void aeron_info_stream_progress_report(packet_info * pinfo, const char * msgtype, aeron_transport_t * transport, guint32 stream_id)
{
    aeron_stream_t * stream = NULL;
    aeron_stream_frame_analysis_t * sfa = NULL;

    if (aeron_window_analysis)
    {
        stream = aeron_transport_stream_find(transport, stream_id);
        if (stream != NULL)
        {
            sfa = aeron_stream_frame_analysis_find(stream, pinfo->fd->num);
        }
    }
    if (sfa != NULL)
    {
        if (sfa->high.term_id == sfa->completed.term_id)
        {
            col_append_sep_fstr(pinfo->cinfo, COL_INFO, " ", "%s (%" G_GUINT32_FORMAT "/%" G_GUINT32_FORMAT " [%" G_GUINT32_FORMAT "])",
                msgtype, sfa->high.term_offset, sfa->completed.term_offset, sfa->outstanding_bytes);
        }
        else
        {
            col_append_sep_fstr(pinfo->cinfo, COL_INFO, " ", "%s (0x%08x:%" G_GUINT32_FORMAT "/0x%08x:%" G_GUINT32_FORMAT " [%" G_GUINT32_FORMAT "])",
                msgtype, sfa->high.term_id, sfa->high.term_offset, sfa->completed.term_id, sfa->completed.term_offset, sfa->outstanding_bytes);
        }
    }
    else
    {
        col_append_sep_str(pinfo->cinfo, COL_INFO, " ", msgtype);
    }
}

/*----------------------------------------------------------------------------*/
/* Aeron pad message packet dissection functions.                             */
/*----------------------------------------------------------------------------*/
static int dissect_aeron_pad(tvbuff_t * tvb, int offset, packet_info * pinfo, proto_tree * tree, aeron_conversation_info_t * cinfo)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    proto_item * channel_item = NULL;
    guint32 frame_len;
    guint32 pad_len;
    aeron_transport_t * transport;
    guint32 session_id;
    guint32 stream_id;
    guint32 term_id;
    guint32 term_offset;
    int rounded_len = 0;
    aeron_sequence_info_t sinfo;

    frame_len = tvb_get_letohl(tvb, offset + O_AERON_PAD_FRAME_LENGTH);
    rounded_len = (int) aeron_pos_roundup(frame_len);
    term_offset = tvb_get_letohl(tvb, offset + O_AERON_PAD_TERM_OFFSET);
    session_id = tvb_get_letohl(tvb, offset + O_AERON_PAD_SESSION_ID);
    transport = aeron_transport_add(cinfo, session_id, pinfo->fd->num);
    stream_id = tvb_get_letohl(tvb, offset + O_AERON_PAD_STREAM_ID);
    term_id = tvb_get_letohl(tvb, offset + O_AERON_PAD_TERM_ID);
    pad_len = frame_len - L_AERON_PAD;
    sinfo.stream_id = &stream_id;
    sinfo.term_id = &term_id;
    sinfo.offset = &term_offset;
    sinfo.len = frame_len;
    sinfo.data_len = pad_len;
    sinfo.receiver_window = 0;
    sinfo.type = HDR_TYPE_PAD;
    sinfo.flags = 0;
    aeron_sequence_setup(pinfo, transport, &sinfo);

    aeron_info_stream_progress_report(pinfo, "Pad", transport, stream_id);
    item = proto_tree_add_none_format(tree, hf_aeron_pad, tvb, offset, -1, "Pad Frame: Term 0x%x, Ofs %" G_GUINT32_FORMAT ", Len %" G_GUINT32_FORMAT "(%d)",
        term_id, term_offset, frame_len, rounded_len);
    subtree = proto_item_add_subtree(item, ett_aeron_pad);
    channel_item = proto_tree_add_uint64(subtree, hf_aeron_channel, tvb, 0, 0, transport->channel);
    PROTO_ITEM_SET_GENERATED(channel_item);
    proto_tree_add_item(subtree, hf_aeron_pad_version, tvb, offset + O_AERON_PAD_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_flags, tvb, offset + O_AERON_PAD_FLAGS, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_type, tvb, offset + O_AERON_PAD_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_frame_length, tvb, offset + O_AERON_PAD_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_term_offset, tvb, offset + O_AERON_PAD_TERM_OFFSET, 4, ENC_LITTLE_ENDIAN);
    aeron_next_offset_report(tvb, subtree, transport, stream_id, term_id, term_offset, (guint32) rounded_len);
    proto_tree_add_item(subtree, hf_aeron_pad_session_id, tvb, offset + O_AERON_PAD_SESSION_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_stream_id, tvb, offset + O_AERON_PAD_STREAM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_pad_term_id, tvb, offset + O_AERON_PAD_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    aeron_sequence_report(tvb, pinfo, subtree, transport, &sinfo);
    aeron_window_report(tvb, pinfo, subtree, transport, stream_id);
    proto_item_set_len(item, L_AERON_PAD);
    return (L_AERON_PAD);
}

/*----------------------------------------------------------------------------*/
/* Aeron data message packet dissection functions.                            */
/*----------------------------------------------------------------------------*/
static int dissect_aeron_data(tvbuff_t * tvb, int offset, packet_info * pinfo, proto_tree * tree, aeron_conversation_info_t * cinfo)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    guint32 frame_len;
    static const int * flags[] =
    {
        &hf_aeron_data_flags_b,
        &hf_aeron_data_flags_e,
        NULL
    };
    aeron_transport_t * transport;
    guint32 session_id;
    guint32 stream_id;
    guint32 term_id;
    guint32 term_offset;
    guint32 data_len;
    int rounded_len = 0;
    aeron_sequence_info_t sinfo;
    guint32 offset_increment = 0;

    frame_len = tvb_get_letohl(tvb, offset + O_AERON_DATA_FRAME_LENGTH);
    if (frame_len == 0)
    {
        rounded_len = O_AERON_DATA_DATA;
        data_len = 0;
        offset_increment = 0;
    }
    else
    {
        offset_increment = aeron_pos_roundup(frame_len);
        rounded_len = (int) offset_increment;
        data_len = frame_len - O_AERON_DATA_DATA;
    }
    term_offset = tvb_get_letohl(tvb, offset + O_AERON_DATA_TERM_OFFSET);
    session_id = tvb_get_letohl(tvb, offset + O_AERON_DATA_SESSION_ID);
    transport = aeron_transport_add(cinfo, session_id, pinfo->fd->num);
    stream_id = tvb_get_letohl(tvb, offset + O_AERON_DATA_STREAM_ID);
    term_id = tvb_get_letohl(tvb, offset + O_AERON_DATA_TERM_ID);
    sinfo.stream_id = &stream_id;
    sinfo.term_id = &term_id;
    sinfo.offset = &term_offset;
    sinfo.len = frame_len;
    sinfo.data_len = data_len;
    sinfo.receiver_window = 0;
    sinfo.type = HDR_TYPE_DATA;
    sinfo.flags = tvb_get_guint8(tvb, offset + O_AERON_DATA_FLAGS);
    aeron_sequence_setup(pinfo, transport, &sinfo);

    aeron_info_stream_progress_report(pinfo, "Data", transport, stream_id);
    item = proto_tree_add_none_format(tree, hf_aeron_data, tvb, offset, -1, "Data Frame: Term 0x%x, Ofs %" G_GUINT32_FORMAT ", Len %" G_GUINT32_FORMAT "(%d)",
        (guint32) term_id, term_offset, frame_len, rounded_len);
    subtree = proto_item_add_subtree(item, ett_aeron_data);
    item = proto_tree_add_uint64(subtree, hf_aeron_channel, tvb, 0, 0, transport->channel);
    PROTO_ITEM_SET_GENERATED(item);
    proto_tree_add_item(subtree, hf_aeron_data_version, tvb, offset + O_AERON_DATA_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_bitmask(subtree, tvb, offset + O_AERON_DATA_FLAGS, hf_aeron_data_flags, ett_aeron_data_flags, flags, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_data_type, tvb, offset + O_AERON_DATA_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_data_frame_length, tvb, offset + O_AERON_DATA_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_data_term_offset, tvb, offset + O_AERON_DATA_TERM_OFFSET, 4, ENC_LITTLE_ENDIAN);
    aeron_next_offset_report(tvb, subtree, transport, stream_id, term_id, term_offset, offset_increment);
    proto_tree_add_item(subtree, hf_aeron_data_session_id, tvb, offset + O_AERON_DATA_SESSION_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_data_stream_id, tvb, offset + O_AERON_DATA_STREAM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_data_term_id, tvb, offset + O_AERON_DATA_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    if (data_len > 0)
    {
        proto_tree_add_item(subtree, hf_aeron_data_data, tvb, offset + O_AERON_DATA_DATA, data_len, ENC_NA);
    }
    aeron_sequence_report(tvb, pinfo, subtree, transport, &sinfo);
    aeron_window_report(tvb, pinfo, subtree, transport, stream_id);
    proto_item_set_len(item, rounded_len);
#if AERON_REASSEMBLY
    if (aeron_reassemble_fragments)
    {
        guint8 flags_val = tvb_get_guint8(tvb, offset + O_AERON_DATA_FLAGS);
        if ((flags_val & DATA_FLAGS_COMPLETE) != DATA_FLAGS_COMPLETE)
        {
            /* Either begin, end, or nothing (somewhere in the middle of the message) */
            aeron_stream_t * stream = aeron_transport_stream_find(transport, stream_id);
            if (stream != NULL)
            {
                aeron_term_t * term = aeron_stream_term_find(stream, term_id);
                if (term != NULL)
                {
                    aeron_msg_t * msg = NULL;

                    if ((flags_val & DATA_FLAGS_BEGIN) == DATA_FLAGS_BEGIN)
                    {
                        /* Beginning of a message. */
                        msg = aeron_msg_add()
                    }
                    else if ((flags_val & DATA_FLAGS_END) == DATA_FLAGS_END)
                    {
                        /* End of a message. */
                    }
                    else
                    {
                        /* Somwhere in the middle of a message */
                    }
                }
            }
        }
    }
#endif
    return (rounded_len);
}

/*----------------------------------------------------------------------------*/
/* Aeron NAK packet dissection functions.                                     */
/*----------------------------------------------------------------------------*/
static int dissect_aeron_nak(tvbuff_t * tvb, int offset, packet_info * pinfo, proto_tree * tree, aeron_conversation_info_t * cinfo)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    proto_item * channel_item = NULL;
    proto_item * nak_item = NULL;
    guint32 frame_len;
    aeron_transport_t * transport;
    guint32 session_id;
    guint32 stream_id;
    guint32 term_id;
    guint32 nak_term_offset;
    guint32 nak_length;
    int rounded_len = 0;
    aeron_sequence_info_t sinfo;

    frame_len = tvb_get_letohl(tvb, offset + O_AERON_NAK_FRAME_LENGTH);
    rounded_len = (int) aeron_pos_roundup(frame_len);
    session_id = tvb_get_letohl(tvb, offset + O_AERON_NAK_SESSION_ID);
    transport = aeron_transport_add(cinfo, session_id, pinfo->fd->num);
    stream_id = tvb_get_letohl(tvb, offset + O_AERON_NAK_STREAM_ID);
    term_id = tvb_get_letohl(tvb, offset + O_AERON_NAK_TERM_ID);
    nak_term_offset = tvb_get_letohl(tvb, offset + O_AERON_NAK_TERM_OFFSET);
    nak_length = tvb_get_letohl(tvb, offset + O_AERON_NAK_LENGTH);
    sinfo.stream_id = &stream_id;
    sinfo.term_id = &term_id;
    sinfo.offset = NULL;
    sinfo.len = 0;
    sinfo.data_len = 0;
    sinfo.receiver_window = 0;
    sinfo.type = HDR_TYPE_NAK;
    sinfo.flags = 0;
    aeron_sequence_setup(pinfo, transport, &sinfo);

    col_append_sep_str(pinfo->cinfo, COL_INFO, " ", "NAK");
    item = proto_tree_add_none_format(tree, hf_aeron_nak, tvb, offset, -1, "NAK Frame: Term 0x%x, Ofs %" G_GUINT32_FORMAT ", Len %" G_GUINT32_FORMAT,
        term_id, nak_term_offset, nak_length);
    subtree = proto_item_add_subtree(item, ett_aeron_nak);
    channel_item = proto_tree_add_uint64(subtree, hf_aeron_channel, tvb, 0, 0, transport->channel);
    PROTO_ITEM_SET_GENERATED(channel_item);
    proto_tree_add_item(subtree, hf_aeron_nak_version, tvb, offset + O_AERON_NAK_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_flags, tvb, offset + O_AERON_NAK_FLAGS, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_type, tvb, offset + O_AERON_NAK_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_frame_length, tvb, offset + O_AERON_NAK_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_session_id, tvb, offset + O_AERON_NAK_SESSION_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_stream_id, tvb, offset + O_AERON_NAK_STREAM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_term_id, tvb, offset + O_AERON_NAK_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    nak_item = proto_tree_add_item(subtree, hf_aeron_nak_term_offset, tvb, offset + O_AERON_NAK_TERM_OFFSET, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_nak_length, tvb, offset + O_AERON_NAK_LENGTH, 4, ENC_LITTLE_ENDIAN);
    expert_add_info_format(pinfo, nak_item, &ei_aeron_analysis_nak, "NAK offset %" G_GUINT32_FORMAT " length %" G_GUINT32_FORMAT, nak_term_offset, nak_length);
    aeron_sequence_report(tvb, pinfo, subtree, transport, &sinfo);
    proto_item_set_len(item, rounded_len);
    return (rounded_len);
}

static void aeron_window_resize_report(packet_info * pinfo, proto_item * item, aeron_transport_t * transport, guint32 stream_id)
{
    if (transport != NULL)
    {
        if (aeron_window_analysis)
        {
            aeron_stream_t * stream = aeron_transport_stream_find(transport, stream_id);
            if (stream != NULL)
            {
                aeron_stream_frame_analysis_t * sfa = aeron_stream_frame_analysis_find(stream, pinfo->fd->num);
                if (sfa != NULL)
                {
                    if ((sfa->flags & AERON_STREAM_FRAME_ANALYSIS_FLAG_WINDOW_RESIZE) != 0)
                    {
                        expert_add_info(pinfo, item, &ei_aeron_analysis_window_resize);
                    }
                }
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
/* Aeron status message packet dissection functions.                          */
/*----------------------------------------------------------------------------*/
static int dissect_aeron_sm(tvbuff_t * tvb, int offset, packet_info * pinfo, proto_tree * tree, aeron_conversation_info_t * cinfo)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    proto_item * channel_item = NULL;
    guint32 frame_len;
    static const int * flags[] =
    {
        &hf_aeron_sm_flags_s,
        NULL
    };
    guint32 feedback_len;
    aeron_transport_t * transport;
    guint32 session_id;
    guint32 stream_id;
    guint32 term_id;
    guint32 comp_offset;
    guint32 rcv_window;
    int rounded_len = 0;
    aeron_sequence_info_t sinfo;

    frame_len = tvb_get_letohl(tvb, offset + O_AERON_SM_FRAME_LENGTH);
    feedback_len = frame_len - O_AERON_SM_FEEDBACK;
    rounded_len = (int) aeron_pos_roundup(frame_len);
    session_id = tvb_get_letohl(tvb, offset + O_AERON_SM_SESSION_ID);
    transport = aeron_transport_add(cinfo, session_id, pinfo->fd->num);
    stream_id = tvb_get_letohl(tvb, offset + O_AERON_SM_STREAM_ID);
    term_id = tvb_get_letohl(tvb, offset + O_AERON_SM_TERM_ID);
    comp_offset = tvb_get_letohl(tvb, offset + O_AERON_SM_COMPLETED_TERM_OFFSET);
    rcv_window = tvb_get_letohl(tvb, offset + O_AERON_SM_RECEIVER_WINDOW);
    sinfo.stream_id = &stream_id;
    sinfo.term_id = &term_id;
    sinfo.offset = &comp_offset;
    sinfo.len = 0;
    sinfo.data_len = 0;
    sinfo.receiver_window = rcv_window;
    sinfo.type = HDR_TYPE_SM;
    sinfo.flags = 0;
    aeron_sequence_setup(pinfo, transport, &sinfo);

    aeron_info_stream_progress_report(pinfo, "Status", transport, stream_id);
    item = proto_tree_add_none_format(tree, hf_aeron_sm, tvb, offset, -1, "Status Message: Term 0x%x, CompletedOfs %" G_GUINT32_FORMAT ", RcvWindow %" G_GUINT32_FORMAT,
        term_id, comp_offset, rcv_window);
    subtree = proto_item_add_subtree(item, ett_aeron_sm);
    channel_item = proto_tree_add_uint64(subtree, hf_aeron_channel, tvb, 0, 0, transport->channel);
    PROTO_ITEM_SET_GENERATED(channel_item);
    proto_tree_add_item(subtree, hf_aeron_sm_version, tvb, offset + O_AERON_SM_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_bitmask(subtree, tvb, offset + O_AERON_SM_FLAGS, hf_aeron_sm_flags, ett_aeron_sm_flags, flags, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_type, tvb, offset + O_AERON_SM_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_frame_length, tvb, offset + O_AERON_SM_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_session_id, tvb, offset + O_AERON_SM_SESSION_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_stream_id, tvb, offset + O_AERON_SM_STREAM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_term_id, tvb, offset + O_AERON_SM_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_sm_completed_term_offset, tvb, offset + O_AERON_SM_COMPLETED_TERM_OFFSET, 4, ENC_LITTLE_ENDIAN);
    item = proto_tree_add_item(subtree, hf_aeron_sm_receiver_window, tvb, offset + O_AERON_SM_RECEIVER_WINDOW, 4, ENC_LITTLE_ENDIAN);
    aeron_window_resize_report(pinfo, item, transport, stream_id);
    if (feedback_len > 0)
    {
        proto_tree_add_item(subtree, hf_aeron_sm_feedback, tvb, offset + O_AERON_SM_FEEDBACK, feedback_len, ENC_NA);
    }
    aeron_sequence_report(tvb, pinfo, subtree, transport, &sinfo);
    aeron_window_report(tvb, pinfo, subtree, transport, stream_id);
    proto_item_set_len(item, rounded_len);
    return (rounded_len);
}

/*----------------------------------------------------------------------------*/
/* Aeron error packet dissection functions.                                   */
/*----------------------------------------------------------------------------*/
static int dissect_aeron_err(tvbuff_t * tvb, int offset, packet_info * pinfo _U_, proto_tree * tree)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    guint32 len;
    guint32 bad_frame_len;
    gint string_len = 0;
    int ofs;

    col_append_sep_str(pinfo->cinfo, COL_INFO, " ", "Error");
    item = proto_tree_add_item(tree, hf_aeron_err, tvb, offset, -1, ENC_NA);
    subtree = proto_item_add_subtree(item, ett_aeron_err);
    proto_tree_add_item(subtree, hf_aeron_err_version, tvb, offset + O_AERON_ERR_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_err_code, tvb, offset + O_AERON_ERR_CODE, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_err_type, tvb, offset + O_AERON_ERR_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_err_frame_length, tvb, offset + O_AERON_ERR_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    len = tvb_get_letohl(tvb, offset + O_AERON_ERR_FRAME_LENGTH);
    proto_tree_add_item(subtree, hf_aeron_err_off_frame_length, tvb, offset + O_AERON_ERR_OFFENDING_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    bad_frame_len = tvb_get_letohl(tvb, offset + O_AERON_ERR_OFFENDING_FRAME_LENGTH);
    ofs = offset + O_AERON_ERR_OFFENDING_HEADER;
    proto_tree_add_item(subtree, hf_aeron_err_off_hdr, tvb, offset + ofs, bad_frame_len, ENC_LITTLE_ENDIAN);
    ofs += bad_frame_len;
    string_len = len - ofs;
    if (string_len > 0)
    {
        proto_tree_add_item(subtree, hf_aeron_err_string, tvb, offset + ofs, string_len, ENC_NA);
    }
    len = aeron_pos_roundup(len);
    proto_item_set_len(item, (int) len);
    return ((int) len);
}

/*----------------------------------------------------------------------------*/
/* Aeron setup packet dissection functions.                                   */
/*----------------------------------------------------------------------------*/
static void aeron_set_stream_mtu_term_length(packet_info * pinfo, aeron_transport_t * transport, guint32 stream_id, guint32 mtu, guint32 term_length)
{
    if (PINFO_FD_VISITED(pinfo) == 0)
    {
        aeron_stream_t * stream = aeron_transport_stream_find(transport, stream_id);
        if (stream != NULL)
        {
            stream->term_length = term_length;
            stream->mtu = mtu;
            stream->fragment_stride = mtu - L_AERON_DATA;
        }
    }
}

static int dissect_aeron_setup(tvbuff_t * tvb, int offset, packet_info * pinfo, proto_tree * tree, aeron_conversation_info_t * cinfo)
{
    proto_tree * subtree = NULL;
    proto_item * item = NULL;
    guint32 frame_len;
    proto_item * channel_item = NULL;
    aeron_transport_t * transport;
    guint32 session_id;
    guint32 stream_id;
    guint32 active_term_id;
    guint32 initial_term_id;
    guint32 term_offset;
    guint32 term_length;
    guint32 mtu;
    int rounded_len;
    aeron_sequence_info_t sinfo;

    frame_len = tvb_get_letohl(tvb, offset + O_AERON_SETUP_FRAME_LENGTH);
    rounded_len = (int) aeron_pos_roundup(frame_len);
    term_offset = tvb_get_letohl(tvb, offset + O_AERON_SETUP_TERM_OFFSET);
    session_id = tvb_get_letohl(tvb, offset + O_AERON_SETUP_SESSION_ID);
    transport = aeron_transport_add(cinfo, session_id, pinfo->fd->num);
    stream_id = tvb_get_letohl(tvb, offset + O_AERON_SETUP_STREAM_ID);
    initial_term_id = tvb_get_letohl(tvb, offset + O_AERON_SETUP_INITIAL_TERM_ID);
    active_term_id = tvb_get_letohl(tvb, offset + O_AERON_SETUP_ACTIVE_TERM_ID);
    sinfo.stream_id = &stream_id;
    sinfo.term_id = &active_term_id;
    sinfo.offset = NULL;
    sinfo.len = 0;
    sinfo.data_len = 0;
    sinfo.receiver_window = 0;
    sinfo.type = HDR_TYPE_SETUP;
    sinfo.flags = 0;
    aeron_sequence_setup(pinfo, transport, &sinfo);
    term_length = tvb_get_letohl(tvb, offset + O_AERON_SETUP_TERM_LENGTH);
    mtu = tvb_get_letohl(tvb, offset + O_AERON_SETUP_MTU);
    aeron_set_stream_mtu_term_length(pinfo, transport, stream_id, mtu, term_length);

    col_append_sep_str(pinfo->cinfo, COL_INFO, " ", "Setup");
    item = proto_tree_add_none_format(tree, hf_aeron_setup, tvb, offset, -1, "Setup Frame: InitTerm 0x%x, ActiveTerm 0x%x, TermLen %" G_GUINT32_FORMAT ", Ofs %" G_GUINT32_FORMAT ", MTU %" G_GUINT32_FORMAT,
        initial_term_id, (guint32) active_term_id, term_length, term_offset, mtu);
    subtree = proto_item_add_subtree(item, ett_aeron_setup);
    channel_item = proto_tree_add_uint64(subtree, hf_aeron_channel, tvb, 0, 0, transport->channel);
    PROTO_ITEM_SET_GENERATED(channel_item);
    proto_tree_add_item(subtree, hf_aeron_setup_version, tvb, offset + O_AERON_SETUP_VERSION, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_flags, tvb, offset + O_AERON_SETUP_FLAGS, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_type, tvb, offset + O_AERON_SETUP_TYPE, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_frame_length, tvb, offset + O_AERON_SETUP_FRAME_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_term_offset, tvb, offset + O_AERON_SETUP_TERM_OFFSET, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_session_id, tvb, offset + O_AERON_SETUP_SESSION_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_stream_id, tvb, offset + O_AERON_SETUP_STREAM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_initial_term_id, tvb, offset + O_AERON_SETUP_INITIAL_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_active_term_id, tvb, offset + O_AERON_SETUP_ACTIVE_TERM_ID, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_term_length, tvb, offset + O_AERON_SETUP_TERM_LENGTH, 4, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(subtree, hf_aeron_setup_mtu, tvb, offset + O_AERON_SETUP_MTU, 4, ENC_LITTLE_ENDIAN);
    aeron_sequence_report(tvb, pinfo, subtree, transport, &sinfo);
    proto_item_set_len(item, rounded_len);
    return (rounded_len);
}

/*----------------------------------------------------------------------------*/
/* Aeron packet dissector.                                                    */
/*----------------------------------------------------------------------------*/
static int dissect_aeron(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, void * user_data _U_)
{
    int total_dissected_len = 0;
    guint16 frame_type;
    proto_tree * aeron_tree = NULL;
    proto_item * aeron_item;
    int dissected_len = 0;
    int offset = 0;
    int len_remaining = 0;
    aeron_conversation_info_t * cinfo = NULL;

    /* Get enough information to determine the conversation info */
    frame_type = tvb_get_letohs(tvb, offset + 2);
    cinfo = aeron_setup_conversation_info(pinfo, frame_type);
    if (cinfo == NULL)
    {
        return (-1);
    }
    col_add_str(pinfo->cinfo, COL_PROTOCOL, "Aeron");
    col_clear(pinfo->cinfo, COL_INFO);
    col_add_str(pinfo->cinfo, COL_INFO, aeron_format_transport_uri(cinfo));
    col_set_fence(pinfo->cinfo, COL_INFO);

    len_remaining = tvb_reported_length(tvb);
    aeron_item = proto_tree_add_protocol_format(tree, proto_aeron, tvb, offset, -1, "Aeron Protocol");
    aeron_tree = proto_item_add_subtree(aeron_item, ett_aeron);
    while (len_remaining > 0)
    {
        frame_type = tvb_get_letohs(tvb, offset + 2);
        cinfo = aeron_setup_conversation_info(pinfo, frame_type);
        switch (frame_type)
        {
            case HDR_TYPE_PAD:
                dissected_len = dissect_aeron_pad(tvb, offset, pinfo, aeron_tree, cinfo);
                break;
            case HDR_TYPE_DATA:
                dissected_len = dissect_aeron_data(tvb, offset, pinfo, aeron_tree, cinfo);
                break;
            case HDR_TYPE_NAK:
                dissected_len = dissect_aeron_nak(tvb, offset, pinfo, aeron_tree, cinfo);
                break;
            case HDR_TYPE_SM:
                dissected_len = dissect_aeron_sm(tvb, offset, pinfo, aeron_tree, cinfo);
                break;
            case HDR_TYPE_ERR:
                dissected_len = dissect_aeron_err(tvb, offset, pinfo, aeron_tree);
                break;
            case HDR_TYPE_SETUP:
                dissected_len = dissect_aeron_setup(tvb, offset, pinfo, aeron_tree, cinfo);
                break;
            case HDR_TYPE_EXT:
            default:
                return (total_dissected_len);
        }
        total_dissected_len += dissected_len;
        offset += dissected_len;
        len_remaining -= dissected_len;
        proto_item_set_len(aeron_item, dissected_len);
    }
    return (total_dissected_len);
}

static gboolean test_aeron_packet(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, void * user_data)
{
    guint8 ver = 0;
    guint16 packet_type = 0;
    gint len;
    gint len_remaining;
    int rc;

    len_remaining = tvb_reported_length_remaining(tvb, 0);
    if (len_remaining < HDR_LENGTH_MIN)
    {
        return (FALSE);
    }
    ver = tvb_get_guint8(tvb, 0);
    if (ver != 0)
    {
        return (FALSE);
    }
    packet_type = tvb_get_letohs(tvb, 2);
    switch (packet_type)
    {
        case HDR_TYPE_PAD:
        case HDR_TYPE_DATA:
        case HDR_TYPE_NAK:
        case HDR_TYPE_SM:
        case HDR_TYPE_ERR:
        case HDR_TYPE_SETUP:
        case HDR_TYPE_EXT:
            break;
        default:
            return (FALSE);
    }
    len = (gint) (tvb_get_letohl(tvb, 4) & 0x7fffffff);
    if (!((packet_type == HDR_TYPE_DATA) && (len == 0)))
    {
        if (len < HDR_LENGTH_MIN)
        {
            return (FALSE);
        }
    }
    if (packet_type == HDR_TYPE_PAD)
    {
        /* Pad frames can't have a zero term offset */
        guint32 term_offset = tvb_get_letohl(tvb, O_AERON_PAD_TERM_OFFSET);
        if (term_offset == 0)
        {
            return (FALSE);
        }
    }
    else
    {
        if (len > len_remaining)
        {
            return (FALSE);
        }
    }
    rc = dissect_aeron(tvb, pinfo, tree, user_data);
    if (rc == -1)
    {
        return (FALSE);
    }
    return (TRUE);
}

static void aeron_init(void)
{
    aeron_channel = 0;
}

/* Register all the bits needed with the filtering engine */
void proto_register_aeron(void)
{
    static hf_register_info hf[] =
    {
        { &hf_aeron_channel,
            { "Channel", "aeron.channel", FT_UINT64, BASE_DEC, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad,
            { "Pad Frame", "aeron.pad", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_version,
            { "Version", "aeron.pad.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_flags,
            { "Flags", "aeron.pad.flags", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_type,
            { "Type", "aeron.pad.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_pad_frame_length,
            { "Frame Length", "aeron.pad.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_term_offset,
            { "Term Offset", "aeron.pad.term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_session_id,
            { "Session ID", "aeron.pad.session_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_stream_id,
            { "Stream ID", "aeron.pad.stream_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_pad_term_id,
            { "Term ID", "aeron.pad.term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data,
            { "Data Frame", "aeron.data", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_version,
            { "Version", "aeron.data.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_flags,
            { "Flags", "aeron.data.flags", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_flags_b,
            { "Begin Message", "aeron.data.flags.b", FT_BOOLEAN, 8, TFS(&tfs_set_notset), DATA_FLAGS_BEGIN, NULL, HFILL } },
        { &hf_aeron_data_flags_e,
            { "End Message", "aeron.data.flags.e", FT_BOOLEAN, 8, TFS(&tfs_set_notset), DATA_FLAGS_END, NULL, HFILL } },
        { &hf_aeron_data_type,
            { "Type", "aeron.data.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_data_frame_length,
            { "Frame Length", "aeron.data.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_term_offset,
            { "Term Offset", "aeron.data.term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_next_offset,
            { "Next Offset", "aeron.data.next_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_next_offset_term,
            { "Next Offset Term", "aeron.data.next_offset_term", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_next_offset_first_frame,
            { "Next Offset First Frame", "aeron.data.next_offset_first_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_session_id,
            { "Session ID", "aeron.data.session_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_stream_id,
            { "Stream ID", "aeron.data.stream_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_term_id,
            { "Term ID", "aeron.data.term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_data_data,
            { "Data", "aeron.data.data", FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak,
            { "NAK Frame", "aeron.nak", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_version,
            { "Version", "aeron.nak.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_flags,
            { "Flags", "aeron.nak.flags", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_type,
            { "Type", "aeron.nak.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_nak_frame_length,
            { "Frame Length", "aeron.nak.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_session_id,
            { "Session ID", "aeron.nak.session_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_stream_id,
            { "Stream ID", "aeron.nak.stream_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_term_id,
            { "Term ID", "aeron.nak.term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_term_offset,
            { "Term Offset", "aeron.nak.term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_nak_length,
            { "Length", "aeron.nak.length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm,
            { "Status Message", "aeron.sm", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_version,
            { "Version", "aeron.sm.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_flags,
            { "Flags", "aeron.sm.flags", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_flags_s,
            { "Setup", "aeron.sm.flags.s", FT_BOOLEAN, 8, TFS(&tfs_set_notset), STATUS_FLAGS_SETUP, NULL, HFILL } },
        { &hf_aeron_sm_type,
            { "Type", "aeron.sm.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_sm_frame_length,
            { "Frame Length", "aeron.sm.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_session_id,
            { "Session ID", "aeron.sm.session_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_stream_id,
            { "Stream ID", "aeron.sm.stream_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_term_id,
            { "Term ID", "aeron.sm.term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_completed_term_offset,
            { "Completed Term Offset", "aeron.sm.completed_term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_receiver_window,
            { "Receiver Window", "aeron.sm.receiver_window", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sm_feedback,
            { "Application-specific Feedback", "aeron.sm.feedback", FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err,
            { "Error Header", "aeron.err", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_version,
            { "Version", "aeron.err.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_code,
            { "Error Code", "aeron.err.code", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_type,
            { "Type", "aeron.err.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_err_frame_length,
            { "Frame Length", "aeron.err.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_off_frame_length,
            { "Offending Frame Length", "aeron.err.off_frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_off_hdr,
            { "Offending Header", "aeron.err.off_hdr", FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_err_string,
            { "Error String", "aeron.err.string", FT_STRINGZ, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup,
            { "Setup Frame", "aeron.setup", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_version,
            { "Version", "aeron.setup.version", FT_UINT8, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_flags,
            { "Flags", "aeron.setup.flags", FT_UINT8, BASE_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_type,
            { "Type", "aeron.setup.type", FT_UINT16, BASE_DEC_HEX, VALS(aeron_frame_type), 0x0, NULL, HFILL } },
        { &hf_aeron_setup_frame_length,
            { "Frame Length", "aeron.setup.frame_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_term_offset,
            { "Term Offset", "aeron.setup.term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_session_id,
            { "Session ID", "aeron.setup.session_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_stream_id,
            { "Stream ID", "aeron.setup.stream_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_initial_term_id,
            { "Initial Term ID", "aeron.setup.initial_term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_active_term_id,
            { "Active Term ID", "aeron.setup.active_term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_term_length,
            { "Term Length", "aeron.setup.term_length", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_setup_mtu,
            { "MTU", "aeron.setup.mtu", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis,
            { "Analysis", "aeron.sequence_analysis", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_channel_prev_frame,
            { "Previous Channel Frame", "aeron.sequence_analysis.prev_channel_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_channel_next_frame,
            { "Next Channel Frame", "aeron.sequence_analysis.next_channel_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_stream_prev_frame,
            { "Previous Stream Frame", "aeron.sequence_analysis.prev_stream_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_stream_next_frame,
            { "Next Stream Frame", "aeron.sequence_analysis.next_stream_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_term_prev_frame,
            { "Previous Term Frame", "aeron.sequence_analysis.prev_term_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_term_next_frame,
            { "Next Term Frame", "aeron.sequence_analysis.next_term_frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_term_offset,
            { "Offset also in", "aeron.sequence_analysis.term_offset", FT_NONE, BASE_NONE, NULL, 0x0, "Offset also appears in these frames", HFILL } },
        { &hf_aeron_sequence_analysis_term_offset_frame,
            { "Frame", "aeron.sequence_analysis.term_offset.frame", FT_FRAMENUM, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_sequence_analysis_retransmission,
            { "Frame is a retransmission", "aeron.sequence_analysis.retransmission", FT_BOOLEAN, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis,
            { "Window Analysis", "aeron.window_analysis", FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis_high_term_id,
            { "Highest sent term ID", "aeron.window_analysis.high_term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis_high_term_offset,
            { "Highest sent term offset", "aeron.window_analysis.high_term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis_completed_term_id,
            { "Completed term ID", "aeron.window_analysis.completed_term_id", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis_completed_term_offset,
            { "Completed term offset", "aeron.window_analysis.completed_term_offset", FT_UINT32, BASE_DEC_HEX, NULL, 0x0, NULL, HFILL } },
        { &hf_aeron_window_analysis_outstanding_bytes,
            { "Outstanding bytes", "aeron.window_analysis.outstanding_bytes", FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL } }
    };
    static gint * ett[] =
    {
        &ett_aeron,
        &ett_aeron_pad,
        &ett_aeron_data,
        &ett_aeron_data_flags,
        &ett_aeron_nak,
        &ett_aeron_sm,
        &ett_aeron_sm_flags,
        &ett_aeron_err,
        &ett_aeron_setup,
        &ett_aeron_ext,
        &ett_aeron_sequence_analysis,
        &ett_aeron_sequence_analysis_term_offset,
        &ett_aeron_window_analysis
    };
    static ei_register_info ei[] =
    {
        { &ei_aeron_analysis_nak, { "aeron.analysis.nak", PI_SEQUENCE, PI_NOTE, "NAK", EXPFILL } },
        { &ei_aeron_analysis_window_full, { "aeron.analysis.window_full", PI_SEQUENCE, PI_NOTE, "Receiver window is full", EXPFILL } },
        { &ei_aeron_analysis_idle_rx, { "aeron.analysis.idle_rx", PI_SEQUENCE, PI_NOTE, "This frame contains an Idle RX", EXPFILL } },
        { &ei_aeron_analysis_pacing_rx, { "aeron.analysis.pacing_rx", PI_SEQUENCE, PI_NOTE, "This frame contains a Pacing RX", EXPFILL } },
        { &ei_aeron_analysis_ooo, { "aeron.analysis.ooo", PI_SEQUENCE, PI_NOTE, "This frame contains Out-of-order data", EXPFILL } },
        { &ei_aeron_analysis_ooo_gap, { "aeron.analysis.ooo_gap", PI_SEQUENCE, PI_NOTE, "This frame is an Out-of-order gap", EXPFILL } },
        { &ei_aeron_analysis_keepalive, { "aeron.analysis.keepalive", PI_SEQUENCE, PI_NOTE, "This frame contains a Keepalive", EXPFILL } },
        { &ei_aeron_analysis_window_resize, { "aeron.analysis.window_resize", PI_SEQUENCE, PI_NOTE, "Receiver window resized", EXPFILL } },
        { &ei_aeron_analysis_ooo_sm, { "aeron.analysis.ooo_sm", PI_SEQUENCE, PI_NOTE, "This frame contains an Out-of-order SM", EXPFILL } },
        { &ei_aeron_analysis_keepalive_sm, { "aeron.analysis.keepalive_sm", PI_SEQUENCE, PI_NOTE, "This frame contains a Keepalive SM", EXPFILL } }
    };
    module_t * aeron_module;
    expert_module_t * expert_aeron;

    proto_aeron = proto_register_protocol("Aeron Protocol", "Aeron", "aeron");

    proto_register_field_array(proto_aeron, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    expert_aeron = expert_register_protocol(proto_aeron);
    expert_register_field_array(expert_aeron, ei, array_length(ei));
    aeron_module = prefs_register_protocol(proto_aeron, proto_reg_handoff_aeron);

    aeron_sequence_analysis = global_aeron_sequence_analysis;
    aeron_window_analysis = global_aeron_window_analysis;
#if AERON_REASSEMBLY
    aeron_reassemble_fragments = glocal_aeron_reassemble_fragments;
#endif
    prefs_register_bool_preference(aeron_module,
        "sequence_analysis",
        "Perform transport sequence analysis",
        "Need a better description and name",
        &global_aeron_sequence_analysis);
    prefs_register_bool_preference(aeron_module,
        "window_analysis",
        "Perform receiver window analysis",
        "Need a better description and name",
        &global_aeron_window_analysis);
#if AERON_REASSEMBLY
    prefs_register_bool_preference(aeron_module,
        "reassemble_fragments",
        "Reassemble fragmented data",
        &global_aeron_reassemble_fragments);
#endif
    register_init_routine(aeron_init);
}

/* The registration hand-off routine */
void proto_reg_handoff_aeron(void)
{
    static gboolean already_registered = FALSE;

    if (!already_registered)
    {
        aeron_dissector_handle = new_create_dissector_handle(dissect_aeron, proto_aeron);
        dissector_add_for_decode_as("udp.port", aeron_dissector_handle);
        heur_dissector_add("udp", test_aeron_packet, proto_aeron);
        /* TODO:
        aeron_tap_handle = register_tap("aeron");
        */
    }

    aeron_sequence_analysis = global_aeron_sequence_analysis;
    aeron_window_analysis = global_aeron_window_analysis;
#if AERON_REASSEMBLY
    aeron_reassemble_fragments = global_aeron_reassemble_fragments;
#endif
    already_registered = TRUE;
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=4 expandtab:
 * :indentSize=4:tabSize=4:noTabs=true:
 */