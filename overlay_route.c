#include "mphlr.h"

/*
  Here we implement the actual routing algorithm which is heavily based on BATMAN.
  
  The fundamental difference is that we want to allow the mesh to grow beyond the
  size that could ordinarily be accomodated by the available bandwidth.  Some
  explanation follows.

  BATMAN operates by having nodes periodically send "hello" or originator messages,
  either with a limited distribution or with a sufficiently high TTL to spread
  over the whole network.  

  The latter results in a super-linear bandwidth requirement as the network grows 
  in size.

  What we wish to do is to implement the BATMAN concept, but using link-local traffic
  only.  To do this we need to change the high-TTL originator frames into something
  equivalent, but that does not get automatic network-wide distribution.

  What seems possible is to implement the BATMAN approach for link-local neighbours,
  and then have each node periodically announce the link-score to the peers that
  they know about, whether link-local or more distant.  If the number of reported 
  peers is left unconstrained, super-linear bandwidth consumption will still occur.

  However, if the number of peers that each node announces is limited, then bandwidth
  will be capped at a constant factor (which can be chosen based on the bandwidth
  available). The trade-off being that each node will only be able to see some number
  of "nearest" peers based on the available bandwidth.  

  This seems an entirely reasonable outcome, and at least on the surface would appear
  to solve our problem of wanting to allow a global-scale mesh, even if only local
  connectivity is possible, in contrast to existing mesh protocols that will not allow
  any connectivity once the number of nodes grows beyond a certain point.

  Remaining challenges that we have to think through are how to add a hierarchical
  element to the mesh that might allow us to route traffic beyond a nodes' 
  neighbourhood of peers.

  There is some hope to extend the effective range beyond the immediate neighbourhood
  to some degree by rotating the peers that a node reports on, so that a larger total
  set of nodes becomes known to the mesh, in return for less frequent updates on their
  link scores and optimal routes.

  This actually makes some logical sense, as the general direction in which to route 
  a frame to a distant node is less likely to change more slowly than for nearer nodes.
  So we will attempt this.

  With some careful thought, this statistical announcement of peers also serves to allow
  long-range but very low bandwidth links, e.g., satellite or dial-up, as well as long-shot
  WiFi where bandwidth is less constrained.

  Questions arise as to the possibility of introducing routing loops through the use of
  stale information.  So we will certainly need to have some idea of the freshness of 
  routing data.

  Finally, all this works only for bidirectional links.  We will need to think about how
  to handle mono-directional links.  BATMAN does this well, but I don't have the documentation
  here at 36,000 feet to digest it and think about how to incorporate it.

  Related to this we need to continue thinking about how to handle intermittant links in a more
  formal sense, including getting an idea of when nodes might reappear.

  Turning to the practical side of things, we need to keep track of reachability scores for
  nodes via each of our immediate neighbours.  Recognising the statistical nature of 
  the announcments, we probably want to keep track of some that have ceased to be neighbours
  in case they become neighbours again.

  Probably it makes more sense to have a list of known nodes and the most recent and
  highest scoring nodes by which we may reach them, complete with the sequence numbers of last
  observation that they are based upon, and possibly more information down the track to
  support intermittant links.

*/

#define OVERLAY_SENDER_PREFIX_LENGTH 12
typedef struct overlay_node_observation {
  int valid;
  
  /* Sequence numbers are handled as ranges because the tick
     rate can vary between interfaces, and we want to be able to
     estimate the reliability of links to nodes that may have
     several available interfaces.
     We don't want sequence numbers to wrap too often, but we
     would also like to support fairly fast ticking interfaces,
     e.g., for gigabit type links. So lets go with 1ms granularity. */
  int sequence_range_low;
  int sequence_range_high;
  long long rx_time;
  unsigned char sender_prefix[OVERLAY_SENDER_PREFIX_LENGTH];
} overlay_node_observation;

/* Keep track of last 32 observations of a node.
   Hopefully this is enough, if not, we will increase */
#define OVERLAY_MAX_OBSERVATIONS 32

typedef struct overlay_node {
  unsigned char sid[SID_SIZE];
  int neighbour_id; /* 0=not a neighbour */
  int most_recent_observation_id;
  overlay_node_observation observations[OVERLAY_MAX_OBSERVATIONS];
} overlay_node;

/* For fast handling we will have a number of bins that will be indexed by the
   first few bits of the peer's SIDs, and a number of entries in each bin to
   handle hash collissions while still allowing us to have static memory usage. */
int overlay_bin_count=0;
int overlay_bin_size=0;
overlay_node **overlay_nodes=NULL;

/* We also need to keep track of which nodes are our direct neighbours.
   This means we need to keep an eye on how recently we received DIRECT announcements
   from nodes, and keep a list of the most recent ones.  The challenge is to keep the
   list ordered without having to do copies or have nasty linked-list structures that
   require lots of random memory reads to resolve.

   The simplest approach is to maintain a large cache of neighbours and practise random
   replacement.  It is however succecptible to cache flushing attacks by adversaries, so
   we will need something smarter in the long term.
*/
int overlay_max_neighbours=0;
int overlay_neighbour_count=0;
overlay_node **overlay_neighbours=NULL;

/* allocate structures according to memory availability.
   We size differently because some devices are very constrained,
   e.g., mesh potatoes, some are middle sized, like mobile phones, and
   some are very large, like dedicated servers, that can keep track of
   very many nodes.
   
   The memory allocation is in two main areas:
   
   1. Neighbour list, which is short, requiring just a single pointer per
   direct neighbour.  So this can probably be set to a fairly large value
   on any sized machine, certainly in the thousands, which should be more
   than sufficient for wifi-type interfaces.  1000 neighbours requires
   onlt 8KB on a 64 bit machine, which is probably a reasonable limit per
   MB allocated.  This leaves 1016KB/MB for:
   
   2. The node information (overlay_node) structures.  These take more
   space and have to be sized appropriately.  We also need to choose the
   associativity of the node table based on the size of the structure.
   The smaller the structure the greater the associativity we should have
   so that the fewer the entries the more effectively we use them.  The
   trade-off is that increased associativity results in increased search
   time as the bins are searched for matches.  This is also why for very
   large tables we want low associativity so that we are more CPU efficient.
   
   The bulk of the size ofthe overlay_node structure is the observations
   information, because each observation contains a full 32 byte SID. The
   question is whether a full SID is required, or whether a prefix is
   sufficient, or if there is some even more compact representation possible.
   
   In principle the sender of the observation should be a direct neighbour,
   and so we could just use a neighbour index. However the neighbour indices
   are liable to change or become invalid over time, and we don't want to have
   to trawl through the nodes every time that happens, as otherwise the CPU
   requirements will be crazy.  
   
   This suggests that the use of a prefix is probably more appropriate. The
   prefix must be long enough to be robust against birthday-paradox effects
   and attacks. So at least 8 bytes (64 bits) is mandatory to make it
   reasonably difficult to find a colliding public key prefix.  Prudence 
   suggests that a longer prefix is desirable to give a safety margin, perhaps
   12 bytes (96 bits) being a reasonable figure.  
   
   This approximately halves the memory requirement per node to about 4KB (i.e.
   ~250 nodes per MB), and employing shorter prefixes than 12 bytes will result
   in diminishing returns, so this gives us confidence that it is an appropriate
   figure.
   
   Four-way associativity is probably reasonable for large-memory deployments
   where we have space for many thousands of nodes to keep string comparison
   effort to low levels.
   
   For small-memory deployments where we have space for only a few hundred nodes it
   probably makes sence to go for eight-way associativity just to be on the safe
   side.  However, this is somewhat arbitrary.  Only experience will teach us.
   
   One final note on potential attacks against us is that by having a hashed structure,
   even with modest associativity, is that if an attacker knows the hash function 
   they can readily cause hash collisions and interfere with connectivity to nodes
   on the mesh.  
   
   The most robust solution to this problem would be to use a linear hash function
   that takes, say, 10 of the 32 bytes as input, as this would result in a hash function
   space of:  32!/22! which is > 2^47.  This would then require several times 2^47 
   observation injections by an adversary to cause a hash collision with confidence.
   Actually determining that such a collision had been caused would probably multiply
   the effort required by some small further constant.  
   
   Using a non-linear hash function would raise the space to 32^10 > 2^50, the factor 
   of 8 probably not being worth the CPU complexity of such a non-linear function.
   
   However the question arises as to whether such an extreme approach is required, 
   remembering that changing the hash function does not break the protocol, so 
   such strong hash functions could be employed in future if needed without breaking
   backward compatibility.
   
   So let us then think about some very simple alternatives that might be good enough
   for now, but that are very fast to calculate.
   
   The simplest is to just choose a sufficient number of bytes from the SIDs to create
   a sufficiently long index value.  This gives 32!/(32-n)! where n is the number of
   bytes required, or 32 for the worst-case situation of n.
   
   An improvement is to xor bytes to produce the index value.  Using pairs of bytes
   gets us to something along the lines of 32!/(32-2n)! for production of a single byte,
   which is a modest improvement, but possibly not good enough.  As we increase the number
   of bytes xored together the situation improves to a point. However if we go to far we 
   end up reducing the total space because once more than half of the bytes are involved in
   the xor it is equivalent to the xor of all of the bytes xored with the bytes not included
   in the xor. This means that regardless of the length of the index we need, we probably want
   to use only half of the bytes as input, a this gives a complexity of 32!/16! = 2^73,
   which is plenty.
   
   In fact, this gives us a better result than the previous concept, and can be implemented
   using a very simple algorithm.  All we need to do is get a random ordering of the numbers 
   [0..31], and round robin xor the bytes we need with the [0..15]th elements of the random
   ordering.  
*/

/* The random ordering of bytes for the hash */
int overlay_route_hash_order[16];
int overlay_route_hash_bytes=0;

int overlay_route_init(int mb_ram)
{
  int i,j;
  /* XXX Initialise the random number generator in a robust manner */

  /* Generate hash ordering function */
  for(i=0;i<32;i++) {
    j=32;
    while(j<i) {
      overlay_route_hash_order[i]=random()%32;
      for(j=0;j<i;j++) if (overlay_route_hash_order[i]==overlay_route_hash_order[j]) break;
    }
  }

  int associativity=4;
  int bin_count=1;

  /* Now fiddle it to get bin_count to be a power of two that fits and doesn't waste too much space. */
  long long space=(sizeof(overlay_node*)*1024LL*mb_ram)+sizeof(overlay_node)*bin_count*associativity*1LL;
  while (space<mb_ram*1048576LL&&associativity<8)
    {
      long long space2=(sizeof(overlay_node*)*1024LL*mb_ram)+sizeof(overlay_node)*(bin_count*2LL)*associativity*1LL;
      if (space2<mb_ram*1048576LL) { bin_count*=2; continue; }
      space2=(sizeof(overlay_node*)*1024LL)+sizeof(overlay_node)*bin_count*(associativity+1)*1LL;
      if (space2<mb_ram*1048576LL) { associativity++; continue; }
      break;
    }

  /* Report on the space used */
  {
    space=(sizeof(overlay_node*)*1024LL*mb_ram)+sizeof(overlay_node)*bin_count*associativity*1LL;
    int percent=100LL*space/(mb_ram*1048576LL);
    fprintf(stderr,"Using %d%% of %dMB RAM allows for %d bins with %d-way associativity and %d direct neighbours.\n",
	    percent,mb_ram,bin_count,associativity,1024*mb_ram);
  }

  /* Now allocate the structures */

  overlay_nodes=calloc(sizeof(overlay_node*),bin_count);
  if (!overlay_nodes) return WHY("calloc() failed.");

  overlay_neighbours=calloc(sizeof(overlay_node*),1024*mb_ram);
  if (!overlay_neighbours) {
    free(overlay_nodes);
    return WHY("calloc() failed.");
  }

  for(i=0;i<bin_count;i++)
    {
      overlay_nodes[i]=calloc(sizeof(overlay_node),associativity);
      if (!overlay_nodes[i]) {
	while(--i>=0) free(overlay_nodes[i]);
	free(overlay_nodes);
	free(overlay_neighbours);
	return WHY("calloc() failed.");
      }
    }

  overlay_max_neighbours=1024*mb_ram;
  overlay_bin_count=bin_count;
  overlay_bin_size=associativity;
  fprintf(stderr,"Node and neighbour tables allocated.\n");
  
  return WHY("Not implemented");
}

int overlay_get_nexthop(unsigned char *d,unsigned char *nexthop,int *nexthoplen)
{
  if (!overlay_neighbours) return 0;
  return WHY("Not implemented");
}

int overlay_route_saw_selfannounce(overlay_frame *f)
{
  /* XXX send ack out even if we have no structures setup? */
  if (!overlay_neighbours) return 0;
  return WHY("Not implemented");
}

int overlay_route_saw_selfannounce_ack(overlay_frame *f)
{
  if (!overlay_neighbours) return 0;
  return WHY("Not implemented");  
}
