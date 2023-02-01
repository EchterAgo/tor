/* Copyright (c) 2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file conflux_pool.c
 * \brief Conflux circuit pool management
 */

#define TOR_CONFLUX_PRIVATE
#define CONFLUX_CELL_PRIVATE

#include "core/or/or.h"

#include "app/config/config.h"

#include "core/or/circuitbuild.h"
#include "core/or/circuitlist.h"
#include "core/or/circuitstats.h"
#include "core/or/circuituse.h"
#include "core/or/congestion_control_st.h"
#include "core/or/conflux.h"
#include "core/or/conflux_cell.h"
#include "trunnel/conflux.h"
#include "core/or/conflux_params.h"
#include "core/or/conflux_pool.h"
#include "core/or/conflux_util.h"
#include "core/or/relay.h"
#include "core/or/connection_edge.h"
#include "core/or/edge_connection_st.h"

#include "core/or/crypt_path_st.h"
#include "core/or/or_circuit_st.h"
#include "core/or/origin_circuit_st.h"

#include "feature/nodelist/nodelist.h"

#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/crypto_util.h"

/* Indicate if we are shutting down. This is used so we avoid recovering a
 * conflux set on total shutdown. */
static bool shutting_down = false;

/** The pool of client-side conflux_t that are built, linked, and ready
 * to be used. Indexed by nonce. */
static digest256map_t *client_linked_pool;

/** The pool of origin unlinked_circuits_t indexed by nonce. */
static digest256map_t *client_unlinked_pool;

/** The pool of relay conflux_t indexed by nonce. We call these "server"
 * because they could be onion-service side too (even though we likely will
 * only implement onion service conflux in Arti). The code is littered with
 * asserts to ensure there are no origin circuits in here for now, too. */
static digest256map_t *server_linked_pool;

/** The pool of relay unlinked_circuits_t indexed by nonce. */
static digest256map_t *server_unlinked_pool;

/* A leg is essentially a circuit for a conflux set. We use this object for the
 * unlinked pool. */
typedef struct leg_t {
  /* The circuit of the leg. */
  circuit_t *circ;

  /* The LINK cell content which is used to put the information back in the
   * conflux_t object once all legs have linked and validate the ack. */
  conflux_cell_link_t *link;

  /* Indicate if the leg has received the LINKED or the LINKED_ACK cell
   * depending on its side of the circuit. When all legs are linked, we then
   * finalize the conflux_t object and move it to the linked pool. */
  bool linked;

  /* What time did we send the LINK/LINKED (depending on which side) so we can
   * calculate the RTT. */
  uint64_t link_sent_usec;

  /* The RTT value in usec takend from the LINK <--> LINKED round trip. */
  uint64_t rtt_usec;
} leg_t;

/* Object used to track unlinked circuits which are kept in the unlinked pool
 * until they are linked and moved to the linked pool and global circuit set.
 */
typedef struct unlinked_circuits_t {
  /* If true, indicate that this unlinked set is client side as in the legs are
   * origin circuits. Else, it is on the exit side and thus or circuits. */
  bool is_client;

  /* If true, indicate if the conflux_t is related to a linked set. */
  bool is_for_linked_set;

  /* Conflux object that will be set in each leg once all linked. */
  conflux_t *cfx;

  /* Legs. */
  smartlist_t *legs;
} unlinked_circuits_t;

/** Error code used when linking circuits. Based on those, we decide to
 * relaunch or not. */
typedef enum link_circ_err_t {
  /* Linking was successful. */
  ERR_LINK_CIRC_OK          = 0,
  /* The RTT was not acceptable. */
  ERR_LINK_CIRC_BAD_RTT     = 1,
  /* The leg can't be found. */
  ERR_LINK_CIRC_MISSING_LEG = 2,
  /* The set can't be found. */
  ERR_LINK_CIRC_MISSING_SET = 3,
  /* Invalid leg as in not pass validation. */
  ERR_LINK_CIRC_INVALID_LEG = 4,
} link_circ_err_t;

#ifdef TOR_UNIT_TESTS
digest256map_t *
get_unlinked_pool(bool is_client)
{
  return is_client ? client_unlinked_pool : server_unlinked_pool;
}

digest256map_t *
get_linked_pool(bool is_client)
{
  return is_client ? client_linked_pool : server_linked_pool;
}
#endif

/** Helper: Format at 8 bytes the nonce for logging. */
static inline const char *
fmt_nonce(const uint8_t *nonce)
{
  return hex_str((char *) nonce, 8);
}

/**
 * Return the conflux algorithm for a desired UX value.
 */
static uint8_t
conflux_choose_algorithm(uint8_t desired_ux)
{
  /* TODO-329-TUNING: Pick better algs here*/
  switch (desired_ux) {
    case CONFLUX_UX_NO_OPINION:
      return CONFLUX_ALG_LOWRTT;
    case CONFLUX_UX_MIN_LATENCY:
      return CONFLUX_ALG_MINRTT;
    case CONFLUX_UX_LOW_MEM_LATENCY:
      return CONFLUX_ALG_MINRTT;
    case CONFLUX_UX_LOW_MEM_THROUGHPUT:
      return CONFLUX_ALG_CWNDRTT;
    case CONFLUX_UX_HIGH_THROUGHPUT:
      return CONFLUX_ALG_LOWRTT;
    default:
      /* Trunnel should protect us from this */
      tor_assert_nonfatal_unreached();
      return CONFLUX_ALG_LOWRTT;
  }
}

/** Return a newly allocated conflux_t object. */
static conflux_t *
conflux_new(void)
{
  conflux_t *cfx = tor_malloc_zero(sizeof(*cfx));

  cfx->ooo_q = smartlist_new();
  cfx->legs = smartlist_new();

  return cfx;
}

static void
conflux_free_(conflux_t *cfx)
{
  if (!cfx) {
    return;
  }

  SMARTLIST_FOREACH_BEGIN(cfx->legs, conflux_leg_t *, leg) {
    SMARTLIST_DEL_CURRENT(cfx->legs, leg);
    tor_free(leg);
  } SMARTLIST_FOREACH_END(leg);
  smartlist_free(cfx->legs);

  SMARTLIST_FOREACH(cfx->ooo_q, conflux_cell_t *, cell, tor_free(cell));
  smartlist_free(cfx->ooo_q);

  memwipe(cfx->nonce, 0, sizeof(cfx->nonce));
  tor_free(cfx);
}

/** Wrapper for the free function, set the cfx pointer to NULL after free */
#define conflux_free(cfx) \
    FREE_AND_NULL(conflux_t, conflux_free_, cfx)

/** Helper: Free function for the digest256map_free(). */
static inline void
free_conflux_void_(void *ptr)
{
  conflux_t *cfx = (conflux_t *)ptr;
  conflux_free(cfx);
}

/** Return a newly allocated leg object containing the given circuit and link
 * pointer (no copy). */
static leg_t *
leg_new(circuit_t *circ, conflux_cell_link_t *link)
{
  leg_t *leg = tor_malloc_zero(sizeof(*leg));
  leg->circ = circ;
  leg->link = link;
  return leg;
}

/** Free the given leg object. Passing NULL is safe. */
static void
leg_free(leg_t *leg)
{
  if (!leg) {
    return;
  }
  if (leg->circ) {
    tor_free(leg->circ->conflux_pending_nonce);
    leg->circ->conflux_pending_nonce = NULL;
  }
  tor_free(leg->link);
  tor_free(leg);
}

/** Return a newly allocated unlinked set object for the given nonce. A new
 * conflux object is also created. */
static unlinked_circuits_t *
unlinked_new(const uint8_t *nonce, bool is_client)
{
  unlinked_circuits_t *unlinked = tor_malloc_zero(sizeof(*unlinked));
  unlinked->cfx = conflux_new();
  unlinked->legs = smartlist_new();
  unlinked->is_client = is_client;
  memcpy(unlinked->cfx->nonce, nonce, sizeof(unlinked->cfx->nonce));

  return unlinked;
}

/** Free the given unlinked object. */
static void
unlinked_free(unlinked_circuits_t *unlinked)
{
  if (!unlinked) {
    return;
  }
  /* This cfx is pointing to a linked set. */
  if (!unlinked->is_for_linked_set) {
    conflux_free(unlinked->cfx);
  }
  SMARTLIST_FOREACH(unlinked->legs, leg_t *, leg, leg_free(leg));
  smartlist_free(unlinked->legs);
  tor_free(unlinked);
}

/** Add the given unlinked object to the unlinked pool. */
static void
unlinked_pool_add(unlinked_circuits_t *unlinked, bool is_client)
{
  tor_assert(unlinked);
  if (is_client) {
    digest256map_set(client_unlinked_pool, unlinked->cfx->nonce, unlinked);
  } else {
    digest256map_set(server_unlinked_pool, unlinked->cfx->nonce, unlinked);
  }
}

/** Delete the given unlinked object from the unlinked pool. */
static void
unlinked_pool_del(unlinked_circuits_t *unlinked, bool is_client)
{
  tor_assert(unlinked);

  if (is_client) {
    digest256map_remove(client_unlinked_pool, unlinked->cfx->nonce);
  } else {
    digest256map_remove(server_unlinked_pool, unlinked->cfx->nonce);
  }
}

/** Return an unlinked object for the given nonce else NULL. */
static unlinked_circuits_t *
unlinked_pool_get(const uint8_t *nonce, bool is_client)
{
  tor_assert(nonce);
  if (is_client) {
    return digest256map_get(client_unlinked_pool, nonce);
  } else {
    return digest256map_get(server_unlinked_pool, nonce);
  }
}

/** Delete from the pool and free the given unlinked object. */
static void
unlinked_pool_del_and_free(unlinked_circuits_t *unlinked, bool is_client)
{
  tor_assert(unlinked);
  unlinked_pool_del(unlinked, is_client);
  unlinked_free(unlinked);
}

/** Add the given conflux object to the linked conflux set. */
static void
linked_pool_add(conflux_t *cfx, bool is_client)
{
  tor_assert(cfx);
  if (is_client) {
    digest256map_set(client_linked_pool, cfx->nonce, cfx);
  } else {
    digest256map_set(server_linked_pool, cfx->nonce, cfx);
  }
}

/** Delete from the linked conflux set the given nonce. */
static void
linked_pool_del(const uint8_t *nonce, bool is_client)
{
  tor_assert(nonce);
  if (is_client) {
    digest256map_remove(client_linked_pool, nonce);
  } else {
    digest256map_remove(server_linked_pool, nonce);
  }
}

/** Return a conflux_t object for the given nonce from the linked set. */
static conflux_t *
linked_pool_get(const uint8_t *nonce, bool is_client)
{
  tor_assert(nonce);
  if (is_client) {
    return digest256map_get(client_linked_pool, nonce);
  } else {
    return digest256map_get(server_linked_pool, nonce);
  }
}

/** Add the given leg to the given unlinked object. */
static inline void
unlinked_leg_add(unlinked_circuits_t *unlinked, leg_t *leg)
{
  tor_assert(unlinked);
  tor_assert(leg);

  smartlist_add(unlinked->legs, leg);
}

/** Return an unlinked leg for the given unlinked object and for the given
 * circuit. */
static inline leg_t *
leg_find(const unlinked_circuits_t *unlinked, const circuit_t *circ)
{
  SMARTLIST_FOREACH_BEGIN(unlinked->legs, leg_t *, leg) {
    if (leg->circ == circ) {
      return leg;
    }
  } SMARTLIST_FOREACH_END(leg);
  return NULL;
}

/** Return the given circuit leg from its unlinked set (if any). */
static leg_t *
unlinked_leg_find(const circuit_t *circ, bool is_client)
{
  unlinked_circuits_t *unlinked =
    unlinked_pool_get(circ->conflux_pending_nonce, is_client);
  if (!unlinked) {
    return NULL;
  }
  return leg_find(unlinked, circ);
}

static void
unlinked_leg_del_and_free(unlinked_circuits_t *unlinked,
                          const circuit_t *circ)
{
  tor_assert(circ);
  tor_assert(unlinked);

  SMARTLIST_FOREACH_BEGIN(unlinked->legs, leg_t *, leg) {
    if (leg->circ == circ) {
      SMARTLIST_DEL_CURRENT(unlinked->legs, leg);
      leg_free(leg);
      break;
    }
  } SMARTLIST_FOREACH_END(leg);
}

/**
 * Ensure that the given circuit has no attached streams.
 *
 * This validation function is called at various stages for
 * unlinked circuits, to make sure they have no streams.
 */
static void
validate_circ_has_no_streams(circuit_t *circ)
{
  if (CIRCUIT_IS_ORIGIN(circ)) {
    origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
    if (BUG(ocirc->p_streams)) {
      log_warn(LD_BUG,
         "Unlinked Conflux circuit %u has attached streams.",
               ocirc->global_identifier);
       ocirc->p_streams = NULL;
    }
    if (BUG(ocirc->half_streams)) {
      log_warn(LD_BUG,
        "Unlinked conflux circ %u has half streams.",
               ocirc->global_identifier);
       ocirc->half_streams = NULL;
    }
  } else {
    or_circuit_t *orcirc = TO_OR_CIRCUIT(circ);
    if (BUG(orcirc->n_streams)) {
      log_warn(LD_BUG,
         "Unlinked conflux circuit has attached streams.");
       orcirc->n_streams = NULL;
    }
    if (BUG(orcirc->resolving_streams)) {
      log_warn(LD_BUG,
          "Unlinked conflux circuit has resolving streams.");
      orcirc->resolving_streams = NULL;
    }
  }
}

/** Return true iff the legs in the given unlinked set are valid and coherent
 * to be a linked set. */
static bool
validate_unlinked_legs(unlinked_circuits_t *unlinked)
{
  bool valid = true;
  uint8_t version;
  uint8_t *nonce = NULL;

  tor_assert(unlinked);

  SMARTLIST_FOREACH_BEGIN(unlinked->legs, const leg_t *, leg) {
    if (!nonce) {
      nonce = leg->link->nonce;
      version = leg->link->version;
    } else {
      /* Version and nonce must match in all legs. */
      valid &= (leg->link->version == version &&
                tor_memeq(leg->link->nonce, nonce, sizeof(leg->link->nonce)));
    }

    // If the other ends last sent sequence number is higher than the
    // last sequence number we delivered, we have data loss, and cannot link.
    if (leg->link->last_seqno_sent > unlinked->cfx->last_seq_delivered) {
      log_fn(unlinked->is_client ? LOG_NOTICE : LOG_PROTOCOL_WARN, LD_CIRC,
               "Data loss detected while trying to add a conflux leg.");
      valid = false;

      // TODO-329-ARTI: Instead of closing the set here, we could
      // immediately send a SWITCH cell and re-send the missing data.
      // To do this, though, we would need to constantly buffer at least
      // a cwnd worth of sent data to retransmit. We're not going to try
      // this in C-Tor, but arti could consider it.
    }
    validate_circ_has_no_streams(leg->circ);
  } SMARTLIST_FOREACH_END(leg);

  /* Note that if no legs, it validates. */

  return valid;
}

/** Add up a new leg to the given conflux object. */
static void
cfx_add_leg(conflux_t *cfx, leg_t *leg)
{
  tor_assert(cfx);
  tor_assert(leg);
  tor_assert(leg->link);

  /* Big trouble if we add a leg to the wrong set. */
  tor_assert(tor_memeq(cfx->nonce, leg->link->nonce, sizeof(cfx->nonce)));

  if (BUG(CONFLUX_NUM_LEGS(cfx) > CONFLUX_MAX_CIRCS)) {
    return;
  }

  conflux_leg_t *cleg = tor_malloc_zero(sizeof(*cleg));
  cleg->circ = leg->circ;
  // TODO-329-ARTI: Blindly copying the values from the cell. Is this correct?
  // I think no... When adding new legs, switching to this leg is
  // likely to break, unless the sender tracks what link cell it sent..
  // Is that the best option? Or should we use the max of our legs, here?
  // (It seems the other side will have no idea what our current maxes
  /// are, so this option seems better right now)
  cleg->last_seq_recv = leg->link->last_seqno_sent;
  cleg->last_seq_sent = leg->link->last_seqno_recv;
  cleg->circ_rtts_usec = leg->rtt_usec;
  cleg->linked_sent_usec = leg->link_sent_usec;

  cfx->params.alg = conflux_choose_algorithm(leg->link->desired_ux);

  /* Add leg to given conflux. */
  smartlist_add(cfx->legs, cleg);

  /* Ensure the new circuit has no streams. */
  validate_circ_has_no_streams(leg->circ);

  /* If this is not the first leg, get the first leg, and get
   * the reference streams from it. */
  if (CONFLUX_NUM_LEGS(cfx) > 0) {
    conflux_leg_t *first_leg = smartlist_get(cfx->legs, 0);
    if (CIRCUIT_IS_ORIGIN(first_leg->circ)) {
      origin_circuit_t *old_circ = TO_ORIGIN_CIRCUIT(first_leg->circ);
      origin_circuit_t *new_circ = TO_ORIGIN_CIRCUIT(leg->circ);

      new_circ->p_streams = old_circ->p_streams;
      new_circ->half_streams = old_circ->half_streams;
      /* Sync all legs with the new stream(s). */
      conflux_sync_circ_fields(cfx, old_circ);
    } else {
      or_circuit_t *old_circ = TO_OR_CIRCUIT(first_leg->circ);
      or_circuit_t *new_circ = TO_OR_CIRCUIT(leg->circ);
      new_circ->n_streams = old_circ->n_streams;
      new_circ->resolving_streams = old_circ->resolving_streams;
    }
  }

  if (CIRCUIT_IS_ORIGIN(cleg->circ)) {
    tor_assert_nonfatal(cleg->circ->purpose ==
                        CIRCUIT_PURPOSE_CONFLUX_UNLINKED);
    circuit_change_purpose(cleg->circ, CIRCUIT_PURPOSE_CONFLUX_LINKED);
  }
  conflux_validate_stream_lists(cfx);
}

/**
 * Clean up a circuit from its conflux_t object.
 *
 * Return true if closing this circuit should tear down the entire set,
 * false otherwise.
 */
static bool
cfx_del_leg(conflux_t *cfx, const circuit_t *circ)
{
  conflux_leg_t *leg;
  bool full_teardown = false;

  tor_assert(cfx);
  tor_assert(circ);

  leg = conflux_get_leg(cfx, circ);
  if (!leg) {
    goto end;
  }

  // If the circuit still has inflight data, teardown
  const struct congestion_control_t *cc = circuit_ccontrol(circ);
  tor_assert(cc);
  tor_assert(cc->sendme_inc);
  if (cc->inflight >= cc->sendme_inc) {
    full_teardown = true;
    log_info(LD_CIRC, "Conflux current circuit has closed with "
             "data in flight, tearing down entire set.");
  }

  /* Remove it from the cfx. */
  smartlist_remove(cfx->legs, leg);

  /* After removal, if this leg had the highest sent (or recv)
   * sequence number, it was in active use by us (or the other side).
   * We need to tear down the entire set. */
  // TODO-329-ARTI: If we support resumption, we don't need this.
  if (CONFLUX_NUM_LEGS(cfx) > 0) {
    if (conflux_get_max_seq_sent(cfx) < leg->last_seq_sent ||
        conflux_get_max_seq_recv(cfx) < leg->last_seq_recv) {
      full_teardown = true;
      log_info(LD_CIRC, "Conflux sequence number check failed, "
               "tearing down entire set.");
    }
  }

  /* Cleanup any reference to leg. */
  if (cfx->curr_leg == leg) {
    cfx->curr_leg = NULL;
    full_teardown = true;
    log_info(LD_CIRC, "Conflux current circuit has closed, "
             "tearing down entire set.");
  }
  if (cfx->prev_leg == leg) {
    cfx->prev_leg = NULL;
  }

  tor_free(leg);

 end:
  return full_teardown;
}

/** Close the circuit of each legs of the given unlinked object. */
static void
unlinked_close_all_legs(unlinked_circuits_t *unlinked)
{
  smartlist_t *circ_to_close = NULL;

  tor_assert(unlinked);

  /* Small optimization here, avoid this work if no legs. */
  if (smartlist_len(unlinked->legs) == 0) {
    return;
  }

  /* We will iterate over all legs and put the circuit in its own list and then
   * mark them for close. The unlinked object gets freed opportunistically once
   * there is no more legs attached to it and so we can't hold a reference
   * while closing circuits. */
  circ_to_close = smartlist_new();

  SMARTLIST_FOREACH(unlinked->legs, leg_t *, leg,
                    smartlist_add(circ_to_close, leg->circ));
  unlinked = NULL;

  /* The leg gets cleaned up in the circuit close. */
  SMARTLIST_FOREACH_BEGIN(circ_to_close, circuit_t *, circ) {
    if (CIRCUIT_IS_ORIGIN(circ)) {
      tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_UNLINKED);
    }
    if (!circ->marked_for_close) {
      circuit_mark_for_close(circ, END_CIRC_REASON_INTERNAL);
    }
  } SMARTLIST_FOREACH_END(circ);

  /* Drop the list and ignore its content, we don't have ownership. */
  smartlist_free(circ_to_close);
}

/** Either closee all legs of the given unlinked set or delete it from the pool
 * and free its memory.
 *
 * Important: The unlinked object is freed opportunistically when legs are
 * removed until the point none remains. And so, it is only safe to free the
 * object if no more legs exist.
 */
static void
unlinked_close_or_free(unlinked_circuits_t *unlinked)
{
  if (!unlinked) {
    return;
  }

  /* If we have legs, the circuit close will trigger the unlinked object to be
   * opportunistically freed. Else, we do it explicitly. */
  if (smartlist_len(unlinked->legs) > 0) {
    unlinked_close_all_legs(unlinked);
  } else {
    unlinked_pool_del_and_free(unlinked, unlinked->is_client);
  }
  /* Either the unlinked object has been freed or the last leg close will free
   * it so from this point on, nullify for safety reasons. */
  unlinked = NULL;
}

/** Upon an error condition or a close of an in-use circuit, we must close all
 * linked and unlinked circuits associated with a set. When the last leg of
 * each set is closed, the set is removed from the pool. */
static void
conflux_mark_all_for_close(const uint8_t *nonce, bool is_client, int reason)
{
  /* It is possible that for a nonce we have both an unlinked set and a linked
   * set. This happens if there is a recovery leg launched for an existing
   * linked set. */

  /* Close the unlinked set. */
  unlinked_circuits_t *unlinked = unlinked_pool_get(nonce, is_client);
  if (unlinked) {
    unlinked_close_or_free(unlinked);
  }
  /* In case it gets freed, be safe here. */
  unlinked = NULL;

  /* Close the linked set. It will free itself upon the close of
   * the last leg. */
  conflux_t *linked = linked_pool_get(nonce, is_client);
  if (linked) {
    if (linked->in_full_teardown) {
      return;
    }
    linked->in_full_teardown = true;

    smartlist_t *circ_to_close = smartlist_new();

    SMARTLIST_FOREACH(linked->legs, conflux_leg_t *, leg,
                      smartlist_add(circ_to_close, leg->circ));

    SMARTLIST_FOREACH(circ_to_close, circuit_t *, circ,
                      circuit_mark_for_close(circ, reason));

    /* Drop the list and ignore its content, we don't have ownership. */
    smartlist_free(circ_to_close);
  }
}

/** Helper: Free function taking a void pointer for the digest256map_free. */
static inline void
free_unlinked_void_(void *ptr)
{
  unlinked_circuits_t *unlinked = ptr;
  unlinked_pool_del_and_free(unlinked, unlinked->is_client);
}

/** Attempt to finalize the unlinked set to become a linked set and be put in
 * the linked pool.
 *
 * If this finalized successfully, the given unlinked object is freed. */
static link_circ_err_t
try_finalize_set(unlinked_circuits_t *unlinked)
{
  link_circ_err_t err = ERR_LINK_CIRC_OK;
  bool is_client;

  tor_assert(unlinked);

  /* Without legs, this is not ready to become a linked set. */
  if (BUG(smartlist_len(unlinked->legs) == 0)) {
    err = ERR_LINK_CIRC_MISSING_LEG;
    goto end;
  }

  /* Validate that all legs are coherent and parameters match. On failure, we
   * teardown the whole unlinked set because this means we either have a code
   * flow problem or the Exit is trying to trick us. */
  if (!validate_unlinked_legs(unlinked)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Conflux unlinked set legs are not validating. Tearing it down.");
    conflux_mark_all_for_close(unlinked->cfx->nonce, unlinked->is_client,
                               END_CIRC_REASON_TORPROTOCOL);
    err = ERR_LINK_CIRC_INVALID_LEG;
    goto end;
  }

  /* Check all linked status. All need to be true in order to finalize the set
   * and move it to the linked pool. */
  SMARTLIST_FOREACH_BEGIN(unlinked->legs, const leg_t *, leg) {
    /* We are still waiting on a leg. */
    if (!leg->linked) {
      log_info(LD_CIRC, "Can't finalize conflux set, still waiting on at "
                        "least one leg to link up.");

      goto end;
    }
  } SMARTLIST_FOREACH_END(leg);

  /* Finalize the cfx object by adding all legs into it. */
  SMARTLIST_FOREACH_BEGIN(unlinked->legs, leg_t *, leg) {
    /* Removing the leg from the list is important so we avoid ending up with a
     * leg in the unlinked list that is set with LINKED purpose. */
    SMARTLIST_DEL_CURRENT(unlinked->legs, leg);

    /* We are ready to attach the leg to the cfx object now. */
    cfx_add_leg(unlinked->cfx, leg);

    /* Clean the pending nonce and set the conflux object in the circuit. */
    leg->circ->conflux = unlinked->cfx;

    /* We are done with this leg object. */
    leg_free(leg);
  } SMARTLIST_FOREACH_END(leg);

  is_client = unlinked->is_client;

  /* Add the conflux object to the linked pool. For an existing linked cfx
   * object, we'll simply replace it with itself. */
  linked_pool_add(unlinked->cfx, is_client);

  /* Remove it from the unlinked pool. */
  unlinked_pool_del(unlinked, is_client);

  /* We don't recover a leg when it is linked but if we would like to support
   * session ressumption, this would be very important in order to allow new
   * legs to be created/recovered. */
  unlinked->cfx->num_leg_launch = 0;

  /* Nullify because we are about to free the unlinked object and the cfx has
   * moved to all circuits. */
  unlinked->cfx = NULL;
  unlinked_free(unlinked);

  /* Now that this set is ready to use, try any pending streams again. */
  if (is_client) {
    connection_ap_attach_pending(1);
  }

  log_info(LD_CIRC,
           "Successfully linked a conflux %s set which is now usable.",
           is_client ? "client" : "relay");

 end:
  return err;
}

/** Record the RTT for this client circuit.
 *
 * Return the RTT value. UINT64_MAX is returned if we couldn't find the initial
 * measurement of when the cell was sent or if the leg is missing. */
static uint64_t
record_rtt_client(const circuit_t *circ)
{
  tor_assert(circ);
  tor_assert(circ->conflux_pending_nonce);
  tor_assert(CIRCUIT_IS_ORIGIN(circ));

  leg_t *leg = unlinked_leg_find(circ, true);
  if (leg && leg->link_sent_usec > 0) {
    leg->rtt_usec = monotime_absolute_usec() - leg->link_sent_usec;
    return leg->rtt_usec;
  }
  return UINT64_MAX;
}

/** Record the RTT for this Exit circuit.
 *
 * Return the RTT value. UINT64_MAX is returned if we couldn't find the initial
 * measurement of when the cell was sent or if the leg is missing. */

static uint64_t
record_rtt_exit(const circuit_t *circ)
{
  tor_assert(circ);
  tor_assert(circ->conflux);
  tor_assert(CIRCUIT_IS_ORCIRC(circ));

  conflux_leg_t *leg = conflux_get_leg(circ->conflux, circ);
  if (leg && leg->linked_sent_usec > 0) {
    leg->circ_rtts_usec = monotime_absolute_usec() - leg->linked_sent_usec;
    return leg->circ_rtts_usec;
  }
  return UINT64_MAX;
}

/** For the given circuit, record the RTT from when the LINK or LINKED cell was
 * sent that is this function works for either client or Exit.
 *
 * Return false if the RTT is too high for our standard else true. */
static bool
record_rtt(const circuit_t *circ, bool is_client)
{
  uint64_t rtt_usec;

  tor_assert(circ);

  if (is_client) {
    rtt_usec = record_rtt_client(circ);

    if (rtt_usec >= get_circuit_build_timeout_ms()*1000) {
      log_info(LD_CIRC, "Conflux leg RTT is above circuit build time out "
                        "currently at %f msec. Relaunching.",
               get_circuit_build_timeout_ms());
      return false;
    }
  } else {
    rtt_usec = record_rtt_exit(circ);
  }

  return true;
}

/** Link the given circuit within its unlinked set. This is called when either
 * the LINKED or LINKED_ACK is received depending on which side of the circuit
 * it is.
 *
 * It attempts to finalize the unlinked set as well which, if successful, puts
 * it in the linked pool. */
static link_circ_err_t
link_circuit(circuit_t *circ)
{
  link_circ_err_t err = ERR_LINK_CIRC_OK;
  unlinked_circuits_t *unlinked = NULL;
  bool is_client = false;

  tor_assert(circ);
  if (CIRCUIT_IS_ORIGIN(circ)) {
    tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_UNLINKED);
    is_client = true;
  }

  unlinked = unlinked_pool_get(circ->conflux_pending_nonce, is_client);
  if (BUG(!unlinked)) {
    log_warn(LD_BUG, "Failed to find the unlinked set %s when linking. "
                     "Closing circuit.",
             fmt_nonce(circ->conflux_pending_nonce));
    err = ERR_LINK_CIRC_MISSING_SET;
    goto end;
  }

  leg_t *leg = leg_find(unlinked, circ);
  if (BUG(!leg)) {
    /* Failure to find the leg when linking a circuit is an important problem
     * so log loudly and error. */
    log_warn(LD_BUG, "Failed to find leg for the unlinked set %s when "
                     "linking. Closing circuit.",
             fmt_nonce(unlinked->cfx->nonce));
    err = ERR_LINK_CIRC_MISSING_LEG;
    goto end;
  }

  /* Successful link. Attempt to finalize the set in case this was the last
   * LINKED or LINKED_ACK cell to receive. */
  leg->linked = true;
  err = try_finalize_set(unlinked);

 end:
  return err;
}

static unlinked_circuits_t *
unlinked_get_or_create(const uint8_t *nonce, bool is_client)
{
  unlinked_circuits_t *unlinked;

  tor_assert(nonce);

  unlinked = unlinked_pool_get(nonce, is_client);
  if (!unlinked) {
    unlinked = unlinked_new(nonce, is_client);

    /* If this is a leg of an existing linked set, use that conflux object
     * instead so all legs point to the same. It is put in the leg's circuit
     * once the link is confirmed. */
    conflux_t *cfx = linked_pool_get(nonce, is_client);
    if (cfx) {
      conflux_free(unlinked->cfx);
      unlinked->cfx = cfx;
      unlinked->is_for_linked_set = true;
    }
    /* Add this set to the unlinked pool. */
    unlinked_pool_add(unlinked, is_client);
  }

  return unlinked;
}

/**
 * On the client side, we need to determine if there is already
 * an exit in use for this set, and if so, use that.
 *
 * Otherwise, we return NULL and the exit is decided by the
 * circuitbuild.c code.
 */
static extend_info_t *
get_exit_for_nonce(const uint8_t *nonce)
{
  extend_info_t *exit = NULL;

  tor_assert(nonce);

  // First, check the linked pool for the nonce
  const conflux_t *cfx = linked_pool_get(nonce, true);
  if (cfx) {
    tor_assert(cfx->legs);
    /* Get the exit from the first leg */
    conflux_leg_t *leg = smartlist_get(cfx->legs, 0);
    tor_assert(leg);
    tor_assert(leg->circ);
    tor_assert(TO_ORIGIN_CIRCUIT(leg->circ)->cpath);
    exit = TO_ORIGIN_CIRCUIT(leg->circ)->cpath->prev->extend_info;
    tor_assert(exit);
  } else {
    unlinked_circuits_t *unlinked = NULL;
    unlinked = unlinked_pool_get(nonce, true);

    if (unlinked) {
      tor_assert(unlinked->legs);
      if (smartlist_len(unlinked->legs) > 0) {
        /* Get the exit from the first leg */
        leg_t *leg = smartlist_get(unlinked->legs, 0);
        tor_assert(leg);
        tor_assert(leg->circ);
        tor_assert(TO_ORIGIN_CIRCUIT(leg->circ)->cpath);
        exit = TO_ORIGIN_CIRCUIT(leg->circ)->cpath->prev->extend_info;
        tor_assert(exit);
      }
    }
  }

  return exit;
}

/** Return true iff the given conflux object is allowed to launch a new leg. If
 * the cfx object is NULL, then it is always allowed to launch a new leg. */
static bool
launch_leg_is_allowed(const conflux_t *cfx)
{
  if (!cfx) {
    goto allowed;
  }

  /* The maximum number of retry is the minimum number of legs we are allowed
   * per set plus the maximum amount of retries we are allowed to do. */
  unsigned int max_num_launch =
    conflux_params_get_num_legs_set() +
    conflux_params_get_max_unlinked_leg_retry();

  /* Only log once per nonce if we've reached the maximum. */
  if (cfx->num_leg_launch == max_num_launch) {
    log_info(LD_CIRC, "Maximum number of leg launch reached for nonce %s",
             fmt_nonce(cfx->nonce));
  }

  if (cfx->num_leg_launch >= max_num_launch) {
    return false;
  }

 allowed:
  return true;
}

/*
 * Public API.
 */

/** Launch a new conflux leg for the given nonce.
 *
 * Return true on success else false which teardowns the entire unlinked set if
 * any. */
bool
conflux_launch_leg(const uint8_t *nonce)
{
  int flags = CIRCLAUNCH_NEED_UPTIME | CIRCLAUNCH_NEED_CAPACITY |
              CIRCLAUNCH_NEED_CONFLUX;
  unlinked_circuits_t *unlinked = NULL;
  extend_info_t *exit = NULL;

  tor_assert(nonce);

  /* Get or create a new unlinked object for this leg. */
  unlinked = unlinked_get_or_create(nonce, true);
  tor_assert(unlinked);

  /* If we have an existing linked set, validate the number of leg retries
   * before attempting the launch. */
  if (!launch_leg_is_allowed(unlinked->cfx)) {
    goto err;
  }

  exit = get_exit_for_nonce(nonce);

  if (exit) {
    log_info(LD_CIRC, "Launching conflux leg for nonce %s.", fmt_nonce(nonce));
  } else {
    log_info(LD_CIRC, "Launching new conflux set for nonce %s.",
             fmt_nonce(nonce));
  }

  origin_circuit_t *circ =
    circuit_establish_circuit_conflux(nonce, CIRCUIT_PURPOSE_CONFLUX_UNLINKED,
                                      exit, flags);
  if (!circ) {
    goto err;
  }
  tor_assert(TO_CIRCUIT(circ)->conflux_pending_nonce);

  /* At this point, the unlinked object has either a new conflux_t or the one
   * used by a linked set so it is fine to use the cfx from the unlinked object
   * from now on. */

  /* Get the max_seq_sent and recv from the linked pool, if it exists, and pass
   * to new link cell. */
  uint64_t last_seq_sent = conflux_get_max_seq_sent(unlinked->cfx);
  uint64_t last_seq_recv = unlinked->cfx->last_seq_delivered;

  // TODO-329-ARTI: To support resumption/retransmit, the client should store
  // the last_seq_sent now, so that it can know how much data to retransmit to
  // the server after link. C-Tor will not be implementing this, but arti and
  // arti-relay could (if resumption seems worthwhile; it may not be worth the
  // memory storage there, either).

  /* We have a circuit, create the new leg and attach it to the set. */
  leg_t *leg = leg_new(TO_CIRCUIT(circ),
                       conflux_cell_new_link(nonce,
                                             last_seq_sent, last_seq_recv,
                                             true));

  /* Increase the retry count for this conflux object as in this nonce. */
  unlinked->cfx->num_leg_launch++;

  unlinked_leg_add(unlinked, leg);
  return true;

 err:
  return false;
}

/** The given circuit is conflux pending and has closed. This deletes the leg
 * from the set, attempt to finalize it and relaunch a new leg. If the set is
 * empty after removing this leg, it is deleted. */
static void
unlinked_circuit_closed(circuit_t *circ)
{
  uint8_t nonce[DIGEST256_LEN];
  unlinked_circuits_t *unlinked = NULL;
  bool is_client = false;

  tor_assert(circ);
  tor_assert(circ->conflux_pending_nonce);

  if (CIRCUIT_IS_ORIGIN(circ)) {
    tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_UNLINKED);
    is_client = true;
  }

  unlinked = unlinked_pool_get(circ->conflux_pending_nonce, is_client);

  /* This circuit is part of set that has already been removed previously freed
   * by another leg closing. */
  if (!unlinked) {
    return;
  }

  /* We keep the nonce here because we will try to recover if we can and the
   * pending nonce will get nullified early. */
  memcpy(nonce, circ->conflux_pending_nonce, sizeof(nonce));

  log_info(LD_CIRC, "Conflux unlinked circuit with nonce %s has closed",
           fmt_nonce(nonce));

  /* Remove leg from set. */
  unlinked_leg_del_and_free(unlinked, circ);
  /* The circuit pending nonce has been nullified at this point. */

  /* If no more legs, opportunistically free the unlinked set. */
  if (smartlist_len(unlinked->legs) == 0) {
    unlinked_pool_del_and_free(unlinked, is_client);
  } else if (!shutting_down) {
    /* Launch a new leg for this set to recover. */
    if (CIRCUIT_IS_ORIGIN(circ)) {
      conflux_launch_leg(nonce);
    }
  }
  /* After this, it might have been freed. */
  unlinked = NULL;

  /* Unlinked circuits should not have attached streams, but check
   * anyway, because The Maze. */
  validate_circ_has_no_streams(circ);
}

/** Update all stream pointers to point to this circuit.
 * This is used when a linked circuit is closed and we need to update the
 * streams to point to the remaining circuit
 */
static void
linked_update_stream_backpointers(circuit_t *circ)
{
  tor_assert(circ);
  tor_assert_nonfatal(circ->conflux);

  if (CIRCUIT_IS_ORIGIN(circ)) {
    origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
    tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_LINKED);
    /* Iterate over stream list using next_stream pointer, until null */
    for (edge_connection_t *stream = ocirc->p_streams; stream;
           stream = stream->next_stream) {
      /* Update the circuit pointer of each stream */
      stream->on_circuit = circ;
      stream->cpath_layer = ocirc->cpath->prev;
    }
  } else {
    or_circuit_t *orcirc = TO_OR_CIRCUIT(circ);
    /* Iterate over stream list using next_stream pointer, until null */
    for (edge_connection_t *stream = orcirc->n_streams; stream;
           stream = stream->next_stream) {
      /* Update the circuit pointer of each stream */
      stream->on_circuit = circ;
    }
    /* Iterate over stream list using next_stream pointer, until null */
    for (edge_connection_t *stream = orcirc->resolving_streams; stream;
           stream = stream->next_stream) {
      /* Update the circuit pointer of each stream */
      stream->on_circuit = circ;
    }
  }
}

/** Nullify all streams of the given circuit. */
static void
linked_nullify_streams(circuit_t *circ)
{
  tor_assert(circ);

  if (CIRCUIT_IS_ORIGIN(circ)) {
    origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
    ocirc->p_streams = NULL;
    ocirc->half_streams = NULL;
  } else {
    or_circuit_t *orcirc = TO_OR_CIRCUIT(circ);
    orcirc->n_streams = NULL;
    orcirc->resolving_streams = NULL;
  }
}

/** The given circuit is already linked to a set and has been closed. Remove it
 * from the set and free the pool if no more legs. */
static void
linked_circuit_closed(circuit_t *circ)
{
  bool is_client = false;
  bool full_teardown = false;
  uint8_t nonce[DIGEST256_LEN] = {0};

  tor_assert(circ);
  tor_assert(circ->conflux);

  if (CIRCUIT_IS_ORIGIN(circ)) {
    tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_LINKED);
    is_client = true;
  }

  /* Unlink circuit from its conflux object. */
  full_teardown = cfx_del_leg(circ->conflux, circ);

  if (CONFLUX_NUM_LEGS(circ->conflux) == 0) {
    /* Last leg, remove conflux object from linked set. */
    linked_pool_del(circ->conflux->nonce, is_client);
  } else {
    /* If there are other circuits, update streams backpointers and
     * nullify the stream lists. We do not free those streams in circuit_free_.
     * (They only get freed when the last circuit is freed). */
    conflux_leg_t *leg = smartlist_get(circ->conflux->legs, 0);
    linked_update_stream_backpointers(leg->circ);
    linked_nullify_streams(circ);
  }

  /* Keep the nonce so we can use it through out the rest of the function in
   * case we nullify the conflux object before. Reason is that in the case of a
   * full teardown, this function becomes basically recursive and so we must
   * nullify the conflux object of this circuit now before the recursiveness
   * starts leading to all legs being removed and thus not noticing if we are
   * the last or the first.
   *
   * Not the prettiest but that is the price to pay to live in the C-tor maze
   * and protected by ballrogs. */
  memcpy(nonce, circ->conflux->nonce, sizeof(nonce));

  /* Nullify the conflux object from the circuit being closed iff we have more
   * legs. Reason being that the last leg needs to have the conflux object
   * attached to the circuit so it can be freed in conflux_circuit_free(). */
  if (CONFLUX_NUM_LEGS(circ->conflux) > 0) {
    circ->conflux = NULL;
  }

  /* If this was a teardown condition, we need to mark other circuits,
   * including any potential unlinked circuits, for close.
   *
   * This call is recursive in the sense that linked_circuit_closed() will end
   * up being called for all legs and so by the time we come back here, the
   * linked is likely entirely gone. Thus why this is done last. */
  if (full_teardown) {
    conflux_mark_all_for_close(nonce, is_client, END_CIRC_REASON_FINISHED);
  }
}

/** The given circuit is being freed and it is a linked leg. Clean up and free
 * anything that has to do with this circuit.
 *
 * After this call, the circuit should NOT be referenced anymore anywhere. */
static void
linked_circuit_free(circuit_t *circ, bool is_client)
{
  tor_assert(circ);
  tor_assert(circ->conflux);
  if (is_client) {
    tor_assert(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_LINKED);
  }

  /* Circuit can be freed without being closed and so we try to delete this leg
   * so we can learn if this circuit is the last leg or not. */
  cfx_del_leg(circ->conflux, circ);

  if (CONFLUX_NUM_LEGS(circ->conflux) > 0) {
    /* The last leg will free the streams but until then, we nullify to avoid
     * use-after-free. */
    linked_nullify_streams(circ);
  } else {
    /* We are the last leg. */

    /* Remove from pool in case it is still lingering there else we'll end up
     * in a double free situation. */
    linked_pool_del(circ->conflux->nonce, is_client);

    /* If there is an unlinked circuit that was also created for this set, we
     * need to look for it, and tell it is no longer part of a linked set
     * anymore, so it can be freed properly, or can complete the link if it is
     * able to. Effectively, the conflux_t object lifetime is longer than
     * either the linked or unlinked sets by themselves. This is a situation we
     * could cover with handles, but so far, it is not clear they are an
     * obvious benefit for other cases than this one. */
    unlinked_circuits_t *unlinked =
      unlinked_pool_get(circ->conflux->nonce, is_client);
    if (unlinked) {
      tor_assert(unlinked->is_for_linked_set);
      unlinked->is_for_linked_set = false;
    } else {
      /* We are the last one, clear the conflux object. If an unlinked object
       * has a reference to it, it won't get freed due to is_for_linked_set
       * flag. */
      conflux_free(circ->conflux);
    }
  }
}

/** The given circuit is being freed and it is an unlinked leg. Clean up and
 * free anything that has to do with this circuit.
 *
 * After this call, the circuit should NOT be referenced anymore anywhere. */
static void
unlinked_circuit_free(circuit_t *circ, bool is_client)
{
  tor_assert(circ);
  tor_assert(circ->conflux_pending_nonce);
  if (is_client) {
    tor_assert(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_UNLINKED);
  }

  /* Cleanup circuit reference if a leg exists. This is possible if the circuit
   * was not marked for close before being freed. */
  leg_t *leg = unlinked_leg_find(circ, is_client);
  if (leg) {
    leg->circ = NULL;
  }

  /* Null pointers are safe here. */
  tor_free(circ->conflux_pending_nonce);
}

/** Circuit has been marked for close. */
void
conflux_circuit_has_closed(circuit_t *circ)
{
  /* The unlinked case. If an unlinked set exists, we delete the leg and then
   * attempt to finalize it. After that, we'll launch a new leg to recover. */
  if (circ->conflux_pending_nonce) {
    unlinked_circuit_closed(circ);
  } else if (circ->conflux) {
    linked_circuit_closed(circ);
  }
}

/** Circuit with conflux purpose just opened. */
void
conflux_circuit_has_opened(origin_circuit_t *orig_circ)
{
  circuit_t *circ = NULL;
  leg_t *leg = NULL;

  tor_assert(orig_circ);

  circ = TO_CIRCUIT(orig_circ);

  /* Extra safety layer so we never let a circuit opens if conflux is not
   * enabled. */
  if (!conflux_is_enabled(circ)) {
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    static ratelim_t conflux_ratelim = RATELIM_INIT(600);
    log_fn_ratelim(&conflux_ratelim, LOG_NOTICE, LD_CIRC,
                   "Conflux circuit opened without negotiating "
                   "congestion control");
    return;
  }

  /* Unrelated to conflux. */
  if (circ->conflux_pending_nonce == NULL) {
    goto end;
  }

  log_info(LD_CIRC, "Conflux circuit has opened with nonce %s",
           fmt_nonce(circ->conflux_pending_nonce));

  leg = unlinked_leg_find(circ, true);
  if (BUG(!leg)) {
    log_warn(LD_CIRC, "Unable to find conflux leg in unlinked set.");
    goto end;
  }

  /* On failure here, the circuit is closed and thus the leg and unlinked set
   * will be cleaned up. */
  if (!conflux_cell_send_link(leg->link, orig_circ)) {
    goto end;
  }

  /* Mark the leg on when the LINK cell is sent. Used to timeout the circuit
   * for a minimum RTT when getting the LINKED. */
  leg->link_sent_usec = monotime_absolute_usec();

 end:
  validate_circ_has_no_streams(circ);
  return;
}

/** Process a CONFLUX_LINK cell which arrived on the given circuit. */
void
conflux_process_link(circuit_t *circ, const cell_t *cell,
                     const uint16_t cell_len)
{
  unlinked_circuits_t *unlinked = NULL;
  conflux_cell_link_t *link = NULL;

  tor_assert(circ);
  tor_assert(cell);

  if (!conflux_is_enabled(circ)) {
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  /* This cell can't be received on an origin circuit because only the endpoint
   * creating the circuit sends it. */
  if (CIRCUIT_IS_ORIGIN(circ)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINK cell on an origin circuit. Closing circuit.");
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  if (!conflux_validate_source_hop(circ, NULL)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINK with further hops. Closing circuit.");
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  if (circ->conflux_pending_nonce) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINK on a circuit with a pending nonce. "
           "Closing circuit.");
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  if (circ->conflux) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINK on an already linked circuit "
           "Closing circuit.");
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  /* On errors, logging is emitted in this parsing function. */
  link = conflux_cell_parse_link(cell, cell_len);
  if (!link) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC, "Unable to parse "
           "CONFLUX_LINK cell. Closing circuit.");
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  log_info(LD_CIRC, "Processing a CONFLUX_LINK for set %s",
           fmt_nonce(link->nonce));

  /* Consider this circuit a new leg. We'll now attempt to attach it to an
   * existing set or unlinked one. */
  leg_t *leg = leg_new(circ, link);
  unlinked = unlinked_get_or_create(link->nonce, false);
  tor_assert(unlinked);

  /* Attach leg to the unlinked set. */
  unlinked_leg_add(unlinked, leg);

  /* Set the circuit in a pending conflux state for the LINKED_ACK. */
  circ->conflux_pending_nonce = tor_memdup(leg->link->nonce,
                                           sizeof(leg->link->nonce));

  /* Mark when we send the LINKED. */
  leg->link_sent_usec = monotime_absolute_usec();

  /* Send LINKED. */
  uint64_t last_seq_sent = conflux_get_max_seq_sent(unlinked->cfx);
  uint64_t last_seq_recv = unlinked->cfx->last_seq_delivered;

  // TODO-329-ARTI: To support resumption/retransmit, the server should
  // store the last_seq_sent now, so that it can know how much data
  // to retransmit to the server after link. C-Tor will not be implementing
  // this, but arti and arti-relay could (if resumption seems worthwhile;
  // it may not be worth the memory storage there, either).

  uint8_t nonce[DIGEST256_LEN];
  memcpy(nonce, circ->conflux_pending_nonce, sizeof(nonce));

  /* Link the circuit to the a conflux set immediately before the LINKED is
   * sent. Reason is that once the client sends the LINKED_ACK, there is a race
   * with the BEGIN cell that can be sent immediately after and arrive first.
   * And so, we need to sync the streams before that happens that is before we
   * receive the LINKED_ACK. */
  if (link_circuit(circ) != ERR_LINK_CIRC_OK) {
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  conflux_cell_link_t *linked = conflux_cell_new_link(nonce, last_seq_sent,
                                                      last_seq_recv, false);

  conflux_cell_send_linked(linked, TO_OR_CIRCUIT(circ));
  tor_free(linked);

 end:
  return;
}

/** Process a CONFLUX_LINKED cell which arrived on the given circuit. */
void
conflux_process_linked(circuit_t *circ, crypt_path_t *layer_hint,
                       const cell_t *cell,
                       const uint16_t cell_len)
{
  conflux_cell_link_t *link = NULL;

  tor_assert(circ);

  /*
   * There several ways a malicious exit could create problems when sending
   * back this LINKED cell.
   *
   * 1. Using a different nonce that it knows about from another set. Accepting
   *    it would mean a confirmation attack of linking sets to the same client.
   *    To address that, the cell nonce MUST be matched with the circuit nonce.
   *
   * 2. Re-Sending a LINKED cell on an already linked circuit could create side
   *    channel attacks or unpredictable issues. Circuit is closed.
   *
   * 3. Receiving a LINKED cell on a circuit that was not expecting it. Again,
   *    as (2), can create side channel(s). Circuit is closed.
   *
   * 4. Receiving a LINKED cell from the another hop other than the last one
   *    (exit). Same as (2) and (3) in terms of issues. Circuit is closed.
   */

  if (!conflux_is_enabled(circ)) {
    circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
    goto end;
  }

  /* LINKED cell are in response to a LINK cell which are only sent on an
   * origin circuit and thus received on such.*/
  if (!CIRCUIT_IS_ORIGIN(circ)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received CONFLUX_LINKED cell on a non origin circuit.");
    goto close;
  }

  if (!circ->conflux_pending_nonce) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received a CONFLUX_LINKED cell without having sent a "
           "CONFLUX_LINK cell. Closing circuit.");
    goto close;
  }

  if (circ->conflux) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received a CONFLUX_LINKED cell on a circuit that is already "
           "linked. Closing circuit.");
    goto close;
  }

  if (!conflux_validate_source_hop(circ, layer_hint)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINKED from wrong hop on circuit. Closing circuit.");
    goto close;
  }

  tor_assert_nonfatal(circ->purpose == CIRCUIT_PURPOSE_CONFLUX_UNLINKED);

  /* On errors, logging is emitted in this parsing function. */
  link = conflux_cell_parse_link(cell, cell_len);
  if (!link) {
    goto close;
  }

  log_info(LD_CIRC, "Processing a CONFLUX_LINKED for set %s",
           fmt_nonce(link->nonce));

  /* Make sure the cell nonce matches the one on the circuit that was
   * previously set by the CONFLUX_LINK cell. */
  if (tor_memneq(link->nonce, circ->conflux_pending_nonce,
                 sizeof(*link->nonce))) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received CONFLUX_LINKED but circuit nonce doesn't match "
           "cell nonce. Closing circuit.");
    goto close;
  }

  /* Find the leg from the associated unlinked set. */
  leg_t *leg = unlinked_leg_find(circ, true);
  if (BUG(!leg)) {
    log_warn(LD_CIRC, "Received CONFLUX_LINKED but can't find "
                      "associated leg. Closing circuit.");
    goto close;
  }

  log_info(LD_CIRC, "Successfully processed a CONFLUX_LINKED cell.");

  /* Free the old link, and store the new one. We need to validate
   * the one we get during finalize, not the one we sent. */
  tor_free(leg->link);
  leg->link = link;

  /* Record the RTT for this circuit. On failure, it means the RTT was too
   * high, we relaunch to recover. */
  if (!record_rtt(circ, true)) {
    goto close;
  }

  /* The following will link the circuit with its set and attempt to finalize
   * the set if all expected legs have linked. On error, we close the circuit
   * because it means the unlinked set needs to be teardowned. */
  link_circ_err_t err = link_circuit(circ);
  switch (err) {
  case ERR_LINK_CIRC_OK:
    /* Successfully linked. */
    break;
  case ERR_LINK_CIRC_INVALID_LEG:
  case ERR_LINK_CIRC_MISSING_SET:
    /* No relaunch if the leg is invalid or the set is not found as in the
     * nonce is unknown. */
    break;
  case ERR_LINK_CIRC_BAD_RTT:
  case ERR_LINK_CIRC_MISSING_LEG:
    goto close;
  }

  /* We can send the ack only if we finalize. This will not cause issues,
   * because LINKED_ACK is exempted from multiplexing in
   * conflux_should_multiplex(). */
  if (!conflux_cell_send_linked_ack(TO_ORIGIN_CIRCUIT(circ))) {
    /* On failure, the circuit is closed by the underlying function(s). */
    goto end;
  }

  goto end;

 close:
  circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);

 end:
  return;
}

/** Process a CONFLUX_LINKED_ACK cell which arrived on the given circuit. */
void
conflux_process_linked_ack(circuit_t *circ)
{
  tor_assert(circ);

  if (!conflux_is_enabled(circ)) {
    goto close;
  }

  if (CIRCUIT_IS_ORIGIN(circ)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received CONFLUX_LINKED_ACK cell on an origin circuit. Closing.");
    goto close;
  }

  if (!conflux_validate_source_hop(circ, NULL)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Got a CONFLUX_LINKED_ACK with further hops. Closing circuit.");
    goto close;
  }

  if (BUG(!circ->conflux)) {
    log_fn(LOG_PROTOCOL_WARN, LD_CIRC,
           "Received a CONFLUX_LINKED_ACK cell on a circuit that is not"
           "linked. Closing circuit.");
    goto close;
  }

  log_info(LD_CIRC, "Processing a CONFLUX_LINKED_ACK for set %s",
           fmt_nonce(circ->conflux->nonce));

  /* Record the RTT for this circuit. This should not fail */
  if (BUG(!record_rtt(circ, false))) {
    goto close;
  }

  return;

 close:
  circuit_mark_for_close(circ, END_CIRC_REASON_TORPROTOCOL);
}

/** Called when a circuit is freed.
 *
 * It is possible a conflux circuit gets freed without being closed (for
 * instance SIGTERM) and so this callback is needed in order to finalize the
 * cleanup. */
void
conflux_circuit_about_to_free(circuit_t *circ)
{
  tor_assert(circ);

  bool is_client = CIRCUIT_IS_ORIGIN(circ);

  if (circ->conflux) {
    linked_circuit_free(circ, is_client);
  } else if (circ->conflux_pending_nonce) {
    unlinked_circuit_free(circ, is_client);
  }

  /* Whatever happens, nullify all conflux related pointers. */
  circ->conflux = NULL;
  circ->conflux_pending_nonce = NULL;
}

/** Initialize the conflux pool subsystem. This is called by the subsys
 * manager. */
void
conflux_pool_init(void)
{
  if (!client_linked_pool) {
    client_linked_pool = digest256map_new();
  }
  if (!client_unlinked_pool) {
    client_unlinked_pool = digest256map_new();
  }
  if (!server_linked_pool) {
    server_linked_pool = digest256map_new();
  }
  if (!server_unlinked_pool) {
    server_unlinked_pool = digest256map_new();
  }
}

/** Free and clean up the conflux pool subsystem. This is called by the subsys
 * manager AFTER all circuits have been freed which implies that all objects in
 * the pools aren't referenced anymore. */
void
conflux_pool_free_all(void)
{
  shutting_down = true;

  digest256map_free(client_linked_pool, free_conflux_void_);
  digest256map_free(server_linked_pool, free_conflux_void_);
  digest256map_free(client_unlinked_pool, free_unlinked_void_);
  digest256map_free(server_unlinked_pool, free_unlinked_void_);
}
