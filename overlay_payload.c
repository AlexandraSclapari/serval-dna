/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "serval.h"
#include "conf.h"
#include "str.h"
#include "overlay_buffer.h"
#include "overlay_packet.h"

int overlay_frame_build_header(struct decode_context *context, struct overlay_buffer *buff, 
			       int queue, int type, int modifiers, int ttl, 
			       struct broadcast *broadcast, struct subscriber *next_hop,
			       struct subscriber *destination, struct subscriber *source)
{
  if (ttl < 0 || ttl > PAYLOAD_TTL_MAX)
    return WHYF("invalid ttl=%d", ttl);

  int flags = modifiers & (PAYLOAD_FLAG_CIPHERED | PAYLOAD_FLAG_SIGNED);
  
  if (ttl==1 && !broadcast)
    flags |= PAYLOAD_FLAG_ONE_HOP;
  if (destination && destination==next_hop)
    flags |= PAYLOAD_FLAG_ONE_HOP;
  
  if (source == context->sender)
    flags |= PAYLOAD_FLAG_SENDER_SAME;
  
  if (!destination)
    flags |= PAYLOAD_FLAG_TO_BROADCAST;
  
  if (type!=OF_TYPE_DATA)
    flags |= PAYLOAD_FLAG_LEGACY_TYPE;
  
  if (ob_append_byte(buff, flags)) return -1;
  
  if (!(flags & PAYLOAD_FLAG_SENDER_SAME)){
    if (overlay_address_append(context, buff, source)) return -1;
  }
  
  if (flags & PAYLOAD_FLAG_TO_BROADCAST){
    if (!(flags & PAYLOAD_FLAG_ONE_HOP)){
      if (overlay_broadcast_append(buff, broadcast)) return -1;
    }
  }else{
    if (overlay_address_append(context, buff, destination)) return -1;
    if (!(flags & PAYLOAD_FLAG_ONE_HOP)){
      if (overlay_address_append(context, buff, next_hop)) return -1;
    }
  }
  
  if (!(flags & PAYLOAD_FLAG_ONE_HOP)){
    if (ob_append_byte(buff, ttl | ((queue&3)<<5))) return -1;
  }
  
  if (flags & PAYLOAD_FLAG_LEGACY_TYPE){
    if (ob_append_byte(buff, type)) return -1;
  }
  
  return 0;
}

int overlay_frame_append_payload(struct decode_context *context, overlay_interface *interface, 
				 struct overlay_frame *p, struct overlay_buffer *b)
{
  /* Convert a payload (frame) structure into a series of bytes.
     Assumes that any encryption etc has already been done.
     Will pick a next hop if one has not been chosen.
  */

  ob_checkpoint(b);
  
  if (config.debug.packetconstruction){
    DEBUGF( "+++++\nFrame from %s to %s of type 0x%02x %s:",
	   alloca_tohex_sid(p->source->sid),
	   alloca_tohex_sid(p->destination->sid),p->type,
	   "append_payload stuffing into packet");
    if (p->payload)
      dump("payload contents", &p->payload->bytes[0],p->payload->position);
  }
  
  struct broadcast *broadcast=NULL;
  if ((!p->destination) && !is_all_matching(p->broadcast_id.id,BROADCAST_LEN,0)){
    broadcast = &p->broadcast_id;
  }
  if (overlay_frame_build_header(context, b,
			     p->queue, p->type, p->modifiers, p->ttl,
			     broadcast, p->next_hop, 
			     p->destination, p->source))
    goto cleanup;
  
  if (ob_append_ui16(b, ob_position(p->payload)))
      goto cleanup;
      
  if (ob_append_bytes(b, ob_ptr(p->payload), ob_position(p->payload))) {
    WHY("could not append payload"); 
    goto cleanup;
  }
      
  return 0;
  
cleanup:
  ob_rewind(b);
  return -1;
}

int single_packet_encapsulation(struct overlay_buffer *b, struct overlay_frame *frame){
  overlay_interface *interface=frame->interface;
  int interface_number = interface - overlay_interfaces;
  struct decode_context context;
  bzero(&context, sizeof(struct decode_context));
  
  if (frame->source_full)
    my_subscriber->send_full=1;
  int seq = interface->sequence_number++;
  if (overlay_packet_init_header(ENCAP_SINGLE, &context, b, NULL, 0, interface_number, seq))
    return -1;

  struct broadcast *broadcast=NULL;
  if ((!frame->destination) && !is_all_matching(frame->broadcast_id.id,BROADCAST_LEN,0))
    broadcast = &frame->broadcast_id;

  if (overlay_frame_build_header(&context, b,
				 frame->queue, frame->type, 
				 frame->modifiers, frame->ttl,
				 broadcast, frame->next_hop, 
				 frame->destination, frame->source))
    return -1;
  
  if (ob_append_buffer(b, frame->payload))
    return -1;
  
  return 0;
}

int op_free(struct overlay_frame *p)
{
  if (!p) return WHY("Asked to free NULL");
  if (p->prev&&p->prev->next==p) return WHY("p->prev->next still points here");
  if (p->next&&p->next->prev==p) return WHY("p->next->prev still points here");
  p->prev=NULL;
  p->next=NULL;
  if (p->payload) ob_free(p->payload);
  p->payload=NULL;
  free(p);
  return 0;
}

struct overlay_frame *op_dup(struct overlay_frame *in)
{
  if (!in) return NULL;

  /* clone the frame */
  struct overlay_frame *out=malloc(sizeof(struct overlay_frame));
  if (!out) { WHY("malloc() failed"); return NULL; }

  /* copy main data structure */
  bcopy(in,out,sizeof(struct overlay_frame));

  if (in->payload)
    out->payload=ob_dup(in->payload);
  return out;
}
